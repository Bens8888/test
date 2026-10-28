#include <al/rec/recording.hpp>
#include <al/rec/taps.hpp>

#include <al/util/codec.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace al::rec {

namespace {

// ---------------------------------------------------------------- little-endian byte helpers

struct Writer {
    std::string out;
    void u8(uint8_t v) { out.push_back(char(v)); }
    void u16(uint16_t v) { for (int i = 0; i < 2; ++i) u8(uint8_t(v >> (8 * i))); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) u8(uint8_t(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) u8(uint8_t(v >> (8 * i))); }
    void f32(float v) { uint32_t b; std::memcpy(&b, &v, 4); u32(b); }
    void f64(double v) { uint64_t b; std::memcpy(&b, &v, 8); u64(b); }
    void var(uint64_t v) { while (v >= 0x80) { u8(uint8_t(v | 0x80)); v >>= 7; } u8(uint8_t(v)); }
    void svar(int64_t v) { var((uint64_t(v) << 1) ^ uint64_t(v >> 63)); }
    void str(const std::string& s) { var(s.size()); out += s; }
};

struct Reader {
    const std::string& in;
    size_t pos = 0;
    bool ok = true;
    explicit Reader(const std::string& s) : in(s) {}
    bool need(size_t n) { if (pos + n > in.size()) ok = false; return ok; }
    uint8_t u8() { if (!need(1)) return 0; return uint8_t(in[pos++]); }
    uint16_t u16() { uint16_t v = 0; for (int i = 0; i < 2; ++i) v |= uint16_t(u8()) << (8 * i); return v; }
    uint32_t u32() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(u8()) << (8 * i); return v; }
    uint64_t u64() { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t(u8()) << (8 * i); return v; }
    float f32() { uint32_t b = u32(); float v; std::memcpy(&v, &b, 4); return v; }
    double f64() { uint64_t b = u64(); double v; std::memcpy(&v, &b, 8); return v; }
    uint64_t var() {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            uint8_t b = u8();
            if (!ok) return 0;
            v |= uint64_t(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return 0;
    }
    int64_t svar() { uint64_t v = var(); return int64_t(v >> 1) ^ -int64_t(v & 1); }
    std::string str() {
        uint64_t n = var();
        if (!ok || n > in.size() - pos) { ok = false; return {}; }
        std::string s = in.substr(pos, size_t(n));
        pos += size_t(n);
        return s;
    }
};

constexpr char kMagic[4] = {'A', 'L', 'R', 'C'};

void writeSnap(Writer& w, const PlayerSnap& p) {
    w.f32(p.x);
    w.f32(p.y);
    w.f32(p.yVel);
    w.u8(uint8_t((p.mode & 0x0f) | (p.speed & 0x07) << 4));
    w.u8(uint8_t((p.mini ? 1 : 0) | (p.upsideDown ? 2 : 0) | (p.onGround ? 4 : 0)));
}

PlayerSnap readSnap(Reader& r) {
    PlayerSnap p;
    p.x = r.f32();
    p.y = r.f32();
    p.yVel = r.f32();
    uint8_t a = r.u8(), b = r.u8();
    p.mode = a & 0x0f;
    p.speed = (a >> 4) & 0x07;
    p.mini = b & 1;
    p.upsideDown = b & 2;
    p.onGround = b & 4;
    return p;
}

Json snapJson(const PlayerSnap& p) {
    Json j = Json::object();
    j["x"] = p.x;
    j["y"] = p.y;
    j["yVel"] = p.yVel;
    j["mode"] = int(p.mode);
    j["speed"] = int(p.speed);
    j["mini"] = p.mini;
    j["upsideDown"] = p.upsideDown;
    j["onGround"] = p.onGround;
    return j;
}

PlayerSnap snapFromJson(const Json& j) {
    PlayerSnap p;
    p.x = float(j.get("x", 0.0));
    p.y = float(j.get("y", 0.0));
    p.yVel = float(j.get("yVel", 0.0));
    p.mode = uint8_t(std::clamp(j.get("mode", 0), 0, 7));
    p.speed = uint8_t(std::clamp(j.get("speed", 1), 0, 4));
    p.mini = j.get("mini", false);
    p.upsideDown = j.get("upsideDown", false);
    p.onGround = j.get("onGround", false);
    return p;
}

void setError(std::string* error, const std::string& msg) { if (error) *error = msg; }

// ---------------------------------------------------------------- MessagePack (subset) -> Json

struct MsgPack {
    const std::string& in;
    size_t pos = 0;
    bool ok = true;
    int depth = 0;
    explicit MsgPack(const std::string& s) : in(s) {}
    uint64_t be(int n) {
        if (pos + size_t(n) > in.size()) { ok = false; return 0; }
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 8) | uint8_t(in[pos++]);
        return v;
    }
    Json str(size_t n) {
        if (pos + n > in.size()) { ok = false; return {}; }
        Json j(in.substr(pos, n));
        pos += n;
        return j;
    }
    Json arr(size_t n) {
        Json a = Json::array();
        for (size_t i = 0; i < n && ok; ++i) a.push(value());
        return a;
    }
    Json map(size_t n) {
        Json o = Json::object();
        for (size_t i = 0; i < n && ok; ++i) {
            Json k = value();
            Json v = value();
            std::string key = k.isString() ? k.asString() : k.dump();
            o[key] = std::move(v);
        }
        return o;
    }
    Json value() {
        if (++depth > 64 || pos >= in.size()) { ok = false; --depth; return {}; }
        uint8_t t = uint8_t(in[pos++]);
        Json r;
        if (t <= 0x7f) r = Json(int(t));
        else if (t >= 0xe0) r = Json(int(int8_t(t)));
        else if ((t & 0xf0) == 0x80) r = map(t & 0x0f);
        else if ((t & 0xf0) == 0x90) r = arr(t & 0x0f);
        else if ((t & 0xe0) == 0xa0) r = str(t & 0x1f);
        else switch (t) {
            case 0xc0: break;
            case 0xc2: r = Json(false); break;
            case 0xc3: r = Json(true); break;
            case 0xc4: r = str(size_t(be(1))); break; // bin as string
            case 0xc5: r = str(size_t(be(2))); break;
            case 0xc6: r = str(size_t(be(4))); break;
            case 0xca: { uint32_t b = uint32_t(be(4)); float f; std::memcpy(&f, &b, 4); r = Json(double(f)); break; }
            case 0xcb: { uint64_t b = be(8); double d; std::memcpy(&d, &b, 8); r = Json(d); break; }
            case 0xcc: r = Json(double(be(1))); break;
            case 0xcd: r = Json(double(be(2))); break;
            case 0xce: r = Json(double(be(4))); break;
            case 0xcf: r = Json(double(be(8))); break;
            case 0xd0: r = Json(double(int8_t(be(1)))); break;
            case 0xd1: r = Json(double(int16_t(be(2)))); break;
            case 0xd2: r = Json(double(int32_t(be(4)))); break;
            case 0xd3: r = Json(double(int64_t(be(8)))); break;
            case 0xd9: r = str(size_t(be(1))); break;
            case 0xda: r = str(size_t(be(2))); break;
            case 0xdb: r = str(size_t(be(4))); break;
            case 0xdc: r = arr(size_t(be(2))); break;
            case 0xdd: r = arr(size_t(be(4))); break;
            case 0xde: r = map(size_t(be(2))); break;
            case 0xdf: r = map(size_t(be(4))); break;
            default: ok = false; break; // ext types are not used by GDR
        }
        --depth;
        return r;
    }
};

std::optional<Recording> fromGdrJson(const Json& j, std::string* error) {
    if (!j.isObject() || !j["inputs"].isArray()) { setError(error, "not a GDR replay (no inputs array)"); return std::nullopt; }
    Recording rec;
    rec.source = "gdr";
    double fps = j.get("framerate", 240.0);
    if (!(fps > 0)) fps = 240.0;
    const Json& lvl = j["level"];
    rec.levelName = lvl.get("name", "");
    rec.levelId = lvl.get("id", 0);
    for (const Json& in : j["inputs"].items()) {
        RecEvent e;
        double frame = in.get("frame", -1.0);
        if (frame < 0) continue;
        e.tick = int32_t(std::lround(frame * 240.0 / fps));
        e.time = frame / fps;
        int btn = in.get("btn", 1);
        e.button = Button(std::clamp(btn, 1, 3));
        e.press = in.get("down", true);
        e.player2 = in.get("2p", false);
        rec.events.push_back(e);
    }
    std::stable_sort(rec.events.begin(), rec.events.end(), [](const RecEvent& a, const RecEvent& b) { return a.tick < b.tick; });
    rec.duration = j.get("duration", rec.events.empty() ? 0.0 : rec.events.back().time);
    rec.completed = true;
    return rec;
}

} // namespace

// ---------------------------------------------------------------- binary format

std::string Recording::serialize() const {
    Writer w;
    w.out.append(kMagic, 4);
    w.u16(kVersion);
    w.u16(uint16_t(completed ? 1 : 0));
    w.f64(tps);
    w.f64(songOffset);
    w.u32(uint32_t(levelId));
    w.u64(levelHash);
    w.f64(duration);
    w.str(levelName);
    w.str(source);
    w.var(events.size());
    int32_t prev = 0;
    for (const auto& e : events) {
        w.svar(int64_t(e.tick) - prev);
        prev = e.tick;
        // time is stored as its offset from tick/tps (usually tiny), as float
        w.f32(float(e.time - double(e.tick) / tps));
        uint8_t flags = uint8_t((uint8_t(e.button) & 3) | (e.press ? 4 : 0) | (e.player2 ? 8 : 0) | (e.dual ? 16 : 0) | (e.orbId ? 32 : 0));
        w.u8(flags);
        writeSnap(w, e.p1);
        if (e.dual) writeSnap(w, e.p2);
        if (e.orbId) w.var(uint64_t(e.orbId));
    }
    w.u32(crc32(w.out));
    return w.out;
}

std::optional<Recording> Recording::deserialize(const std::string& bytes, std::string* error) {
    if (bytes.size() < 4 + 2 + 4 || std::memcmp(bytes.data(), kMagic, 4) != 0) { setError(error, "not an AutoLayout recording"); return std::nullopt; }
    uint32_t stored = 0;
    for (int i = 0; i < 4; ++i) stored |= uint32_t(uint8_t(bytes[bytes.size() - 4 + size_t(i)])) << (8 * i);
    if (crc32(std::string_view(bytes).substr(0, bytes.size() - 4)) != stored) { setError(error, "recording is corrupted (checksum mismatch)"); return std::nullopt; }
    Reader r(bytes);
    r.pos = 4;
    uint16_t version = r.u16();
    if (version == 0 || version > kVersion) {
        setError(error, "recording format version " + std::to_string(version) + " is newer than this AutoLayout (" + std::to_string(kVersion) + ")");
        return std::nullopt;
    }
    Recording rec;
    uint16_t flags = r.u16();
    rec.completed = flags & 1;
    rec.tps = r.f64();
    rec.songOffset = r.f64();
    rec.levelId = int32_t(r.u32());
    rec.levelHash = r.u64();
    rec.duration = r.f64();
    rec.levelName = r.str();
    rec.source = r.str();
    uint64_t n = r.var();
    if (!r.ok || n > bytes.size()) { setError(error, "recording header is truncated"); return std::nullopt; }
    if (!(rec.tps > 0)) rec.tps = 240.0;
    int32_t tick = 0;
    rec.events.reserve(size_t(n));
    for (uint64_t i = 0; i < n && r.ok; ++i) {
        RecEvent e;
        tick += int32_t(r.svar());
        e.tick = tick;
        e.time = double(r.f32()) + double(tick) / rec.tps;
        uint8_t f = r.u8();
        e.button = Button(std::clamp(int(f & 3), 1, 3));
        e.press = f & 4;
        e.player2 = f & 8;
        e.dual = f & 16;
        e.p1 = readSnap(r);
        if (e.dual) e.p2 = readSnap(r);
        if (f & 32) e.orbId = int32_t(r.var());
        rec.events.push_back(e);
    }
    if (!r.ok || r.pos + 4 != bytes.size()) { setError(error, "recording data is truncated"); return std::nullopt; }
    return rec;
}

// ---------------------------------------------------------------- JSON

Json Recording::toJson() const {
    Json j = Json::object();
    j["format"] = "autolayout-recording";
    j["version"] = int(kVersion);
    j["levelName"] = levelName;
    j["levelId"] = levelId;
    j["levelHash"] = std::to_string(levelHash);
    j["tps"] = tps;
    j["songOffset"] = songOffset;
    j["source"] = source;
    j["completed"] = completed;
    j["duration"] = duration;
    Json arr = Json::array();
    for (const auto& e : events) {
        Json o = Json::object();
        o["tick"] = int(e.tick);
        o["time"] = e.time;
        o["button"] = int(e.button);
        o["press"] = e.press;
        if (e.player2) o["player2"] = true;
        if (e.dual) o["dual"] = true;
        o["p1"] = snapJson(e.p1);
        if (e.dual) o["p2"] = snapJson(e.p2);
        if (e.orbId) o["orbId"] = int(e.orbId);
        arr.push(std::move(o));
    }
    j["events"] = std::move(arr);
    return j;
}

std::optional<Recording> Recording::fromJson(const Json& j, std::string* error) {
    if (!j.isObject() || j.get("format", "") != std::string("autolayout-recording")) { setError(error, "not an AutoLayout JSON recording"); return std::nullopt; }
    int version = j.get("version", 0);
    if (version <= 0 || version > kVersion) { setError(error, "recording JSON version " + std::to_string(version) + " is not supported"); return std::nullopt; }
    Recording rec;
    rec.levelName = j.get("levelName", "");
    rec.levelId = j.get("levelId", 0);
    rec.levelHash = std::strtoull(j.get("levelHash", "0").c_str(), nullptr, 10);
    rec.tps = j.get("tps", 240.0);
    if (!(rec.tps > 0)) rec.tps = 240.0;
    rec.songOffset = j.get("songOffset", 0.0);
    rec.source = j.get("source", "json");
    rec.completed = j.get("completed", false);
    rec.duration = j.get("duration", 0.0);
    for (const Json& o : j["events"].items()) {
        RecEvent e;
        e.tick = o.get("tick", 0);
        e.time = o.get("time", double(e.tick) / rec.tps);
        e.button = Button(std::clamp(o.get("button", 1), 1, 3));
        e.press = o.get("press", true);
        e.player2 = o.get("player2", false);
        e.dual = o.get("dual", false);
        e.p1 = snapFromJson(o["p1"]);
        if (e.dual) e.p2 = snapFromJson(o["p2"]);
        e.orbId = o.get("orbId", 0);
        rec.events.push_back(e);
    }
    return rec;
}

// ---------------------------------------------------------------- conversions

bot::InputTrack Recording::toInputTrack(bool player2) const {
    bot::InputTrack t;
    for (const auto& e : events) {
        if (e.button != Button::Jump || e.player2 != player2) continue;
        int tick = tps == 240.0 ? e.tick : int(std::lround(double(e.tick) * 240.0 / tps));
        t.add(tick, e.press);
    }
    t.normalize();
    return t;
}

std::vector<double> Recording::pressTimes(bool player2) const {
    std::vector<double> out;
    for (const auto& e : events)
        if (e.button == Button::Jump && e.player2 == player2 && e.press) out.push_back(songOffset + e.time);
    return out;
}

// ---------------------------------------------------------------- imports

std::optional<Recording> importGdr(const std::string& bytes, std::string* error) {
    size_t first = bytes.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && bytes[first] == '{') {
        auto j = Json::parse(bytes, error);
        if (!j) return std::nullopt;
        return fromGdrJson(*j, error);
    }
    MsgPack mp(bytes);
    Json j = mp.value();
    if (!mp.ok) { setError(error, "not a GDR replay (invalid MessagePack)"); return std::nullopt; }
    return fromGdrJson(j, error);
}

std::optional<Recording> importFrameList(const std::string& text, double fps, std::string* error) {
    Recording rec;
    rec.source = "frames";
    rec.completed = true;
    struct Raw { double frame; int btn; bool down; bool p2; };
    std::vector<Raw> raw;
    size_t lineNo = 0;
    for (const auto& line0 : split(text, '\n')) {
        ++lineNo;
        std::string line(trim(line0));
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = std::string(trim(std::string_view(line).substr(0, hash)));
        if (line.empty()) continue;
        for (char& ch : line) if (ch == ',' || ch == ';' || ch == '\t') ch = ' ';
        std::vector<std::string> tok;
        for (const auto& t : split(line, ' ')) if (!t.empty()) tok.push_back(std::string(t));
        if (tok.empty()) continue;
        if (tok[0] == "fps" || tok[0] == "tps" || tok[0] == "framerate") {
            double v = 0;
            if (tok.size() >= 2 && parseDouble(tok[1], v) && v > 0) fps = v;
            continue;
        }
        auto num = [](const std::string& s, bool& ok) {
            if (s == "true" || s == "down" || s == "press") return 1.0;
            if (s == "false" || s == "up" || s == "release") return 0.0;
            char* end = nullptr;
            double v = std::strtod(s.c_str(), &end);
            ok = ok && end && *end == 0;
            return v;
        };
        bool ok = true;
        Raw r{num(tok[0], ok), 1, true, false};
        if (tok.size() == 2) r.down = num(tok[1], ok) != 0;
        else if (tok.size() == 3) { r.down = num(tok[1], ok) != 0; r.p2 = num(tok[2], ok) != 0; }
        else if (tok.size() >= 4) { r.btn = int(num(tok[1], ok)); r.down = num(tok[2], ok) != 0; r.p2 = num(tok[3], ok) != 0; }
        else ok = false;
        if (!ok || r.frame < 0) { setError(error, "frame list: cannot read line " + std::to_string(lineNo) + ": " + std::string(line0)); return std::nullopt; }
        raw.push_back(r);
    }
    if (!(fps > 0)) fps = 240.0;
    for (const auto& r : raw) {
        RecEvent e;
        e.tick = int32_t(std::lround(r.frame * 240.0 / fps));
        e.time = r.frame / fps;
        e.button = Button(std::clamp(r.btn, 1, 3));
        e.press = r.down;
        e.player2 = r.p2;
        rec.events.push_back(e);
    }
    std::stable_sort(rec.events.begin(), rec.events.end(), [](const RecEvent& a, const RecEvent& b) { return a.tick < b.tick; });
    rec.duration = rec.events.empty() ? 0.0 : rec.events.back().time;
    if (rec.events.empty()) { setError(error, "frame list contains no inputs"); return std::nullopt; }
    return rec;
}

std::optional<Recording> importAny(const std::string& bytes, std::string* error) {
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), kMagic, 4) == 0) return Recording::deserialize(bytes, error);
    size_t first = bytes.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { setError(error, "empty file"); return std::nullopt; }
    if (bytes[first] == '{') {
        auto j = Json::parse(bytes, error);
        if (!j) return std::nullopt;
        if (j->get("format", "") == std::string("autolayout-recording")) return Recording::fromJson(*j, error);
        if (j->get("format", "") == std::string("autolayout-taps")) {
            auto t = TapTrack::fromJson(*j);
            if (!t || t->empty()) { setError(error, "the tap track has no taps"); return std::nullopt; }
            return t->toRecording();
        }
        return fromGdrJson(*j, error);
    }
    uint8_t c0 = uint8_t(bytes[first]);
    if ((c0 & 0xf0) == 0x80 || c0 == 0xde || c0 == 0xdf) {
        auto r = importGdr(bytes, error);
        if (r) return r;
    }
    return importFrameList(bytes, 240.0, error);
}

// ---------------------------------------------------------------- practice stitching

void Stitcher::begin(const std::string& levelName, int32_t levelId, uint64_t levelHash, double songOffset) {
    m_meta = Recording{};
    m_meta.levelName = levelName;
    m_meta.levelId = levelId;
    m_meta.levelHash = levelHash;
    m_meta.songOffset = songOffset;
    m_events.clear();
    m_checkpoints.clear();
    m_active = true;
    m_completed = false;
    m_endTick = 0;
    m_endTime = 0;
    m_attempts = 1;
}

void Stitcher::onButton(const RecEvent& e) {
    if (!m_active || m_completed) return;
    // events arrive in order; anything "from the future" of a respawn was truncated already
    if (!m_events.empty() && e.tick < m_events.back().tick) truncateAfter(e.tick - 1);
    m_events.push_back(e);
}

void Stitcher::onOrb(int32_t tick, int32_t objectId) {
    if (!m_active || m_completed) return;
    for (auto it = m_events.rbegin(); it != m_events.rend(); ++it) {
        if (tick - it->tick > 12) break;
        if (it->press && it->button == Button::Jump && it->tick <= tick) { if (!it->orbId) it->orbId = objectId; break; }
    }
}

void Stitcher::onCheckpoint(int32_t tick) {
    if (!m_active || m_completed) return;
    m_checkpoints.push_back(tick);
}

void Stitcher::onCheckpointRemoved() {
    if (!m_checkpoints.empty()) m_checkpoints.pop_back();
}

bool Stitcher::heldAfter(bool p2) const {
    for (auto it = m_events.rbegin(); it != m_events.rend(); ++it)
        if (it->button == Button::Jump && it->player2 == p2) return it->press;
    return false;
}

void Stitcher::truncateAfter(int32_t tick) {
    m_events.erase(std::remove_if(m_events.begin(), m_events.end(), [&](const RecEvent& e) { return e.tick > tick; }), m_events.end());
}

void Stitcher::onRespawn(int32_t tick, bool holdingP1, bool holdingP2) {
    if (!m_active || m_completed) return;
    ++m_attempts;
    int32_t cp = m_checkpoints.empty() ? 0 : m_checkpoints.back();
    if (m_checkpoints.empty()) { m_events.clear(); }
    else truncateAfter(cp);
    // the kept path must continue with the button state the player actually has now
    const int32_t at = std::max(cp, tick);
    for (bool p2 : {false, true}) {
        bool want = p2 ? holdingP2 : holdingP1;
        if (heldAfter(p2) != want) {
            RecEvent e;
            e.tick = at;
            e.time = double(at) / 240.0;
            e.press = want;
            e.player2 = p2;
            m_events.push_back(e);
        }
    }
}

void Stitcher::onRestart() {
    if (!m_active || m_completed) return;
    ++m_attempts;
    m_events.clear();
    m_checkpoints.clear();
}

void Stitcher::onDeath(int32_t tick) {
    (void)tick; // the respawn (or restart) that follows decides what is discarded
}

void Stitcher::onComplete(int32_t tick, double time) {
    if (!m_active || m_completed) return;
    truncateAfter(tick);
    m_completed = true;
    m_endTick = tick;
    m_endTime = time;
}

Recording Stitcher::result() const {
    Recording r = m_meta;
    r.events = m_events;
    r.completed = m_completed;
    r.duration = m_completed ? m_endTime : (m_events.empty() ? 0.0 : m_events.back().time);
    return r;
}

} // namespace al::rec
