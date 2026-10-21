"""SMPTE 12M linear timecode (LTC): encoding to audio and decoding from audio.

Bits are sent least significant first in an 80-bit word using biphase mark coding:
every bit cell starts with a transition, and a 1 has a second transition mid-cell.
"""

from __future__ import annotations

from dataclasses import dataclass
from functools import lru_cache

import numpy as np

from .timecode import SUPPORTED_RATES, Timecode

BITS_PER_FRAME = 80
SYNC_WORD = (0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1)  # bits 64-79 in transmit order

# The polarity correction bit sits in a different place at 25 fps.
_POLARITY_BIT = {25: 59, 30: 27}


def _put(bits: list[int], start: int, width: int, value: int) -> None:
    for i in range(width):
        bits[start + i] = (value >> i) & 1


def _get(bits, start: int, width: int) -> int:
    return sum(bits[start + i] << i for i in range(width))


def frame_bits(tc: Timecode, user_bits: int = 0) -> list[int]:
    """The 80 bits of one LTC frame in transmit order."""
    hours, minutes, seconds, frames = tc.hmsf
    bits = [0] * BITS_PER_FRAME
    _put(bits, 0, 4, frames % 10)
    _put(bits, 8, 2, frames // 10)
    _put(bits, 16, 4, seconds % 10)
    _put(bits, 24, 3, seconds // 10)
    _put(bits, 32, 4, minutes % 10)
    _put(bits, 40, 3, minutes // 10)
    _put(bits, 48, 4, hours % 10)
    _put(bits, 56, 2, hours // 10)
    for group, start in enumerate((4, 12, 20, 28, 36, 44, 52, 60)):
        _put(bits, start, 4, (user_bits >> (4 * group)) & 0xF)
    bits[64:80] = SYNC_WORD
    # An even number of ones keeps every frame starting on the same polarity.
    if sum(bits) % 2:
        bits[_POLARITY_BIT[tc.rate]] = 1
    return bits


def samples_per_frame(sample_rate: int, rate: int) -> int:
    spf = sample_rate / rate
    if spf != int(spf):
        raise ValueError(f"{sample_rate} Hz is not a whole number of samples per frame at {rate} fps")
    return int(spf)


@lru_cache(maxsize=256)
def _encode_cached(frames: int, rate: int, user_bits: int, sample_rate: int) -> np.ndarray:
    bits = frame_bits(Timecode(frames, rate), user_bits)
    spf = samples_per_frame(sample_rate, rate)
    # Half-cell boundaries, rounded so fractional cell lengths do not drift.
    edges = [int(i * spf / (2 * BITS_PER_FRAME) + 0.5) for i in range(2 * BITS_PER_FRAME + 1)]  # same as C++ lround
    out = np.empty(spf, dtype=np.float32)
    level = -1.0
    for i, bit in enumerate(bits):
        level = -level
        out[edges[2 * i]:edges[2 * i + 1]] = level
        if bit:
            level = -level
        out[edges[2 * i + 1]:edges[2 * i + 2]] = level
    out.setflags(write=False)
    return out


def encode_frame(tc: Timecode, sample_rate: int = 48000, user_bits: int = 0) -> np.ndarray:
    """One frame of LTC audio at full scale (+/-1.0).

    The level before the frame is taken as -1, so the first sample is +1 and the last
    half-cell returns to -1; consecutive frames therefore join without a glitch.
    """
    return _encode_cached(tc.frames, tc.rate, user_bits, sample_rate)


def encode_run(start: Timecode, count: int, sample_rate: int = 48000, user_bits: int = 0) -> np.ndarray:
    """`count` consecutive frames of LTC audio starting at `start`."""
    return np.concatenate([encode_frame(start.next(i), sample_rate, user_bits) for i in range(count)])


@dataclass(frozen=True)
class DecodedFrame:
    timecode: Timecode
    user_bits: int
    start_sample: int  # absolute sample index of the frame's first transition
    end_sample: int  # absolute sample index where the next frame starts


class LtcDecoder:
    """Streaming LTC decoder for 25 and 30 fps.

    Feed it consecutive blocks of samples; it returns every complete frame it finds,
    with the sample position at which that frame started.
    """

    def __init__(self, sample_rate: int, hysteresis: float = 0.05):
        self.sample_rate = sample_rate
        self.hysteresis = hysteresis
        # Start between the 25 and 30 fps cell lengths; the estimate adapts per cell.
        self._cell = sample_rate / (BITS_PER_FRAME * sum(SUPPORTED_RATES) / len(SUPPORTED_RATES))
        self._state = 0  # -1, 0 (unknown), +1
        self._last_transition: int | None = None
        self._half_start: int | None = None  # cell start of a pending first half of a 1
        self._bits: list[int] = []
        self._starts: list[int] = []
        self._position = 0

    def feed(self, samples: np.ndarray) -> list[DecodedFrame]:
        found = []
        h = self.hysteresis
        data = np.asarray(samples, dtype=np.float32)
        for index in self._transitions(data, h):
            found.extend(self._on_transition(self._position + index))
        self._position += len(data)
        return found

    def _transitions(self, data: np.ndarray, h: float):
        state = self._state
        hi = data > h
        lo = data < -h
        # Only samples outside the hysteresis band can change state.
        for i in np.flatnonzero(hi | lo):
            new = 1 if hi[i] else -1
            if new != state:
                if state != 0:
                    yield int(i)
                state = new
        self._state = state

    def _on_transition(self, pos: int) -> list[DecodedFrame]:
        last, self._last_transition = self._last_transition, pos
        if last is None:
            return []
        interval = pos - last
        cell = self._cell
        if interval > 1.5 * cell or interval < 0.25 * cell:
            # Silence, noise or a rate change: start over.
            self._reset_word()
            if 0.75 * self.sample_rate / (BITS_PER_FRAME * 40) < interval < 1.5 * self.sample_rate / (BITS_PER_FRAME * 20):
                self._cell = float(interval)
            return []
        if interval > 0.75 * cell:
            if self._half_start is not None:
                self._reset_word()
                return []
            self._cell = 0.9 * cell + 0.1 * interval
            return self._push(0, last, pos)
        self._cell = 0.9 * cell + 0.1 * 2 * interval
        if self._half_start is None:
            self._half_start = last
            return []
        start, self._half_start = self._half_start, None
        return self._push(1, start, pos)

    def _reset_word(self) -> None:
        self._half_start = None
        self._bits.clear()
        self._starts.clear()

    def _push(self, bit: int, cell_start: int, cell_end: int) -> list[DecodedFrame]:
        self._bits.append(bit)
        self._starts.append(cell_start)
        if len(self._bits) > BITS_PER_FRAME:
            del self._bits[0]
            del self._starts[0]
        if len(self._bits) < BITS_PER_FRAME or tuple(self._bits[64:]) != SYNC_WORD:
            return []
        frame = self._decode(self._bits, self._starts[0], cell_end)
        self._bits.clear()
        self._starts.clear()
        return [frame] if frame else []

    def _decode(self, bits: list[int], start: int, end: int) -> DecodedFrame | None:
        rate = min(SUPPORTED_RATES, key=lambda r: abs(self.sample_rate / r - (end - start)))
        frames = _get(bits, 0, 4) + 10 * _get(bits, 8, 2)
        seconds = _get(bits, 16, 4) + 10 * _get(bits, 24, 3)
        minutes = _get(bits, 32, 4) + 10 * _get(bits, 40, 3)
        hours = _get(bits, 48, 4) + 10 * _get(bits, 56, 2)
        user_bits = sum(_get(bits, s, 4) << (4 * g) for g, s in enumerate((4, 12, 20, 28, 36, 44, 52, 60)))
        try:
            tc = Timecode.from_hmsf(hours, minutes, seconds, frames, rate)
        except ValueError:
            return None
        return DecodedFrame(tc, user_bits, start, end)
