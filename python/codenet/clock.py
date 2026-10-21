"""Clock offset estimation between a receiver and a host (NTP-style exchange)."""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass


@dataclass(frozen=True)
class ClockSample:
    offset_ns: int  # host clock minus local clock
    rtt_ns: int


class ClockOffsetEstimator:
    """Tracks the offset of one host's clock relative to the local clock.

    Each exchange gives t1 (local send), t2 (host receive), t3 (host send) and
    t4 (local receive). Network delay only ever adds to the round trip, so the
    sample with the smallest round trip in a recent window is the most trustworthy.
    """

    def __init__(self, window: int = 16):
        self._samples: deque[ClockSample] = deque(maxlen=window)

    def add(self, t1: int, t2: int, t3: int, t4: int) -> ClockSample:
        sample = ClockSample(offset_ns=((t2 - t1) + (t3 - t4)) // 2, rtt_ns=(t4 - t1) - (t3 - t2))
        if sample.rtt_ns >= 0:
            self._samples.append(sample)
        return sample

    @property
    def ready(self) -> bool:
        return bool(self._samples)

    @property
    def best(self) -> ClockSample | None:
        return min(self._samples, key=lambda s: s.rtt_ns) if self._samples else None

    def host_to_local(self, host_ns: int) -> int:
        best = self.best
        if best is None:
            raise RuntimeError("no clock sync samples yet")
        return host_ns - best.offset_ns
