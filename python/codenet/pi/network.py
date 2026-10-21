"""Network settings through NetworkManager (nmcli), as used by Raspberry Pi OS Bookworm."""

from __future__ import annotations

import ipaddress
import subprocess
from dataclasses import dataclass


@dataclass(frozen=True)
class Connection:
    name: str
    device: str
    kind: str  # "ethernet", "wifi", ...
    method: str  # "auto" (DHCP) or "manual" (static)
    addresses: tuple[str, ...]


def _split_terse(line: str) -> list[str]:
    """Split one line of `nmcli -t` output, which escapes ':' inside values as '\\:'."""
    out, cur, esc = [], "", False
    for ch in line:
        if esc:
            cur += ch
            esc = False
        elif ch == "\\":
            esc = True
        elif ch == ":":
            out.append(cur)
            cur = ""
        else:
            cur += ch
    out.append(cur)
    return out


def parse_active(output: str) -> list[tuple[str, str, str]]:
    """(name, device, type) for each line of `nmcli -t -f NAME,DEVICE,TYPE connection show --active`."""
    rows = []
    for line in output.splitlines():
        parts = _split_terse(line)
        if len(parts) >= 3 and parts[1] and parts[1] != "lo":
            kind = parts[2].replace("802-3-ethernet", "ethernet").replace("802-11-wireless", "wifi")
            rows.append((parts[0], parts[1], kind))
    return rows


def parse_device_addresses(output: str) -> dict[str, list[str]]:
    """Interface to IPv4 addresses (with prefix) from `ip -4 -o addr show`."""
    found: dict[str, list[str]] = {}
    for line in output.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[2] == "inet":
            found.setdefault(parts[1], []).append(parts[3])
    return found


def _run(args: list[str]) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def connections() -> list[Connection]:
    active = parse_active(_run(["nmcli", "-t", "-f", "NAME,DEVICE,TYPE", "connection", "show", "--active"]))
    addrs = parse_device_addresses(_run(["ip", "-4", "-o", "addr", "show"]))
    out = []
    for name, device, kind in active:
        method = _run(["nmcli", "-g", "ipv4.method", "connection", "show", name]).strip()
        out.append(Connection(name, device, kind, method, tuple(addrs.get(device, []))))
    return out


def validate_static(address: str, gateway: str = "", dns: str = "") -> str:
    """Check a static address like 192.168.7.10/24; returns it normalised or raises ValueError."""
    if "/" not in address:
        address += "/24"
    iface = ipaddress.IPv4Interface(address)
    if gateway:
        gw = ipaddress.IPv4Address(gateway)
        if gw not in iface.network:
            raise ValueError(f"gateway {gw} is not inside {iface.network}")
    for server in filter(None, (d.strip() for d in dns.split(","))):
        ipaddress.IPv4Address(server)
    return str(iface)


def static_commands(connection: str, address: str, gateway: str = "", dns: str = "") -> list[list[str]]:
    address = validate_static(address, gateway, dns)
    mod = ["sudo", "nmcli", "connection", "modify", connection, "ipv4.method", "manual",
           "ipv4.addresses", address, "ipv4.gateway", gateway, "ipv4.dns", dns.replace(" ", "")]
    return [mod, ["sudo", "nmcli", "connection", "up", connection]]


def dhcp_commands(connection: str) -> list[list[str]]:
    mod = ["sudo", "nmcli", "connection", "modify", connection, "ipv4.method", "auto",
           "ipv4.addresses", "", "ipv4.gateway", "", "ipv4.dns", ""]
    return [mod, ["sudo", "nmcli", "connection", "up", connection]]


def wifi_powersave_off_commands(connection: str) -> list[list[str]]:
    """Turn Wi-Fi power saving off for good (2 = disable) and apply it now."""
    return [["sudo", "nmcli", "connection", "modify", connection, "802-11-wireless.powersave", "2"],
            ["sudo", "nmcli", "connection", "up", connection]]


def apply(commands: list[list[str]]) -> None:
    for cmd in commands:
        subprocess.run(cmd, check=True)
