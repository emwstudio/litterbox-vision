import json
import socket
from dataclasses import asdict
from pathlib import Path
from urllib.parse import urlparse

import httpx
from fastapi import APIRouter, Depends, File, HTTPException, Request, UploadFile
from fastapi.responses import FileResponse

from app.api_admin import require_local_action
from app.api_devices import require_device_token
from app.domain import new_id
from app.ota import MAX_FIRMWARE_BYTES, validate_firmware

router = APIRouter(prefix="/api/ota", tags=["ota"])
FIRMWARE_NAME = "catvision_camera.bin"


def _firmware_path(request: Request) -> Path:
    return request.app.state.settings.data_dir / "ota" / FIRMWARE_NAME


def _manifest_path(request: Request) -> Path:
    return _firmware_path(request).with_name("manifest.json")


def _manifest(request: Request) -> dict:
    path = _manifest_path(request)
    if not path.is_file():
        raise HTTPException(status_code=404, detail="no OTA firmware uploaded")
    return json.loads(path.read_text())


@router.post("/firmware", dependencies=[Depends(require_local_action)])
async def upload_firmware(request: Request, firmware: UploadFile = File(...)) -> dict:
    if not (firmware.filename or "").lower().endswith(".bin"):
        raise HTTPException(status_code=422, detail="OTA upload must be a .bin file")
    data = await firmware.read(MAX_FIRMWARE_BYTES + 1)
    if len(data) > MAX_FIRMWARE_BYTES:
        raise HTTPException(status_code=413, detail="firmware exceeds the 4 MiB OTA slot")
    try:
        info = validate_firmware(data)
    except (ValueError, UnicodeError) as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    path = _firmware_path(request)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".upload-{new_id()}.tmp")
    manifest = _manifest_path(request)
    manifest_temporary = path.with_name(f".manifest-{new_id()}.tmp")
    try:
        temporary.write_bytes(data)
        temporary.replace(path)
        manifest_temporary.write_text(json.dumps(asdict(info), ensure_ascii=False, indent=2))
        manifest_temporary.replace(manifest)
    finally:
        temporary.unlink(missing_ok=True)
        manifest_temporary.unlink(missing_ok=True)
    return {**asdict(info), "download_url": "/api/ota/firmware.bin"}


@router.get("/firmware.bin", dependencies=[Depends(require_device_token)])
def download_firmware(request: Request) -> FileResponse:
    path = _firmware_path(request)
    if not path.is_file():
        raise HTTPException(status_code=404, detail="no OTA firmware uploaded")
    info = validate_firmware(path.read_bytes())
    return FileResponse(
        path,
        media_type="application/octet-stream",
        headers={"Cache-Control": "no-store", "X-Content-SHA256": info.sha256},
    )


@router.get("/manifest", dependencies=[Depends(require_device_token)])
def firmware_manifest(request: Request) -> dict:
    return _manifest(request)


@router.post("/deploy", dependencies=[Depends(require_local_action)])
async def deploy_firmware(request: Request) -> dict:
    settings = request.app.state.settings
    if not settings.device_token:
        raise HTTPException(status_code=503, detail="DEVICE_TOKEN is not configured")
    camera = urlparse(settings.camera_stream_url)
    if camera.scheme not in ("http", "https") or not camera.hostname:
        raise HTTPException(status_code=503, detail="camera URL is not configured")
    if not _firmware_path(request).is_file():
        raise HTTPException(status_code=404, detail="no OTA firmware uploaded")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
        connection.connect((camera.hostname, camera.port or 80))
        hub_ip = connection.getsockname()[0]
    download_url = f"http://{hub_ip}:{request.url.port or 80}/api/ota/firmware.bin"
    camera_url = f"{camera.scheme}://{camera.netloc}/ota"
    async with httpx.AsyncClient(timeout=10, trust_env=False) as client:
        try:
            response = await client.post(
                camera_url,
                headers={"X-Device-Token": settings.device_token.get_secret_value()},
                json={"url": download_url},
            )
        except httpx.HTTPError as exc:
            raise HTTPException(status_code=502, detail="camera OTA request failed") from exc
    if response.status_code != 202:
        raise HTTPException(status_code=502, detail=f"camera rejected OTA with status {response.status_code}")
    return {"accepted": True, "camera": response.json(), "download_url": download_url}
