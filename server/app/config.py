"""
Application configuration loaded from environment variables.

All settings have sensible defaults that work for local (non-Docker) development
with services running on localhost.
"""

from pydantic_settings import BaseSettings


class Settings(BaseSettings):
    database_url: str = "postgresql+asyncpg://greenbox:greenbox@localhost:5432/greenbox"

    mqtt_host: str = "localhost"
    mqtt_port: int = 1883
    mqtt_username: str = "greenbox-server"
    mqtt_password: str = "serverpass"

    # ThingsBoard runs as a separate container; the server is no longer
    # in the telemetry path. Devices publish directly to TB MQTT using
    # their per-device access tokens (set via the provisioning form).


settings = Settings()
