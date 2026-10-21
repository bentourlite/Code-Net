"""Output and readback devices: a USB soundcard via PortAudio, a MIF4 via USB MIDI."""

from __future__ import annotations

import logging
import threading
import time

from .core import NS_PER_S, ScheduledFrame
from .mtc import messages_for_frame
from .readback import ReadbackMonitor
from .render import LtcRenderer

log = logging.getLogger(__name__)


class DacClock:
    """Turns PortAudio's per-callback DAC timestamps into a smooth sample clock.

    Callback timestamps jitter by tens of microseconds; counting samples does not.
    The count is anchored to the timestamps and slowly steered to follow them.
    """

    def __init__(self, sample_rate: int, resync_ns: int = 2_000_000, gain: float = 0.01):
        self.sample_rate = sample_rate
        self.resync_ns = resync_ns
        self.gain = gain
        self._base_ns: float | None = None
        self._samples = 0

    def block_start(self, measured_ns: int, frames: int) -> int:
        if self._base_ns is None:
            self._base_ns, self._samples = float(measured_ns), 0
        predicted = self._base_ns + self._samples * NS_PER_S / self.sample_rate
        error = measured_ns - predicted
        if abs(error) > self.resync_ns:
            log.info("audio clock resync by %.2f ms", error / 1e6)
            self._base_ns, self._samples, predicted = float(measured_ns), 0, float(measured_ns)
        else:
            self._base_ns += error * self.gain
            predicted += error * self.gain
        self._samples += frames
        return int(predicted)


class SoundcardOutput:
    """Plays LTC through an audio device, optionally reading a loop back on its input."""

    def __init__(self, renderer: LtcRenderer, device=None, sample_rate: int = 48000, blocksize: int = 64,
                 readback: ReadbackMonitor | None = None, readback_device=None):
        import sounddevice as sd  # only needed on the Pi

        self.renderer = renderer
        self.readback = readback
        self.dac = DacClock(sample_rate)
        # Many USB codecs only accept stereo on their hardware device, so open every
        # output channel (up to two) and put the same LTC on each.
        out_channels = max(1, min(2, sd.query_devices(device, "output")["max_output_channels"]))
        self.output = sd.OutputStream(device=device, samplerate=sample_rate, channels=out_channels, dtype="float32",
                                      blocksize=blocksize, latency="low", callback=self._play)
        self.input = None
        if readback is not None:
            in_channels = max(1, min(2, sd.query_devices(readback_device, "input")["max_input_channels"]))
            self.input = sd.InputStream(device=readback_device, samplerate=sample_rate, channels=in_channels,
                                        dtype="float32", blocksize=256, latency="low", callback=self._capture)

    @staticmethod
    def _to_local(stream_time: float, now_stream: float) -> int:
        return time.monotonic_ns() + int((stream_time - now_stream) * NS_PER_S)

    def _play(self, outdata, frames, time_info, status):
        if status.output_underflow:
            log.warning("audio output underflow")
        start = self.dac.block_start(self._to_local(time_info.outputBufferDacTime, time_info.currentTime), frames)
        outdata[:] = self.renderer.render(start, frames)[:, None]

    def _capture(self, indata, frames, time_info, status):
        start = self._to_local(time_info.inputBufferAdcTime, time_info.currentTime)
        self.readback.feed(indata[:, 0].copy(), start, list(self.renderer.emitted))

    def start(self) -> None:
        self.output.start()
        if self.input is not None:
            self.input.start()

    def stop(self) -> None:
        self.output.stop()
        if self.input is not None:
            self.input.stop()


def find_midi_port(hint: str = "MIF") -> str | None:
    import mido

    for name in mido.get_output_names():
        if hint.lower() in name.lower():
            return name
    return None


class MtcOutput:
    """Sends MTC to a MIF4 (or any MIDI port) on the receiver's frame schedule."""

    def __init__(self, frames, port_name: str, clock=time.monotonic_ns):
        import mido

        self.frames = frames
        self.clock = clock
        self.port = mido.open_output(port_name)
        self._mido = mido
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="codenet-mtc", daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1)
        self.port.close()

    def _send_at(self, due_ns: int, data: bytes) -> None:
        while (wait := due_ns - self.clock()) > 0:
            time.sleep(min(wait / NS_PER_S, 0.0005))
        self.port.send(self._mido.Message.from_bytes(list(data)))

    def _run(self) -> None:
        previous: ScheduledFrame | None = None
        while not self._stop.is_set():
            frame = self.frames(self.clock())
            if frame is None or frame == previous:
                time.sleep(0.0005)
                if frame is None:
                    previous = None
                continue
            duration = NS_PER_S // frame.timecode.rate
            for fraction, data in messages_for_frame(frame.timecode, previous.timecode if previous else None):
                self._send_at(frame.start_ns + int(fraction * duration), data)
            previous = frame
