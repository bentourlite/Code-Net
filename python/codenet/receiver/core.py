"""Source selection, failover and frame scheduling for one receiver.

This module is pure logic: every method takes the current local time, so it can be
driven by the real network loop or by tests.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum

from ..clock import ClockOffsetEstimator
from ..protocol import FrameFlags, FramePacket
from ..timecode import Timecode

NS_PER_S = 1_000_000_000


def frame_offset_ns(count: int, rate: int) -> int:
    """Start of the frame `count` frames after a reference, without rounding drift."""
    return count * NS_PER_S // rate


class Role(Enum):
    MAIN = "main"
    BACKUP = "backup"


@dataclass
class SourceState:
    source: bytes
    role: Role
    clock: ClockOffsetEstimator = field(default_factory=ClockOffsetEstimator)
    last_packet: FramePacket | None = None
    last_rx_ns: int | None = None
    valid_since_ns: int | None = None
    # Latest playing frame, converted to the local clock.
    anchor: Timecode | None = None
    anchor_start_ns: int | None = None


@dataclass(frozen=True)
class ScheduledFrame:
    timecode: Timecode
    user_bits: int
    start_ns: int  # local clock time at which the frame's first sample must be output


@dataclass(frozen=True)
class Switch:
    at_ns: int
    from_role: Role | None
    to_role: Role | None
    reason: str


@dataclass
class ReceiverConfig:
    main: bytes | None  # None until a host app assigns one
    backup: bytes | None = None
    # A source is lost after this many frame periods without a packet. Keep this no
    # larger than max_predict_frames so failover lands before the prediction runs out.
    loss_timeout_frames: int = 2
    # How many frames past the newest received frame may be generated. Packets for a
    # frame only arrive once the host has decoded it, so at least one frame of
    # look-ahead is needed; beyond this the output stops (no freewheel, no hold).
    max_predict_frames: int = 2
    # Main must be valid this long before the receiver returns to it from backup.
    return_to_main_after_ns: int = 10 * NS_PER_S
    auto_return: bool = True
    # Trim applied to every frame, for example a measured device delay (positive = earlier).
    output_advance_ns: int = 0
    nominal_rate: int = 25  # used for timeouts before any packet has arrived


class Receiver:
    def __init__(self, config: ReceiverConfig):
        self.config = config
        self.sources: dict[bytes, SourceState] = {}
        if config.main is not None:
            self.sources[config.main] = SourceState(config.main, Role.MAIN)
        if config.backup is not None:
            self.sources[config.backup] = SourceState(config.backup, Role.BACKUP)
        self.active: SourceState | None = None
        self.switches: list[Switch] = []
        self._return_requested = False

    # ---- inputs -------------------------------------------------------------

    def on_frame(self, packet: FramePacket, now_ns: int) -> None:
        state = self.sources.get(packet.source)
        if state is None:
            return  # not one of our sources
        state.last_packet = packet
        state.last_rx_ns = now_ns
        if packet.playing and state.clock.ready:
            state.anchor = packet.timecode
            state.anchor_start_ns = state.clock.host_to_local(packet.frame_start_ns)
        elif not packet.playing:
            state.anchor = None
            state.anchor_start_ns = None
        self.update(now_ns)

    def on_sync(self, source: bytes, t1: int, t2: int, t3: int, t4: int) -> None:
        state = self.sources.get(source)
        if state is not None:
            state.clock.add(t1, t2, t3, t4)

    def set_sources(self, main: bytes | None, backup: bytes | None, now_ns: int) -> None:
        """Change the main and backup hosts. A host that stays keeps its clock sync and state."""
        old = self.sources
        self.config.main, self.config.backup = main, backup
        self.sources = {}
        for source, role in ((main, Role.MAIN), (backup, Role.BACKUP)):
            if source is None:
                continue
            state = old.get(source) or SourceState(source, role)
            state.role = role
            self.sources[source] = state
        if self.active is not None and self.active.source not in self.sources:
            self.switches.append(Switch(now_ns, self.active.role, None, "host unassigned"))
            self.active = None
        self._return_requested = True  # follow the newly chosen main straight away if it is valid
        self.update(now_ns)

    def request_return_to_main(self) -> None:
        self._return_requested = True

    # ---- state --------------------------------------------------------------

    def is_valid(self, state: SourceState, now_ns: int) -> bool:
        """A source is valid while its packets keep arriving and its clock is synced.

        A stopped transport is still valid: it is a deliberate stop, not a loss.
        """
        pkt = state.last_packet
        if pkt is None or state.last_rx_ns is None or not state.clock.ready:
            return False
        if pkt.flags & FrameFlags.SOURCE_LOST:
            return False
        timeout = frame_offset_ns(self.config.loss_timeout_frames, pkt.timecode.rate)
        return now_ns - state.last_rx_ns < timeout

    def update(self, now_ns: int) -> None:
        main = self.sources.get(self.config.main) if self.config.main is not None else None
        backup = self.sources.get(self.config.backup) if self.config.backup is not None else None
        for state in self.sources.values():
            if self.is_valid(state, now_ns):
                if state.valid_since_ns is None:
                    state.valid_since_ns = now_ns
            else:
                state.valid_since_ns = None

        main_ok = main is not None and main.valid_since_ns is not None
        backup_ok = backup is not None and backup.valid_since_ns is not None
        current = self.active

        if current is main and main_ok:
            target, reason = main, None
        elif current is backup and backup_ok:
            target, reason = backup, None
            stable = main_ok and now_ns - main.valid_since_ns >= self.config.return_to_main_after_ns
            if main_ok and (self._return_requested or (self.config.auto_return and stable)):
                target, reason = main, "returned to main"
        elif main_ok:
            target, reason = main, "main valid"
        elif backup_ok:
            target, reason = backup, "main lost" if current is main else "backup valid"
        else:
            target, reason = None, "no valid source"

        if target is not current:
            self.switches.append(Switch(now_ns, current.role if current else None,
                                        target.role if target else None, reason or ""))
            self.active = target
        if self.active is main:
            self._return_requested = False

    # ---- output -------------------------------------------------------------

    def frame_at(self, local_ns: int) -> ScheduledFrame | None:
        """The frame that should be on the output at `local_ns`, or None for silence."""
        state = self.active
        if state is None or state.anchor is None or state.anchor_start_ns is None:
            return None
        rate = state.anchor.rate
        start = state.anchor_start_ns - self.config.output_advance_ns
        elapsed = local_ns - start
        if elapsed < 0:
            return None
        k = elapsed * rate // NS_PER_S
        if k > self.config.max_predict_frames:
            return None
        user_bits = state.last_packet.user_bits if state.last_packet else 0
        return ScheduledFrame(state.anchor.next(k), user_bits, start + frame_offset_ns(k, rate))
