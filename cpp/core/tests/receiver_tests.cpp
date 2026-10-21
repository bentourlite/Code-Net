// Receiver, renderer, MTC and network service tests. The cases mirror
// python/tests/test_receiver_core.py so both implementations agree.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "codenet/ltc.hpp"
#include "codenet/output.hpp"
#include "codenet/receiver.hpp"
#include "codenet/rx_service.hpp"

using namespace codenet;

static int failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

static const SourceId MAIN = makeSourceId("mac-a"), BACKUP = makeSourceId("mac-b");
static const int64_t FRAME = kNsPerSecond / 25;

static Timecode tcAt(int k) {
    Timecode base;
    Timecode::fromHMSF(1, 0, 0, 0, 25, base);
    return base.next(uint32_t(k));
}

static FramePacket frame(const SourceId& src, int k, uint8_t flags = kFlagPlaying) {
    FramePacket p;
    p.source = src;
    p.sequence = uint32_t(k);
    p.timecode = tcAt(k);
    p.frameStartNs = k * FRAME;
    p.flags = flags;
    return p;
}

static Receiver make(ReceiverConfig cfg = {}) {
    cfg.main = MAIN;
    cfg.backup = BACKUP;
    Receiver rx(cfg);
    rx.onSync(MAIN, 0, 0, 0, 0);  // host clocks equal to the local clock
    rx.onSync(BACKUP, 0, 0, 0, 0);
    return rx;
}

// The packet for frame k arrives once the frame has been decoded, one frame after it started.
static void feed(Receiver& rx, int k, std::vector<SourceId> sources = {MAIN, BACKUP}, uint8_t flags = kFlagPlaying) {
    for (auto& s : sources) rx.onFrame(frame(s, k, flags), (k + 1) * FRAME);
}

static void testFollowsMainAndPredicts() {
    auto rx = make();
    feed(rx, 0);
    CHECK(rx.active() && rx.active()->role == Role::Main);
    auto f = rx.frameAt(FRAME + 1);
    CHECK(f && f->timecode.toString() == "01:00:00:01" && f->startNs == FRAME);
}

static void testFailoverToBackup() {
    auto rx = make();
    for (int k = 0; k < 5; ++k) feed(rx, k);
    for (int k = 5; k < 8; ++k) feed(rx, k, {BACKUP});
    CHECK(rx.active()->role == Role::Backup);
    CHECK(rx.switches().back().reason == "main lost");
}

static void testStoppedMainIsFollowed() {
    auto rx = make();
    feed(rx, 0);
    for (int k = 1; k < 6; ++k) feed(rx, k, {MAIN, BACKUP}, kFlagNone);
    CHECK(rx.active()->role == Role::Main);
    CHECK(!rx.frameAt(6 * FRAME));
}

static void testSourceLostFailsOverImmediately() {
    auto rx = make();
    feed(rx, 0);
    rx.onFrame(frame(MAIN, 1, kFlagSourceLost), 2 * FRAME);
    rx.onFrame(frame(BACKUP, 1), 2 * FRAME);
    CHECK(rx.active()->role == Role::Backup);
}

static void testReturnsToMainAfterStablePeriod() {
    ReceiverConfig cfg;
    cfg.returnToMainAfterNs = 10 * FRAME;
    auto rx = make(cfg);
    for (int k = 0; k < 3; ++k) feed(rx, k, {BACKUP});
    CHECK(rx.active()->role == Role::Backup);
    for (int k = 3; k < 10; ++k) feed(rx, k);
    CHECK(rx.active()->role == Role::Backup);
    for (int k = 10; k < 16; ++k) feed(rx, k);
    CHECK(rx.active()->role == Role::Main);
}

static void testGaplessFailover() {
    auto rx = make();
    for (int k = 0; k < 10; ++k) feed(rx, k);
    // Main stops after frame 9; keep ticking every millisecond and check output never drops.
    bool gap = false;
    for (int64_t t = 11 * FRAME; t < 20 * FRAME; t += 1'000'000) {
        const int k = int(t / FRAME) - 1;
        if ((k + 1) * FRAME <= t && (k + 1) * FRAME > t - 1'000'000) rx.onFrame(frame(BACKUP, k), t);
        rx.update(t);
        if (!rx.frameAt(t)) gap = true;
    }
    CHECK(!gap);
    CHECK(rx.active()->role == Role::Backup);
}

static void testAssignmentSwapsRoles() {
    auto rx = make();
    feed(rx, 0);
    rx.setSources(BACKUP, MAIN, 2 * FRAME);
    CHECK(rx.find(BACKUP)->role == Role::Main && rx.find(MAIN)->role == Role::Backup);
    CHECK(rx.find(BACKUP)->clock.ready());
    feed(rx, 1);
    CHECK(rx.active()->source == BACKUP);
}

static void testNoMainWaitsForAssignment() {
    Receiver rx(ReceiverConfig{});
    rx.onSync(MAIN, 0, 0, 0, 0);
    feed(rx, 0, {MAIN});
    CHECK(rx.active() == nullptr && !rx.frameAt(FRAME));
    rx.setSources(MAIN, std::nullopt, FRAME);
    rx.onSync(MAIN, 0, 0, 0, 0);
    feed(rx, 1, {MAIN});
    CHECK(rx.active() && rx.active()->role == Role::Main);
}

static void testClockOffset() {
    ClockOffsetEstimator c;
    c.add(0, 1'000'500, 1'000'600, 2'000);    // rtt 1900
    c.add(10, 1'000'210, 1'000'220, 230);     // rtt 210, offset 1'000'095
    CHECK(c.best().rttNs == 210);
    CHECK(c.best().offsetNs == 1'000'095);
    CHECK(c.hostToLocal(2'000'095) == 1'000'000);
}

// Rendered LTC decodes back to the scheduled frames, each starting on its scheduled sample.
static void testRendererPlacesFrames() {
    const int sr = 48000;
    Schedule s;
    s.active = true;
    s.anchor = tcAt(0);
    s.anchorStartNs = 1'000'000;  // 1 ms into the block
    s.maxPredictFrames = 4;
    LtcRenderer r([&s](int64_t t) { return s.frameAt(t); }, sr, 0.5f);
    std::vector<float> audio(size_t(sr / 5));
    for (size_t i = 0; i < audio.size(); i += 256)
        r.render(int64_t(i) * kNsPerSecond / sr, audio.data() + i, int(std::min<size_t>(256, audio.size() - i)));
    LtcDecoder dec(sr);
    std::vector<DecodedFrame> found;
    dec.process(audio.data(), int(audio.size()), [&](const DecodedFrame& f) { found.push_back(f); });
    CHECK(found.size() >= 3);
    for (auto& f : found) {
        const int k = int(f.timecode.frames - tcAt(0).frames);
        if (f.startSample != 48 + int64_t(k) * sr / 25) std::fprintf(stderr, "k=%d start=%lld\n", k, (long long)f.startSample);
        CHECK(f.startSample == 48 + int64_t(k) * sr / 25);
    }
}

static std::string hex(const std::vector<uint8_t>& b) {
    std::string s;
    char buf[3];
    for (auto x : b) {
        std::snprintf(buf, sizeof buf, "%02x", x);
        s += buf;
    }
    return s;
}

static std::string mtcLine(const std::string& tc, std::optional<Timecode>& prev, int rate = 25) {
    int h, m, s, f;
    std::sscanf(tc.c_str(), "%d:%d:%d:%d", &h, &m, &s, &f);
    Timecode t;
    Timecode::fromHMSF(h, m, s, f, rate, t);
    std::string out;
    for (auto& msg : mtc::messagesForFrame(t, prev)) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.2g:", msg.fraction);
        out += (out.empty() ? "" : " ") + std::string(buf) + hex(msg.bytes);
    }
    prev = t;
    return out;
}

// Reference output from python/codenet/receiver/mtc.py.
static void testMtcMatchesPython() {
    std::optional<Timecode> prev;
    CHECK(mtcLine("01:02:03:04", prev) == "0:f07f7f010121020304f7");
    CHECK(mtcLine("01:02:03:05", prev) == "0:f105 0.25:f110 0.5:f123 0.75:f130");
    CHECK(mtcLine("01:02:03:06", prev) == "0:f142 0.25:f150 0.5:f161 0.75:f172");
    CHECK(mtcLine("10:00:00:00", prev) == "0:f07f7f01012a000000f7 0:f100 0.25:f110 0.5:f120 0.75:f130");
    std::optional<Timecode> none;
    CHECK(mtcLine("23:59:59:29", none, 30) == "0:f07f7f0101773b3b1df7");
}

static std::vector<uint8_t> bytes(const FramePacket& p) {
    uint8_t b[FramePacket::kSize];
    encode(p, b);
    return {b, b + sizeof b};
}

static void testServiceHandlesPacketsAndAssignments() {
    RxServiceConfig sc;
    sc.name = "mac-rx";
    ReceiverConfig rc;
    rc.main = MAIN;
    RxService svc(sc, rc);  // not started: packets are fed by hand
    std::string assigned;
    svc.setAssignCallback([&](const std::string& m, const std::string& b) { assigned = m + "/" + b; });

    auto status = svc.buildStatus(0);
    CHECK(status.find("\"idle\":\"no Code Net packets") != std::string::npos);

    auto f = bytes(frame(makeSourceId("mac-c"), 0));
    svc.handle(f.data(), f.size(), "10.0.0.3", FRAME);
    status = svc.buildStatus(FRAME);
    CHECK(status.find("\"heard\":[\"mac-c\"]") != std::string::npos);

    Assign a;
    a.receiver = makeSourceId("someone-else");
    a.main = makeSourceId("mac-c");
    a.sequence = 1;
    uint8_t out[Assign::kSize];
    encode(a, out);
    svc.handle(out, sizeof out, "10.0.0.3", FRAME);
    CHECK(assigned.empty());

    a.receiver = makeSourceId("mac-rx");
    a.backup = MAIN;
    a.sequence = 2;
    encode(a, out);
    svc.handle(out, sizeof out, "10.0.0.3", FRAME);
    CHECK(assigned == "mac-c/mac-a");
    status = svc.buildStatus(FRAME);
    CHECK(status.find("\"main\":\"mac-c\",\"backup\":\"mac-a\"") != std::string::npos);
    CHECK(status.find("\"address\":\"10.0.0.3\"") != std::string::npos);

    // Clock sync for the new main, then a playing frame: output starts.
    SyncResponse r;
    r.host = makeSourceId("mac-c");
    uint8_t sr[SyncResponse::kSize];
    putHeader(sr, MessageType::SyncResponse);
    std::memcpy(sr + 6, r.host.data(), 16);
    std::memset(sr + 22, 0, sizeof sr - 22);
    svc.handle(sr, sizeof sr, "10.0.0.3", 0);
    f = bytes(frame(makeSourceId("mac-c"), 1));
    svc.handle(f.data(), f.size(), "10.0.0.3", 2 * FRAME);
    auto out1 = svc.frameAt(2 * FRAME + 1);
    CHECK(out1 && out1->timecode == tcAt(2));
    status = svc.buildStatus(2 * FRAME + 1);
    CHECK(status.find("\"following\":\"main\"") != std::string::npos);
    CHECK(status.find("\"output\":\"01:00:00:02\"") != std::string::npos);
    CHECK(status.size() < kMaxStatusBytes);
}

int main() {
    testFollowsMainAndPredicts();
    testFailoverToBackup();
    testStoppedMainIsFollowed();
    testSourceLostFailsOverImmediately();
    testReturnsToMainAfterStablePeriod();
    testGaplessFailover();
    testAssignmentSwapsRoles();
    testNoMainWaitsForAssignment();
    testClockOffset();
    testRendererPlacesFrames();
    testMtcMatchesPython();
    testServiceHandlesPacketsAndAssignments();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all receiver tests passed");
    return 0;
}
