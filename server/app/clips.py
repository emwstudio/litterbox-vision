import math
import subprocess
import tempfile
from datetime import datetime
from pathlib import Path
from zoneinfo import ZoneInfo

from app.audio_recording import AudioSegment
from app.images import decode_jpeg, encode_jpeg
from app.ring_buffer import FrameRingBuffer, VideoFrame


class ClipError(RuntimeError):
    pass


class ClipWriter:
    def __init__(self, ring: FrameRingBuffer, data_dir: Path, fps: int = 10, ffmpeg_path: str = "ffmpeg", timezone_info: ZoneInfo | None = None):
        self.ring = ring
        self.data_dir = data_dir
        self.fps = fps
        self.ffmpeg_path = ffmpeg_path
        self.timezone = timezone_info or ZoneInfo("Asia/Shanghai")

    def write(self, event_id: str, start_ns: int, end_ns: int) -> tuple[str, str]:
        return self.write_frames(event_id, self.ring.since(start_ns, end_ns))

    def write_frames(self, event_id: str, frames: list[VideoFrame], audio: AudioSegment | None = None) -> tuple[str, str]:
        if not frames:
            raise ClipError("no frames available for event clip")
        frames = sorted(frames, key=lambda frame: frame.captured_at_ns)
        first_ns = frames[0].captured_at_ns
        last_ns = frames[-1].captured_at_ns
        slot_count = max(1, math.ceil((last_ns - first_ns) * self.fps / 1_000_000_000) + 1)
        day = datetime.fromtimestamp(first_ns / 1_000_000_000, tz=self.timezone).strftime("%Y-%m-%d")
        directory = self.data_dir / "videos" / day
        directory.mkdir(parents=True, exist_ok=True)
        video = directory / f"{event_id}.mp4"
        cover = directory / f"{event_id}.jpg"
        aligned_audio = None
        first_image = decode_jpeg(frames[0].jpeg)
        height, width = first_image.shape[:2]
        command = [
            self.ffmpeg_path, "-hide_banner", "-loglevel", "error", "-y",
            "-f", "rawvideo", "-pix_fmt", "bgr24", "-s", f"{width}x{height}",
            "-r", str(self.fps), "-i", "pipe:0",
        ]
        if audio is not None:
            aligned_audio = self._align_audio(audio, first_ns, slot_count, event_id)
            command += [
                "-f", "s16le", "-ar", str(audio.sample_rate), "-ac", str(audio.channels),
                "-i", str(aligned_audio),
            ]
        command += [
            "-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2",
            "-c:v", "libx264", "-preset", "veryfast", "-pix_fmt", "yuv420p",
        ]
        if audio is None:
            command.append("-an")
        else:
            command += ["-c:a", "aac", "-b:a", "64k", "-t", f"{slot_count / self.fps:.6f}"]
        command += ["-movflags", "+faststart", str(video)]
        process = None
        try:
            with tempfile.TemporaryFile() as errors:
                process = subprocess.Popen(command, stdin=subprocess.PIPE, stderr=errors)
                if process.stdin is None:
                    raise ClipError("FFmpeg stdin is unavailable")
                current_index = 0
                decoded_index = -1
                current_image = None
                for slot in range(slot_count):
                    target_ns = first_ns + slot * 1_000_000_000 / self.fps
                    while current_index + 1 < len(frames) and frames[current_index + 1].captured_at_ns <= target_ns:
                        current_index += 1
                    if decoded_index != current_index:
                        current_image = decode_jpeg(frames[current_index].jpeg)
                        decoded_index = current_index
                    if current_image.shape[:2] != (height, width):
                        raise ClipError("frame dimensions changed during event")
                    process.stdin.write(current_image.tobytes())
                    if slot == slot_count // 2:
                        cover.write_bytes(encode_jpeg(current_image))
                process.stdin.close()
                return_code = process.wait(timeout=60)
                if return_code != 0:
                    errors.seek(0)
                    raise ClipError(errors.read().decode("utf-8", errors="replace"))
        except Exception as exc:
            if process is not None and process.poll() is None:
                process.kill()
                process.wait()
            video.unlink(missing_ok=True)
            cover.unlink(missing_ok=True)
            if isinstance(exc, ClipError):
                raise
            raise ClipError(f"FFmpeg failed: {exc}") from exc
        finally:
            if aligned_audio is not None:
                aligned_audio.unlink(missing_ok=True)
        if not video.exists() or video.stat().st_size == 0:
            raise ClipError("FFmpeg produced an empty clip")
        base = self.data_dir / "videos"
        return str(video.relative_to(base)), str(cover.relative_to(base))

    def _align_audio(self, audio: AudioSegment, first_ns: int, slot_count: int, event_id: str) -> Path:
        spool = self.data_dir / "spool"
        spool.mkdir(parents=True, exist_ok=True)
        output_path = spool / f"{event_id}.aligned.pcm"
        bytes_per_sample = audio.channels * 2
        target_samples = math.ceil(slot_count * audio.sample_rate / self.fps)
        source_offset = max(0, round((first_ns - audio.start_ns) * audio.sample_rate / 1_000_000_000))
        leading_samples = max(0, round((audio.start_ns - first_ns) * audio.sample_rate / 1_000_000_000))
        written_samples = 0
        try:
            with audio.path.open("rb") as source, output_path.open("wb") as output:
                source.seek(source_offset * bytes_per_sample)
                while written_samples < target_samples and leading_samples > 0:
                    count = min(leading_samples, target_samples - written_samples)
                    output.write(bytes(count * bytes_per_sample))
                    written_samples += count
                    leading_samples -= count
                while written_samples < target_samples:
                    count = min(320, target_samples - written_samples)
                    payload = source.read(count * bytes_per_sample)
                    if not payload:
                        output.write(bytes(count * bytes_per_sample))
                    else:
                        output.write(payload)
                        payload_samples = len(payload) // bytes_per_sample
                        written_samples += payload_samples
                        if payload_samples < count:
                            output.write(bytes((count - payload_samples) * bytes_per_sample))
                        continue
                    written_samples += count
        except Exception:
            output_path.unlink(missing_ok=True)
            raise
        return output_path
