import asyncio
import logging
from time import time_ns

import httpx

from app.ring_buffer import FrameRingBuffer

logger = logging.getLogger(__name__)
logger.setLevel(logging.INFO)
logger.addHandler(logging.StreamHandler())


class StreamIngestor:
    def __init__(self, source_url: str, ring: FrameRingBuffer, token: str = "", reconnect_seconds: float = 2):
        self.source_url = source_url
        self.ring = ring
        self.token = token
        self.reconnect_seconds = reconnect_seconds
        self._task: asyncio.Task | None = None
        self._stop = asyncio.Event()
        self.last_error = ""

    @property
    def online(self) -> bool:
        latest = self.ring.latest()
        return latest is not None and (time_ns() - latest.captured_at_ns) < 3_000_000_000

    @property
    def frame_age_seconds(self) -> float | None:
        latest = self.ring.latest()
        return None if latest is None else max(0, (time_ns() - latest.captured_at_ns) / 1_000_000_000)

    def start(self) -> None:
        if self._task is None or self._task.done():
            self._stop.clear()
            self._task = asyncio.create_task(self._run(), name="camera-stream")

    async def stop(self) -> None:
        self._stop.set()
        if self._task is not None:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
            self._task = None

    async def _run(self) -> None:
        headers = {"X-Device-Token": self.token} if self.token else {}
        timeout = httpx.Timeout(10, read=30)
        async with httpx.AsyncClient(headers=headers, timeout=timeout, trust_env=False) as client:
            while not self._stop.is_set():
                try:
                    async with client.stream("GET", self.source_url) as response:
                        response.raise_for_status()
                        self.last_error = ""
                        await self._read_frames(response)
                except asyncio.CancelledError:
                    raise
                except (httpx.HTTPError, ValueError) as exc:
                    self.last_error = f"{type(exc).__name__}: {exc}"
                    logger.warning("Camera stream reconnecting: %s", self.last_error)
                try:
                    await asyncio.wait_for(self._stop.wait(), timeout=self.reconnect_seconds)
                except TimeoutError:
                    pass

    async def _read_frames(self, response: httpx.Response) -> None:
        buffer = bytearray()
        frames = 0
        sample_started_ns = None
        async for chunk in response.aiter_bytes():
            if self._stop.is_set():
                return
            buffer.extend(chunk)
            while True:
                start = buffer.find(b"\xff\xd8")
                if start < 0:
                    if buffer and buffer[-1] == 0xFF:
                        buffer[:] = b"\xff"
                    else:
                        buffer.clear()
                    break
                if start > 0:
                    del buffer[:start]
                end = buffer.find(b"\xff\xd9", 2)
                if end < 0:
                    break
                jpeg = bytes(buffer[: end + 2])
                del buffer[: end + 2]
                if self.ring.add(jpeg):
                    frames += 1
                    frame_ns = self.ring.latest().captured_at_ns
                    if frames == 1:
                        sample_started_ns = frame_ns
                    elif frames % 16 == 0:
                        logger.info("ingest frames=%d interval_ms=%.1f fps=%.2f", frames,
                                    (frame_ns - sample_started_ns) / 1_000_000,
                                    16_000_000_000 / (frame_ns - sample_started_ns))
                        sample_started_ns = frame_ns
            if len(buffer) > 8 * 1024 * 1024:
                buffer.clear()
                raise ValueError("MJPEG frame exceeded 8 MiB")
