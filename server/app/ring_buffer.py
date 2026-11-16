from bisect import bisect_left
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass
from threading import Lock
from time import time_ns


@dataclass(frozen=True, slots=True)
class VideoFrame:
    jpeg: bytes
    captured_at_ns: int


class FrameRingBuffer:
    def __init__(self, max_age_seconds: int = 45, max_frames: int = 450, on_frame: Callable[[VideoFrame], None] | None = None):
        self.max_age_ns = max_age_seconds * 1_000_000_000
        self.max_frames = max_frames
        self.on_frame = on_frame
        self._frames: deque[VideoFrame] = deque()
        self._lock = Lock()

    def add(self, jpeg: bytes, captured_at_ns: int | None = None) -> bool:
        if not jpeg.startswith(b"\xff\xd8") or not jpeg.endswith(b"\xff\xd9"):
            return False
        timestamp = time_ns() if captured_at_ns is None else captured_at_ns
        with self._lock:
            capture_times = [frame.captured_at_ns for frame in self._frames]
            index = bisect_left(capture_times, timestamp)
            if index < len(capture_times) and capture_times[index] == timestamp:
                return False
            frame = VideoFrame(jpeg=jpeg, captured_at_ns=timestamp)
            self._frames.insert(index, frame)
            cutoff = timestamp - self.max_age_ns
            while self._frames and (self._frames[0].captured_at_ns < cutoff or len(self._frames) > self.max_frames):
                self._frames.popleft()
        if self.on_frame is not None:
            self.on_frame(frame)
        return True

    def latest(self) -> VideoFrame | None:
        with self._lock:
            return self._frames[-1] if self._frames else None

    def since(self, start_ns: int, end_ns: int) -> list[VideoFrame]:
        with self._lock:
            return [frame for frame in self._frames if start_ns <= frame.captured_at_ns <= end_ns]

    def __len__(self) -> int:
        with self._lock:
            return len(self._frames)
