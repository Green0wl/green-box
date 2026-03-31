from datetime import datetime, timezone
from uuid import uuid4

from fastapi import APIRouter, Depends, HTTPException, Query
from sqlalchemy import text
from sqlalchemy.ext.asyncio import AsyncSession

from app.database import get_session
from app.models import Device, DeviceConfig
from app.mqtt import publish_config
from app.schemas import ConfigCreateRequest

router = APIRouter(prefix="/api", tags=["api"])


@router.get("/devices")
async def list_devices(session: AsyncSession = Depends(get_session)):
    result = await session.execute(text(
        "SELECT device_id, status, firmware_version, last_seen_at "
        "FROM devices ORDER BY last_seen_at DESC"
    ))
    devices = [
        {
            "device_id": row.device_id,
            "status": row.status,
            "firmware_version": row.firmware_version,
            "last_seen_at": row.last_seen_at.isoformat() if row.last_seen_at else None,
        }
        for row in result
    ]
    return {"devices": devices}


@router.get("/devices/{device_id}")
async def get_device(device_id: str, session: AsyncSession = Depends(get_session)):
    device = await session.get(Device, device_id)
    if not device:
        raise HTTPException(404, "Device not found")

    # Latest applied config per port
    configs_result = await session.execute(text(
        "SELECT DISTINCT ON (port) * FROM device_configs "
        "WHERE device_id = :did ORDER BY port, created_at DESC"
    ), {"did": device_id})
    configs = [
        {
            "config_id": r.config_id,
            "port": r.port,
            "watering_schedule": r.watering_schedule,
            "humidity_threshold_pct": r.humidity_threshold_pct,
            "status": r.status,
            "created_at": r.created_at.isoformat() if r.created_at else None,
            "pushed_at": r.pushed_at.isoformat() if r.pushed_at else None,
            "acked_at": r.acked_at.isoformat() if r.acked_at else None,
        }
        for r in configs_result
    ]

    # Recent watering events
    events_result = await session.execute(text(
        "SELECT * FROM watering_events "
        "WHERE device_id = :did ORDER BY device_timestamp DESC LIMIT 10"
    ), {"did": device_id})
    events = [
        {
            "port": r.port,
            "event_type": r.event_type,
            "device_timestamp": r.device_timestamp.isoformat() if r.device_timestamp else None,
            "trigger": r.trigger,
            "humidity_at_start_pct": float(r.humidity_at_start_pct) if r.humidity_at_start_pct else None,
            "humidity_at_stop_pct": float(r.humidity_at_stop_pct) if r.humidity_at_stop_pct else None,
            "planned_duration_s": r.planned_duration_s,
            "actual_duration_s": r.actual_duration_s,
            "stop_reason": r.stop_reason,
        }
        for r in events_result
    ]

    return {
        "device_id": device.device_id,
        "mac_address": device.mac_address,
        "firmware_version": device.firmware_version,
        "hardware_revision": device.hardware_revision,
        "status": device.status,
        "first_seen_at": device.first_seen_at.isoformat() if device.first_seen_at else None,
        "last_seen_at": device.last_seen_at.isoformat() if device.last_seen_at else None,
        "configs": configs,
        "recent_watering": events,
    }


@router.post("/devices/{device_id}/config", status_code=201)
async def push_config_endpoint(
    device_id: str,
    body: ConfigCreateRequest,
    session: AsyncSession = Depends(get_session),
):
    device = await session.get(Device, device_id)
    if not device:
        raise HTTPException(404, "Device not found")

    now = datetime.now(timezone.utc)
    config_id = f"cfg-{uuid4().hex[:12]}"

    config = DeviceConfig(
        device_id=device_id,
        config_id=config_id,
        port=body.port,
        watering_schedule=[s.model_dump() for s in body.watering_schedule],
        humidity_threshold_pct=body.humidity_threshold_pct,
        status="pending",
        created_at=now,
    )
    session.add(config)
    await session.commit()

    mqtt_payload = {
        "config_id": config_id,
        "timestamp": now.isoformat(),
        "port": body.port,
        "watering_schedule": [s.model_dump() for s in body.watering_schedule],
        "humidity_threshold_pct": body.humidity_threshold_pct,
    }
    await publish_config(device_id, mqtt_payload)

    config.status = "pushed"
    config.pushed_at = datetime.now(timezone.utc)
    await session.commit()

    return {
        "config_id": config_id,
        "port": body.port,
        "status": "pushed",
        "created_at": now.isoformat(),
    }


@router.get("/devices/{device_id}/config")
async def get_config(
    device_id: str,
    port: int = Query(ge=1, le=2),
    session: AsyncSession = Depends(get_session),
):
    result = await session.execute(text(
        "SELECT * FROM device_configs "
        "WHERE device_id = :did AND port = :port "
        "ORDER BY created_at DESC LIMIT 1"
    ), {"did": device_id, "port": port})
    row = result.first()
    if not row:
        raise HTTPException(404, "No config found for this port")

    return {
        "config_id": row.config_id,
        "port": row.port,
        "watering_schedule": row.watering_schedule,
        "humidity_threshold_pct": row.humidity_threshold_pct,
        "status": row.status,
        "created_at": row.created_at.isoformat() if row.created_at else None,
        "pushed_at": row.pushed_at.isoformat() if row.pushed_at else None,
        "acked_at": row.acked_at.isoformat() if row.acked_at else None,
    }


@router.get("/devices/{device_id}/watering")
async def get_watering_history(
    device_id: str,
    port: int | None = Query(default=None, ge=1, le=2),
    from_: str | None = Query(default=None, alias="from"),
    to: str | None = None,
    limit: int = Query(default=50, ge=1, le=200),
    session: AsyncSession = Depends(get_session),
):
    query = "SELECT * FROM watering_events WHERE device_id = :did"
    params: dict = {"did": device_id}

    if port is not None:
        query += " AND port = :port"
        params["port"] = port
    if from_:
        query += " AND device_timestamp >= :from_ts"
        params["from_ts"] = from_
    if to:
        query += " AND device_timestamp <= :to_ts"
        params["to_ts"] = to

    query += " ORDER BY device_timestamp DESC LIMIT :lim"
    params["lim"] = limit

    result = await session.execute(text(query), params)
    events = [
        {
            "port": r.port,
            "event_type": r.event_type,
            "device_timestamp": r.device_timestamp.isoformat() if r.device_timestamp else None,
            "trigger": r.trigger,
            "scheduled_time": str(r.scheduled_time) if r.scheduled_time else None,
            "humidity_at_start_pct": float(r.humidity_at_start_pct) if r.humidity_at_start_pct else None,
            "humidity_at_stop_pct": float(r.humidity_at_stop_pct) if r.humidity_at_stop_pct else None,
            "planned_duration_s": r.planned_duration_s,
            "actual_duration_s": r.actual_duration_s,
            "stop_reason": r.stop_reason,
        }
        for r in result
    ]

    return {"device_id": device_id, "count": len(events), "watering_events": events}
