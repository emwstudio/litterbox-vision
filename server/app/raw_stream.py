import asyncio
from hmac import compare_digest
import logging
import struct
from time import perf_counter

logger = logging.getLogger("catvision.raw_stream")
logger.setLevel(logging.INFO)
logger.addHandler(logging.StreamHandler())

from app.images import DeltaFrameEncoder
from app.live_stream import live_frame_packet
from app.ring_buffer import FrameRingBuffer


class RawLiveStreamServer:
    def __init__(self, ring: FrameRingBuffer, layout_store, token: str, host: str = "0.0.0.0", port: int = 8001):
        self.ring = ring
        self.layout_store = layout_store
        self.token = token
        self.host = host
        self.port = port
        self._server: asyncio.AbstractServer | None = None

    async def start(self) -> None:
        if self._server is None:
            self._server = await asyncio.start_server(self.handle, self.host, self.port)

    async def stop(self) -> None:
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()
            self._server = None

    async def handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            handshake = await reader.readexactly(8)
            magic, target_index, token_size, reserved = struct.unpack("<4sBBH", handshake)
            if magic != b"CVLT" or target_index >= 2 or reserved != 0 or token_size > 128:
                raise ValueError("invalid raw stream handshake")
            supplied_token = await reader.readexactly(token_size)
            if not compare_digest(supplied_token, self.token.encode()):
                raise ValueError("invalid raw stream token")
            target = ("monitor", "stats_top")[target_index]
            writer.write(b"CVOK")
            await writer.drain()
            encoder = DeltaFrameEncoder()
            previous_capture = None
            frame_count = 0
            while True:
                frame = self.ring.latest()
                if frame is None or frame.captured_at_ns == previous_capture:
                    await asyncio.sleep(0.001)
                    continue
                started = perf_counter()
                content, frame_crc, delta = await asyncio.to_thread(
                    encoder.encode_parts,
                    frame.jpeg,
                    self.layout_store.load(),
                    target,
                    full=previous_capture is None,
                    parts=8,
                )
                encoded = perf_counter()
                writer.write(live_frame_packet(content, frame_crc, delta, magic=b"CVP1", parts=8))
                await writer.drain()
                sent = perf_counter()
                acknowledgement = await asyncio.wait_for(reader.readexactly(8), timeout=2)
                if frame_count < 3 or frame_count % 100 == 0:
                    logger.info("raw frame target=%s index=%u bytes=%u encode_ms=%.1f send_ms=%.1f ack_ms=%.1f",
                                target, frame_count, len(content), (encoded - started) * 1000, (sent - encoded) * 1000,
                                (perf_counter() - sent) * 1000)
                if acknowledgement != b"CVAK" + struct.pack("<I", frame_crc):
                    raise ValueError("frame acknowledgement does not match")
                previous_capture = frame.captured_at_ns
                frame_count += 1
        except (asyncio.IncompleteReadError, TimeoutError, ConnectionError, ValueError) as exc:
            logger.warning("raw client disconnected target=%s reason=%s detail=%s", locals().get("target", "unknown"), type(exc).__name__, exc)
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except (ConnectionError, TimeoutError):
                pass
