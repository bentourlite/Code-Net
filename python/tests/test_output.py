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
