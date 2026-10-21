import pytest

from codenet.clock import ClockOffsetEstimator
from codenet.protocol import (FrameFlags, FramePacket, ProtocolError, SyncRequest, SyncResponse, decode,
                              source_id, source_name)
from codenet.timecode import Timecode


def test_frame_packet_round_trip():
    pkt = FramePacket(source_id("mac-a"), 0, 42, Timecode.parse("01:02:03:04", 30), 0xDEADBEEF,
                      123_456_789_012, FrameFlags.PLAYING)
    data = pkt.encode()
    assert len(data) == 46
    assert decode(data) == pkt
    assert source_name(pkt.source) == "mac-a"


def test_sync_round_trip():
    req = SyncRequest(source_id("pi-1"), 7, 1000)
    assert decode(req.encode()) == req
    resp = SyncResponse(source_id("mac-a"), 7, 1000, 5000, 5100)
    assert decode(resp.encode()) == resp


@pytest.mark.parametrize("data", [b"", b"XXXX\x01\x01" + bytes(40), b"CNET\x02\x01" + bytes(40), b"CNET\x01\x01"])
def test_rejects_bad_packets(data):
    with pytest.raises(ProtocolError):
        decode(data)


def test_clock_estimator_prefers_lowest_round_trip():
    est = ClockOffsetEstimator()
    # Host is 1_000_000 ns ahead. Symmetric 100 ns delay each way.
    est.add(t1=0, t2=1_000_100, t3=1_000_200, t4=300)
    # An asymmetric, slow exchange gives a worse offset and a longer round trip.
    est.add(t1=1000, t2=1_005_000, t3=1_005_100, t4=1200 + 5000)
    assert est.best.offset_ns == 1_000_000
    assert est.host_to_local(2_000_000) == 1_000_000


def test_status_report_round_trip():
    from codenet.protocol import StatusReport

    rep = StatusReport(source_id("pi-1"), b'{"output":"01:00:00:00"}')
    assert decode(rep.encode()) == rep
    with pytest.raises(ValueError):
        StatusReport(source_id("pi-1"), b"x" * 2000).encode()
