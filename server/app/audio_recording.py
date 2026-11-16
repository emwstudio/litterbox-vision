from dataclasses import dataclass
from pathlib import Path

from app.audio_stream import AudioChunk


@dataclass(frozen=True, slots=True)
class AudioSegment:
    path: Path
    start_ns: int
    sample_rate: int
    channels: int
    sample_count: int
    missing_samples: int


class AudioSegmentWriter:
    def __init__(self, path: Path):
        self.path = path
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._file = self.path.open("wb")
        self._start_ns = 0
        self._next_sample_index = None
        self._sample_rate = 16000
        self._channels = 1
        self._sample_count = 0
        self._missing_samples = 0
        self._segment = None

    def append(self, chunk: AudioChunk) -> None:
        if self._file is None:
            raise ValueError("audio segment is already finished")
        if not chunk.pcm:
            return
        if (self._next_sample_index is not None and chunk.sequence == 0 and chunk.sample_index == 0
                and self._next_sample_index > chunk.sample_count):
            expected_end_ns = self._start_ns + self._sample_count * 1_000_000_000 // self._sample_rate
            missing_ns = max(0, chunk.start_ns - expected_end_ns)
            self._silence(missing_ns * self._sample_rate // 1_000_000_000)
            self._next_sample_index = None
        if self._next_sample_index is None:
            if self._sample_count == 0:
                self._start_ns = chunk.start_ns
            self._sample_rate = chunk.sample_rate
            self._channels = chunk.channels
        else:
            end_index = chunk.sample_index + chunk.sample_count
            if end_index <= self._next_sample_index:
                return
            if chunk.sample_index > self._next_sample_index:
                self._silence(chunk.sample_index - self._next_sample_index)
        first_sample = max(chunk.sample_index, self._next_sample_index or chunk.sample_index)
        skip = first_sample - chunk.sample_index
        offset = skip * chunk.channels * 2
        payload = chunk.pcm[offset:]
        self._file.write(payload)
        self._sample_count += len(payload) // (chunk.channels * 2)
        self._next_sample_index = chunk.sample_index + chunk.sample_count

    def finish(self) -> AudioSegment | None:
        if self._file is None:
            return self._segment
        self._file.close()
        self._file = None
        if self._sample_count == 0:
            self.path.unlink(missing_ok=True)
            return None
        self._segment = AudioSegment(
            path=self.path,
            start_ns=self._start_ns,
            sample_rate=self._sample_rate,
            channels=self._channels,
            sample_count=self._sample_count,
            missing_samples=self._missing_samples,
        )
        return self._segment

    def _silence(self, sample_count: int) -> None:
        bytes_per_sample = self._channels * 2
        remaining = sample_count
        while remaining:
            count = min(remaining, 320)
            self._file.write(bytes(count * bytes_per_sample))
            self._sample_count += count
            self._missing_samples += count
            remaining -= count
