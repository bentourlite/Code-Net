import numpy as np

from codenet.ltc import LtcDecoder
from codenet.protocol import FrameFlags, FramePacket, source_id
from codenet.receiver.core import NS_PER_S, Receiver, ReceiverConfig, ScheduledFrame
from codenet.receiver.mtc import full_frame, messages_for_frame, quarter_frame
from codenet.receiver.readback import Confirmation, ReadbackMonitor
from codenet.receiver.render import LtcRenderer
from codenet.timecode import Timecode

SR = 48000


def continuous_schedule(start: Timecode, t0: int, frames: int):
    def at(t):
        k = (t - t0) * start.rate // NS_PER_S
        if t < t0 or k >= frames:
            return None
        return ScheduledFrame(start.next(k), 0, t0 + k * NS_PER_S // start.rate)
    return at


def render_all(renderer, block_start, total, block=128):
    chunks = []
    for i in range(0, total, block):
        chunks.append(renderer.render(block_start + i * NS_PER_S // SR, min(block, total - i)))
    return np.concatenate(chunks)


def test_rendered_frames_start_on_schedule():
    start = Timecode.parse("00:59:59:20", 25)
    t0 = 7 * NS_PER_S // 1000  # first frame 7 ms into the stream
    r = LtcRenderer(continuous_schedule(start, t0, 12), SR)
    audio = render_all(r, 0, SR // 2)
    decoded = LtcDecoder(SR).feed(audio)
    assert len(decoded) >= 9
    for f in decoded:
        k = f.timecode.frames - start.frames
        expected_sample = round((t0 + k * NS_PER_S // 25) * SR / NS_PER_S)
        assert abs(f.start_sample - expected_sample) <= 1


def test_silence_when_nothing_scheduled():
    r = LtcRenderer(lambda t: None, SR)
    assert not r.render(0, 256).any()


def test_end_to_end_from_packets():
    main = source_id("mac-a")
    rx = Receiver(ReceiverConfig(main=main))
    rx.on_sync(main, 0, 1000, 1000, 0)  # host clock is 1000 ns ahead
    rate, start = 30, Timecode.parse("02:00:00:00", 30)
    r = LtcRenderer(rx.frame_at, SR)
    audio = []
    block = 64
    # Simulate: the packet for frame k arrives one frame after it starts.
    for i in range(0, SR // 2, block):
        t = i * NS_PER_S // SR
        k = t * rate // NS_PER_S - 1
        if k >= 0:
            rx.on_frame(FramePacket(main, 0, k, start.next(k), 0, k * NS_PER_S // rate + 1000, FrameFlags.PLAYING), t)
        audio.append(r.render(t, block))
    decoded = LtcDecoder(SR).feed(np.concatenate(audio))
    tcs = [f.timecode for f in decoded]
    assert len(tcs) >= 10
    assert all(b.follows(a) for a, b in zip(tcs, tcs[1:]))


def test_mtc_quarter_frames():
    tc = Timecode.parse("12:34:56:24", 25)
    pieces = [quarter_frame(tc, p) for p in range(8)]
    assert pieces[0] == bytes((0xF1, 0x08))  # frames LS nibble of 24
    assert pieces[1] == bytes((0xF1, 0x11))  # frames MS nibble
    assert pieces[6] == bytes((0xF1, 0x6C))  # hours LS nibble of 12
    assert pieces[7] == bytes((0xF1, 0x72))  # 25 fps code 1, hours MS bit 0
    assert full_frame(tc) == bytes((0xF0, 0x7F, 0x7F, 0x01, 0x01, 0x2C, 34, 56, 24, 0xF7))


def test_mtc_sequence_spans_two_frames_and_marks_jumps():
    a = Timecode.parse("00:00:01:00", 30)
    first = messages_for_frame(a, None)
    assert first[0][1][0] == 0xF0 and len(first) == 5
    second = messages_for_frame(a.next(), a)
    assert [m[1][1] >> 4 for m in second] == [4, 5, 6, 7]


def test_readback_confirms_and_reports_offset():
    start = Timecode.parse("00:00:10:00", 25)
    r = LtcRenderer(continuous_schedule(start, 0, 20), SR)
    audio = render_all(r, 0, SR // 2)
    mon = ReadbackMonitor(SR)
    # Pretend the loop adds 1 ms: captured audio is the output delayed by 48 samples.
    delayed = np.concatenate([np.zeros(48, np.float32), audio])
    for i in range(0, len(delayed), 256):
        mon.feed(delayed[i:i + 256], i * NS_PER_S // SR, r.emitted)
    status = mon.status(SR // 2 * NS_PER_S // SR, outputting=True)
    assert status.state is Confirmation.CONFIRMED
    assert abs(status.offset_ns - 1_000_000) < 30_000


def test_readback_missing_when_nothing_decodes():
    mon = ReadbackMonitor(SR)
    mon.feed(np.zeros(4800, np.float32), 0, [])
    assert mon.status(NS_PER_S, outputting=True).state is Confirmation.MISSING
    assert mon.status(NS_PER_S, outputting=False).state is Confirmation.IDLE


def test_dac_clock_ignores_single_outliers_but_follows_sustained_jumps():
    from codenet.receiver.devices import DacClock

    clock = DacClock(SR)
    block = 64
    step = block * NS_PER_S // SR
    starts = [clock.block_start(i * step, block) for i in range(10)]
    # One callback timestamp 3 ms late must not move the sample clock.
    assert abs(clock.block_start(10 * step + 3_000_000, block) - 10 * step) < 1000
    assert abs(clock.block_start(11 * step, block) - 11 * step) < 1000
    # A sustained 5 ms jump is followed after a few blocks.
    for i in range(12, 40):
        last = clock.block_start(i * step + 5_000_000, block)
    assert abs(last - (39 * step + 5_000_000)) < 100_000
    assert starts[0] == 0


def _loop(renderer, mon, delay_samples, t_start, seconds=1.0):
    """Render and feed the output back into the readback monitor `delay_samples` late."""
    audio = render_all(renderer, t_start, int(SR * seconds))
    delayed = np.concatenate([np.zeros(delay_samples, np.float32), audio])
    for i in range(0, len(delayed), 256):
        mon.feed(delayed[i:i + 256], t_start + i * NS_PER_S // SR, renderer.emitted)


def test_calibration_measures_the_loop_and_cancels_it():
    from codenet.protocol import Calibrate, CalibrateMode
    from codenet.receiver.calibrate import Calibrator

    start = Timecode.parse("00:00:10:00", 25)
    advance = [0]
    mon = ReadbackMonitor(SR, advance_ns=lambda: advance[0])
    cal = Calibrator(mon, lambda: advance[0], lambda ns: advance.__setitem__(0, ns), measure_ns=NS_PER_S // 2,
                     min_frames=10)

    def schedule(t):
        # A continuous 25 fps timeline, started early by the current advance.
        return continuous_schedule(start, -advance[0], 1000)(t)

    cal.request(Calibrate(source_id("pi-1"), CalibrateMode.MEASURE, 0, 1), "10.0.0.2", 0)
    assert cal.status()["state"] == "measuring"
    _loop(LtcRenderer(schedule, SR), mon, 96, 0)  # 2 ms of unreported device delay
    cal.tick(NS_PER_S)
    st = cal.status()
    assert st["state"] == "done" and abs(st["measured_ms"] - 2.0) < 0.05
    assert abs(advance[0] - 2_000_000) < 50_000

    # With the advance applied the same loop now lands on the timeline.
    mon2 = ReadbackMonitor(SR, advance_ns=lambda: advance[0])
    _loop(LtcRenderer(schedule, SR), mon2, 96, 0)
    rb = mon2.status(NS_PER_S, outputting=True)
    assert rb.state is Confirmation.CONFIRMED and abs(rb.offset_ns) < 50_000


def test_calibration_set_and_failures():
    from codenet.protocol import Calibrate, CalibrateMode
    from codenet.receiver.calibrate import Calibrator

    advance = [0]
    cal = Calibrator(None, lambda: advance[0], lambda ns: advance.__setitem__(0, ns))
    cal.request(Calibrate(source_id("pi-1"), CalibrateMode.SET, 4500, 7), "h", 0)
    assert advance[0] == 4_500_000 and cal.status()["state"] == "done"
    cal.request(Calibrate(source_id("pi-1"), CalibrateMode.MEASURE, 0, 8), "h", 0)
    assert cal.status()["state"] == "failed" and "readback" in cal.status()["message"]
    # A repeat of the same request is ignored.
    advance[0] = 0
    cal.request(Calibrate(source_id("pi-1"), CalibrateMode.SET, 4500, 8), "h", 0)
    assert advance[0] == 0

    mon = ReadbackMonitor(SR)
    cal = Calibrator(mon, lambda: 0, lambda ns: None, measure_ns=100)
    cal.request(Calibrate(source_id("pi-1"), CalibrateMode.MEASURE, 0, 1), "h", 0)
    cal.tick(200)
    assert cal.status()["state"] == "failed" and "heard nothing" in cal.status()["message"]
    assert mon.on_offset is None
