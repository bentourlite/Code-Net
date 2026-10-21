"""Command-line entry point for the Raspberry Pi receiver."""

from __future__ import annotations

import argparse
import logging
import signal
import threading
import time

from ..net import frame_receiver_socket
from ..protocol import FRAME_PORT, MULTICAST_GROUP, source_id
from .core import NS_PER_S, Receiver, ReceiverConfig
from .readback import ReadbackMonitor
from .render import LtcRenderer
from .service import ReceiverService

log = logging.getLogger("codenet.receiver")


def parse_args(argv=None):
    p = argparse.ArgumentParser(description="Code Net receiver: one LTC output from a main and a backup host.")
    p.add_argument("--name", default="pi", help="this receiver's name (up to 16 bytes)")
    p.add_argument("--main", required=True, help="name of the main host")
    p.add_argument("--backup", help="name of the backup host")
    p.add_argument("--output", choices=("soundcard", "mif4"), default="soundcard")
    p.add_argument("--device", help="audio device name or index for LTC output")
    p.add_argument("--midi-port", help="MIDI port for MTC (default: first port whose name contains MIF)")
    p.add_argument("--sample-rate", type=int, default=48000)
    p.add_argument("--blocksize", type=int, default=64)
    p.add_argument("--level", type=float, default=0.5, help="LTC output level, 0 to 1")
    p.add_argument("--advance-ms", type=float, default=0.0, help="start each frame this much earlier (device delay)")
    p.add_argument("--readback-device", help="audio input that hears the LTC output, for confirmation")
    p.add_argument("--no-auto-return", action="store_true", help="only return to main when asked")
    p.add_argument("--group", default=MULTICAST_GROUP, help="multicast group, or 'none' for unicast")
    p.add_argument("--port", type=int, default=FRAME_PORT)
    p.add_argument("-v", "--verbose", action="store_true")
    return p.parse_args(argv)


def main(argv=None) -> None:
    args = parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    config = ReceiverConfig(main=source_id(args.main), backup=source_id(args.backup) if args.backup else None,
                            auto_return=not args.no_auto_return,
                            output_advance_ns=int(args.advance_ms * 1_000_000))
    receiver = Receiver(config)
    sock = frame_receiver_socket(args.port, None if args.group == "none" else args.group)
    service = ReceiverService(receiver, sock, source_id(args.name))
    readback = ReadbackMonitor(args.sample_rate) if args.readback_device else None

    if args.output == "soundcard":
        from .devices import SoundcardOutput

        renderer = LtcRenderer(service.frame_at, args.sample_rate, args.level)
        device = SoundcardOutput(renderer, args.device, args.sample_rate, args.blocksize, readback, args.readback_device)
    else:
        from .devices import MtcOutput, SoundcardOutput, find_midi_port

        port = args.midi_port or find_midi_port()
        if port is None:
            raise SystemExit("no MIF4 MIDI port found; pass --midi-port")
        log.info("sending MTC to %s", port)
        device = MtcOutput(service.frame_at, port)
        renderer = None
        if readback is not None:
            # Readback for the MIF4 path: a silent renderer keeps the emitted-frame history.
            renderer = LtcRenderer(service.frame_at, args.sample_rate, 0.0)
            loop = SoundcardOutput(renderer, args.device, args.sample_rate, args.blocksize, readback, args.readback_device)
            loop.start()

    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    service.start()
    device.start()
    log.info("receiver %s following main=%s backup=%s", args.name, args.main, args.backup or "-")
    try:
        while not stop.wait(1.0):
            now = time.monotonic_ns()
            frame = service.frame_at(now)
            line = f"output {frame.timecode}" if frame else f"output stopped ({service.describe_idle(now)})"
            if readback is not None:
                st = readback.status(now, outputting=frame is not None)
                offset = f" {st.offset_ns / 1e6:+.2f} ms" if st.offset_ns is not None else ""
                line += f", readback {st.state.value}{offset}"
            log.info(line)
    finally:
        device.stop()
        service.stop()


if __name__ == "__main__":
    main()
