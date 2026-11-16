import hashlib
import struct
from dataclasses import dataclass

MAX_FIRMWARE_BYTES = 4 * 1024 * 1024
APP_DESC_OFFSET = 32
APP_DESC_MAGIC = 0xABCD5432
VERSION_OFFSET = 48
PROJECT_OFFSET = 80
TEXT_FIELD_BYTES = 32


@dataclass(frozen=True, slots=True)
class FirmwareInfo:
    size: int
    version: str
    project_name: str
    sha256: str


def validate_firmware(data: bytes) -> FirmwareInfo:
    if not data or len(data) > MAX_FIRMWARE_BYTES:
        raise ValueError("firmware size must be between 1 byte and 4 MiB")
    if len(data) < PROJECT_OFFSET + TEXT_FIELD_BYTES:
        raise ValueError("firmware header is truncated")
    if data[0] != 0xE9:
        raise ValueError("not an ESP application image")
    if struct.unpack_from("<H", data, 13)[0] != 0:
        raise ValueError("firmware chip is not ESP32-S3")
    if struct.unpack_from("<I", data, APP_DESC_OFFSET)[0] != APP_DESC_MAGIC:
        raise ValueError("firmware application descriptor is missing")
    version = _text(data, VERSION_OFFSET)
    project_name = _text(data, PROJECT_OFFSET)
    if project_name != "catvision_camera":
        raise ValueError("firmware project is not catvision_camera")
    if not version:
        raise ValueError("firmware version is empty")
    return FirmwareInfo(size=len(data), version=version, project_name=project_name, sha256=hashlib.sha256(data).hexdigest())


def _text(data: bytes, offset: int) -> str:
    return data[offset:offset + TEXT_FIELD_BYTES].split(b"\0", 1)[0].decode("utf-8", errors="strict")
