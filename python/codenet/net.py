"""UDP sockets for frame packets and clock sync."""

from __future__ import annotations

import socket
import struct

from .protocol import FRAME_PORT, MULTICAST_GROUP


def frame_receiver_socket(port: int = FRAME_PORT, group: str | None = MULTICAST_GROUP,
                          interface: str = "0.0.0.0") -> socket.socket:
    """A socket that receives frame packets, joined to the multicast group if one is given.

    The same socket sends clock sync requests, so replies come back to it.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    sock.bind(("", port))
    if group:
        mreq = struct.pack("4s4s", socket.inet_aton(group), socket.inet_aton(interface))
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    # Frame packets are tiny; ask for low-delay handling where the OS supports it.
    try:
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, 0xB8)  # DSCP EF
    except OSError:
        pass
    return sock


def sender_socket(ttl: int = 1, interface: str | None = None) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, ttl)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    if interface:
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(interface))
    try:
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, 0xB8)
    except OSError:
        pass
    return sock
