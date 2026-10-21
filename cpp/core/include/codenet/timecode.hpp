// SMPTE timecode at integer frame rates (25 and 30 fps).
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace codenet {

struct Timecode {
    uint32_t frames = 0;  // frames since 00:00:00:00
    uint8_t rate = 25;

    static bool supportedRate(int rate) { return rate == 25 || rate == 30; }
    static uint32_t framesPerDay(int rate) { return uint32_t(rate) * 60 * 60 * 24; }

    static bool fromHMSF(int h, int m, int s, int f, int rate, Timecode& out) {
        if (!supportedRate(rate) || h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59 || f < 0 || f >= rate)
            return false;
        out.frames = uint32_t(((h * 60 + m) * 60 + s) * rate + f);
        out.rate = uint8_t(rate);
        return true;
    }

    void toHMSF(int& h, int& m, int& s, int& f) const {
        uint32_t secs = frames / rate;
        f = int(frames % rate);
        s = int(secs % 60);
        m = int((secs / 60) % 60);
        h = int(secs / 3600);
    }

    Timecode next(uint32_t count = 1) const { return {(frames + count) % framesPerDay(rate), rate}; }
    bool follows(const Timecode& previous) const { return rate == previous.rate && previous.next() == *this; }

    std::string toString() const {
        int h, m, s, f;
        toHMSF(h, m, s, f);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%02d:%02d:%02d:%02d", h, m, s, f);
        return buf;
    }

    bool operator==(const Timecode& o) const { return frames == o.frames && rate == o.rate; }
    bool operator!=(const Timecode& o) const { return !(*this == o); }
};

}  // namespace codenet
