"""
MQTT message handlers.

Each handler is invoked by the background MQTT loop in `app.mqtt` when a
message arrives on a topic the server subscribes to. Handlers are async
and create their own database sessions because they run outside any FastAPI
request context.

ThingsBoard is no longer in this server's responsibility — devices publish
telemetry directly to the ThingsBoard MQTT broker using their per-device
access tokens (see firmware mqtt_manager). The server still listens to
`event/humidity` to keep `last_seen_at` up to date.
"""

import json
import logging
from datetime import datetime, time, timezone

import aiomqtt

from app.database import async_session
from app.models import Device, DeviceConfig, WateringEvent

log = logging.getLogger(__name__)

# These intervals are reserved for later use; the MVP firmware does not
# consume them, but the registration response includes them so the protocol
# is forward-compatible.
TELEMETRY_INTERVAL_S = 30
OTA_CHECK_INTERVAL_S = 3600


async def handle_registration(client: aiomqtt.Client, device_id: str, payload: bytes):
    """Phase 2: register-or-update a device and reply on .../reg/response.

    On first contact, inserts a new row in `devices`. On subsequent contacts
    updates firmware/hardware version and `last_seen_at`. The response
    informs the device whether it was newly registered or already known.
    """
    try:
        data = json.loads(payload)
    except json.JSONDecodeError:
        log.warning("Invalid JSON in registration from %s", device_id)
        return

    now = datetime.now(timezone.utc)

    async with async_session() as session:
        device = await session.get(Device, device_id)

        if device is None:
            device = Device(
                device_id=device_id,
                mac_address=data.get("mac_address", ""),
                firmware_version=data.get("firmware_version", ""),
                hardware_revision=data.get("hardware_revision", ""),
                status="active",
                first_seen_at=now,
                last_seen_at=now,
            )
            session.add(device)
            status = "registered"
            message = "New device registered"
            log.info("Registered new device: %s", device_id)
        else:
            device.firmware_version = data.get("firmware_version", device.firmware_version)
            device.hardware_revision = data.get("hardware_revision", device.hardware_revision)
            device.last_seen_at = now
            status = "already_registered"
            message = "Device re-registered (updated)"
            log.info("Re-registered device: %s", device_id)

        await session.commit()

    response = json.dumps({
        "status": status,
        "message": message,
        "server_time": now.isoformat(),
        "config": {
            "telemetry_interval_s": TELEMETRY_INTERVAL_S,
            "ota_check_interval_s": OTA_CHECK_INTERVAL_S,
        },
    })

    topic = f"greenbox/{device_id}/reg/response"
    await client.publish(topic, response.encode(), qos=1, retain=False)
    log.info("Sent registration response to %s: %s", device_id, status)


async def handle_config_ack(device_id: str, payload: bytes):
    """Phase 3: persist the device's response to a config push.

    Updates `device_configs.status` to `applied` or `rejected` and stamps
    `acked_at`. Per MVP scope only `config_id` and `status` are extracted;
    other fields in the ack payload are reserved for later use.
    """
    try:
        data = json.loads(payload)
    except json.JSONDecodeError:
        log.warning("Invalid JSON in config ack from %s", device_id)
        return

    config_id = data.get("config_id")
    ack_status = data.get("status")
    if not config_id or not ack_status:
        log.warning("Missing config_id or status in ack from %s", device_id)
        return

    now = datetime.now(timezone.utc)

    async with async_session() as session:
        device = await session.get(Device, device_id)
        if device:
            device.last_seen_at = now

        from sqlalchemy import text
        result = await session.execute(
            text("SELECT id FROM device_configs WHERE config_id = :cid"),
            {"cid": config_id},
        )
        row = result.first()
        if row:
            config = await session.get(DeviceConfig, row.id)
            if config:
                config.status = ack_status
                config.acked_at = now

        await session.commit()

    log.info("Config ack from %s: config_id=%s status=%s", device_id, config_id, ack_status)


def _parse_time(val: str | None) -> time | None:
    """Parse 'HH:MM' from a watering event into a datetime.time, or None."""
    if not val:
        return None
    try:
        parts = val.split(":")
        return time(int(parts[0]), int(parts[1]))
    except (ValueError, IndexError):
        return None


async def handle_humidity_event(device_id: str, payload: bytes):
    """Update device last_seen_at on periodic humidity telemetry.

    These events arrive on `greenbox/+/event/humidity` every ~30s. They are
    NOT stored in the relational DB (high-rate time-series belongs in the
    cloud dashboard's TSDB, where the device publishes directly using its
    own ThingsBoard access token). The handler only marks the device alive.
    """
    try:
        json.loads(payload)  # validate; values not used server-side
    except json.JSONDecodeError:
        log.warning("Invalid JSON in humidity event from %s", device_id)
        return

    now = datetime.now(timezone.utc)
    async with async_session() as session:
        device = await session.get(Device, device_id)
        if device:
            device.last_seen_at = now
            await session.commit()


async def handle_watering_event(device_id: str, payload: bytes):
    """Phase 4: persist a watering event and update device last_seen_at.

    Watering events arrive on `greenbox/+/event/watering`. The server stores
    both the device-supplied timestamp and a server-side `received_at` to
    handle clock drift.
    """
    try:
        data = json.loads(payload)
    except json.JSONDecodeError:
        log.warning("Invalid JSON in watering event from %s", device_id)
        return

    now = datetime.now(timezone.utc)

    device_ts_str = data.get("timestamp", "")
    try:
        device_ts = datetime.fromisoformat(device_ts_str)
    except (ValueError, TypeError):
        device_ts = now

    event = WateringEvent(
        device_id=device_id,
        port=data.get("port", 1),
        event_type=data.get("event", ""),
        device_timestamp=device_ts,
        received_at=now,
        trigger=data.get("trigger", "schedule"),
        scheduled_time=_parse_time(data.get("scheduled_time")),
        humidity_at_start_pct=data.get("humidity_at_start_pct"),
        humidity_at_stop_pct=data.get("humidity_at_stop_pct"),
        planned_duration_s=data.get("planned_duration_s"),
        actual_duration_s=data.get("actual_duration_s"),
        stop_reason=data.get("stop_reason"),
    )

    async with async_session() as session:
        session.add(event)

        device = await session.get(Device, device_id)
        if device:
            device.last_seen_at = now

        await session.commit()

    log.info("Watering event from %s: port=%s type=%s", device_id, event.port, event.event_type)
