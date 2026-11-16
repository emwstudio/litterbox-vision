import json
import logging
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path
from threading import Lock
from time import monotonic

from app.audio_recording import AudioSegment, AudioSegmentWriter
from app.audio_stream import AudioChunk
from app.ring_buffer import VideoFrame


@dataclass(frozen=True, slots=True)
class RecordedEvent:
    frames: list[VideoFrame]
    audio: AudioSegment | None


class EventRecorder:
    def __init__(self, directory: Path, diagnostic_dir: Path | None = None):
        self.directory = directory
        self.directory.mkdir(parents=True, exist_ok=True)
        self.diagnostic_dir = diagnostic_dir
        self._files = {}
        self._audio_writers = {}
        self._lock = Lock()

    def start(self, event_id: str, pre_frames: list[VideoFrame], pre_audio=()) -> None:
        started = monotonic()
        with self._lock:
            self.discard(event_id)
            path = self.directory / f"{event_id}.frames"
            self._files[event_id] = path.open("wb")
            writer = AudioSegmentWriter(self.directory / f"{event_id}.pcm")
            self._audio_writers[event_id] = writer
            for chunk in pre_audio:
                writer.append(chunk)
            for frame in pre_frames:
                self._write(event_id, frame)
        logging.getLogger("catvision.recorder").warning(
            "Start event %s frames=%d audio_chunks=%d elapsed_ms=%.3f",
            event_id, len(pre_frames), len(pre_audio), (monotonic() - started) * 1000,
        )

    def append(self, frame: VideoFrame) -> None:
        with self._lock:
            for event_id in list(self._files):
                self._write(event_id, frame)

    def append_audio(self, chunk: AudioChunk) -> None:
        with self._lock:
            for writer in list(self._audio_writers.values()):
                writer.append(chunk)

    def finish(self, event_id: str) -> RecordedEvent:
        logging.getLogger("catvision.recorder").warning("Finish event %s diagnostic_dir=%s", event_id, self.diagnostic_dir)
        with self._lock:
            file = self._files.pop(event_id, None)
            writer = self._audio_writers.pop(event_id, None)
            audio = writer.finish() if writer is not None else None
            if file is None:
                return RecordedEvent([], audio)
            file.close()
            path = self.directory / f"{event_id}.frames"
            frames = []
            with path.open("rb") as stored:
                while True:
                    header = stored.read(12)
                    if not header:
                        break
                    if len(header) != 12:
                        raise ValueError("truncated event recording header")
                    timestamp, size = struct.unpack(">QI", header)
                    if size == 0 or size > 16 * 1024 * 1024:
                        raise ValueError("invalid event recording frame size")
                    jpeg = stored.read(size)
                    if len(jpeg) != size:
                        raise ValueError("truncated event recording frame")
                    frames.append(VideoFrame(jpeg=jpeg, captured_at_ns=timestamp))
            self._write_diagnostic(event_id, frames)
            path.unlink(missing_ok=True)
            return RecordedEvent(frames, audio)

    def discard(self, event_id: str) -> None:
        file = self._files.pop(event_id, None)
        if file is not None:
            file.close()
        writer = self._audio_writers.pop(event_id, None)
        if writer is not None:
            writer.finish()
        (self.directory / f"{event_id}.frames").unlink(missing_ok=True)
        (self.directory / f"{event_id}.pcm").unlink(missing_ok=True)

    def close(self) -> None:
        with self._lock:
            for file in self._files.values():
                file.close()
            self._files.clear()
            for writer in self._audio_writers.values():
                writer.finish()
            self._audio_writers.clear()

    def _write_diagnostic(self, event_id: str, frames: list[VideoFrame]) -> None:
        if self.diagnostic_dir is None:
            return
        try:
            self.diagnostic_dir.mkdir(parents=True, exist_ok=True)
            document = {
                "event_id": event_id,
                "frames": [
                    {"timestamp_ns": frame.captured_at_ns, "crc32": zlib.crc32(frame.jpeg)}
                    for frame in frames
                ],
            }
            temporary = self.diagnostic_dir / f"{event_id}.json.tmp"
            temporary.write_text(json.dumps(document, ensure_ascii=False))
            temporary.replace(self.diagnostic_dir / f"{event_id}.json")
        except OSError as exc:
            logging.getLogger("catvision.recorder").warning("Frame diagnostic failed for %s: %s", event_id, exc)

    def _write(self, event_id: str, frame: VideoFrame) -> None:
        file = self._files[event_id]
        file.write(struct.pack(">QI", frame.captured_at_ns, len(frame.jpeg)))
        file.write(frame.jpeg)
