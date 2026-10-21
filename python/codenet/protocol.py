"""Code Net wire protocol (version 1). See docs/protocol.md for the full description."""

from __future__ import annotations

import struct
from dataclasses import dataclass
from enum import IntEnum, IntFlag

from .timecode import Timecode

MAGIC = b"CNET"
VERSION = 1

FRAME_PORT = 5260  # receivers listen here for frame packets
SYNC_PORT = 5261  # hosts answer clock sync requests here
MULTICAST_GROUP = "239.255.60.60"

SOURCE_ID_SIZE = 16


class MessageType(IntEnum):
    FRAME = 1
    SYNC_REQUEST = 2
    SYNC_RESPONSE = 3
    STATUS = 4  # receiver -> host, JSON summary once a second
    ASSIGN = 5  # host -> receiver, sets its main and backup hosts
    CALIBRATE = 6  # host -> receiver, measures or sets its output delay compensation


class FrameFlags(IntFlag):
    NONE = 0
    PLAYING = 1  # LTC is being decoded on the host; the frame is live
    SOURCE_LOST = 2  # the host app is running but its plugin has stopped delivering


def source_id(name: str) -> bytes:
    """A 16-byte source id from a short host name (UTF-8, zero padded)."""
    raw = name.encode("utf-8")
    if not raw or len(raw) > SOURCE_ID_SIZE:
        raise ValueError(f"source name must be 1 to {SOURCE_ID_SIZE} bytes: {name!r}")
    return raw.ljust(SOURCE_ID_SIZE, b"\0")


def source_name(sid: bytes) -> str:
    return sid.rstrip(b"\0").decode("utf-8", "replace")


_HEADER = struct.Struct(">4sBB")
_FRAME = struct.Struct(">4sBBBB16sHIIIq")
_SYNC_REQUEST = struct.Struct(">4sBB16sIq")
_SYNC_RESPONSE = struct.Struct(">4sBB16sIqqq")
_STATUS_HEADER = struct.Struct(">4sBB16s")
_ASSIGN = struct.Struct(">4sBB16s16s16sI")
_CALIBRATE = struct.Struct(">4sBB16sBiI")
MAX_STATUS_BYTES = 1200


@dataclass(frozen=True)
class FramePacket:
    """One decoded LTC frame from a host.

    `frame_start_ns` is the host's clock reading (CLOCK_MONOTONIC-style, nanoseconds)
    at which the first sample of this frame leaves the host's audio output. When the
    flags do not include PLAYING the packet is a heartbeat and `timecode` is the last
    position seen.
    """

    source: bytes
    stream: int
    sequence: int
    timecode: Timecode
    user_bits: int
    frame_start_ns: int
    flags: FrameFlags

    @property
    def playing(self) -> bool:
        return bool(self.flags & FrameFlags.PLAYING)

    def encode(self) -> bytes:
        return _FRAME.pack(MAGIC, VERSION, MessageType.FRAME, int(self.flags), self.timecode.rate,
                           self.source, self.stream, self.sequence & 0xFFFFFFFF, self.timecode.frames,
                           self.user_bits & 0xFFFFFFFF, self.frame_start_ns)


@dataclass(frozen=True)
class SyncRequest:
    receiver: bytes
    sequence: int
    t1: int  # receiver clock when sent

    def encode(self) -> bytes:
        return _SYNC_REQUEST.pack(MAGIC, VERSION, MessageType.SYNC_REQUEST, self.receiver,
                                  self.sequence & 0xFFFFFFFF, self.t1)


@dataclass(frozen=True)
class SyncResponse:
    host: bytes
    sequence: int
    t1: int  # receiver clock when the request was sent (echoed)
    t2: int  # host clock when the request arrived
    t3: int  # host clock when this response was sent

    def encode(self) -> bytes:
        return _SYNC_RESPONSE.pack(MAGIC, VERSION, MessageType.SYNC_RESPONSE, self.host,
                                   self.sequence & 0xFFFFFFFF, self.t1, self.t2, self.t3)


@dataclass(frozen=True)
class StatusReport:
    """A receiver's state for the host app's node list. `payload` is a JSON object."""

    receiver: bytes
    payload: bytes

    def encode(self) -> bytes:
        if len(self.payload) > MAX_STATUS_BYTES:
            raise ValueError("status payload too large")
        return _STATUS_HEADER.pack(MAGIC, VERSION, MessageType.STATUS, self.receiver) + self.payload


@dataclass(frozen=True)
class Assign:
    """Sets a receiver's main and backup hosts (from the host app's dashboard)."""

    receiver: bytes
    main: bytes
    backup: bytes | None
    sequence: int

    def encode(self) -> bytes:
        return _ASSIGN.pack(MAGIC, VERSION, MessageType.ASSIGN, self.receiver, self.main,
                            self.backup or bytes(SOURCE_ID_SIZE), self.sequence & 0xFFFFFFFF)


class CalibrateMode(IntEnum):
    MEASURE = 0  # time the output from its readback loop and set the advance to match
    SET = 1  # set the advance to `value_us`


@dataclass(frozen=True)
class Calibrate:
    """Measures or sets a receiver's output advance, so every device puts out the same
    frame at the same moment whatever its own latency."""

    receiver: bytes
    mode: CalibrateMode
    value_us: int  # the advance for SET; ignored for MEASURE
    sequence: int

    def encode(self) -> bytes:
        return _CALIBRATE.pack(MAGIC, VERSION, MessageType.CALIBRATE, self.receiver, self.mode, self.value_us,
                               self.sequence & 0xFFFFFFFF)


Message = FramePacket | SyncRequest | SyncResponse | StatusReport | Assign | Calibrate


class ProtocolError(ValueError):
    pass


def decode(data: bytes) -> Message:
    if len(data) < _HEADER.size:
        raise ProtocolError("packet too short")
    magic, version, kind = _HEADER.unpack_from(data)
    if magic != MAGIC:
        raise ProtocolError("not a Code Net packet")
    if version != VERSION:
        raise ProtocolError(f"unsupported protocol version {version}")
    try:
        if kind == MessageType.FRAME:
            _, _, _, flags, rate, src, stream, seq, frames, ub, start = _FRAME.unpack(data)
            return FramePacket(src, stream, seq, Timecode(frames, rate), ub, start, FrameFlags(flags))
        if kind == MessageType.SYNC_REQUEST:
            _, _, _, rid, seq, t1 = _SYNC_REQUEST.unpack(data)
            return SyncRequest(rid, seq, t1)
        if kind == MessageType.SYNC_RESPONSE:
            _, _, _, hid, seq, t1, t2, t3 = _SYNC_RESPONSE.unpack(data)
            return SyncResponse(hid, seq, t1, t2, t3)
        if kind == MessageType.STATUS:
            _, _, _, rid = _STATUS_HEADER.unpack_from(data)
            return StatusReport(rid, bytes(data[_STATUS_HEADER.size:]))
        if kind == MessageType.ASSIGN:
            _, _, _, rid, main, backup, seq = _ASSIGN.unpack(data)
            if not main.strip(b"\0"):
                raise ProtocolError("assignment without a main host")
            return Assign(rid, main, backup if backup.strip(b"\0") else None, seq)
        if kind == MessageType.CALIBRATE:
            _, _, _, rid, mode, value, seq = _CALIBRATE.unpack(data)
            return Calibrate(rid, CalibrateMode(mode), value, seq)
    except struct.error as exc:
        raise ProtocolError(f"malformed packet: {exc}") from exc
    except ValueError as exc:
        raise ProtocolError(f"invalid packet contents: {exc}") from exc
    raise ProtocolError(f"unknown message type {kind}")
