import asyncio
from collections import deque
import logging
from time import time_ns
import zlib

from app.udp_protocol import CVJ1_CHUNK_BYTES, build_nack_request, read_cvj1_packet


class UdpFrameReassembler:
    NACK_DELAYS_NS = (40_000_000, 160_000_000, 400_000_000, 800_000_000)

    def __init__(self, max_pending: int = 64, timeout_ns: int = 2_500_000_000,
                 max_pending_bytes: int = 4 * 1024 * 1024, clock=time_ns):
        self.max_pending = max_pending
        self.timeout_ns = timeout_ns
        self.max_pending_bytes = max_pending_bytes
        self._clock = clock
        self._pending = {}
        self._gap_pending = {}
        self._pending_bytes = 0
        self._completed = {}
        self._finished = deque()
        self._recent_losses = deque(maxlen=32)
        self._highest_seq = None
        self._highest_first_ns = None
        self._stats = {
            "completed": 0,
            "stale": 0,
            "incomplete": 0,
            "invalid": 0,
            "crc_errors": 0,
            "resets": 0,
            "packets": 0,
            "sequence_gaps": 0,
            "nack_requests": 0,
            "repaired_frames": 0,
            "repair_timeouts": 0,
            "last_repair_ms": 0.0,
        }

    def _record_loss(self, seq: int, frame: dict, reason: str) -> None:
        self._stats["incomplete"] += 1
        self._recent_losses.append({
            "seq": seq,
            "reason": reason,
            "first_seen_ns": frame["first_seen_ns"],
            "last_seen_ns": frame["updated_ns"],
            "duration_ms": (frame["updated_ns"] - frame["first_seen_ns"]) / 1_000_000,
            "received_chunks": frame["received"],
            "total_chunks": frame["count"],
            "missing_indices": [index for index in range(frame["count"]) if not frame["seen"][index >> 3] & (1 << (index & 7))],
            "nack_attempts": frame["nack_attempts"],
        })

    def _discard_pending(self, seq: int, reason: str) -> None:
        frame = self._pending.pop(seq)
        self._pending_bytes -= frame["frame_size"]
        if reason == "timeout" and frame["nack_attempts"]:
            self._stats["repair_timeouts"] += 1
        self._record_loss(seq, frame, reason)

    def _expire(self) -> None:
        now = self._clock()
        for seq in [seq for seq, frame in self._pending.items() if now - frame["updated_ns"] >= self.timeout_ns]:
            self._discard_pending(seq, "timeout")
        for seq in [seq for seq, frame in self._gap_pending.items() if now - frame["updated_ns"] >= self.timeout_ns]:
            frame = self._gap_pending.pop(seq)
            self._stats["incomplete"] += 1
            if frame["nack_attempts"]:
                self._stats["repair_timeouts"] += 1
            self._recent_losses.append({
                "seq": seq,
                "reason": "timeout",
                "first_seen_ns": frame["first_seen_ns"],
                "last_seen_ns": frame["updated_ns"],
                "duration_ms": (frame["updated_ns"] - frame["first_seen_ns"]) / 1_000_000,
                "received_chunks": 0,
                "total_chunks": 0,
                "missing_indices": [],
                "full_frame": True,
                "nack_attempts": frame["nack_attempts"],
            })

    def _register_gaps(self, seq: int) -> None:
        now = self._clock()
        if self._highest_seq is None:
            self._highest_seq = seq
            self._highest_first_ns = now
            return
        if seq <= self._highest_seq:
            return
        previous_seq = self._highest_seq
        previous_ns = self._highest_first_ns
        self._stats["sequence_gaps"] += seq - previous_seq - 1
        for missing in range(previous_seq + 1, min(seq, previous_seq + 129)):
            if missing in self._completed or missing in self._pending:
                continue
            fraction = (missing - previous_seq) / (seq - previous_seq)
            estimated_ns = previous_ns + round((now - previous_ns) * fraction)
            self._gap_pending[missing] = {
                "first_seen_ns": estimated_ns,
                "updated_ns": now,
                "next_nack_ns": now + self.NACK_DELAYS_NS[0],
                "nack_attempts": 0,
            }
        self._highest_seq = seq
        self._highest_first_ns = now

    def nacks_due(self, limit: int = 2) -> list[bytes]:
        self._expire()
        now = self._clock()
        candidates = [(frame["next_nack_ns"], seq, False) for seq, frame in self._pending.items() if now >= frame["next_nack_ns"]]
        candidates += [(frame["next_nack_ns"], seq, True) for seq, frame in self._gap_pending.items() if now >= frame["next_nack_ns"]]
        requests = []
        for _, seq, full_frame in sorted(candidates)[:limit]:
            if full_frame:
                frame = self._gap_pending[seq]
                request = build_nack_request(seq, 0, 0, 0)
            else:
                frame = self._pending[seq]
                mask = sum(1 << index for index in range(frame["count"]) if not frame["seen"][index >> 3] & (1 << (index & 7)))
                if mask == 0:
                    continue
                request = build_nack_request(seq, 0, frame["count"], mask)
            frame["nack_attempts"] += 1
            frame["next_nack_ns"] = now + self.NACK_DELAYS_NS[min(frame["nack_attempts"], 3)]
            self._stats["nack_requests"] += 1
            requests.append(request)
        return requests

    def push(self, packet: bytes) -> tuple[int, bytes, int] | None:
        self._expire()
        try:
            seq, index, count, frame_size, expected_crc, payload = read_cvj1_packet(packet)
        except ValueError:
            self._stats["invalid"] += 1
            return None
        self._stats["packets"] += 1
        if seq == 0 and index == 0 and (self._pending or self._gap_pending or self._completed):
            self._stats["resets"] += 1
            self._pending.clear()
            self._gap_pending.clear()
            self._pending_bytes = 0
            self._completed.clear()
            self._finished.clear()
            self._highest_seq = None
            self._highest_first_ns = None
        self._register_gaps(seq)
        if seq in self._completed:
            self._stats["stale"] += 1
            return None
        frame = self._pending.get(seq)
        if frame is None:
            while self._pending and (len(self._pending) >= self.max_pending or self._pending_bytes + frame_size > self.max_pending_bytes):
                oldest = min(self._pending, key=lambda item: self._pending[item]["updated_ns"])
                self._discard_pending(oldest, "pending_limit")
            now = self._clock()
            gap = self._gap_pending.pop(seq, None)
            first_seen_ns = gap["first_seen_ns"] if gap is not None else now
            nack_attempts = gap["nack_attempts"] if gap is not None else 0
            frame = {
                "count": count,
                "frame_size": frame_size,
                "crc": expected_crc,
                "buffer": bytearray(frame_size),
                "seen": bytearray((count + 7) // 8),
                "received": 0,
                "first_seen_ns": first_seen_ns,
                "updated_ns": now,
                "next_nack_ns": now + self.NACK_DELAYS_NS[min(nack_attempts, 3)],
                "nack_attempts": nack_attempts,
            }
            self._pending[seq] = frame
            self._pending_bytes += frame_size
        if frame["count"] != count or frame["frame_size"] != frame_size or frame["crc"] != expected_crc:
            self._stats["invalid"] += 1
            self._discard_pending(seq, "invalid")
            return None
        now = self._clock()
        frame["updated_ns"] = now
        frame["next_nack_ns"] = now + self.NACK_DELAYS_NS[min(frame["nack_attempts"], 3)]
        mask = 1 << (index & 7)
        if frame["seen"][index >> 3] & mask:
            return None
        offset = index * CVJ1_CHUNK_BYTES
        frame["buffer"][offset:offset + len(payload)] = payload
        frame["seen"][index >> 3] |= mask
        frame["received"] += 1
        if frame["received"] != frame["count"]:
            return None
        jpeg = bytes(frame["buffer"])
        first_seen_ns = frame["first_seen_ns"]
        if frame["nack_attempts"]:
            self._stats["repaired_frames"] += 1
            self._stats["last_repair_ms"] = (now - first_seen_ns) / 1_000_000
        self._pending.pop(seq)
        self._pending_bytes -= frame_size
        if zlib.crc32(jpeg) != expected_crc:
            self._stats["crc_errors"] += 1
            return None
        if not jpeg.startswith(b"\xff\xd8") or not jpeg.endswith(b"\xff\xd9"):
            self._stats["invalid"] += 1
            return None
        self._completed[seq] = None
        if len(self._completed) > 256:
            self._completed.pop(next(iter(self._completed)))
        self._stats["completed"] += 1
        self._finished.append((seq, jpeg, first_seen_ns))
        if self._finished:
            return self._finished.popleft()
        return None

    def stats(self) -> dict[str, object]:
        return {
            **self._stats,
            "pending_bytes": self._pending_bytes,
            "pending": [
                {
                    "seq": seq,
                    "received_chunks": frame["received"],
                    "total_chunks": frame["count"],
                    "first_seen_ns": frame["first_seen_ns"],
                    "last_seen_ns": frame["updated_ns"],
                    "nack_attempts": frame["nack_attempts"],
                }
                for seq, frame in self._pending.items()
            ],
            "gap_pending": [
                {"seq": seq, "first_seen_ns": frame["first_seen_ns"], "nack_attempts": frame["nack_attempts"]}
                for seq, frame in self._gap_pending.items()
            ],
            "recent_losses": list(self._recent_losses),
        }


class _DatagramReceiver(asyncio.DatagramProtocol):
    def __init__(self, owner):
        self.owner = owner

    def datagram_received(self, data, addr):
        self.owner._receive(data, addr)

    def error_received(self, exc):
        self.owner.last_error = str(exc)


class UdpFrameIngestor:
    def __init__(self, ring, host="0.0.0.0", port=5060, fallback=None, source_ip=None, timeout=5.0):
        self.ring = ring
        self.host = host
        self.port = port
        self.fallback = fallback
        self.source_ip = source_ip
        self.timeout = timeout
        self.last_error = ""
        self._transport = None
        self._watchdog = None
        self._nack_task = None
        self._receiver = UdpFrameReassembler()
        self._last_complete_ns = None
        self._fallback_active = False
        self._fallback_stops = set()
        self._sources = {}
        self._repair_address = None

    @property
    def online(self):
        latest = self.ring.latest()
        return latest is not None and time_ns() - latest.captured_at_ns < 3_000_000_000

    @property
    def frame_age_seconds(self):
        latest = self.ring.latest()
        return None if latest is None else max(0, (time_ns() - latest.captured_at_ns) / 1_000_000_000)

    def stats(self) -> dict[str, object]:
        stats = self._receiver.stats()
        stats["fallback_active"] = self._fallback_active
        stats["port"] = self.port
        stats["repair_port"] = 5062
        stats["sources"] = dict(self._sources)
        return stats

    async def start(self):
        if self._transport is not None:
            return
        loop = asyncio.get_running_loop()
        self._transport, _ = await loop.create_datagram_endpoint(
            lambda: _DatagramReceiver(self), local_addr=(self.host, self.port)
        )
        self.port = self._transport.get_extra_info("sockname")[1]
        self._watchdog = asyncio.create_task(self._watchdog_loop())
        self._nack_task = asyncio.create_task(self._nack_loop())

    async def stop(self):
        for task in (self._watchdog, self._nack_task):
            if task is not None:
                task.cancel()
        await asyncio.gather(*(task for task in (self._watchdog, self._nack_task) if task is not None), return_exceptions=True)
        self._watchdog = None
        self._nack_task = None
        if self._transport is not None:
            self._transport.close()
            self._transport = None
        if self._fallback_active and self.fallback is not None:
            self._fallback_active = False
            await self.fallback.stop()
        if self._fallback_stops:
            await asyncio.gather(*self._fallback_stops, return_exceptions=True)
            self._fallback_stops.clear()

    def _receive(self, data, addr):
        if self.source_ip is not None and addr[0] != self.source_ip:
            return
        if addr[0] not in self._sources and len(self._sources) < 16:
            self._sources[addr[0]] = 0
        if addr[0] in self._sources:
            self._sources[addr[0]] += 1
        if self._repair_address is None:
            self._repair_address = (addr[0], 5062)
        result = self._receiver.push(data)
        if result is None:
            return
        seq, jpeg, captured_at_ns = result
        self._last_complete_ns = time_ns()
        if self._fallback_active and self.fallback is not None:
            self._fallback_active = False
            task = asyncio.create_task(self.fallback.stop())
            self._fallback_stops.add(task)
            task.add_done_callback(self._fallback_stops.discard)
        self.ring.add(jpeg, captured_at_ns)
        self.last_error = ""
        stats = self._receiver.stats()
        if stats["completed"] % 64 == 0:
            logging.getLogger("catvision.udp_ingest").info(
                "seq=%d completed=%d stale=%d incomplete=%d crc_errors=%d fallback=%s",
                seq, stats["completed"], stats["stale"], stats["incomplete"], stats["crc_errors"],
                self._fallback_active,
            )

    async def _nack_loop(self):
        while True:
            await asyncio.sleep(0.02)
            if self._transport is None or self._repair_address is None:
                continue
            try:
                for request in self._receiver.nacks_due():
                    self._transport.sendto(request, self._repair_address)
            except Exception as exc:
                self.last_error = str(exc)
                logging.getLogger("catvision.udp_ingest").warning("NACK loop failed: %s", exc)
                await asyncio.sleep(0.5)

    async def _watchdog_loop(self):
        while True:
            await asyncio.sleep(self.timeout)
            if self._fallback_active or self.fallback is None:
                continue
            if self._last_complete_ns is None or time_ns() - self._last_complete_ns >= self.timeout * 1_000_000_000:
                self._fallback_active = True
                self.fallback.start()
