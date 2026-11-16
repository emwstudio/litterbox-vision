import ipaddress
from datetime import datetime
from hmac import compare_digest
from pathlib import Path

from fastapi import APIRouter, Depends, Header, HTTPException, Request
from fastapi.responses import FileResponse

router = APIRouter(prefix="/api", tags=["devices"])


def require_device_token(request: Request, x_device_token: str = Header(default="")) -> None:
    if request.client is not None and ipaddress.ip_address(request.client.host).is_loopback:
        return
    configured = request.app.state.settings.device_token
    if configured is None:
        raise HTTPException(status_code=503, detail="DEVICE_TOKEN is not configured")
    if not x_device_token or not compare_digest(x_device_token, configured.get_secret_value()):
        raise HTTPException(status_code=401, detail="invalid device token")


@router.get("/display/stats.json", dependencies=[Depends(require_device_token)])
def display_stats(request: Request, date: str | None = None) -> dict[str, object]:
    selected = date or datetime.now(request.app.state.stats.timezone).date().isoformat()
    try:
        return request.app.state.stats.daily(selected)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail="date must be YYYY-MM-DD") from exc


@router.get("/media/{relative_path:path}", dependencies=[Depends(require_device_token)])
def media(relative_path: str, request: Request) -> FileResponse:
    root = request.app.state.settings.data_dir.resolve()
    path = (root / relative_path).resolve()
    try:
        path.relative_to(root)
    except ValueError as exc:
        raise HTTPException(status_code=403, detail="media path is outside the data directory") from exc
    if not path.is_file():
        raise HTTPException(status_code=404, detail="media not found")
    return FileResponse(path)
