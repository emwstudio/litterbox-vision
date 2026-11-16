import asyncio
import logging
import time
from pathlib import Path

import cv2

from app.ring_buffer import FrameRingBuffer

logger = logging.getLogger(__name__)
logger.setLevel(logging.INFO)
logger.addHandler(logging.StreamHandler())


class DemoVideoIngestor:
    def __init__(self, path: Path, ring: FrameRingBuffer, fps: int = 16):
        if fps <= 0:
            raise ValueError("demo video fps must be positive")
        self.path = Path(path)
        self.ring = ring
        self.fps = fps
        self.last_error = ""
        self.frames_written = 0
        self.loops = 0
        self.source_fps = 0.0
        self.source_duration_seconds = 0.0
        self._next_output_frame = 0
        self._task: asyncio.Task | None = None
        self._stop = asyncio.Event()

    @property
    def online(self) -> bool:
        latest = self.ring.latest()
        return latest is not None and (time.time_ns() - latest.captured_at_ns) < 3_000_000_000

    @property
    def frame_age_seconds(self) -> float | None:
        latest = self.ring.latest()
        return None if latest is None else max(0, (time.time_ns() - latest.captured_at_ns) / 1_000_000_000)

    def start(self) -> None:
        if self._task is None or self._task.done():
            self._stop.clear()
            self.frames_written = 0
            self.loops = 0
            self._next_output_frame = 0
            self._task = asyncio.create_task(self._run(), name="demo-video-stream")

    async def stop(self) -> None:
        self._stop.set()
        if self._task is not None:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
            self._task = None

    def _next_jpeg(self, capture: cv2.VideoCapture) -> bytes:
        source_fps = capture.get(cv2.CAP_PROP_FPS)
        if source_fps <= 0:
            raise ValueError("demo video has an invalid source frame rate")
        self.source_fps = source_fps
        while True:
            ok, frame = capture.read()
            if not ok:
                self.loops += 1
                self._next_output_frame = 0
                capture.set(cv2.CAP_PROP_POS_FRAMES, 0)
                ok, frame = capture.read()
                if not ok:
                    raise ValueError(f"demo video cannot be decoded: {self.path}")
            source_index = int(capture.get(cv2.CAP_PROP_POS_FRAMES)) - 1
            if source_index + 1e-6 < self._next_output_frame * source_fps / self.fps:
                continue
            self._next_output_frame += 1
            encoded, jpeg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
            if not encoded:
                raise ValueError("demo video JPEG encoding failed")
            return jpeg.tobytes()

    async def _run(self) -> None:
        while not self._stop.is_set():
            capture = None
            try:
                capture = cv2.VideoCapture(str(self.path))
                if not capture.isOpened():
                    raise ValueError(f"demo video cannot be opened: {self.path}")
                self.source_fps = capture.get(cv2.CAP_PROP_FPS)
                if self.source_fps <= 0:
                    raise ValueError("demo video has an invalid source frame rate")
                self.source_duration_seconds = capture.get(cv2.CAP_PROP_FRAME_COUNT) / self.source_fps
                self._next_output_frame = 0
                self.last_error = ""
                next_send = time.monotonic()
                while not self._stop.is_set():
                    jpeg = await asyncio.to_thread(self._next_jpeg, capture)
                    if self.ring.add(jpeg):
                        self.frames_written += 1
                    next_send += 1 / self.fps
                    remaining = next_send - time.monotonic()
                    if remaining > 0:
                        await asyncio.sleep(remaining)
                    else:
                        next_send = time.monotonic()
            except (ValueError, cv2.error) as exc:
                self.last_error = f"{type(exc).__name__}: {exc}"
                logger.warning("Demo video reconnecting: %s", self.last_error)
            finally:
                if capture is not None:
                    capture.release()
            try:
                await asyncio.wait_for(self._stop.wait(), timeout=2)
            except TimeoutError:
                pass
