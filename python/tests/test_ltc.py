import numpy as np
import pytest

from codenet.ltc import LtcDecoder, encode_frame, encode_run, frame_bits, samples_per_frame
from codenet.timecode import Timecode


@pytest.mark.parametrize("rate", [25, 30])
def test_frame_bits_have_even_ones_and_sync_word(rate):
    for frames in (0, 1, 12345, rate * 3600 * 23 + 7):
        bits = frame_bits(Timecode(frames, rate), user_bits=0x12345678)
        assert sum(bits) % 2 == 0
        assert bits[64:] == [0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1]


@pytest.mark.parametrize("rate", [25, 30])
@pytest.mark.parametrize("sample_rate", [44100, 48000, 96000])
def test_round_trip(rate, sample_rate):
    start = Timecode.parse("01:59:59:%02d" % (rate - 3), rate)
    count = 10
    audio = np.concatenate([np.zeros(500, np.float32), encode_run(start, count, sample_rate, 0xCAFE), np.zeros(10)])
    decoded = LtcDecoder(sample_rate).feed(audio)
    spf = samples_per_frame(sample_rate, rate)
    # The first frame is used to lock on and the last is only closed by the next frame's
    # first transition, so a run out of silence loses at most those two.
    assert len(decoded) >= count - 2
    first = decoded[0].timecode.frames - start.frames
    for i, frame in enumerate(decoded):
        k = first + i
        assert frame.timecode == start.next(k)
        assert frame.user_bits == 0xCAFE
        assert frame.start_sample == 500 + k * spf
        assert frame.end_sample == 500 + (k + 1) * spf


def test_streaming_in_small_blocks_and_scaled_level():
    rate, sr = 25, 48000
    start = Timecode.parse("10:00:00:00", rate)
    audio = 0.2 * encode_run(start, 8, sr)
    decoder = LtcDecoder(sr)
    out = []
    for i in range(0, len(audio), 64):
        out.extend(decoder.feed(audio[i:i + 64]))
    assert len(out) >= 6
    first = out[0].timecode.frames - start.frames
    assert [f.timecode for f in out] == [start.next(first + k) for k in range(len(out))]


def test_frames_join_without_glitch():
    a = encode_frame(Timecode(0, 25))
    assert a[0] == 1.0 and a[-1] == -1.0
