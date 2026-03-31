from datetime import datetime, time
from decimal import Decimal

from sqlalchemy import JSON, DateTime, SmallInteger, String, Time
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column

TZDateTime = DateTime(timezone=True)


class Base(DeclarativeBase):
    pass


class Device(Base):
    __tablename__ = "devices"

    device_id: Mapped[str] = mapped_column(String(20), primary_key=True)
    mac_address: Mapped[str] = mapped_column(String(17))
    firmware_version: Mapped[str] = mapped_column(String(20))
    hardware_revision: Mapped[str] = mapped_column(String(10))
    status: Mapped[str] = mapped_column(String(20), default="active")
    first_seen_at: Mapped[datetime] = mapped_column(TZDateTime)
    last_seen_at: Mapped[datetime] = mapped_column(TZDateTime)


class DeviceConfig(Base):
    __tablename__ = "device_configs"

    id: Mapped[int] = mapped_column(primary_key=True)
    device_id: Mapped[str] = mapped_column(String(20))
    config_id: Mapped[str] = mapped_column(String(50), unique=True)
    port: Mapped[int] = mapped_column(SmallInteger)
    watering_schedule: Mapped[dict] = mapped_column(JSON)
    humidity_threshold_pct: Mapped[int]
    status: Mapped[str] = mapped_column(String(20), default="pending")
    created_at: Mapped[datetime] = mapped_column(TZDateTime)
    pushed_at: Mapped[datetime | None] = mapped_column(TZDateTime)
    acked_at: Mapped[datetime | None] = mapped_column(TZDateTime)


class WateringEvent(Base):
    __tablename__ = "watering_events"

    id: Mapped[int] = mapped_column(primary_key=True)
    device_id: Mapped[str] = mapped_column(String(20))
    port: Mapped[int] = mapped_column(SmallInteger)
    event_type: Mapped[str] = mapped_column(String(20))
    device_timestamp: Mapped[datetime] = mapped_column(TZDateTime)
    received_at: Mapped[datetime] = mapped_column(TZDateTime)
    trigger: Mapped[str] = mapped_column(String(20))
    scheduled_time: Mapped[time | None] = mapped_column(Time)
    humidity_at_start_pct: Mapped[Decimal | None]
    humidity_at_stop_pct: Mapped[Decimal | None]
    planned_duration_s: Mapped[int | None]
    actual_duration_s: Mapped[int | None]
    stop_reason: Mapped[str | None] = mapped_column(String(30))
