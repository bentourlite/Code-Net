"""codenet-menu: set up and monitor a Code Net receiver over SSH."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

from ..receiver import settings as settings_mod
from ..receiver import status as status_mod
from . import network

SERVICE = "codenet-receiver"


# ---- small terminal helpers -------------------------------------------------

def clear() -> None:
    print("\033[2J\033[H", end="")


def pause() -> None:
    input("\nPress Enter to go back. ")


def ask(prompt: str, default: str = "") -> str:
    shown = f" [{default}]" if default != "" else ""
    answer = input(f"{prompt}{shown}: ").strip()
    return answer if answer else default


def ask_yes(prompt: str, default: bool = False) -> bool:
    answer = ask(prompt + " (y/n)", "y" if default else "n").lower()
    return answer.startswith("y")


def choose(title: str, options: list[tuple[str, str]]) -> str:
    """Show numbered options; returns the chosen key."""
    print(title)
    for i, (_, label) in enumerate(options, 1):
        print(f"  {i}) {label}")
    while True:
        answer = input("> ").strip().lower()
        if answer.isdigit() and 1 <= int(answer) <= len(options):
            return options[int(answer) - 1][0]
        for key, _ in options:
            if answer == key:
                return key
        print("Type one of the numbers above.")


def run(cmd: list[str], check: bool = False) -> int:
    print("$ " + " ".join(cmd))
    return subprocess.run(cmd, check=check).returncode


# ---- monitor ----------------------------------------------------------------

def render_monitor(st: dict | None, now: float | None = None, service_state: str = "") -> str:
    """The live monitor screen, as text."""
    now = time.time() if now is None else now
    lines = ["CODE NET RECEIVER MONITOR      (Ctrl+C to go back)", ""]
    if st is None:
        lines.append("No status yet. Is the receiver running?")
        if service_state:
            lines.append(f"Service: {service_state}")
        return "\n".join(lines)
    age = now - st.get("time", 0)
    stale = age > 3
    lines.append(f"Receiver   {st.get('name', '?')}    up {_duration(st.get('uptime_s', 0))}"
                 + ("    Wi-Fi mode" if st.get("wifi_mode") else ""))
    if service_state:
        lines.append(f"Service    {service_state}")
    if stale:
        lines.append(f"!! Status is {age:.0f} s old: the receiver may have stopped.")
    lines.append("")
    if st.get("output"):
        lines.append(f"OUTPUT     {st['output']}  @ {st.get('rate')} fps   following {st.get('following')}")
    else:
        lines.append(f"OUTPUT     stopped   ({st.get('idle_reason') or 'no source'})")
    lines.append(f"Device     {st.get('output_device', '?')}")
    rb = st.get("readback")
    if rb:
        offset = f"  {rb['offset_ms']:+.2f} ms" if rb.get("offset_ms") is not None else ""
        lines.append(f"Readback   {rb['state'].upper()}{offset}  ({rb.get('timecode') or '-'})")
    lines.append("")
    lines.append(f"{'HOST':<17}{'ROLE':<8}{'ADDRESS':<17}{'STATE':<10}{'TIMECODE':<13}{'LAST':>7}"
                 f"{'GAP':>7}{'CLOCK':>8}{'RTT':>9}")
    for src in st.get("sources", []):
        state = "PLAYING" if src["valid"] and src["playing"] else "STOPPED" if src["valid"] else "LOST"
        if src.get("last_packet_ms_ago") is None:
            state = "NOT SEEN"
        lines.append(
            f"{src['name']:<17}{src['role']:<8}{src.get('address') or '-':<17}{state:<10}"
            f"{src.get('last_timecode') or '-':<13}{_ms(src.get('last_packet_ms_ago')):>7}"
            f"{_ms(src.get('longest_gap_ms')):>7}{'synced' if src.get('clock_offset_ms') is not None else '-':>8}"
            f"{_ms(src.get('round_trip_ms'), 2):>9}")
    others = st.get("other_hosts") or {}
    if others:
        lines.append("")
        lines.append("Other hosts heard: " + ", ".join(f"{n} ({a})" for n, a in sorted(others.items())))
    switches = st.get("recent_switches") or []
    if switches:
        lines.append("")
        lines.append("Recent switches:")
        for sw in reversed(switches):
            lines.append(f"  {sw['seconds_ago']:>5} s ago  {sw['from'] or 'nothing'} -> {sw['to'] or 'nothing'}"
                         f"  ({sw['reason']})")
    return "\n".join(lines)


def _ms(value, digits: int = 0) -> str:
    if value is None:
        return "-"
    return f"{value:.{digits}f}ms"


def _duration(seconds: float) -> str:
    seconds = int(seconds)
    h, rem = divmod(seconds, 3600)
    m, s = divmod(rem, 60)
    return f"{h}h{m:02d}m" if h else f"{m}m{s:02d}s"


def service_state() -> str:
    if not shutil.which("systemctl"):
        return ""
    out = subprocess.run(["systemctl", "is-active", SERVICE], capture_output=True, text=True).stdout.strip()
    enabled = subprocess.run(["systemctl", "is-enabled", SERVICE], capture_output=True, text=True).stdout.strip()
    return f"{out or 'unknown'}, {'starts at boot' if enabled == 'enabled' else 'not started at boot'}"


def monitor(status_path: Path) -> None:
    try:
        while True:
            screen = render_monitor(status_mod.read(status_path), service_state=service_state())
            clear()
            print(screen, flush=True)
            time.sleep(1)
    except KeyboardInterrupt:
        pass


# ---- settings ---------------------------------------------------------------

def audio_devices() -> tuple[list[str], list[str]]:
    """(output names, input names) of audio devices, hardware devices first."""
    try:
        import sounddevice as sd

        devices = sd.query_devices()
    except Exception as exc:  # PortAudio missing, no devices
        print(f"(could not list audio devices: {exc})")
        return [], []
    outs = [d["name"] for d in devices if d["max_output_channels"] > 0 and "hw:" in d["name"]]
    ins = [d["name"] for d in devices if d["max_input_channels"] > 0 and "hw:" in d["name"]]
    return outs, ins


def midi_ports() -> list[str]:
    try:
        import mido

        return list(mido.get_output_names())
    except Exception as exc:
        print(f"(could not list MIDI ports: {exc})")
        return []


def _short_device(name: str) -> str:
    """'USB Audio CODEC: - (hw:4,0)' -> 'USB Audio CODEC', which survives card renumbering."""
    return name.split(":")[0].strip()


def pick(title: str, names: list[str], current: str, allow_none: str | None = None) -> str:
    options = [(str(i), n) for i, n in enumerate(names, 1)]
    if allow_none:
        options.append(("none", allow_none))
    options.append(("keep", f"keep current ({current or 'not set'})"))
    key = choose(title, options)
    if key == "keep":
        return current
    if key == "none":
        return ""
    return _short_device(names[int(key) - 1])


def edit_settings(config_path: Path, status_path: Path) -> bool:
    """Walk through the receiver settings. Returns True if they were saved."""
    s = settings_mod.load(config_path)
    clear()
    print("RECEIVER SETTINGS   (press Enter to keep the value in brackets)\n")
    st = status_mod.read(status_path)
    heard = sorted((st or {}).get("other_hosts", {}).items()) + [
        (src["name"], src.get("address")) for src in (st or {}).get("sources", []) if src.get("address")]
    if heard:
        print("Hosts heard on the network: " + ", ".join(f"{n} ({a})" for n, a in sorted(set(heard))) + "\n")

    s.name = ask("This receiver's name", s.name)
    s.main = ask("Main host name", s.main)
    s.backup = ask("Backup host name (type - for none)", s.backup or "-")
    s.backup = "" if s.backup == "-" else s.backup
    s.output = choose("\nOutput:", [("soundcard", "LTC from a USB soundcard"),
                                    ("mif4", "MTC to a Rosendahl MIF4 (USB MIDI)")])
    outs, ins = audio_devices()
    if s.output == "soundcard":
        s.device = pick("\nSoundcard for LTC output:", outs, s.device)
        s.level = float(ask("Output level, 0 to 1", str(s.level)))
    else:
        ports = midi_ports()
        s.midi_port = pick("\nMIDI port for the MIF4:", ports, s.midi_port, allow_none="find automatically")
    s.readback_device = pick("\nInput that hears the LTC output (for confirmation):", ins, s.readback_device,
                             allow_none="no confirmation")
    s.advance_ms = float(ask("Device delay to compensate, in ms", str(s.advance_ms)))
    mode = choose("\nNetwork mode:", [("multicast", f"multicast group {settings_mod.MULTICAST_GROUP} (normal)"),
                                       ("unicast", "unicast: hosts send straight to this Pi's address")])
    s.group = settings_mod.MULTICAST_GROUP if mode == "multicast" else "none"
    s.wifi = ask_yes("Wi-Fi testing mode (rides through Wi-Fi gaps; not for shows)", s.wifi)
    s.auto_return = ask_yes("Return to main automatically after 10 s of stable signal", s.auto_return)

    problems = s.validate()
    if problems:
        print("\nNot saved: " + "; ".join(problems))
        pause()
        return False
    print()
    for key, value in s.to_dict().items():
        print(f"  {key:<20} {value}")
    if not ask_yes("\nSave these settings", True):
        return False
    settings_mod.save(s, config_path)
    print(f"Saved to {config_path}.")
    if ask_yes("Restart the receiver now so they take effect", True):
        run(["sudo", "systemctl", "restart", SERVICE])
    pause()
    return True


# ---- network ----------------------------------------------------------------

def network_menu() -> None:
    while True:
        clear()
        print("NETWORK\n")
        try:
            conns = network.connections()
        except (OSError, subprocess.CalledProcessError) as exc:
            print(f"Could not read network settings: {exc}")
            pause()
            return
        for c in conns:
            mode = "static" if c.method == "manual" else "DHCP"
            print(f"  {c.device:<8} {c.kind:<9} {mode:<7} {', '.join(c.addresses) or 'no address':<22} ({c.name})")
        print()
        options = [(f"c{i}", f"Change {c.device} ({c.name})") for i, c in enumerate(conns)]
        options.append(("back", "Back"))
        key = choose("What would you like to do?", options)
        if key == "back":
            return
        change_connection(conns[int(key[1:])])


def change_connection(c: network.Connection) -> None:
    print(f"\n{c.device}: {c.name}")
    actions = [("static", "Set a static IP address"), ("dhcp", "Use DHCP (address from the router)")]
    if c.kind == "wifi":
        actions.append(("powersave", "Turn Wi-Fi power saving off permanently"))
    actions.append(("back", "Back"))
    action = choose("", actions)
    if action == "back":
        return
    if action == "powersave":
        network.apply(network.wifi_powersave_off_commands(c.name))
        pause()
        return
    if action == "static":
        current = c.addresses[0] if c.addresses else "192.168.7.10/24"
        while True:
            address = ask("Address with prefix, e.g. 192.168.7.10/24", current)
            gateway = ask("Gateway (blank for none)", "")
            dns = ask("DNS servers, comma separated (blank for none)", "")
            try:
                commands = network.static_commands(c.name, address, gateway, dns)
                break
            except ValueError as exc:
                print(f"That doesn't look right: {exc}")
    else:
        commands = network.dhcp_commands(c.name)
    print("\nThese commands will run:")
    for cmd in commands:
        print("  " + " ".join(f"'{a}'" if (" " in a or not a) else a for a in cmd))
    if os.environ.get("SSH_CONNECTION"):
        print("\nYou are connected over SSH. If this is the interface you came in on, the session will "
              "drop; reconnect to the new address.")
    if ask_yes("Apply now", False):
        try:
            network.apply(commands)
            print("Done.")
        except subprocess.CalledProcessError as exc:
            print(f"Failed: {exc}")
    pause()


# ---- service ----------------------------------------------------------------

def service_menu() -> None:
    while True:
        clear()
        print(f"RECEIVER SERVICE   ({service_state() or 'systemd not available'})\n")
        key = choose("", [("restart", "Start or restart"), ("stop", "Stop"),
                          ("enable", "Start automatically at boot"), ("disable", "Don't start at boot"),
                          ("logs", "Show the last 40 log lines"), ("follow", "Follow the log live (Ctrl+C to stop)"),
                          ("back", "Back")])
        if key == "back":
            return
        try:
            if key in ("restart", "stop"):
                run(["sudo", "systemctl", key, SERVICE])
            elif key in ("enable", "disable"):
                run(["sudo", "systemctl", key, SERVICE])
            elif key == "logs":
                run(["journalctl", "-u", SERVICE, "-n", "40", "--no-pager"])
            elif key == "follow":
                run(["journalctl", "-u", SERVICE, "-f", "-n", "20"])
        except KeyboardInterrupt:
            pass
        pause()


def devices_screen() -> None:
    clear()
    print("AUDIO AND MIDI DEVICES\n")
    outs, ins = audio_devices()
    print("Audio outputs:")
    for n in outs or ["(none)"]:
        print("  " + n)
    print("Audio inputs:")
    for n in ins or ["(none)"]:
        print("  " + n)
    print("MIDI outputs:")
    for n in midi_ports() or ["(none)"]:
        print("  " + n)
    pause()


def start_receiver(config_path: Path, status_path: Path) -> None:
    """Start (or restart) the service, after checking the settings are usable."""
    problems = settings_mod.load(config_path).validate()
    if problems:
        print("\nThe receiver can't start yet: " + "; ".join(problems) + ".")
        if ask_yes("Set it up now", True):
            if not edit_settings(config_path, status_path):
                return
        else:
            return
    run(["sudo", "systemctl", "restart", SERVICE])
    if ask_yes("Also start it automatically at boot", True):
        run(["sudo", "systemctl", "enable", SERVICE])
    if ask_yes("Open the live monitor", True):
        monitor(status_path)
    else:
        pause()


# ---- main menu --------------------------------------------------------------

def main(argv=None) -> None:
    p = argparse.ArgumentParser(description="Set up and monitor a Code Net receiver.")
    p.add_argument("--config", type=Path, default=settings_mod.DEFAULT_CONFIG)
    p.add_argument("--status-file", type=Path, default=settings_mod.DEFAULT_STATUS)
    p.add_argument("--monitor", action="store_true", help="go straight to the live monitor")
    args = p.parse_args(argv)
    if args.monitor:
        monitor(args.status_file)
        return
    try:
        while True:
            clear()
            s = settings_mod.load(args.config)
            st = status_mod.read(args.status_file)
            out = (st or {}).get("output") or "stopped"
            print("CODE NET RECEIVER")
            print(f"  {s.name}: main {s.main or 'not set'}, backup {s.backup or 'none'}, "
                  f"{s.output} {s.device or s.midi_port or ''}".rstrip())
            print(f"  Output {out}    Service {service_state() or 'n/a'}\n")
            key = choose("", [("start", "Start receiver (or restart it)"), ("stop", "Stop receiver"),
                              ("monitor", "Live monitor"), ("settings", "Receiver settings"),
                              ("network", "Network and IP addresses"), ("service", "Service: start at boot, logs"),
                              ("devices", "List audio and MIDI devices"),
                              ("main", "Return to main host now"), ("quit", "Quit")])
            if key == "quit":
                return
            if key == "start":
                start_receiver(args.config, args.status_file)
            elif key == "stop":
                run(["sudo", "systemctl", "stop", SERVICE])
                pause()
            elif key == "monitor":
                monitor(args.status_file)
            elif key == "settings":
                edit_settings(args.config, args.status_file)
            elif key == "network":
                network_menu()
            elif key == "service":
                service_menu()
            elif key == "devices":
                devices_screen()
            elif key == "main":
                run(["sudo", "systemctl", "kill", "-s", "SIGUSR1", SERVICE])
                pause()
    except (KeyboardInterrupt, EOFError):
        print()
        sys.exit(0)


if __name__ == "__main__":
    main()
