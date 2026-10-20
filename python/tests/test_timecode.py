import pytest

from codenet.timecode import Timecode


def test_parse_and_format():
    tc = Timecode.parse("01:02:03:04", 25)
    assert str(tc) == "01:02:03:04"
    assert tc.frames == ((1 * 60 + 2) * 60 + 3) * 25 + 4


def test_wraps_at_midnight():
    tc = Timecode.parse("23:59:59:29", 30)
    assert str(tc.next()) == "00:00:00:00"
    assert tc.next().follows(tc)


def test_rejects_bad_values():
    with pytest.raises(ValueError):
        Timecode.parse("00:00:00:25", 25)
    with pytest.raises(ValueError):
        Timecode(0, 24)
