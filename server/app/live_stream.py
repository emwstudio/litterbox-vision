import asyncio
from collections.abc import AsyncIterator
import struct

from app.domain import Layout
from app.images import DeltaFrameEncoder
from app.ring_buffer import FrameRingBuffer

LIVE_FRAME_MAGIC = b"CVF1"
FRAME_HEADER = struct.Struct("<4sIIHH")


def live_frame_packet(content: bytes, frame_crc: int, delta: bool, magic: bytes = LIVE_FRAME_MAGIC, parts: int = 0) -> bytes:
    return FRAME_HEADER.pack(magic, len(content), frame_crc, int(delta), parts) + content


async def live_packets(
    ring: FrameRingBuffer,
    layout: Layout,
    target: str,
) -> AsyncIterator[bytes]:
    encoder = DeltaFrameEncoder()
    previous_crc = 0
    previous_capture = None
    while True:
        frame = ring.latest()
        if frame is None or frame.captured_at_ns == previous_capture:
            await asyncio.sleep(0.001)
            continue
        content, frame_crc, delta = await asyncio.to_thread(
            encoder.encode,
            frame.jpeg,
            layout,
            target,
            False,
            previous_crc,
            big_endian=True,
        )
        previous_crc = frame_crc
        previous_capture = frame.captured_at_ns
        yield live_frame_packet(content, frame_crc, delta)
