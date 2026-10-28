// Recorder tests: binary/JSON round trips, format versioning and corruption, GDR (JSON and
// MessagePack) and frame-list imports, practice-mode stitching, and a recorded run replayed by
// the simulator.
#include "test.hpp"

#include <al/bot/bot.hpp>
#include <al/gen/test_pattern.hpp>
#include <al/physics/sim.hpp>
#include <al/rec/recording.hpp>
#include <al/util/codec.hpp>

using namespace al;
using namespace al::rec;

namespace {

Recording sampleRecording() {
    Recording r;
    r.levelName = "Test level";
    r.levelId = 128;
    r.levelHash = 0x1234567890abcdefull;
    r.songOffset = 1.5;
    r.completed = true;
    r.duration = 12.25;
    for (int i = 0; i < 40; ++i) {
        RecEvent e;
        e.tick = 100 + i * 37;
        e.time = e.tick / 240.0 + (i % 3) * 0.001;
        e.press = i % 2 == 0;
        e.player2 = i % 7 == 3;
        e.button = i % 11 == 5 ? Button::Left : Button::Jump;
        e.p1 = {float(i * 31.5), float(15 + i), float(-3.5 + i * 0.25), uint8_t(i % 8), uint8_t(i % 5), i % 2 == 0, i % 3 == 0, i % 4 == 0};
        e.dual = i % 5 == 0;
        if (e.dual) e.p2 = {float(i * 31.5), float(200 - i), 1.25f, 1, 2, false, true, false};
        e.orbId = i % 6 == 0 ? 36 : 0;
        r.events.push_back(e);
    }
    return r;
}

bool sameEvents(const Recording& a, const Recording& b, double timeTol) {
    if (a.events.size() != b.events.size()) return false;
    for (size_t i = 0; i < a.events.size(); ++i) {
        RecEvent x = a.events[i], y = b.events[i];
        if (std::fabs(x.time - y.time) > timeTol) return false;
        x.time = y.time = 0;
        if (!(x == y)) return false;
    }
    return true;
}

// MessagePack writer for the test (fixmap/fixarray/fixstr/uint/bool only).
struct MpW {
    std::string s;
    void map(int n) { s.push_back(char(0x80 | n)); }
    void arr(int n) { s.push_back(char(0x90 | n)); }
    void str(const std::string& v) { s.push_back(char(0xa0 | int(v.size()))); s += v; }
    void u(unsigned v) {
        if (v < 128) s.push_back(char(v));
        else { s.push_back(char(0xcd)); s.push_back(char(v >> 8)); s.push_back(char(v & 0xff)); }
    }
    void b(bool v) { s.push_back(char(v ? 0xc3 : 0xc2)); }
};

} // namespace

TEST(recording_binary_roundtrip) {
    Recording r = sampleRecording();
    std::string bin = r.serialize();
    std::string err;
    auto back = Recording::deserialize(bin, &err);
    REQUIRE(back.has_value());
    CHECK(back->levelName == r.levelName);
    CHECK(back->levelId == r.levelId);
    CHECK(back->levelHash == r.levelHash);
    CHECK(back->completed);
    CHECK_NEAR(back->songOffset, 1.5, 1e-12);
    CHECK(sameEvents(r, *back, 1e-6));
    std::printf("    %zu events -> %zu bytes (%.1f bytes/event)\n", r.events.size(), bin.size(), double(bin.size()) / double(r.events.size()));
    CHECK(bin.size() < r.events.size() * 40 + 80);
}

TEST(recording_json_roundtrip) {
    Recording r = sampleRecording();
    std::string text = r.toJson().dump(1);
    auto j = Json::parse(text);
    REQUIRE(j.has_value());
    auto back = Recording::fromJson(*j);
    REQUIRE(back.has_value());
    CHECK(back->levelHash == r.levelHash);
    CHECK(sameEvents(r, *back, 1e-9));
    // importAny recognises both formats
    CHECK(importAny(text).has_value());
    CHECK(importAny(r.serialize()).has_value());
}

TEST(recording_versioning_and_corruption) {
    Recording r = sampleRecording();
    std::string bin = r.serialize();
    // a newer format version is rejected with a clear message (checksum recomputed)
    std::string newer = bin.substr(0, bin.size() - 4);
    newer[4] = char(Recording::kVersion + 1);
    uint32_t c = crc32(newer);
    for (int i = 0; i < 4; ++i) newer.push_back(char(c >> (8 * i)));
    std::string err;
    CHECK(!Recording::deserialize(newer, &err).has_value());
    CHECK_MSG(err.find("newer") != std::string::npos, "error: %s", err.c_str());
    // a flipped byte is caught by the checksum
    std::string bad = bin;
    bad[bad.size() / 2] ^= 0x40;
    CHECK(!Recording::deserialize(bad, &err).has_value());
    CHECK(err.find("checksum") != std::string::npos);
    // truncation
    CHECK(!Recording::deserialize(bin.substr(0, bin.size() / 2), &err).has_value());
    CHECK(!Recording::deserialize("garbage", &err).has_value());
}

TEST(recording_import_gdr_json_and_msgpack) {
    const char* json = R"({"author":"x","framerate":60,"level":{"id":55,"name":"Lvl"},
        "inputs":[{"frame":30,"btn":1,"2p":false,"down":true},{"frame":36,"btn":1,"2p":false,"down":false},
                  {"frame":90,"btn":1,"2p":true,"down":true}]})";
    std::string err;
    auto r = importGdr(json, &err);
    REQUIRE(r.has_value());
    CHECK(r->levelId == 55);
    CHECK(r->levelName == "Lvl");
    REQUIRE(r->events.size() == 3);
    CHECK(r->events[0].tick == 120); // frame 30 at 60 fps = 0.5 s = step 120
    CHECK(r->events[1].tick == 144 && !r->events[1].press);
    CHECK(r->events[2].player2);
    // the same as MessagePack
    MpW w;
    w.map(3);
    w.str("framerate"); w.u(60);
    w.str("level"); w.map(2); w.str("id"); w.u(55); w.str("name"); w.str("Lvl");
    w.str("inputs"); w.arr(2);
    w.map(4); w.str("frame"); w.u(30); w.str("btn"); w.u(1); w.str("2p"); w.b(false); w.str("down"); w.b(true);
    w.map(4); w.str("frame"); w.u(300); w.str("btn"); w.u(1); w.str("2p"); w.b(false); w.str("down"); w.b(false);
    auto m = importAny(w.s, &err);
    REQUIRE(m.has_value());
    CHECK(m->source == "gdr");
    REQUIRE(m->events.size() == 2);
    CHECK(m->events[1].tick == 1200);
}

TEST(recording_import_frame_list) {
    std::string text = "# exported frames\nfps 120\n10 1\n13 0\n40, 1, 1\n50 1 0 0\n";
    std::string err;
    auto r = importFrameList(text, 240, &err);
    REQUIRE(r.has_value());
    REQUIRE(r->events.size() == 4);
    CHECK(r->events[0].tick == 20);
    CHECK(r->events[2].player2);
    CHECK(!r->events[3].press);
    CHECK(!importFrameList("10 x\n", 240, &err).has_value());
}

TEST(recording_practice_stitching) {
    Stitcher st;
    st.begin("L", 1, 0, 0);
    auto ev = [](int t, bool press) { RecEvent e; e.tick = t; e.time = t / 240.0; e.press = press; return e; };
    st.onButton(ev(100, true));
    st.onButton(ev(105, false));
    st.onCheckpoint(200);
    st.onButton(ev(250, true));      // this attempt dies
    st.onButton(ev(256, false));
    st.onDeath(300);
    st.onRespawn(200, false, false); // back at the checkpoint
    st.onButton(ev(262, true));
    st.onButton(ev(268, false));
    st.onButton(ev(390, true));      // held through the next checkpoint
    st.onCheckpoint(400);
    st.onButton(ev(420, false));
    st.onDeath(450);
    st.onRespawn(400, false, false); // released after respawning: a release is inserted at 400
    st.onButton(ev(500, true));
    st.onButton(ev(505, false));
    st.onComplete(600, 2.5);
    st.onButton(ev(700, true));      // after completion: ignored
    Recording r = st.result();
    CHECK(r.completed);
    CHECK(st.attempts() == 3);
    std::vector<std::pair<int, bool>> got;
    for (auto& e : r.events) got.push_back({e.tick, e.press});
    std::vector<std::pair<int, bool>> want{{100, true}, {105, false}, {262, true}, {268, false}, {390, true}, {400, false}, {500, true}, {505, false}};
    CHECK_MSG(got == want, "stitched %zu events", got.size());
    // a full restart discards everything
    Stitcher s2;
    s2.begin("L", 1, 0, 0);
    s2.onButton(ev(10, true));
    s2.onRestart();
    s2.onButton(ev(20, true));
    s2.onButton(ev(25, false));
    CHECK(s2.result().events.size() == 2);
    CHECK(s2.result().events[0].tick == 20);
}

TEST(recording_replays_in_the_simulator) {
    // a "recorded" successful run of the test pattern (made by the bot) survives format round
    // trips and replays to the end in the simulator
    level::Level L;
    L.settings.fixGravityBug = true;
    L.objects = gen::makeTestPattern(0);
    phys::World W(L);
    auto solved = bot::autoSolve(W, {});
    REQUIRE(solved.solved);
    Stitcher st;
    st.begin("pattern", 0, 0, 0);
    for (auto& e : solved.inputs.events) {
        RecEvent re;
        re.tick = e.tick;
        re.time = e.tick / 240.0;
        re.press = e.press;
        st.onButton(re);
    }
    st.onComplete(solved.inputs.lastTick() + 1000, 10);
    Recording r = st.result();
    auto back = Recording::deserialize(r.serialize());
    REQUIRE(back.has_value());
    auto track = back->toInputTrack();
    CHECK(bot::isBeatable(W, track));
    // the frame list export of the same run (60 fps frames are too coarse: use 240)
    std::string frames = "fps 240\n";
    for (auto& e : track.events) frames += std::to_string(e.tick) + " " + (e.press ? "1" : "0") + "\n";
    auto fl = importFrameList(frames);
    REQUIRE(fl.has_value());
    CHECK(bot::isBeatable(W, fl->toInputTrack()));
}
