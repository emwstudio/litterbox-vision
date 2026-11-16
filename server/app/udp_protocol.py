import struct
import zlib

CVJ1_HEADER = struct.Struct("<4sIHHII")
CVJ1_CHUNK_BYTES = 1400
CVJ1_MAX_FRAME_BYTES = 128 * 1024
CVJ1_MAX_CHUNKS = 128


def packetize_frame(seq: int, jpeg: bytes) -> list[bytes]:
    if not 0 <= seq <= 0xffffffff or not jpeg or len(jpeg) > CVJ1_MAX_FRAME_BYTES:
        raise ValueError("invalid JPEG frame")
    crc = zlib.crc32(jpeg)
    count = (len(jpeg) + CVJ1_CHUNK_BYTES - 1) // CVJ1_CHUNK_BYTES
    return [
        CVJ1_HEADER.pack(b"CVJ1", seq, index, count, len(jpeg), crc) + jpeg[offset:offset + CVJ1_CHUNK_BYTES]
        for index, offset in enumerate(range(0, len(jpeg), CVJ1_CHUNK_BYTES))
    ]


def read_cvj1_packet(packet: bytes) -> tuple[int, int, int, int, int, bytes]:
    if len(packet) < CVJ1_HEADER.size:
        raise ValueError("truncated CVJ1 header")
    magic, seq, index, count, frame_size, crc = CVJ1_HEADER.unpack_from(packet)
    if magic != b"CVJ1":
        raise ValueError("invalid CVJ1 magic")
    payload = packet[CVJ1_HEADER.size:]
    offset = index * CVJ1_CHUNK_BYTES
    if (
        count == 0
        or count > CVJ1_MAX_CHUNKS
        or index >= count
        or frame_size == 0
        or frame_size > CVJ1_MAX_FRAME_BYTES
        or count != (frame_size + CVJ1_CHUNK_BYTES - 1) // CVJ1_CHUNK_BYTES
        or offset + len(payload) > frame_size
        or (index + 1 < count and len(payload) != CVJ1_CHUNK_BYTES)
        or (index + 1 == count and offset + len(payload) != frame_size)
    ):
        raise ValueError("invalid CVJ1 fragment")
    return seq, index, count, frame_size, crc, payload


CVN1_STRUCT = struct.Struct("<4sIHH16s")


def _validate_nack(sequence: int, first_index: int, index_count: int, mask: int) -> None:
    if not 0 <= sequence <= 0xFFFFFFFF:
        raise ValueError("invalid NACK sequence")
    if not 0 <= first_index < CVJ1_MAX_CHUNKS or not 0 <= index_count <= CVJ1_MAX_CHUNKS:
        raise ValueError("invalid NACK range")
    if first_index + index_count > CVJ1_MAX_CHUNKS:
        raise ValueError("NACK range exceeds frame capacity")
    if not 0 <= mask < (1 << CVJ1_MAX_CHUNKS):
        raise ValueError("invalid NACK mask")
    if index_count == 0:
        if first_index != 0 or mask != 0:
            raise ValueError("full-frame NACK must use a zero range and mask")
    elif mask == 0 or mask >= (1 << index_count):
        raise ValueError("NACK mask is outside the requested range")


def build_nack_request(sequence: int, first_index: int, index_count: int, mask: int) -> bytes:
    _validate_nack(sequence, first_index, index_count, mask)
    return CVN1_STRUCT.pack(b"CVN1", sequence, first_index, index_count, mask.to_bytes(16, "little"))


def read_nack_request(packet: bytes) -> tuple[int, int, int, int]:
    if len(packet) != CVN1_STRUCT.size:
        raise ValueError("invalid NACK length")
    magic, sequence, first_index, index_count, raw_mask = CVN1_STRUCT.unpack(packet)
    if magic != b"CVN1":
        raise ValueError("invalid NACK magic")
    mask = int.from_bytes(raw_mask, "little")
    _validate_nack(sequence, first_index, index_count, mask)
    return sequence, first_index, index_count, mask
