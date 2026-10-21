"""Output confirmation: decode the LTC that actually left the device and compare."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Iterable

import numpy as np

from ..ltc import LtcDecoder
from .core import NS_PER_S, ScheduledFrame


class Confirmation(Enum):
    CONFIRMED = "confirmed"
    DRIFT = "drift"
    MISSING = "missing"  # we are outputting but nothing decodes back
    IDLE = "idle"  # nothing is being output


@dataclass(frozen=True)
class ReadbackStatus:
    state: Confirmation
    last_timecode: str | None = None
    offset_ns: int | None = None  # decoded start minus scheduled start


class ReadbackMonitor:
    """Feeds input audio (from a loop of the MIF4's or soundcard's LTC output) to a
    decoder and matches each decoded frame against what the renderer emitted."""

    def __init__(self, sample_rate: int, tolerance_ns: int = 2_000_000, missing_after_ns: int = 500_000_000):
        self.sample_rate = sample_rate
        self.tolerance_ns = tolerance_ns
        self.missing_after_ns = missing_after_ns
        self.decoder = LtcDecoder(sample_rate)
        self._block_time: dict[int, int] = {}
        self._last_match_ns: int | None = None
        self._last: ReadbackStatus = ReadbackStatus(Confirmation.IDLE)

    def feed(self, samples: np.ndarray, block_start_ns: int, emitted: Iterable[ScheduledFrame]) -> None:
        """`block_start_ns` is the local time at which the block's first sample was captured."""
        first_index = self.decoder._position
        expected = {f.timecode: f for f in emitted}
        for frame in self.decoder.feed(samples):
            start_ns = block_start_ns + (frame.start_sample - first_index) * NS_PER_S // self.sample_rate
            sched = expected.get(frame.timecode)
            if sched is None:
                self._last = ReadbackStatus(Confirmation.DRIFT, str(frame.timecode))
                continue
            offset = start_ns - sched.start_ns
            state = Confirmation.CONFIRMED if abs(offset) <= self.tolerance_ns else Confirmation.DRIFT
            self._last = ReadbackStatus(state, str(frame.timecode), offset)
            self._last_match_ns = start_ns

    def status(self, now_ns: int, outputting: bool) -> ReadbackStatus:
        if not outputting:
            return ReadbackStatus(Confirmation.IDLE)
        if self._last_match_ns is None or now_ns - self._last_match_ns > self.missing_after_ns:
            return ReadbackStatus(Confirmation.MISSING, self._last.last_timecode)
        return self._last
