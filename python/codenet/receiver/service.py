"""The receiver's network loop: frame packets in, clock sync out, state ticked every ms."""

from __future__ import annotations

import logging
import select
import socket
import threading
import time

from ..protocol import SYNC_PORT, FramePacket, ProtocolError, SyncRequest, SyncResponse, decode, source_name
from .core import Receiver

log = logging.getLogger(__name__)


class ReceiverService:
    def __init__(self, receiver: Receiver, sock: socket.socket, receiver_id: bytes,
                 sync_interval_s: float = 0.25, clock=time.monotonic_ns):
        self.receiver = receiver
        self.sock = sock
        self.receiver_id = receiver_id
        self.sync_interval_s = sync_interval_s
        self.clock = clock
        self.lock = threading.Lock()
        self.host_addresses: dict[bytes, str] = {}
        self.packets_received = 0
        self._ignored: set[bytes] = set()
        self._seq = 0
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def frame_at(self, local_ns: int):
        with self.lock:
            return self.receiver.frame_at(local_ns)

    def describe_idle(self, now_ns: int) -> str:
        """Why nothing is being output, for the status line."""
        with self.lock:
            if self.packets_received == 0:
                return "no Code Net packets received yet; check the sender, its --to address and the network"
            reasons = []
            for state in self.receiver.sources.values():
                name = source_name(state.source)
                if state.last_packet is None:
                    reasons.append(f"nothing from {name}")
                elif not state.clock.ready:
                    reasons.append(f"{name} heard but its clock is not synced; check the Mac firewall")
                elif not state.last_packet.playing:
                    reasons.append(f"{name} is stopped")
                elif not self.receiver.is_valid(state, now_ns):
                    reasons.append(f"{name} has gone quiet")
            return "; ".join(reasons) or "waiting"

    def start(self) -> None:
        self._thread = threading.Thread(target=self.run, name="codenet-net", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=1)

    def run(self) -> None:
        next_sync = 0.0
        last_active = None
        while not self._stop.is_set():
            ready, _, _ = select.select([self.sock], [], [], 0.001)
            if ready:
                data, addr = self.sock.recvfrom(512)
                self._handle(data, addr[0])
            now = time.monotonic()
            if now >= next_sync:
                self._send_sync()
                next_sync = now + self.sync_interval_s
            with self.lock:
                self.receiver.update(self.clock())
                active = self.receiver.active
                if active is not last_active:
                    switch = self.receiver.switches[-1] if self.receiver.switches else None
                    log.warning("now following %s (%s)", active.role.value if active else "nothing",
                                switch.reason if switch else "")
                    last_active = active

    def _handle(self, data: bytes, host: str) -> None:
        t4 = self.clock()
        try:
            msg = decode(data)
        except ProtocolError as exc:
            log.debug("dropped packet from %s: %s", host, exc)
            return
        with self.lock:
            self.packets_received += 1
            if isinstance(msg, FramePacket):
                if msg.source not in self.receiver.sources and msg.source not in self._ignored:
                    log.warning("ignoring frames from host %s at %s: not this receiver's main or backup",
                                source_name(msg.source), host)
                    self._ignored.add(msg.source)
                if msg.source in self.receiver.sources and self.host_addresses.get(msg.source) != host:
                    log.info("host %s is at %s", source_name(msg.source), host)
                    self.host_addresses[msg.source] = host
                self.receiver.on_frame(msg, t4)
            elif isinstance(msg, SyncResponse):
                self.receiver.on_sync(msg.host, msg.t1, msg.t2, msg.t3, t4)

    def _send_sync(self) -> None:
        for source, host in list(self.host_addresses.items()):
            self._seq += 1
            req = SyncRequest(self.receiver_id, self._seq, self.clock())
            try:
                self.sock.sendto(req.encode(), (host, SYNC_PORT))
            except OSError as exc:
                log.debug("sync to %s failed: %s", host, exc)
