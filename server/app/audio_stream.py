import asyncio
import struct
from collections import deque
from dataclasses import dataclass
from threading import Lock
from time import time_ns
from typing import Protocol

AUDIO_MAGIC = b"CVA1"
AUDIO_HEADER_BYTES = 24
AUDIO_SAMPLE_RATE = 16000
AUDIO_SAMPLE_COUNT = 320
AUDIO_CHANNELS = 1
AUDIO_BITS = 16
AUDIO_PCM_BYTES = AUDIO_SAMPLE_COUNT * 2
AUDIO_PACKET_BYTES = AUDIO_HEADER_BYTES + AUDIO_PCM_BYTES


@dataclass(frozen=True, slots=True)
class AudioChunk:
    sequence: int
    sample_index: int
    pcm: bytes
    captured_at_ns: int
    sample_rate: int = AUDIO_SAMPLE_RATE
    channels: int = AUDIO_CHANNELS

    @property
    def start_ns(self) -> int:
        return self.captured_at_ns

    @property
    def sample_count(self) -> int:
        return len(self.pcm) // (2 * self.channels)

    @property
    def duration_ns(self) -> int:
        return self.sample_count * 1_000_000_000 // self.sample_rate

    @property
    def end_ns(self) -> int:
        return self.start_ns + self.duration_ns


class AudioSink(Protocol):
    def append_audio(self, chunk: AudioChunk) -> None: ...


def read_audio_packet(packet: bytes, received_at_ns: int) -> AudioChunk:
    if len(packet) < AUDIO_HEADER_BYTES:
        raise ValueError("truncated audio header")
    magic, sequence, sample_index, sample_rate, count, channels, bits = struct.unpack(">4sIQIHBB", packet[:AUDIO_HEADER_BYTES])
    if magic != AUDIO_MAGIC:
        raise ValueError("invalid audio packet magic")
    if (sample_rate, count, channels, bits) != (AUDIO_SAMPLE_RATE, AUDIO_SAMPLE_COUNT, AUDIO_CHANNELS, AUDIO_BITS):
        raise ValueError("unsupported audio format")
    if len(packet) != AUDIO_PACKET_BYTES:
        raise ValueError("audio packet size does not match its format")
    duration_ns = count * 1_000_000_000 // sample_rate
    return AudioChunk(
        sequence=sequence,
        sample_index=sample_index,
        pcm=packet[AUDIO_HEADER_BYTES:],
        captured_at_ns=received_at_ns - duration_ns,
        sample_rate=sample_rate,
        channels=channels,
    )


class AudioRingBuffer:
    def __init__(self, max_age_seconds: int = 60):
        self.max_age_ns = max_age_seconds * 1_000_000_000
        self._chunks: deque[AudioChunk] = deque()
        self._lock = Lock()

    def add(self, chunk: AudioChunk) -> bool:
        with self._lock:
            if self._chunks and chunk.captured_at_ns <= self._chunks[-1].captured_at_ns:
                return False
            self._chunks.append(chunk)
            cutoff = chunk.captured_at_ns - self.max_age_ns
            while self._chunks and self._chunks[0].captured_at_ns < cutoff:
                self._chunks.popleft()
            return True

    def latest(self) -> AudioChunk | None:
        with self._lock:
            return self._chunks[-1] if self._chunks else None

    def since(self, start_ns: int, end_ns: int) -> list[AudioChunk]:
        with self._lock:
            return [chunk for chunk in self._chunks if start_ns <= chunk.captured_at_ns <= end_ns]

    def __len__(self) -> int:
        with self._lock:
            return len(self._chunks)


class _AudioDatagramReceiver(asyncio.DatagramProtocol):
    def __init__(self, owner: "UdpAudioIngestor"):
        self.owner = owner

    def datagram_received(self, data: bytes, addr) -> None:
        self.owner._receive(data, addr)

    def error_received(self, exc: Exception) -> None:
        self.owner.last_error = str(exc)


class UdpAudioIngestor:
    def __init__(self, ring: AudioRingBuffer, recorder: AudioSink | None = None,
                 host: str = "0.0.0.0", port: int = 5061, source_ip: str | None = None):
        self.ring = ring
        self.recorder = recorder
        self.host = host
        self.port = port
        self.source_ip = source_ip
        self.last_error = ""
        self._transport = None
        self._next_sample_index = None
        self._stats = {
            "received": 0,
            "invalid": 0,
            "out_of_order": 0,
            "missing_samples": 0,
            "resets": 0,
            "write_errors": 0,
        }

    @property
    def online(self) -> bool:
        latest = self.ring.latest()
        return latest is not None and 0 <= time_ns() - latest.captured_at_ns < 3_000_000_000

    def stats(self) -> dict[str, int]:
        return {**self._stats, "port": self.port}

    async def start(self) -> None:
        if self._transport is not None:
            return
        loop = asyncio.get_running_loop()
        self._transport, _ = await loop.create_datagram_endpoint(
            lambda: _AudioDatagramReceiver(self), local_addr=(self.host, self.port)
        )
        self.port = self._transport.get_extra_info("sockname")[1]

    async def stop(self) -> None:
        if self._transport is not None:
            self._transport.close()
            self._transport = None
            await asyncio.sleep(0)

    def _receive(self, data: bytes, addr) -> None:
        if self.source_ip is not None and addr[0] != self.source_ip:
            return
        try:
            chunk = read_audio_packet(data, time_ns())
        except ValueError as exc:
            self._stats["invalid"] += 1
            self.last_error = str(exc)
            return
        expected = self._next_sample_index
        if expected is not None:
            if chunk.sequence == 0 and chunk.sample_index == 0:
                self._stats["resets"] += 1
            elif chunk.sample_index < expected:
                self._stats["out_of_order"] += 1
                return
            elif chunk.sample_index > expected:
                self._stats["missing_samples"] += chunk.sample_index - expected
        if not self.ring.add(chunk):
            self._stats["out_of_order"] += 1
            return
        self._next_sample_index = chunk.sample_index + chunk.sample_count
        self._stats["received"] += 1
        self.last_error = ""
        if self.recorder is not None:
            try:
                self.recorder.append_audio(chunk)
            except (OSError, ValueError) as exc:
                self._stats["write_errors"] += 1
                self.last_error = str(exc)
