"""Live receiver status, written once a second for the Pi menu's monitor."""

from __future__ import annotations

import json
import os
import time
from pathlib import Path

from ..protocol import source_name
from .core import Receiver


def snapshot(receiver: Receiver, now_ns: int, host_addresses: dict[bytes, str], gaps_ms: dict[str, float],
             output_device: str, readback: dict | None, idle_reason: str | None, started: float) -> dict:
    """A JSON-ready picture of the receiver. Call with the service lock held."""
    frame = receiver.frame_at(now_ns)
    sources = []
    for state in receiver.sources.values():
        name = source_name(state.source)
        best = state.clock.best
        sources.append({
            "name": name,
            "role": state.role.value,
            "address": host_addresses.get(state.source),
            "valid": receiver.is_valid(state, now_ns),
            "playing": bool(state.last_packet and state.last_packet.playing),
            "last_timecode": str(state.last_packet.timecode) if state.last_packet else None,
            "last_packet_ms_ago": round((now_ns - state.last_rx_ns) / 1e6) if state.last_rx_ns else None,
            "clock_offset_ms": round(best.offset_ns / 1e6, 3) if best else None,
            "round_trip_ms": round(best.rtt_ns / 1e6, 3) if best else None,
            "longest_gap_ms": gaps_ms.get(name),
        })
    switches = [{"seconds_ago": round((now_ns - s.at_ns) / 1e9), "from": s.from_role.value if s.from_role else None,
                 "to": s.to_role.value if s.to_role else None, "reason": s.reason}
                for s in receiver.switches[-5:]]
    return {
        "time": time.time(),
        "uptime_s": round(time.time() - started),
        "following": receiver.active.role.value if receiver.active else None,
        "output": str(frame.timecode) if frame else None,
        "rate": frame.timecode.rate if frame else None,
        "idle_reason": None if frame else idle_reason,
        "output_device": output_device,
        "readback": readback,
        "sources": sources,
        "recent_switches": switches,
    }


def write(path: Path, data: dict) -> None:
    """Write atomically so a reader never sees half a file."""
    path = Path(path)
    tmp = path.with_name(f".{path.name}.{os.getpid()}")
    tmp.write_text(json.dumps(data))
    tmp.replace(path)


def read(path: Path) -> dict | None:
    try:
        return json.loads(Path(path).read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        return None
