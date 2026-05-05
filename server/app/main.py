"""
FastAPI application entry point.

Exposes the REST API and HTML views, and runs the MQTT subscriber loop
as a background asyncio task that lives for the entire app lifetime
(via FastAPI's lifespan context manager). The MQTT loop is the bridge
between devices and the database; without it, registrations, config
acks and watering events would not be persisted.
"""

import asyncio
import logging
from contextlib import asynccontextmanager

from fastapi import FastAPI

from app.api import router as api_router
from app.mqtt import mqtt_loop
from app.views import router as views_router

logging.basicConfig(level=logging.INFO, format="%(levelname)s %(name)s: %(message)s")


@asynccontextmanager
async def lifespan(app: FastAPI):
    """Start the MQTT loop on app startup, cancel cleanly on shutdown."""
    task = asyncio.create_task(mqtt_loop())
    yield
    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        pass


app = FastAPI(title="GreenBox Server", lifespan=lifespan)
app.include_router(api_router)
app.include_router(views_router)
