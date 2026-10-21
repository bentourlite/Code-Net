"""Command-line entry point for the Raspberry Pi receiver."""

from __future__ import annotations

import argparse
import logging
import signal
import threading
import time
from pathlib import Path

from ..net import frame_receiver_socket
from ..protocol import source_id
from . import settings as settings_mod
from . import status
from .core import Receiver, ReceiverConfig
from .readback import ReadbackMonitor
from .render import LtcRenderer
from .service import ReceiverService

log = logging.getLogger("codenet.receiver")

# Command-line options and the settings field each one sets.
_OPTIONS = [
    ("--name", "name", str, "this receiver's name (up to 16 bytes)"),
    ("--main", "main", str, "name of the main host"),
    ("--backup", "backup", str, "name of the backup host"),
    ("--output", "output", str, "soundcard or mif4"),
    ("--device", "device", str, "audio device name or index for LTC output"),
    ("--midi-port", "midi_port", str, "MIDI port for MTC (default: first port whose name contains MIF)"),
    ("--sample-rate", "sample_rate", int, "audio sample rate"),
    ("--blocksize", "blocksize", int, "audio block size in samples"),
    ("--level", "level", float, "LTC output level, 0 to 1"),
    ("--advance-ms", "advance_ms", float, "start each frame this much earlier (device delay)"),
    ("--readback-device", "readback_device", str, "audio input that hears the LTC output, for confirmation"),
    ("--loss-timeout-frames", "loss_timeout_frames", int, "frames without packets before a host counts as lost"),
    ("--max-predict-frames", "max_predict_frames", int, "frames generated past the newest packet before stopping"),
    ("--group", "group", str, "multicast group, or 'none' for unicast"),
    ("--port", "port", int, "UDP port for frame packets"),
]


def parse_args(argv=None):
    p = argparse.ArgumentParser(
        description="Code Net receiver: one LTC output from a main and a backup host. "
                    "Settings come from the config file; options given here override it.")
    p.add_argument("--config", type=Path, default=None,
                   help=f"settings file (default {settings_mod.DEFAULT_CONFIG} if it exists)")
    p.add_argument("--status-file", type=Path, default=None,
                   help=f"where to write live status (default {settings_mod.DEFAULT_STATUS} if writable)")
    for flag, dest, kind, text in _OPTIONS:
        p.add_argument(flag, dest=dest, type=kind, default=None, help=text)
    p.add_argument("--wifi", dest="wifi", action="store_true", default=None,
                   help="testing over Wi-Fi: ride through packet gaps of up to 6 frames (240 ms at 25 fps)")
    p.add_argument("--no-auto-return", dest="auto_return", action="store_false", default=None,
                   help="only return to main when asked")
    p.add_argument("-v", "--verbose", action="store_true")
    return p.parse_args(argv)


def resolve_settings(args) -> settings_mod.ReceiverSettings:
    path = args.config or settings_mod.DEFAULT_CONFIG
    if args.config is not None and not path.exists():
        raise SystemExit(f"config file {path} not found")
    settings = settings_mod.load(path)
    for _, dest, _, _ in _OPTIONS + [(None, "wifi", None, None), (None, "auto_return", None, None)]:
        value = getattr(args, dest)
        if value is not None:
            setattr(settings, dest, value)
    return settings


def _status_path(args) -> Path | None:
    path = args.status_file or settings_mod.DEFAULT_STATUS
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
        return path
    except OSError:
        if args.status_file:
            raise
        return None


def main(argv=None) -> None:
    args = parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    s = resolve_settings(args)
    problems = s.validate()
    if problems:
        raise SystemExit("invalid settings: " + "; ".join(problems) + " (run codenet-menu to set them up)")
    loss_timeout, max_predict = s.effective_windows()
    if s.wifi:
        log.warning("Wi-Fi mode: output continues up to %d frames after packets stop; use wired for shows",
                    max_predict)
    if loss_timeout > max_predict:
        log.warning("loss timeout is longer than the prediction window; failover will leave a gap")

    config = ReceiverConfig(main=source_id(s.main), backup=source_id(s.backup) if s.backup else None,
                            loss_timeout_frames=loss_timeout, max_predict_frames=max_predict,
                            auto_return=s.auto_return, output_advance_ns=int(s.advance_ms * 1_000_000))
    receiver = Receiver(config)
    sock = frame_receiver_socket(s.port, None if s.group == "none" else s.group)
    service = ReceiverService(receiver, sock, source_id(s.name))
    readback = ReadbackMonitor(s.sample_rate) if s.readback_device else None
    device_name = s.device or "default"

    if s.output == "soundcard":
        from .devices import SoundcardOutput

        renderer = LtcRenderer(service.frame_at, s.sample_rate, s.level)
        device = SoundcardOutput(renderer, s.device or None, s.sample_rate, s.blocksize, readback,
                                 s.readback_device or None)
    else:
        from .devices import MtcOutput, SoundcardOutput, find_midi_port

        port = s.midi_port or find_midi_port()
        if port is None:
            raise SystemExit("no MIF4 MIDI port found; set midi_port")
        log.info("sending MTC to %s", port)
        device_name = port
        device = MtcOutput(service.frame_at, port)
        if readback is not None:
            # Readback for the MIF4 path: a silent renderer keeps the emitted-frame history.
            renderer = LtcRenderer(service.frame_at, s.sample_rate, 0.0)
            SoundcardOutput(renderer, s.device or None, s.sample_rate, s.blocksize, readback,
                            s.readback_device or None).start()

    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    signal.signal(signal.SIGTERM, lambda *_: stop.set())

    def return_to_main(*_):
        with service.lock:
            receiver.request_return_to_main()
        log.warning("return to main requested")

    signal.signal(signal.SIGUSR1, return_to_main)
    status_path = _status_path(args)
    started = time.time()
    service.start()
    device.start()
    log.info("receiver %s following main=%s backup=%s, output %s on %s", s.name, s.main, s.backup or "-",
             s.output, device_name)
    try:
        while not stop.wait(1.0):
            now = time.monotonic_ns()
            frame = service.frame_at(now)
            idle = None if frame else service.describe_idle(now)
            gaps = service.take_max_gaps()
            line = f"output {frame.timecode}" if frame else f"output stopped ({idle})"
            if gaps:
                line += ", longest packet gap " + ", ".join(f"{n} {g:.0f} ms" for n, g in gaps.items())
            rb = None
            if readback is not None:
                st = readback.status(now, outputting=frame is not None)
                rb = {"state": st.state.value, "timecode": st.last_timecode,
                      "offset_ms": round(st.offset_ns / 1e6, 2) if st.offset_ns is not None else None}
                offset = f" {rb['offset_ms']:+.2f} ms" if rb["offset_ms"] is not None else ""
                line += f", readback {st.state.value}{offset}"
            log.info(line)
            if status_path is not None:
                with service.lock:
                    snap = status.snapshot(receiver, now, dict(service.host_addresses), gaps,
                                           f"{s.output}: {device_name}", rb, idle, started)
                snap["name"] = s.name
                snap["wifi_mode"] = s.wifi
                snap["other_hosts"] = dict(service.other_hosts)
                try:
                    status.write(status_path, snap)
                except OSError as exc:
                    log.debug("could not write status: %s", exc)
    finally:
        device.stop()
        service.stop()


if __name__ == "__main__":
    main()
