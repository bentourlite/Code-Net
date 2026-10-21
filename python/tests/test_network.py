"""End to end over localhost UDP: test sender -> receiver service -> renderer -> decoder."""

import socket
import threading
import time

import numpy as np
import pytest

from codenet.ltc import LtcDecoder, encode_run
from codenet.net import frame_receiver_socket
from codenet.protocol import source_id
from codenet.receiver.core import NS_PER_S, Receiver, ReceiverConfig, Role
from codenet.receiver.render import LtcRenderer
from codenet.receiver.service import ReceiverService
from codenet.sender import Sender, SyncResponder
from codenet.timecode import Timecode

SR = 48000


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture
def sync_port(monkeypatch):
    port = free_port()
    monkeypatch.setattr("codenet.receiver.service.SYNC_PORT", port)
    return port


def test_live_pipeline_over_udp(sync_port):
    port = free_port()
    start = Timecode.parse("05:00:00:00", 25)
    audio = 0.5 * encode_run(start, 40, SR)  # 1.6 s of LTC

    rx = Receiver(ReceiverConfig(main=source_id("mac-a")))
    service = ReceiverService(rx, frame_receiver_socket(port, group=None), source_id("pi"), sync_interval_s=0.05)
    sync = SyncResponder(source_id("mac-a"), sync_port)
    sender = Sender("mac-a", audio, SR, ("127.0.0.1", port), latency_ns=20_000_000)
    sync.start()
    service.start()
    sending = threading.Thread(target=sender.run, daemon=True)
    sending.start()

    renderer = LtcRenderer(service.frame_at, SR)
    out = []
    t_start = time.monotonic_ns()
    block = 480  # render 10 ms at a time against the real clock
    t = t_start
    while t < t_start + 1_500_000_000:
        out.append(renderer.render(t, block))
        t += block * NS_PER_S // SR
        while time.monotonic_ns() < t:
            time.sleep(0.0005)
    # Read the state while the sender is still running; once it stops the receiver
    # rightly drops main within two frames.
    active = rx.active
    service.send_status(b'{"name":"pi"}')
    time.sleep(0.2)
    sender.stop()
    sending.join(timeout=1)
    service.stop()
    sync.stop()

    assert active is not None and active.role is Role.MAIN
    assert sync.receivers["pi"][1] == b'{"name":"pi"}'
    decoded = LtcDecoder(SR).feed(np.concatenate(out))
    tcs = [f.timecode for f in decoded]
    assert len(tcs) >= 20, f"only {len(tcs)} frames decoded"
    assert all(b.follows(a) for a, b in zip(tcs, tcs[1:]))
