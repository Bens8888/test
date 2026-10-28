#include <al/level/level.hpp>
#include <al/util/codec.hpp>

#include <algorithm>
#include <cstring>

namespace al::level {

void LevelObject::setProp(int key, double value) { props[key] = fmtNum(value); }

std::string LevelObject::prop(int key, const std::string& def) const {
    auto it = props.find(key);
    return it == props.end() ? def : it->second;
}

double LevelObject::propNum(int key, double def) const {
    auto it = props.find(key);
    double v;
    if (it == props.end() || !parseDouble(it->second, v)) return def;
    return v;
}

ColorChannel makeChannel(int id, Color c, bool blending) {
    ColorChannel ch;
    ch.id = id;
    ch.color = c;
    ch.blending = blending;
    return ch;
}

std::string serializeObject(const LevelObject& o) {
    std::string s;
    s.reserve(48);
    auto kv = [&](int k, const std::string& v) {
        if (!s.empty()) s += ',';
        s += std::to_string(k);
        s += ',';
        s += v;
    };
    kv(1, std::to_string(o.id));
    kv(2, fmtNum(o.x));
    kv(3, fmtNum(o.y));
    if (o.flipX) kv(4, "1");
    if (o.flipY) kv(5, "1");
    if (o.rotation != 0) kv(6, fmtNum(o.rotation));
    if (o.editorLayer != 0) kv(20, std::to_string(o.editorLayer));
    if (o.colorMain != 0) kv(21, std::to_string(o.colorMain));
    if (o.colorDetail != 0) kv(22, std::to_string(o.colorDetail));
    if (o.zLayer != 0) kv(24, std::to_string(o.zLayer));
    if (o.zOrder != 0) kv(25, std::to_string(o.zOrder));
    if (o.scale != 1) kv(32, fmtNum(o.scale));
    if (!o.groups.empty()) {
        std::string g;
        for (size_t i = 0; i < o.groups.size(); ++i) { if (i) g += '.'; g += std::to_string(o.groups[i]); }
        kv(57, g);
    }
    if (o.scaleX != 1) kv(128, fmtNum(o.scaleX));
    if (o.scaleY != 1) kv(129, fmtNum(o.scaleY));
    for (auto& [k, v] : o.props) {
        if (k == 1 || k == 2 || k == 3 || k == 4 || k == 5 || k == 6 || k == 20 || k == 21 || k == 22 || k == 24 || k == 25 || k == 32 ||
            k == 57 || k == 128 || k == 129)
            continue;
        kv(k, v);
    }
    for (auto& [k, v] : o.strProps) { s += ','; s += k; s += ','; s += v; }
    return s;
}

std::optional<LevelObject> parseObject(const std::string& s) {
    auto parts = split(s, ',');
    if (parts.size() < 2) return std::nullopt;
    LevelObject o;
    bool haveId = false;
    for (size_t i = 0; i + 1 < parts.size(); i += 2) {
        int k;
        if (!parseInt(parts[i], k)) {
            // string keys inside objects (start-pos kA keys)
            o.strProps.emplace_back(std::string(parts[i]), std::string(parts[i + 1]));
            continue;
        }
        std::string_view v = parts[i + 1];
        double d = 0;
        int n = 0;
        switch (k) {
            case 1: if (parseInt(v, n)) { o.id = n; haveId = true; } break;
            case 2: if (parseDouble(v, d)) o.x = d; break;
            case 3: if (parseDouble(v, d)) o.y = d; break;
            case 4: o.flipX = v == "1"; break;
            case 5: o.flipY = v == "1"; break;
            case 6: if (parseDouble(v, d)) o.rotation = d; break;
            case 20: if (parseInt(v, n)) o.editorLayer = n; break;
            case 21: if (parseInt(v, n)) o.colorMain = n; break;
            case 22: if (parseInt(v, n)) o.colorDetail = n; break;
            case 24: if (parseInt(v, n)) o.zLayer = n; break;
            case 25: if (parseInt(v, n)) o.zOrder = n; break;
            case 32: if (parseDouble(v, d)) o.scale = d; break;
            case 57: {
                for (auto g : split(v, '.')) if (parseInt(g, n)) o.groups.push_back(n);
                break;
            }
            case 128: if (parseDouble(v, d)) o.scaleX = d; break;
            case 129: if (parseDouble(v, d)) o.scaleY = d; break;
            default: o.props[k] = std::string(v); break;
        }
    }
    if (!haveId) return std::nullopt;
    return o;
}

static std::string channelString(const ColorChannel& c) {
    std::string s;
    s += "1_" + std::to_string(c.color.r) + "_2_" + std::to_string(c.color.g) + "_3_" + std::to_string(c.color.b);
    s += "_11_255_12_255_13_255_4_-1_6_" + std::to_string(c.id);
    if (c.blending) s += "_5_1";
    s += "_7_" + fmtNum(c.opacity) + "_15_1_18_0_8_1";
    if (c.copyFrom > 0) {
        s += "_9_" + std::to_string(c.copyFrom);
        if (!c.copyHsv.empty()) s += "_10_" + c.copyHsv;
    }
    return s;
}

std::string serializeHeader(const LevelSettings& st) {
    std::string s;
    auto kv = [&](const std::string& k, const std::string& v) {
        if (!s.empty()) s += ',';
        s += k;
        s += ',';
        s += v;
    };
    if (!st.colors.empty()) {
        std::string cs;
        for (auto& c : st.colors) { cs += channelString(c); cs += '|'; }
        kv("kS38", cs);
    }
    kv("kA13", fmtNum(st.songOffset));
    kv("kA15", st.fadeIn ? "1" : "0");
    kv("kA16", st.fadeOut ? "1" : "0");
    kv("kA14", "");
    kv("kA6", std::to_string(st.background));
    kv("kA7", std::to_string(st.ground));
    kv("kA25", std::to_string(st.middleground));
    kv("kA17", std::to_string(st.groundLine));
    kv("kA18", std::to_string(st.font));
    kv("kS39", "0");
    kv("kA2", std::to_string(int(st.startMode)));
    kv("kA3", st.startMini ? "1" : "0");
    kv("kA8", st.startDual ? "1" : "0");
    kv("kA4", std::to_string(speedToKA4(st.startSpeed)));
    kv("kA9", "0");
    kv("kA10", st.twoPlayer ? "1" : "0");
    kv("kA22", st.platformer ? "1" : "0");
    kv("kA20", "0");
    kv("kA28", "0");
    kv("kA29", "0");
    kv("kA11", st.startFlipped ? "1" : "0");
    kv("kA32", st.fixGravityBug ? "1" : "0");
    kv("kA33", "1");
    kv("kA34", st.fixRobotJump ? "1" : "0");
    kv("kA37", "1");
    kv("kA38", "1");
    kv("kA39", st.fixRadius ? "1" : "0");
    kv("kA40", "1");
    kv("kA41", "1");
    kv("kA42", "1");
    for (auto& [k, v] : st.extra) kv(k, v);
    return s;
}

static ColorChannel parseChannel(std::string_view s) {
    ColorChannel c;
    auto p = split(s, '_');
    for (size_t i = 0; i + 1 < p.size(); i += 2) {
        int k, n;
        double d;
        if (!parseInt(p[i], k)) continue;
        auto v = p[i + 1];
        switch (k) {
            case 1: if (parseInt(v, n)) c.color.r = uint8_t(std::clamp(n, 0, 255)); break;
            case 2: if (parseInt(v, n)) c.color.g = uint8_t(std::clamp(n, 0, 255)); break;
            case 3: if (parseInt(v, n)) c.color.b = uint8_t(std::clamp(n, 0, 255)); break;
            case 5: c.blending = v == "1"; break;
            case 6: if (parseInt(v, n)) c.id = n; break;
            case 7: if (parseDouble(v, d)) c.opacity = float(d); break;
            case 9: if (parseInt(v, n)) c.copyFrom = n; break;
            case 10: c.copyHsv = std::string(v); break;
            default: break;
        }
    }
    return c;
}

LevelSettings parseHeader(const std::string& s) {
    LevelSettings st;
    st.fixGravityBug = false; // absent compatibility flags mean legacy behaviour
    st.fixRobotJump = false;
    st.fixRadius = false;
    auto parts = split(s, ',');
    for (size_t i = 0; i + 1 < parts.size(); i += 2) {
        std::string k(parts[i]);
        std::string_view v = parts[i + 1];
        int n = 0;
        double d = 0;
        bool b = v == "1";
        if (k == "kS38") { for (auto ch : split(v, '|')) if (!ch.empty()) st.colors.push_back(parseChannel(ch)); }
        else if (k == "kA2") { if (parseInt(v, n) && n >= 0 && n < int(Mode::Count)) st.startMode = Mode(n); }
        else if (k == "kA3") st.startMini = b;
        else if (k == "kA4") { if (parseInt(v, n)) st.startSpeed = speedFromKA4(n); }
        else if (k == "kA8") st.startDual = b;
        else if (k == "kA10") st.twoPlayer = b;
        else if (k == "kA11") st.startFlipped = b;
        else if (k == "kA22") st.platformer = b;
        else if (k == "kA13") { if (parseDouble(v, d)) st.songOffset = d; }
        else if (k == "kA15") st.fadeIn = b;
        else if (k == "kA16") st.fadeOut = b;
        else if (k == "kA6") { if (parseInt(v, n)) st.background = n; }
        else if (k == "kA7") { if (parseInt(v, n)) st.ground = n; }
        else if (k == "kA17") { if (parseInt(v, n)) st.groundLine = n; }
        else if (k == "kA18") { if (parseInt(v, n)) st.font = n; }
        else if (k == "kA25") { if (parseInt(v, n)) st.middleground = n; }
        else if (k == "kA32") st.fixGravityBug = b;
        else if (k == "kA34") st.fixRobotJump = b;
        else if (k == "kA39") st.fixRadius = b;
        else if (k == "kA14" || k == "kS39" || k == "kA9" || k == "kA20" || k == "kA28" || k == "kA29" || k == "kA33" ||
                 k == "kA37" || k == "kA38" || k == "kA40" || k == "kA41" || k == "kA42") continue; // rewritten on save
        else st.extra.emplace_back(k, std::string(v));
    }
    return st;
}

std::string Level::toLevelString() const {
    std::string s = serializeHeader(settings);
    s += ';';
    for (auto& o : objects) {
        s += serializeObject(o);
        s += ';';
    }
    return s;
}

std::optional<Level> Level::fromLevelString(const std::string& inner, std::string* error) {
    Level L;
    auto segs = split(inner, ';');
    bool first = true;
    for (auto seg : segs) {
        if (seg.empty()) continue;
        if (first) {
            first = false;
            // The header's first key is non-numeric ("kS38", "kA13", ...)
            if (!seg.empty() && (seg[0] == 'k')) {
                L.settings = parseHeader(std::string(seg));
                continue;
            }
        }
        auto o = parseObject(std::string(seg));
        if (o) L.objects.push_back(std::move(*o));
    }
    if (L.objects.empty() && segs.size() < 2) {
        if (error) *error = "empty level string";
        return std::nullopt;
    }
    return L;
}

double Level::lastObjectX() const {
    double m = 0;
    for (auto& o : objects) {
        auto k = o.info().kind;
        if (k == Kind::Trigger || k == Kind::Deco) continue;
        m = std::max(m, o.x);
    }
    return m;
}

std::string Level::toGmd() const {
    std::string x = "<?xml version=\"1.0\"?><plist version=\"1.0\" gjver=\"2.0\"><dict>";
    auto ks = [&](const char* k, const std::string& v) { x += "<k>"; x += k; x += "</k><s>"; x += xmlEscape(v); x += "</s>"; };
    auto ki = [&](const char* k, long long v) { x += "<k>"; x += k; x += "</k><i>"; x += std::to_string(v); x += "</i>"; };
    ki("kCEK", 4);
    ks("k2", name);
    if (!description.empty()) ks("k3", base64Encode(description, true));
    ks("k4", gdCompress(toLevelString()));
    if (!creator.empty()) ks("k5", creator);
    x += "<k>k13</k><t />";
    ki("k21", 2);
    ki("k16", 1);
    if (customSongId > 0) ki("k45", customSongId);
    else ki("k8", officialSong);
    ki("k50", 45);
    x += "</dict></plist>";
    return x;
}

std::optional<Level> Level::fromGmd(const std::string& xmlIn, std::string* error) {
    std::string xml = xmlIn;
    for (char& c : xml) if (c == '\0') c = ' ';
    // Collect top-level <k>key</k><tag>value</tag> pairs (short and long tag names).
    std::map<std::string, std::string> kv;
    size_t pos = 0;
    auto findTag = [&](const char* open, size_t from) { return xml.find(open, from); };
    while (true) {
        size_t k0 = findTag("<k>", pos);
        size_t kk0 = findTag("<key>", pos);
        bool longKey = kk0 != std::string::npos && (k0 == std::string::npos || kk0 < k0);
        size_t start = longKey ? kk0 : k0;
        if (start == std::string::npos) break;
        size_t ks = start + (longKey ? 5 : 3);
        size_t ke = xml.find(longKey ? "</key>" : "</k>", ks);
        if (ke == std::string::npos) break;
        std::string key = xml.substr(ks, ke - ks);
        size_t vp = ke + (longKey ? 6 : 4);
        while (vp < xml.size() && (xml[vp] == ' ' || xml[vp] == '\n' || xml[vp] == '\r' || xml[vp] == '\t')) ++vp;
        if (vp >= xml.size() || xml[vp] != '<') break;
        size_t te = xml.find('>', vp);
        if (te == std::string::npos) break;
        std::string tag = xml.substr(vp + 1, te - vp - 1);
        std::string value;
        if (!tag.empty() && tag.back() == '/') {
            value = "1"; // <t /> or <true/>
            pos = te + 1;
        } else if (tag == "d" || tag == "dict") {
            // nested dict: skip to its end (not needed for level data)
            size_t de = xml.find(tag == "d" ? "</d>" : "</dict>", te);
            pos = de == std::string::npos ? xml.size() : de + 1;
            kv[key] = "";
            continue;
        } else {
            std::string close = "</" + tag + ">";
            size_t ve = xml.find(close, te + 1);
            if (ve == std::string::npos) break;
            value = xmlUnescape(xml.substr(te + 1, ve - te - 1));
            pos = ve + close.size();
        }
        if (!kv.count(key)) kv[key] = value;
    }
    auto it = kv.find("k4");
    if (it == kv.end() || it->second.empty()) { if (error) *error = "no level data (k4)"; return std::nullopt; }
    auto inner = gdDecompress(it->second);
    if (!inner) { if (error) *error = "could not decompress level data"; return std::nullopt; }
    auto L = Level::fromLevelString(*inner, error);
    if (!L) return std::nullopt;
    if (kv.count("k2")) L->name = kv["k2"];
    if (kv.count("k5")) L->creator = kv["k5"];
    if (kv.count("k3")) { auto d = base64Decode(kv["k3"]); if (d) L->description = *d; }
    int n = 0;
    if (kv.count("k45") && parseInt(kv["k45"], n)) L->customSongId = n;
    if (kv.count("k8") && parseInt(kv["k8"], n)) L->officialSong = n;
    return L;
}

} // namespace al::level
