"""Receiver settings, stored as JSON so the Pi menu and the service share them."""

from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass, fields
from pathlib import Path

from ..protocol import FRAME_PORT, MULTICAST_GROUP

DEFAULT_CONFIG = Path(os.environ.get("CODENET_CONFIG", "/etc/codenet/receiver.json"))
DEFAULT_STATUS = Path(os.environ.get("CODENET_STATUS", "/run/codenet/status.json"))


@dataclass
class ReceiverSettings:
    name: str = "pi"
    main: str = ""
    backup: str = ""
    output: str = "soundcard"  # "soundcard" or "mif4"
    device: str = ""  # audio output device name; empty = system default
    midi_port: str = ""  # empty = first port containing "MIF"
    readback_device: str = ""  # empty = no output confirmation
    sample_rate: int = 48000
    blocksize: int = 64
    level: float = 0.5
    advance_ms: float = 0.0
    loss_timeout_frames: int = 2
    max_predict_frames: int = 2
    wifi: bool = False
    auto_return: bool = True
    group: str = MULTICAST_GROUP  # multicast group, or "none" for unicast
    port: int = FRAME_PORT

    def validate(self) -> list[str]:
        problems = []
        for label, value in (("name", self.name), ("main", self.main), ("backup", self.backup)):
            if label == "name" and not value:
                problems.append(f"{label} is not set")
            if len(value.encode("utf-8")) > 16:
                problems.append(f"{label} must be 16 bytes or fewer")
        if self.backup and not self.main:
            problems.append("a backup needs a main host")
        if self.main and self.main == self.backup:
            problems.append("main and backup must be different hosts")
        if self.output not in ("soundcard", "mif4"):
            problems.append("output must be soundcard or mif4")
        if not 0.0 <= self.level <= 1.0:
            problems.append("level must be between 0 and 1")
        if self.loss_timeout_frames < 1 or self.max_predict_frames < 1:
            problems.append("loss timeout and prediction window must be at least 1 frame")
        return problems

    def effective_windows(self) -> tuple[int, int]:
        """(loss timeout, prediction window) in frames, after Wi-Fi mode."""
        if self.wifi:
            return max(self.loss_timeout_frames, 6), max(self.max_predict_frames, 6)
        return self.loss_timeout_frames, self.max_predict_frames

    @classmethod
    def from_dict(cls, data: dict) -> ReceiverSettings:
        known = {f.name: f for f in fields(cls)}
        out = cls()
        for key, value in data.items():
            if key in known:
                setattr(out, key, type(getattr(out, key))(value))
        return out

    def to_dict(self) -> dict:
        return asdict(self)


def load(path: Path = DEFAULT_CONFIG) -> ReceiverSettings:
    try:
        return ReceiverSettings.from_dict(json.loads(Path(path).read_text()))
    except FileNotFoundError:
        return ReceiverSettings()


def save(settings: ReceiverSettings, path: Path = DEFAULT_CONFIG) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(settings.to_dict(), indent=2) + "\n")
    tmp.replace(path)
