"""IMA ADPCM block codec, bit-exact with main/audio_dsp.c on the device.

Block layout: int16 predictor (little endian), uint8 step index, uint8 reserved,
then 4-bit codes, low nibble first. Every block decodes on its own.
"""

from __future__ import annotations

import struct
from typing import Iterable, Iterator

BLOCK_SAMPLES = 256
BLOCK_BYTES = 4 + BLOCK_SAMPLES // 2

_STEP = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
]
_INDEX_ADJ = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def _clamp16(v: int) -> int:
    return 32767 if v > 32767 else (-32768 if v < -32768 else v)


class Encoder:
    """Stateful encoder: the state carries across blocks like the device's."""

    def __init__(self) -> None:
        self.predictor = 0
        self.index = 0

    def _sample(self, sample: int) -> int:
        step = _STEP[self.index]
        diff = sample - self.predictor
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        delta = step >> 3
        if diff >= step:
            code |= 4
            diff -= step
            delta += step
        if diff >= step >> 1:
            code |= 2
            diff -= step >> 1
            delta += step >> 1
        if diff >= step >> 2:
            code |= 1
            delta += step >> 2
        self.predictor = _clamp16(self.predictor - delta if code & 8 else self.predictor + delta)
        self.index = max(0, min(88, self.index + _INDEX_ADJ[code]))
        return code

    def encode_block(self, pcm: list[int]) -> bytes:
        n = min(len(pcm), BLOCK_SAMPLES) & ~1
        out = bytearray(struct.pack("<hBB", self.predictor, self.index, 0))
        for i in range(0, n, 2):
            lo = self._sample(pcm[i])
            hi = self._sample(pcm[i + 1])
            out.append(lo | (hi << 4))
        return bytes(out)


def decode_block(block: bytes) -> list[int]:
    if len(block) < 4:
        return []
    predictor, index, _ = struct.unpack_from("<hBB", block)
    index = min(index, 88)
    out: list[int] = []
    for byte in block[4:]:
        for code in (byte & 0x0F, byte >> 4):
            step = _STEP[index]
            delta = step >> 3
            if code & 4:
                delta += step
            if code & 2:
                delta += step >> 1
            if code & 1:
                delta += step >> 2
            predictor = _clamp16(predictor - delta if code & 8 else predictor + delta)
            index = max(0, min(88, index + _INDEX_ADJ[code]))
            out.append(predictor)
    return out


def decode_stream(data: bytes) -> list[int]:
    """Decode concatenated blocks; a trailing partial block is decoded too."""
    pcm: list[int] = []
    for off in range(0, len(data), BLOCK_BYTES):
        pcm.extend(decode_block(data[off:off + BLOCK_BYTES]))
    return pcm


def encode_stream(pcm: Iterable[int]) -> Iterator[bytes]:
    enc = Encoder()
    buf: list[int] = []
    for s in pcm:
        buf.append(s)
        if len(buf) == BLOCK_SAMPLES:
            yield enc.encode_block(buf)
            buf = []
    if len(buf) >= 2:
        yield enc.encode_block(buf)


def pcm_to_bytes(pcm: list[int]) -> bytes:
    return struct.pack(f"<{len(pcm)}h", *pcm)


def bytes_to_pcm(data: bytes) -> list[int]:
    n = len(data) // 2
    return list(struct.unpack(f"<{n}h", data[: n * 2]))
