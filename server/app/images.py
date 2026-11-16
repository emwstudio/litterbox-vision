from pathlib import Path
from threading import Lock
import struct
import zlib

import cv2
import numpy as np

from app.domain import BoxRegion, Layout, Rect

TARGET_SIZES = {"monitor": (466, 466), "stats": (480, 480), "stats_top": (480, 240)}


def decode_jpeg(data: bytes) -> np.ndarray:
    image = cv2.imdecode(np.frombuffer(data, dtype=np.uint8), cv2.IMREAD_COLOR)
    if image is None:
        raise ValueError("invalid JPEG frame")
    return image


def encode_jpeg(image: np.ndarray, quality: int = 85) -> bytes:
    ok, encoded = cv2.imencode(".jpg", image, [cv2.IMWRITE_JPEG_QUALITY, quality])
    if not ok:
        raise ValueError("JPEG encoding failed")
    return encoded.tobytes()


def rotate_image(image: np.ndarray, rotation: int) -> np.ndarray:
    if rotation == 90:
        return cv2.rotate(image, cv2.ROTATE_90_CLOCKWISE)
    if rotation == 180:
        return cv2.rotate(image, cv2.ROTATE_180)
    if rotation == 270:
        return cv2.rotate(image, cv2.ROTATE_90_COUNTERCLOCKWISE)
    return image


def rect_pixels(rect: Rect, width: int, height: int) -> tuple[int, int, int, int]:
    left = round(rect.x * width)
    top = round(rect.y * height)
    right = round((rect.x + rect.width) * width)
    bottom = round((rect.y + rect.height) * height)
    return left, top, right, bottom


def rgb(hex_color: str) -> tuple[int, int, int]:
    return tuple(int(hex_color[index : index + 2], 16) for index in (5, 3, 1))


def overlay_layout(image: np.ndarray, layout: Layout) -> np.ndarray:
    rendered = image.copy()
    height, width = rendered.shape[:2]
    for index, box in enumerate(layout.boxes, start=1):
        regions = [(box.bounds, 2)]
        if box.entry is not None:
            regions.append((box.entry, 1))
        if box.exit_top is not None:
            regions.append((box.exit_top, 1))
        for rect, thickness in regions:
            left, top, right, bottom = rect_pixels(rect, width, height)
            cv2.rectangle(rendered, (left, top), (right, bottom), rgb(box.color), thickness)
        left, top, _, _ = rect_pixels(box.bounds, width, height)
        cv2.putText(rendered, str(index), (left + 4, max(18, top + 20)), cv2.FONT_HERSHEY_SIMPLEX, 0.6, rgb(box.color), 2)
    return rendered


def _render_display_frame(jpeg: bytes, layout: Layout, target: str, overlay: bool) -> np.ndarray:
    if target not in TARGET_SIZES:
        raise ValueError("unknown display target")
    image = rotate_image(decode_jpeg(jpeg), layout.rotation)
    if overlay:
        image = overlay_layout(image, layout)
    target_width, target_height = TARGET_SIZES[target]
    scales = (target_width / image.shape[1], target_height / image.shape[0])
    scale = max(scales) if target == "stats_top" else min(scales)
    resized = cv2.resize(image, (max(1, round(image.shape[1] * scale)), max(1, round(image.shape[0] * scale))), interpolation=cv2.INTER_AREA)
    source_x = max(0, (resized.shape[1] - target_width) // 2)
    source_y = max(0, (resized.shape[0] - target_height) // 2)
    cropped = resized[source_y : source_y + target_height, source_x : source_x + target_width]
    canvas = np.zeros((target_height, target_width, 3), dtype=np.uint8)
    x = (target_width - cropped.shape[1]) // 2
    y = (target_height - cropped.shape[0]) // 2
    canvas[y : y + cropped.shape[0], x : x + cropped.shape[1]] = cropped
    return canvas


def display_frame(jpeg: bytes, layout: Layout, target: str, overlay: bool) -> bytes:
    return encode_jpeg(_render_display_frame(jpeg, layout, target, overlay))


def display_frame_rgb565(jpeg: bytes, layout: Layout, target: str, overlay: bool, big_endian: bool = False) -> bytes:
    frame = _render_display_frame(jpeg, layout, target, overlay)
    blue, green, red = cv2.split(frame)
    pixels = ((red.astype(np.uint16) >> 3) << 11) | ((green.astype(np.uint16) >> 2) << 5) | (blue.astype(np.uint16) >> 3)
    return pixels.astype(">u2" if big_endian else "<u2", copy=False).tobytes()


def display_frame_zlib(jpeg: bytes, layout: Layout, target: str, overlay: bool) -> bytes:
    return zlib.compress(display_frame_rgb565(jpeg, layout, target, overlay), level=6)


class DeltaFrameEncoder:
    def __init__(self):
        self._previous: dict[str, tuple[bytes, int]] = {}
        self._lock = Lock()

    def encode(self, jpeg: bytes, layout: Layout, target: str, overlay: bool, base_crc: int, big_endian: bool = False) -> tuple[bytes, int, bool]:
        raw = display_frame_rgb565(jpeg, layout, target, overlay, big_endian=big_endian)
        crc = zlib.crc32(raw)
        with self._lock:
            previous = self._previous.get(target)
            if previous is not None and previous[1] == base_crc:
                delta = np.bitwise_xor(np.frombuffer(raw, dtype=np.uint8), np.frombuffer(previous[0], dtype=np.uint8)).tobytes()
                content = zlib.compress(delta, level=6)
                is_delta = True
            else:
                content = zlib.compress(raw, level=6)
                is_delta = False
            self._previous[target] = (raw, crc)
        return content, crc, is_delta

    def encode_parts(self, jpeg: bytes, layout: Layout, target: str, full: bool = False, parts: int = 4) -> tuple[bytes, int, bool]:
        if parts not in (2, 4, 8):
            raise ValueError("parts must be 2 or 4")
        raw = display_frame_rgb565(jpeg, layout, target, False, big_endian=True)
        width, height = TARGET_SIZES[target]
        crc = zlib.crc32(raw)
        encoded = []
        with self._lock:
            previous = self._previous.get(target)
            delta_mode = previous is not None and not full
            for index in range(parts):
                start = (height * index // parts) * width * 2
                end = (height * (index + 1) // parts) * width * 2
                part = raw[start:end]
                payload = np.bitwise_xor(
                    np.frombuffer(part, dtype=np.uint8),
                    np.frombuffer(previous[0][start:end], dtype=np.uint8),
                ).tobytes() if delta_mode else part
                content = zlib.compress(payload, level=6)
                encoded.append(struct.pack("<IIIB3x", len(content), zlib.crc32(part), len(part), int(delta_mode)) + content)
            self._previous[target] = (raw, crc)
        return b"".join(encoded), crc, delta_mode

    def encode_halves(self, jpeg: bytes, layout: Layout, target: str, full: bool = False) -> tuple[bytes, int, bool]:
        return self.encode_parts(jpeg, layout, target, full=full, parts=2)


def write_badge(jpeg: bytes, layout: Layout, box: BoxRegion, destination: Path, size: int = 64) -> None:
    image = rotate_image(decode_jpeg(jpeg), layout.rotation)
    left, top, right, bottom = rect_pixels(box.bounds, image.shape[1], image.shape[0])
    crop = image[max(0, top) : min(image.shape[0], bottom), max(0, left) : min(image.shape[1], right)]
    if crop.size == 0:
        raise ValueError(f"empty badge crop for {box.id}")
    crop = cv2.resize(crop, (size, size), interpolation=cv2.INTER_AREA)
    mask = np.zeros((size, size), dtype=np.uint8)
    cv2.rectangle(mask, (0, 0), (size, size), 255, -1)
    cv2.circle(mask, (size // 2, size // 2), size // 2, 255, -1)
    rgba = cv2.cvtColor(crop, cv2.COLOR_BGR2BGRA)
    rgba[:, :, 3] = mask
    cv2.circle(rgba, (size // 2, size // 2), size // 2 - 2, rgb(box.color), 4)
    destination.parent.mkdir(parents=True, exist_ok=True)
    ok, encoded = cv2.imencode(".png", rgba)
    if not ok:
        raise ValueError("badge encoding failed")
    temporary = destination.with_suffix(".tmp")
    temporary.write_bytes(encoded.tobytes())
    temporary.replace(destination)
