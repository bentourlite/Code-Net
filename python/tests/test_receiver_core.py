from codenet.protocol import FrameFlags, FramePacket, source_id
from codenet.receiver.core import NS_PER_S, Receiver, ReceiverConfig, Role
from codenet.timecode import Timecode

MAIN, BACKUP = source_id("mac-a"), source_id("mac-b")
FRAME = NS_PER_S // 25


def make(**kw):
    rx = Receiver(ReceiverConfig(main=MAIN, backup=BACKUP, **kw))
    # Host clocks equal to the local clock for these tests.
    rx.on_sync(MAIN, 0, 0, 0, 0)
    rx.on_sync(BACKUP, 0, 0, 0, 0)
    return rx


def frame(src, k, flags=FrameFlags.PLAYING, base=Timecode.parse("01:00:00:00", 25)):
    return FramePacket(src, 0, k, base.next(k), 0, k * FRAME, flags)


def feed(rx, k, sources=(MAIN, BACKUP), flags=FrameFlags.PLAYING):
    # Packet for frame k arrives once the frame has been decoded, one frame after it started.
    for s in sources:
        rx.on_frame(frame(s, k, flags), (k + 1) * FRAME)


def test_follows_main_and_predicts_next_frame():
    rx = make()
    feed(rx, 0)
    assert rx.active.role is Role.MAIN
    f = rx.frame_at(FRAME + 1)
    assert str(f.timecode) == "01:00:00:01" and f.start_ns == FRAME


def test_switches_to_backup_when_main_goes_quiet():
    rx = make()
    for k in range(5):
        feed(rx, k)
    for k in range(5, 10):
        feed(rx, k, sources=(BACKUP,))
    assert rx.active.role is Role.BACKUP
    assert rx.switches[-1].reason == "main lost"
    assert str(rx.frame_at(10 * FRAME).timecode) == "01:00:00:10"


def test_output_stops_when_both_lost():
    rx = make()
    feed(rx, 0)
    # Within the prediction window output continues...
    assert rx.frame_at(2 * FRAME + 1) is not None
    # ...then stops: no freewheel.
    assert rx.frame_at(3 * FRAME + 1) is None
    rx.update(10 * FRAME)
    assert rx.active is None


def test_stopped_main_is_followed_not_failed_over():
    rx = make()
    feed(rx, 0)
    rx.on_frame(frame(MAIN, 1, FrameFlags.NONE), 2 * FRAME)
    rx.on_frame(frame(BACKUP, 1), 2 * FRAME)
    assert rx.active.role is Role.MAIN
    assert rx.frame_at(2 * FRAME) is None


def test_source_lost_flag_fails_over_immediately():
    rx = make()
    feed(rx, 0)
    rx.on_frame(frame(MAIN, 1, FrameFlags.SOURCE_LOST), 2 * FRAME)
    assert rx.active.role is Role.BACKUP


def test_returns_to_main_after_stable_period():
    rx = make(return_to_main_after_ns=NS_PER_S)
    feed(rx, 0)
    for k in range(1, 5):
        feed(rx, k, sources=(BACKUP,))
    assert rx.active.role is Role.BACKUP
    for k in range(5, 20):
        feed(rx, k)
    assert rx.active.role is Role.BACKUP  # main back for under a second
    for k in range(20, 60):
        feed(rx, k)
    assert rx.active.role is Role.MAIN
    assert rx.switches[-1].reason == "returned to main"


def test_manual_return_to_main():
    rx = make(auto_return=False)
    feed(rx, 0)
    for k in range(1, 5):
        feed(rx, k, sources=(BACKUP,))
    feed(rx, 5)
    assert rx.active.role is Role.BACKUP
    rx.request_return_to_main()
    feed(rx, 6)
    assert rx.active.role is Role.MAIN


def test_ignores_unknown_sources_and_unsynced_clock():
    rx = Receiver(ReceiverConfig(main=MAIN))
    rx.on_frame(frame(source_id("stranger"), 0), FRAME)
    rx.on_frame(frame(MAIN, 0), FRAME)
    assert rx.active is None  # no clock sync yet


def test_failover_is_gapless_when_ticked():
    rx = make()
    for k in range(5):
        feed(rx, k)
    outputs = []
    # Main stops after frame 4; backup keeps going. The network loop ticks every ms.
    t, k = 5 * FRAME, 5
    while t < 12 * FRAME:
        if t >= (k + 1) * FRAME:
            rx.on_frame(frame(BACKUP, k), t)
            k += 1
        rx.update(t)
        f = rx.frame_at(t)
        outputs.append(f.timecode.frames if f else None)
        t += NS_PER_S // 1000
    assert None not in outputs
    assert rx.active.role is Role.BACKUP


def test_service_explains_why_output_is_idle():
    import socket

    from codenet.receiver.service import ReceiverService

    rx = Receiver(ReceiverConfig(main=MAIN))
    svc = ReceiverService(rx, socket.socket(socket.AF_INET, socket.SOCK_DGRAM), source_id("pi"))
    assert "no Code Net packets" in svc.describe_idle(0)
    svc._handle(frame(MAIN, 0).encode(), "10.0.0.2")
    assert "clock is not synced" in svc.describe_idle(FRAME)
    svc._handle(frame(source_id("mac-z"), 0).encode(), "10.0.0.3")
    assert source_id("mac-z") in svc._ignored


def test_wider_windows_ride_through_wifi_sized_gaps():
    rx = make(loss_timeout_frames=6, max_predict_frames=6)
    feed(rx, 0)
    t = 4 * FRAME  # ~130 ms with no packets
    rx.update(t)
    assert rx.active is not None and rx.frame_at(t) is not None


def test_assignment_swaps_roles_and_keeps_clock_sync():
    rx = make()
    feed(rx, 0)
    assert rx.active.role is Role.MAIN
    rx.set_sources(BACKUP, MAIN, 2 * FRAME)
    assert rx.sources[BACKUP].role is Role.MAIN and rx.sources[MAIN].role is Role.BACKUP
    assert rx.sources[BACKUP].clock.ready
    feed(rx, 1)
    assert rx.active.source == BACKUP


def test_assignment_drops_an_unassigned_active_host():
    rx = make()
    feed(rx, 0)
    other = source_id("mac-c")
    rx.set_sources(other, None, 2 * FRAME)
    assert rx.active is None and rx.switches[-1].reason == "host unassigned"
    assert list(rx.sources) == [other]


def test_service_applies_assignment_from_a_host():
    import socket

    from codenet.protocol import Assign
    from codenet.receiver.service import ReceiverService

    rx = Receiver(ReceiverConfig(main=MAIN))
    svc = ReceiverService(rx, socket.socket(socket.AF_INET, socket.SOCK_DGRAM), source_id("pi"))
    saved = []
    svc.on_assign = lambda main, backup: saved.append((main, backup))
    other = source_id("mac-c")
    svc._handle(frame(other, 0).encode(), "10.0.0.3")
    assert "mac-c" in svc.other_hosts

    svc._handle(Assign(source_id("pi-2"), other, MAIN, 1).encode(), "10.0.0.3")  # another receiver's
    assert saved == []
    svc._handle(Assign(source_id("pi"), other, MAIN, 2).encode(), "10.0.0.3")
    svc._handle(Assign(source_id("pi"), other, MAIN, 2).encode(), "10.0.0.3")  # repeat is ignored
    assert saved == [("mac-c", "mac-a")]
    assert rx.config.main == other and svc.host_addresses[other] == "10.0.0.3"
    svc._handle(Assign(source_id("pi"), other, other, 3).encode(), "10.0.0.3")  # main == backup
    assert saved == [("mac-c", "mac-a")]


def test_receiver_without_a_main_waits_for_an_assignment():
    rx = Receiver(ReceiverConfig(main=None))
    rx.on_sync(MAIN, 0, 0, 0, 0)
    feed(rx, 0, sources=(MAIN,))
    assert rx.active is None and rx.frame_at(FRAME) is None
    rx.set_sources(MAIN, None, FRAME)
    rx.on_sync(MAIN, 0, 0, 0, 0)
    feed(rx, 1, sources=(MAIN,))
    assert rx.active.role is Role.MAIN
