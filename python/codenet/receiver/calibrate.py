"""Output delay calibration: time the output through its readback loop and set the
advance so this receiver's frames leave the device on the shared timeline."""

from __future__ import annotations

import statistics
import threading
from typing import Callable

from ..protocol import Calibrate, CalibrateMode
from .core import NS_PER_S
from .readback import ReadbackMonitor


class Calibrator:
    """Handles Calibrate requests from host apps.

    `get_advance`/`set_advance` read and apply the output advance in ns; `set_advance` is
    also where the caller saves it. Call `tick` regularly to finish a measurement.
    """

    def __init__(self, readback: ReadbackMonitor | None, get_advance: Callable[[], int],
                 set_advance: Callable[[int], None], measure_ns: int = 3 * NS_PER_S, min_frames: int = 20,
                 max_spread_ns: int = 2_000_000):
        self.readback = readback
        self.get_advance = get_advance
        self.set_advance = set_advance
        self.measure_ns = measure_ns
        self.min_frames = min_frames
        self.max_spread_ns = max_spread_ns
        self._lock = threading.Lock()
        self._last: tuple[str, int] | None = None
        self._sequence: int | None = None
        self._state: str | None = None  # measuring, done or failed
        self._message = ""
        self._measured_ns: int | None = None
        self._started_ns = 0
        self._offsets: list[int] = []

    def request(self, msg: Calibrate, host: str, now_ns: int) -> None:
        if self._last == (host, msg.sequence):
            return  # the host app repeats each request until it sees it in our status
        self._last = (host, msg.sequence)
        with self._lock:
            self._sequence = msg.sequence
            self._measured_ns = None
            if msg.mode == CalibrateMode.SET:
                self._stop_listening()
                self.set_advance(msg.value_us * 1000)
                self._state, self._message = "done", "set by hand"
            elif self.readback is None:
                self._state = "failed"
                self._message = "no readback input; loop the output back into an input and set readback_device"
            else:
                self._state, self._message = "measuring", ""
                self._started_ns = now_ns
                self._offsets = []
                self.readback.on_offset = self._offsets.append

    def tick(self, now_ns: int) -> None:
        with self._lock:
            if self._state != "measuring" or now_ns - self._started_ns < self.measure_ns:
                return
            self._stop_listening()
            offsets = sorted(self._offsets)
            if len(offsets) < self.min_frames:
                self._state = "failed"
                self._message = (f"heard {len(offsets)} frames back; play timecode and check the loop cable"
                                 if offsets else "heard nothing back; play timecode and check the loop cable")
                return
            # The middle half, so a dropped or misread frame can't skew the result.
            middle = offsets[len(offsets) // 4: len(offsets) - len(offsets) // 4]
            spread = middle[-1] - middle[0]
            if spread > self.max_spread_ns:
                self._state = "failed"
                self._message = f"readings spread over {spread / 1e6:.1f} ms; the output is unstable"
                return
            measured = round(statistics.median(middle))
            self._measured_ns = measured
            self.set_advance(self.get_advance() + measured)
            self._state, self._message = "done", "measured"

    def _stop_listening(self) -> None:
        if self.readback is not None:
            self.readback.on_offset = None

    def status(self) -> dict | None:
        """For the status report: the latest request's progress."""
        with self._lock:
            if self._sequence is None:
                return None
            return {"seq": self._sequence, "state": self._state, "message": self._message,
                    "measured_ms": round(self._measured_ns / 1e6, 2) if self._measured_ns is not None else None}
