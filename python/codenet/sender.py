"""A test host: plays an LTC WAV file (or generated LTC) onto the network.

It does what the Ableton plugin and Mac app will do, without Ableton: decode LTC
audio in real time, send a frame packet for every decoded frame, send heartbeats
while there is no LTC, and answer clock sync requests. Use it for bench tests of
receivers from any computer.
"""

from __future__ import annotations

import argparse
import logging
import socket
import threading
import time
import wave

import numpy as np

from .ltc import LtcDecoder, encode_run
from .net import sender_socket
from .protocol import (FRAME_PORT, MULTICAST_GROUP, SYNC_PORT, FrameFlags, FramePacket, ProtocolError,
                       SyncRequest, SyncResponse, decode, source_id)
from .timecode import Timecode

log = logging.getLogger("codenet.sender")
NS_PER_S = 1_000_000_000


def read_wav(path: str) -> tuple[np.ndarray, int]:
    """First channel of a PCM WAV file as float32, and its sample rate."""
    with wave.open(path, "rb") as w:
        width, channels, rate = w.getsampwidth(), w.getnchannels(), w.getframerate()
        raw = w.readframes(w.getnframes())
    if width == 2:
        data = np.frombuffer(raw, "<i2").astype(np.float32) / 32768
    elif width == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3)
        ints = (b[:, 0].astype(np.int32) | b[:, 1].astype(np.int32) << 8 | b[:, 2].astype(np.int32) << 16)
        data = np.where(ints >= 1 << 23, ints - (1 << 24), ints).astype(np.float32) / (1 << 23)
    elif width == 4:
        data = np.frombuffer(raw, "<i4").astype(np.float32) / (1 << 31)
    else:
        raise ValueError(f"unsupported sample width {width}")
    return data[::channels], rate


class SyncResponder:
    """Answers receivers' clock sync requests with this host's monotonic clock."""

    def __init__(self, host: bytes, port: int = SYNC_PORT, clock=time.monotonic_ns):
        self.host = host
        self.clock = clock
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("", port))
        self.sock.settimeout(0.2)
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="codenet-sync", daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1)
        self.sock.close()

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                data, addr = self.sock.recvfrom(512)
            except socket.timeout:
                continue
            except OSError:
                return
            t2 = self.clock()
            try:
                req = decode(data)
            except ProtocolError:
                continue
            if isinstance(req, SyncRequest):
                resp = SyncResponse(self.host, req.sequence, req.t1, t2, self.clock())
                self.sock.sendto(resp.encode(), addr)


class Sender:
    def __init__(self, name: str, audio: np.ndarray, sample_rate: int, destination: tuple[str, int],
                 latency_ns: int = 0, block: int = 64, loop: bool = False, clock=time.monotonic_ns):
        self.source = source_id(name)
        self.audio = audio
        self.sample_rate = sample_rate
        self.destination = destination
        self.latency_ns = latency_ns
        self.block = block
        self.loop = loop
        self.clock = clock
        self.sock = sender_socket()
        self.sequence = 0
        self._send_error: str | None = None
        self._stop = threading.Event()

    def stop(self) -> None:
        self._stop.set()

    def _send(self, tc: Timecode, user_bits: int, start_ns: int, flags: FrameFlags) -> None:
        self.sequence += 1
        pkt = FramePacket(self.source, 0, self.sequence, tc, user_bits, start_ns, flags)
        try:
            self.sock.sendto(pkt.encode(), self.destination)
        except OSError as exc:
            # Keep running: the network may come back. Report each new error once.
            if str(exc) != self._send_error:
                log.error("cannot send to %s:%d: %s%s", *self.destination, exc,
                          " (on macOS, allow Terminal under Privacy & Security > Local Network)"
                          if exc.errno == 65 else "")
                self._send_error = str(exc)
            return
        if self._send_error is not None:
            log.info("sending to %s:%d again", *self.destination)
            self._send_error = None

    def run(self) -> None:
        decoder = LtcDecoder(self.sample_rate)
        t0 = self.clock()
        position = 0  # samples played since t0
        last: tuple[Timecode, int] | None = None
        last_frame_ns = t0
        next_heartbeat = t0
        while not self._stop.is_set():
            index = position % len(self.audio) if self.loop else position
            if index >= len(self.audio):
                chunk = np.zeros(self.block, np.float32)
            else:
                chunk = self.audio[index:index + self.block]
            # Pace to real time: this block "plays" from t0 + position / rate.
            due = t0 + (position + len(chunk)) * NS_PER_S // self.sample_rate
            while (wait := due - self.clock()) > 0:
                time.sleep(min(wait / NS_PER_S, 0.001))
            for frame in decoder.feed(chunk):
                start_ns = t0 + frame.start_sample * NS_PER_S // self.sample_rate + self.latency_ns
                self._send(frame.timecode, frame.user_bits, start_ns, FrameFlags.PLAYING)
                last = (frame.timecode, frame.user_bits)
                last_frame_ns = self.clock()
            now = self.clock()
            if last is not None and now - last_frame_ns > 2 * NS_PER_S // last[0].rate and now >= next_heartbeat:
                # No LTC: the transport is stopped. Keep the heartbeat going at frame rate.
                self._send(last[0], last[1], now, FrameFlags.NONE)
                next_heartbeat = now + NS_PER_S // last[0].rate
            position += len(chunk)


def main(argv=None) -> None:
    p = argparse.ArgumentParser(description="Code Net test sender: LTC audio onto the network.")
    p.add_argument("--name", required=True, help="this host's name, as the receivers know it")
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--wav", help="LTC audio file to play")
    src.add_argument("--generate", metavar="HH:MM:SS:FF", help="generate LTC from this position")
    p.add_argument("--rate", type=int, default=25, choices=(25, 30), help="frame rate for --generate")
    p.add_argument("--seconds", type=float, default=600, help="length of generated LTC")
    p.add_argument("--loop", action="store_true")
    p.add_argument("--latency-ms", type=float, default=0.0, help="host output latency to add to frame times")
    p.add_argument("--to", default=MULTICAST_GROUP, help="multicast group or a receiver's address")
    p.add_argument("--port", type=int, default=FRAME_PORT)
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO)

    if args.wav:
        audio, sample_rate = read_wav(args.wav)
    else:
        sample_rate = 48000
        start = Timecode.parse(args.generate, args.rate)
        audio = 0.5 * encode_run(start, int(args.seconds * args.rate), sample_rate)
    sender = Sender(args.name, audio, sample_rate, (args.to, args.port), int(args.latency_ms * 1e6), loop=args.loop)
    sync = SyncResponder(sender.source)
    sync.start()
    log.info("sending %s to %s:%d", args.wav or f"generated LTC from {args.generate}", args.to, args.port)
    try:
        sender.run()
    except KeyboardInterrupt:
        pass
    finally:
        sync.stop()


if __name__ == "__main__":
    main()
