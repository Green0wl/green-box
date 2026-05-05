"""
Pydantic request schemas for the REST API.

Optional fields default to disabled values (empty list, 0) so a
half-configured port (e.g. only humidity, no schedule) is a valid input
shape and the firmware can decide which trigger to enable.
"""

from typing import Literal

from pydantic import BaseModel, Field


class WateringSlot(BaseModel):
    """One scheduled watering: time of day plus pump run duration."""

    time: str
    duration_s: int = Field(gt=0)


class ConfigCreateRequest(BaseModel):
    """Body of POST /api/devices/{id}/config.

    All trigger fields are optional. Set both schedule and humidity to
    use both triggers; leave either at its default to disable that
    trigger.
    """

    port: Literal[1, 2]
    watering_schedule: list[WateringSlot] = []
    humidity_threshold_pct: int = Field(default=0, ge=0, le=100)
    humidity_duration_s: int = Field(default=0, ge=0)
