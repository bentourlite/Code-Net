// Unit tests for the shared C++ core. Plain asserts, no framework.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "codenet/ltc.hpp"
#include "codenet/protocol.hpp"

using namespace codenet;

static int failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                            \
        }                                                                          \
    } while (0)

static void testTimecode() {
    Timecode tc;
    CHECK(Timecode::fromHMSF(23, 59, 59, 29, 30, tc));
    CHECK(tc.toString() == "23:59:59:29");
    CHECK(tc.next().toString() == "00:00:00:00");
    CHECK(!Timecode::fromHMSF(0, 0, 0, 25, 25, tc));
}

static void testBits() {
    for (int rate : {25, 30}) {
        Timecode tc;
        Timecode::fromHMSF(12, 34, 56, rate - 1, rate, tc);
        auto bits = ltcFrameBits(tc, 0x12345678);
        int ones = 0;
        for (auto b : bits) ones += b;
        CHECK(ones % 2 == 0);
        CHECK(bits[64] == 0 && bits[66] == 1 && bits[78] == 0 && bits[79] == 1);
    }
}

static void testRoundTrip(int rate, int sampleRate, int block) {
    Timecode start;
    Timecode::fromHMSF(1, 59, 59, rate - 3, rate, start);
    std::vector<float> audio(500, 0.0f);
    const int count = 10;
    for (int k = 0; k < count; ++k) {
        auto f = ltcEncodeFrame(start.next(uint32_t(k)), sampleRate, 0xCAFE);
        for (float x : f) audio.push_back(0.3f * x);
    }
    audio.resize(audio.size() + 10, 0.0f);
    LtcDecoder dec(sampleRate);
    std::vector<DecodedFrame> found;
    for (size_t i = 0; i < audio.size(); i += size_t(block)) {
        const int n = int(std::min(audio.size() - i, size_t(block)));
        dec.process(audio.data() + i, n, [&](const DecodedFrame& f) { found.push_back(f); });
    }
    CHECK(found.size() >= size_t(count - 2));
    if (found.empty()) return;
    const int64_t spf = sampleRate / rate;
    const uint32_t first = found[0].timecode.frames - start.frames;
    for (size_t i = 0; i < found.size(); ++i) {
        const uint32_t k = first + uint32_t(i);
        CHECK(found[i].timecode == start.next(k));
        CHECK(found[i].userBits == 0xCAFE);
        CHECK(found[i].startSample == 500 + int64_t(k) * spf);
        CHECK(found[i].endSample == 500 + int64_t(k + 1) * spf);
    }
}

static void testPacket() {
    FramePacket p;
    p.source = makeSourceId("mac-a");
    p.sequence = 42;
    Timecode::fromHMSF(1, 2, 3, 4, 30, p.timecode);
    p.userBits = 0xDEADBEEF;
    p.frameStartNs = 123456789012;
    p.flags = kFlagPlaying;
    uint8_t buf[FramePacket::kSize];
    encode(p, buf);
    FramePacket q;
    CHECK(decode(buf, sizeof buf, q));
    CHECK(q.source == p.source && q.sequence == 42 && q.timecode == p.timecode);
    CHECK(q.userBits == 0xDEADBEEF && q.frameStartNs == 123456789012 && q.flags == kFlagPlaying);
    buf[4] = 2;
    CHECK(!decode(buf, sizeof buf, q));
}

int main() {
    testTimecode();
    testBits();
    for (int rate : {25, 30})
        for (int sr : {44100, 48000, 96000})
            for (int block : {32, 512}) testRoundTrip(rate, sr, block);
    testPacket();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all core tests passed");
    return 0;
}
