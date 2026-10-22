"""The receiver's network loop: frame packets in, clock sync out, state ticked every ms."""

from __future__ import annotations

import logging
import select
import socket
import threading
import time

from ..protocol import (SYNC_PORT, Assign, Calibrate, FramePacket, ProtocolError, StatusReport, SyncRequest, SyncResponse, decode,
                        source_name)
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
        self._last_frame_rx: dict[bytes, int] = {}
        self._max_gap_ns: dict[bytes, int] = {}
        self._ignored: set[bytes] = set()
        self.other_hosts: dict[str, str] = {}  # hosts heard in the last 10 s that are not main or backup
        self._other_heard_ns: dict[str, int] = {}
        self._seq = 0
        self._last_assign: tuple[str, int] | None = None
        # Called (outside the lock) with (main, backup) names after a host app reassigns this receiver.
        self.on_assign = None
        # Called (outside the lock) with (Calibrate, host, now_ns) for requests addressed to us.
        self.on_calibrate = None
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def frame_at(self, local_ns: int):
        with self.lock:
            return self.receiver.frame_at(local_ns)

    def take_max_gaps(self) -> dict[str, float]:
        """Longest gap between frame packets per host since the last call, in ms."""
        with self.lock:
            gaps = {source_name(s): g / 1e6 for s, g in self._max_gap_ns.items()}
            self._max_gap_ns.clear()
            return gaps

    def describe_idle(self, now_ns: int) -> str:
        """Why nothing is being output, for the status line."""
        with self.lock:
            if not self.receiver.sources:
                return "no main host assigned yet; pick one for this receiver in the Code Net app on a Mac"
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
                if msg.source not in self.receiver.sources:
                    self.other_hosts[source_name(msg.source)] = host
                    self._other_heard_ns[source_name(msg.source)] = t4
                if msg.source in self.receiver.sources and self.host_addresses.get(msg.source) != host:
                    log.info("host %s is at %s", source_name(msg.source), host)
                    self.host_addresses[msg.source] = host
                if msg.source in self.receiver.sources:
                    last = self._last_frame_rx.get(msg.source)
                    if last is not None:
                        self._max_gap_ns[msg.source] = max(self._max_gap_ns.get(msg.source, 0), t4 - last)
                    self._last_frame_rx[msg.source] = t4
                self.receiver.on_frame(msg, t4)
            elif isinstance(msg, SyncResponse):
                self.receiver.on_sync(msg.host, msg.t1, msg.t2, msg.t3, t4)
        if isinstance(msg, Assign) and msg.receiver == self.receiver_id:
            self._assign(msg, host, t4)
        if isinstance(msg, Calibrate) and msg.receiver == self.receiver_id and self.on_calibrate is not None:
            self.on_calibrate(msg, host, t4)

    def _assign(self, msg: Assign, host: str, now_ns: int) -> None:
        if self._last_assign == (host, msg.sequence):
            return  # the host app repeats each assignment until it sees it in our status
        self._last_assign = (host, msg.sequence)
        if msg.backup == msg.main:
            log.warning("ignored assignment from %s: main and backup are the same host", host)
            return
        main, backup = source_name(msg.main), source_name(msg.backup) if msg.backup else ""
        with self.lock:
            current = (self.receiver.config.main, self.receiver.config.backup)
            if current == (msg.main, msg.backup):
                return
            self.receiver.set_sources(msg.main, msg.backup, now_ns)
            for source in list(self.host_addresses):
                if source not in self.receiver.sources:
                    self.other_hosts[source_name(source)] = self.host_addresses.pop(source)
                    self._other_heard_ns[source_name(source)] = now_ns
            for source in self.receiver.sources:
                name = source_name(source)
                if source not in self.host_addresses and name in self.other_hosts:
                    self.host_addresses[source] = self.other_hosts.pop(name)
                    self._other_heard_ns.pop(name, None)
        log.warning("host at %s assigned main=%s backup=%s", host, main, backup or "-")
        if self.on_assign is not None:
            self.on_assign(main, backup)

    def send_status(self, payload: bytes) -> None:
        """Report this receiver's status to every host it has heard from."""
        packet = StatusReport(self.receiver_id, payload).encode()
        with self.lock:
            now = self.clock()
            for name, heard in list(self._other_heard_ns.items()):
                if now - heard > 10_000_000_000:
                    del self._other_heard_ns[name]
                    self.other_hosts.pop(name, None)
            hosts = set(self.host_addresses.values()) | set(self.other_hosts.values())
        for host in hosts:
            try:
                self.sock.sendto(packet, (host, SYNC_PORT))
            except OSError as exc:
                log.debug("status to %s failed: %s", host, exc)

    def _send_sync(self) -> None:
        for source, host in list(self.host_addresses.items()):
            self._seq += 1
            req = SyncRequest(self.receiver_id, self._seq, self.clock())
            try:
                self.sock.sendto(req.encode(), (host, SYNC_PORT))
            except OSError as exc:
                log.debug("sync to %s failed: %s", host, exc)
