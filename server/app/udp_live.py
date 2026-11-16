import asyncio
import logging
import queue
import socket
import threading
import time
import zlib

from app.images import display_frame
from app.udp_protocol import packetize_frame


class UniqueFrameQueue:
    def __init__(self, capacity):
        self._queue = queue.Queue(capacity)
        self._lock = threading.Lock()
        self._last_crc = None
        self.dropped = 0

    def push(self, jpeg: bytes) -> bool:
        crc = zlib.crc32(jpeg)
        with self._lock:
            if crc == self._last_crc:
                return False
            self._last_crc = crc
        if self._queue.full():
            self._queue.get_nowait()
            self.dropped += 1
        self._queue.put_nowait(jpeg)
        return True

    def get(self, timeout=None):
        try:
            return self._queue.get(timeout=timeout)
        except queue.Empty:
            return None

    def pending(self) -> int:
        return self._queue.qsize()


class UdpLivePush:
    def __init__(self, ring, layout_store, targets: dict[str, tuple[str, int]], fps=12, queue_size=8, resolve_interval=30.0):
        self.ring = ring
        self.layout_store = layout_store
        self.fps = fps
        self.queue_size = queue_size
        self.targets = targets
        self.resolve_interval = resolve_interval
        self._transports = {}
        self._queues = {}
        self._tasks = []
        self._log = logging.getLogger("catvision.udp_live")
        self._log.setLevel(logging.INFO)
        self._log.addHandler(logging.StreamHandler())
        self._log.propagate = False

    async def _open_target(self, target):
        loop = asyncio.get_running_loop()
        host, port = self.targets[target]
        addresses = await loop.getaddrinfo(host, port, family=socket.AF_INET, type=socket.SOCK_DGRAM)
        transport, _ = await loop.create_datagram_endpoint(asyncio.DatagramProtocol, remote_addr=addresses[0][4])
        previous = self._transports.get(target)
        self._transports[target] = transport
        if previous is not None:
            previous.close()

    async def start(self):
        for target in self.targets:
            await self._open_target(target)
            self._queues[target] = UniqueFrameQueue(self.queue_size)
            self._tasks.append(asyncio.create_task(self._send_loop(target)))
        self._tasks.append(asyncio.create_task(self._produce_loop()))

    async def stop(self):
        for task in self._tasks:
            task.cancel()
        await asyncio.gather(*self._tasks, return_exceptions=True)
        self._tasks.clear()
        for transport in self._transports.values():
            transport.close()
        self._transports.clear()

    async def _produce_loop(self):
        previous_capture = None
        while True:
            frame = self.ring.latest()
            if frame is None or frame.captured_at_ns == previous_capture:
                await asyncio.sleep(0.002)
                continue
            for target in self.targets:
                jpeg = await asyncio.to_thread(display_frame, frame.jpeg, self.layout_store.load(), target, False)
                self._queues[target].push(jpeg)
            previous_capture = frame.captured_at_ns
            await asyncio.sleep(0.001)

    async def _send_loop(self, target):
        while self._queues[target].pending() < self.queue_size:
            await asyncio.sleep(0.005)
        sequence = 0
        sent = 0
        window_started = time.monotonic()
        next_resolution = window_started + self.resolve_interval
        while True:
            started = time.monotonic()
            if started >= next_resolution:
                try:
                    await self._open_target(target)
                except OSError as error:
                    self._log.warning("target=%s address refresh failed: %s", target, error)
                next_resolution = time.monotonic() + self.resolve_interval
            jpeg = self._queues[target].get(0)
            if jpeg is not None:
                for packet in packetize_frame(sequence, jpeg):
                    self._transports[target].sendto(packet)
                    await asyncio.sleep(0.001)
                sequence = (sequence + 1) & 0xffffffff
                sent += 1
                if sent % 12 == 0:
                    now = time.monotonic()
                    self._log.info("target=%s fps=%.2f queue=%d dropped=%d", target,
                                   12 / (now - window_started), self._queues[target].pending(),
                                   self._queues[target].dropped)
                    window_started = now
            remaining = 1 / self.fps - (time.monotonic() - started)
            if remaining > 0:
                await asyncio.sleep(remaining)
