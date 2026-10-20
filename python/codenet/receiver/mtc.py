"""MIDI timecode (MTC) for the Rosendahl MIF4, which turns it into LTC on its XLR output."""

from __future__ import annotations

from ..timecode import Timecode

_RATE_CODE = {25: 1, 30: 3}


def quarter_frame(tc: Timecode, piece: int) -> bytes:
    """MTC quarter-frame message `piece` (0-7) describing `tc`."""
    hours, minutes, seconds, frames = tc.hmsf
    value = (frames, seconds, minutes, hours)[piece // 2]
    nibble = value & 0x0F if piece % 2 == 0 else value >> 4
    if piece == 7:
        nibble = (_RATE_CODE[tc.rate] << 1) | (hours >> 4 & 1)
    return bytes((0xF1, (piece << 4) | nibble))


def full_frame(tc: Timecode) -> bytes:
    """MTC full-frame SysEx, sent when the position jumps or output starts."""
    hours, minutes, seconds, frames = tc.hmsf
    return bytes((0xF0, 0x7F, 0x7F, 0x01, 0x01, (_RATE_CODE[tc.rate] << 5) | hours, minutes, seconds, frames, 0xF7))


def messages_for_frame(tc: Timecode, previous: Timecode | None) -> list[tuple[float, bytes]]:
    """MTC messages for one output frame, as (fraction of the frame, message) pairs.

    Eight quarter frames span two frames and describe the frame at which piece 0 was
    sent, so even frames send pieces 0-3 of their own time and odd frames send
    pieces 4-7 of the previous frame's time. A jump sends a full frame first.
    """
    out: list[tuple[float, bytes]] = []
    continuous = previous is not None and tc.follows(previous)
    if not continuous:
        out.append((0.0, full_frame(tc)))
    if tc.frames % 2 == 0:
        out.extend((q / 4, quarter_frame(tc, q)) for q in range(4))
    elif continuous:
        out.extend((q / 4, quarter_frame(previous, q + 4)) for q in range(4))
    return out
