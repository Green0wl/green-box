from pydantic_settings import BaseSettings


class Settings(BaseSettings):
    database_url: str = "postgresql+asyncpg://greenbox:greenbox@localhost:5432/greenbox"
    mqtt_host: str = "localhost"
    mqtt_port: int = 1883
    mqtt_username: str = "greenbox-server"
    mqtt_password: str = "serverpass"


settings = Settings()
