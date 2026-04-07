from typing import Literal

from pydantic import BaseModel, Field


class WateringSlot(BaseModel):
    time: str
    duration_s: int = Field(gt=0)


class ConfigCreateRequest(BaseModel):
    port: Literal[1, 2]
    watering_schedule: list[WateringSlot] = []
    humidity_threshold_pct: int = Field(default=0, ge=0, le=100)
    humidity_duration_s: int = Field(default=0, ge=0)
