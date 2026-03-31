from fastapi import APIRouter, Depends, Request
from fastapi.responses import HTMLResponse
from fastapi.templating import Jinja2Templates
from sqlalchemy import text
from sqlalchemy.ext.asyncio import AsyncSession

from app.database import get_session
from app.models import Device

router = APIRouter(tags=["views"])
templates = Jinja2Templates(directory="templates")


@router.get("/", response_class=HTMLResponse)
async def device_list_page(request: Request, session: AsyncSession = Depends(get_session)):
    result = await session.execute(text(
        "SELECT device_id, status, firmware_version, hardware_revision, last_seen_at "
        "FROM devices ORDER BY last_seen_at DESC"
    ))
    devices = result.all()
    return templates.TemplateResponse("devices.html", {
        "request": request,
        "devices": devices,
    })


@router.get("/devices/{device_id}", response_class=HTMLResponse)
async def device_detail_page(
    request: Request,
    device_id: str,
    session: AsyncSession = Depends(get_session),
):
    device = await session.get(Device, device_id)
    if not device:
        return HTMLResponse("Device not found", status_code=404)

    configs_result = await session.execute(text(
        "SELECT DISTINCT ON (port) * FROM device_configs "
        "WHERE device_id = :did ORDER BY port, created_at DESC"
    ), {"did": device_id})
    configs = {r.port: r for r in configs_result}

    events_result = await session.execute(text(
        "SELECT * FROM watering_events "
        "WHERE device_id = :did ORDER BY device_timestamp DESC LIMIT 20"
    ), {"did": device_id})
    events = events_result.all()

    return templates.TemplateResponse("device_detail.html", {
        "request": request,
        "device": device,
        "configs": configs,
        "events": events,
    })
