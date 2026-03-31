import asyncio
import json
import logging

import aiomqtt

from app.config import settings
from app.handlers import handle_config_ack, handle_registration, handle_watering_event

log = logging.getLogger(__name__)

_client: aiomqtt.Client | None = None


async def mqtt_loop():
    global _client

    while True:
        try:
            async with aiomqtt.Client(
                hostname=settings.mqtt_host,
                port=settings.mqtt_port,
                username=settings.mqtt_username,
                password=settings.mqtt_password,
            ) as client:
                _client = client
                log.info("MQTT connected to %s:%s", settings.mqtt_host, settings.mqtt_port)

                await client.subscribe("greenbox/+/reg/request", qos=1)
                await client.subscribe("greenbox/+/config/ack", qos=1)
                await client.subscribe("greenbox/+/event/watering", qos=1)

                async for message in client.messages:
                    parts = str(message.topic).split("/")
                    if len(parts) != 4:
                        continue
                    _, device_id, domain, action = parts

                    try:
                        if domain == "reg" and action == "request":
                            await handle_registration(client, device_id, message.payload)
                        elif domain == "config" and action == "ack":
                            await handle_config_ack(device_id, message.payload)
                        elif domain == "event" and action == "watering":
                            await handle_watering_event(device_id, message.payload)
                    except Exception:
                        log.exception("Error handling %s/%s from %s", domain, action, device_id)

        except aiomqtt.MqttError as e:
            _client = None
            log.warning("MQTT disconnected: %s — reconnecting in 5s", e)
            await asyncio.sleep(5)
        except asyncio.CancelledError:
            _client = None
            raise


async def publish_config(device_id: str, payload: dict):
    if _client is None:
        raise RuntimeError("MQTT client not connected")
    topic = f"greenbox/{device_id}/config/push"
    await _client.publish(topic, json.dumps(payload).encode(), qos=1, retain=True)
    log.info("Published config to %s", topic)
