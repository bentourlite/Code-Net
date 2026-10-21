import json

import pytest

from codenet.pi import network
from codenet.pi.menu import _short_device, render_monitor
from codenet.protocol import FrameFlags, FramePacket, source_id
from codenet.receiver import settings, status
from codenet.receiver.core import NS_PER_S, Receiver, ReceiverConfig
from codenet.receiver.main import parse_args, resolve_settings
from codenet.timecode import Timecode


def test_settings_round_trip_and_validation(tmp_path):
    path = tmp_path / "receiver.json"
    s = settings.ReceiverSettings(name="pi-foh", main="mac-a", backup="mac-b", wifi=True)
    settings.save(s, path)
    loaded = settings.load(path)
    assert loaded == s
    assert loaded.effective_windows() == (6, 6)
    assert settings.ReceiverSettings(main="").validate() == ["main is not set"]
    assert "main and backup must be different hosts" in settings.ReceiverSettings(main="a", backup="a").validate()


def test_settings_ignore_unknown_keys(tmp_path):
    path = tmp_path / "receiver.json"
    path.write_text(json.dumps({"main": "mac-a", "something_new": 1, "level": "0.7"}))
    s = settings.load(path)
    assert s.main == "mac-a" and s.level == 0.7


def test_command_line_overrides_config(tmp_path):
    path = tmp_path / "receiver.json"
    settings.save(settings.ReceiverSettings(name="pi-1", main="mac-a", backup="mac-b"), path)
    s = resolve_settings(parse_args(["--config", str(path), "--backup", "mac-c", "--wifi"]))
    assert (s.name, s.main, s.backup, s.wifi, s.auto_return) == ("pi-1", "mac-a", "mac-c", True, True)
    s = resolve_settings(parse_args(["--config", str(path), "--no-auto-return"]))
    assert s.auto_return is False


def test_parse_nmcli_and_ip_output():
    active = "Wired connection 1:eth0:802-3-ethernet\nhome\\:5G:wlan0:802-11-wireless\nlo:lo:loopback\n"
    assert network.parse_active(active) == [("Wired connection 1", "eth0", "ethernet"), ("home:5G", "wlan0", "wifi")]
    ip = ("1: lo    inet 127.0.0.1/8 scope host lo\n"
          "2: eth0    inet 192.168.7.10/24 brd 192.168.7.255 scope global eth0\n"
          "3: wlan0    inet 192.168.8.19/24 brd 192.168.8.255 scope global dynamic wlan0\n")
    assert network.parse_device_addresses(ip)["eth0"] == ["192.168.7.10/24"]


def test_static_ip_commands_validate_input():
    cmds = network.static_commands("Wired connection 1", "192.168.7.10", "192.168.7.1", "1.1.1.1, 8.8.8.8")
    assert cmds[0][-8:] == ["ipv4.method", "manual", "ipv4.addresses", "192.168.7.10/24",
                            "ipv4.gateway", "192.168.7.1", "ipv4.dns", "1.1.1.1,8.8.8.8"]
    assert cmds[1] == ["sudo", "nmcli", "connection", "up", "Wired connection 1"]
    with pytest.raises(ValueError):
        network.static_commands("x", "192.168.7.300/24")
    with pytest.raises(ValueError):
        network.static_commands("x", "192.168.7.10/24", gateway="10.0.0.1")


def test_short_device_name():
    assert _short_device("USB Audio CODEC: - (hw:4,0)") == "USB Audio CODEC"


def test_status_snapshot_and_monitor_screen(tmp_path):
    main, backup = source_id("mac-a"), source_id("mac-b")
    rx = Receiver(ReceiverConfig(main=main, backup=backup))
    rx.on_sync(main, 0, 0, 0, 0)
    tc = Timecode.parse("01:00:00:00", 25)
    rx.on_frame(FramePacket(main, 0, 1, tc, 0, 0, FrameFlags.PLAYING), NS_PER_S // 25)
    snap = status.snapshot(rx, NS_PER_S // 25 + 1, {main: "10.0.0.2"}, {"mac-a": 41.0},
                           "soundcard: USB Audio CODEC", None, None, 0)
    snap.update(name="pi-1", other_hosts={"mac-z": "10.0.0.9"})
    path = tmp_path / "status.json"
    status.write(path, snap)
    screen = render_monitor(status.read(path), now=snap["time"])
    assert "OUTPUT     01:00:00:01" in screen
    assert "mac-a" in screen and "PLAYING" in screen and "10.0.0.2" in screen
    assert "mac-b" in screen and "NOT SEEN" in screen
    assert "mac-z (10.0.0.9)" in screen


def test_monitor_without_status():
    assert "Is the receiver running" in render_monitor(None)
