"""SMPTE timecode values at integer frame rates (25 and 30 fps)."""

from __future__ import annotations

from dataclasses import dataclass

SUPPORTED_RATES = (25, 30)
FRAMES_PER_DAY = {rate: rate * 60 * 60 * 24 for rate in SUPPORTED_RATES}


@dataclass(frozen=True, order=True)
class Timecode:
    """A timecode position, stored as a frame count since 00:00:00:00."""

    frames: int
    rate: int

    def __post_init__(self) -> None:
        if self.rate not in SUPPORTED_RATES:
            raise ValueError(f"unsupported frame rate {self.rate}; supported: {SUPPORTED_RATES}")
        if not 0 <= self.frames < FRAMES_PER_DAY[self.rate]:
            raise ValueError(f"frame count {self.frames} outside one day at {self.rate} fps")

    @classmethod
    def from_hmsf(cls, hours: int, minutes: int, seconds: int, frames: int, rate: int) -> Timecode:
        if not (0 <= hours < 24 and 0 <= minutes < 60 and 0 <= seconds < 60 and 0 <= frames < rate):
            raise ValueError(f"invalid timecode {hours}:{minutes}:{seconds}:{frames} at {rate} fps")
        return cls(((hours * 60 + minutes) * 60 + seconds) * rate + frames, rate)

    @classmethod
    def parse(cls, text: str, rate: int) -> Timecode:
        parts = text.replace(";", ":").split(":")
        if len(parts) != 4:
            raise ValueError(f"expected HH:MM:SS:FF, got {text!r}")
        return cls.from_hmsf(*(int(p) for p in parts), rate=rate)

    @property
    def hmsf(self) -> tuple[int, int, int, int]:
        seconds, frames = divmod(self.frames, self.rate)
        minutes, seconds = divmod(seconds, 60)
        hours, minutes = divmod(minutes, 60)
        return hours, minutes, seconds, frames

    def next(self, count: int = 1) -> Timecode:
        """The timecode `count` frames later, wrapping at midnight."""
        return Timecode((self.frames + count) % FRAMES_PER_DAY[self.rate], self.rate)

    def follows(self, previous: Timecode) -> bool:
        """True when this frame is the one directly after `previous`."""
        return self.rate == previous.rate and previous.next() == self

    @property
    def frame_duration_ns(self) -> int:
        return 1_000_000_000 // self.rate

    def __str__(self) -> str:
        return "%02d:%02d:%02d:%02d" % self.hmsf
