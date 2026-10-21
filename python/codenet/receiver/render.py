"""Turns the receiver's frame schedule into LTC audio, block by block."""

from __future__ import annotations

from collections import deque
from typing import Callable

import numpy as np

from ..ltc import encode_frame
from .core import NS_PER_S, ScheduledFrame

FrameSource = Callable[[int], "ScheduledFrame | None"]


class LtcRenderer:
    """Fills audio blocks with LTC so each frame starts on its scheduled sample.

    `render` is given the local clock time at which the block's first sample reaches
    the output (the DAC), so the audio device's own buffering is compensated for.
    """

    def __init__(self, frames: FrameSource, sample_rate: int = 48000, level: float = 0.5, history: int = 64):
        self.frames = frames
        self.sample_rate = sample_rate
        self.level = level
        # Frames actually written to the output, for readback confirmation.
        self.emitted: deque[ScheduledFrame] = deque(maxlen=history)

    def _sample_time(self, block_start_ns: int, index: int) -> int:
        return block_start_ns + index * NS_PER_S // self.sample_rate

    def render(self, block_start_ns: int, count: int) -> np.ndarray:
        # Each sample takes the frame covering the middle of its sample period, which rounds
        # frame starts to the nearest sample without a sample being skipped or repeated.
        half_sample_ns = NS_PER_S // (2 * self.sample_rate)
        out = np.zeros(count, dtype=np.float32)
        i = 0
        while i < count:
            t = self._sample_time(block_start_ns, i) + half_sample_ns
            frame = self.frames(t)
            if frame is None:
                i += self._samples_until_next_check(count - i)
                continue
            wave = encode_frame(frame.timecode, self.sample_rate, frame.user_bits)
            offset = (t - frame.start_ns) * self.sample_rate // NS_PER_S
            if offset >= len(wave):
                # t is just past this frame's last sample: hold its final level.
                out[i] = wave[-1]
                i += 1
                continue
            n = min(len(wave) - offset, count - i)
            out[i:i + n] = wave[offset:offset + n]
            if offset == 0 and (not self.emitted or self.emitted[-1] != frame):
                self.emitted.append(frame)
            i += n
        out *= self.level
        return out

    def _samples_until_next_check(self, remaining: int) -> int:
        # While silent, look again every half millisecond so a new frame starts promptly.
        return max(1, min(remaining, self.sample_rate // 2000))
