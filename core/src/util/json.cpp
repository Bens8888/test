#include <al/util/json.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace al {

bool Json::has(std::string_view k) const {
    if (m_type != Type::Object) return false;
    for (auto& [key, v] : m_obj) if (key == k) return true;
    return false;
}

const Json& Json::operator[](std::string_view k) const {
    static const Json nul;
    if (m_type != Type::Object) return nul;
    for (auto& [key, v] : m_obj) if (key == k) return v;
    return nul;
}

Json& Json::operator[](std::string_view k) {
    if (m_type != Type::Object) *this = object();
    for (auto& [key, v] : m_obj) if (key == k) return v;
    m_obj.emplace_back(std::string(k), Json());
    return m_obj.back().second;
}

static void escapeStr(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += char(c);
                }
        }
    }
    out += '"';
}

void Json::dumpTo(std::string& out, int indent, int depth) const {
    auto nl = [&](int d) {
        if (indent < 0) return;
        out += '\n';
        out.append(size_t(indent * d), ' ');
    };
    switch (m_type) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += m_bool ? "true" : "false"; break;
        case Type::Number: {
            if (!std::isfinite(m_num)) { out += "0"; break; }
            double r = std::round(m_num);
            char buf[40];
            if (r == m_num && std::fabs(r) < 9e15) std::snprintf(buf, sizeof(buf), "%lld", (long long)r);
            else std::snprintf(buf, sizeof(buf), "%.9g", m_num);
            out += buf;
            break;
        }
        case Type::String: escapeStr(out, m_str); break;
        case Type::Array: {
            out += '[';
            if (m_arr.empty()) { out += ']'; break; }
            for (size_t i = 0; i < m_arr.size(); ++i) {
                if (i) out += ',';
                nl(depth + 1);
                m_arr[i].dumpTo(out, indent, depth + 1);
            }
            nl(depth);
            out += ']';
            break;
        }
        case Type::Object: {
            out += '{';
            if (m_obj.empty()) { out += '}'; break; }
            for (size_t i = 0; i < m_obj.size(); ++i) {
                if (i) out += ',';
                nl(depth + 1);
                escapeStr(out, m_obj[i].first);
                out += indent >= 0 ? ": " : ":";
                m_obj[i].second.dumpTo(out, indent, depth + 1);
            }
            nl(depth);
            out += '}';
            break;
        }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

namespace {
struct Parser {
    std::string_view s;
    size_t i = 0;
    std::string err;
    int depth = 0;

    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i; }
    bool fail(const char* m) { if (err.empty()) { err = m; err += " at offset " + std::to_string(i); } return false; }

    bool parseString(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail("expected string");
        ++i;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c == '\\') {
                if (i >= s.size()) return fail("bad escape");
                char e = s[i++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (i + 4 > s.size()) return fail("bad unicode escape");
                        unsigned cp = std::strtoul(std::string(s.substr(i, 4)).c_str(), nullptr, 16);
                        i += 4;
                        if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                            unsigned lo = std::strtoul(std::string(s.substr(i + 2, 4)).c_str(), nullptr, 16);
                            i += 6;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        if (cp < 0x80) out += char(cp);
                        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
                        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
                        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
                        break;
                    }
                    default: return fail("bad escape");
                }
            } else {
                out += c;
            }
        }
        return fail("unterminated string");
    }

    bool parseValue(Json& out) {
        if (++depth > 256) return fail("nesting too deep");
        ws();
        if (i >= s.size()) return fail("unexpected end");
        char c = s[i];
        bool ok = true;
        if (c == '{') {
            ++i;
            out = Json::object();
            ws();
            if (i < s.size() && s[i] == '}') { ++i; }
            else {
                for (;;) {
                    ws();
                    std::string key;
                    if (!parseString(key)) { ok = false; break; }
                    ws();
                    if (i >= s.size() || s[i] != ':') { ok = fail("expected ':'"); break; }
                    ++i;
                    Json v;
                    if (!parseValue(v)) { ok = false; break; }
                    out[key] = std::move(v);
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == '}') { ++i; break; }
                    ok = fail("expected ',' or '}'");
                    break;
                }
            }
        } else if (c == '[') {
            ++i;
            out = Json::array();
            ws();
            if (i < s.size() && s[i] == ']') { ++i; }
            else {
                for (;;) {
                    Json v;
                    if (!parseValue(v)) { ok = false; break; }
                    out.push(std::move(v));
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == ']') { ++i; break; }
                    ok = fail("expected ',' or ']'");
                    break;
                }
            }
        } else if (c == '"') {
            std::string str;
            ok = parseString(str);
            out = Json(std::move(str));
        } else if (s.substr(i, 4) == "true") { i += 4; out = Json(true); }
        else if (s.substr(i, 5) == "false") { i += 5; out = Json(false); }
        else if (s.substr(i, 4) == "null") { i += 4; out = Json(); }
        else if (c == '-' || (c >= '0' && c <= '9')) {
            size_t st = i;
            ++i;
            while (i < s.size() && (std::strchr("0123456789.eE+-", s[i]) != nullptr)) ++i;
            std::string num(s.substr(st, i - st));
            char* end = nullptr;
            double v = std::strtod(num.c_str(), &end);
            if (end == num.c_str()) ok = fail("bad number");
            out = Json(v);
        } else {
            ok = fail("unexpected character");
        }
        --depth;
        return ok;
    }
};
} // namespace

std::optional<Json> Json::parse(std::string_view text, std::string* error) {
    Parser p{text};
    Json out;
    if (!p.parseValue(out)) { if (error) *error = p.err; return std::nullopt; }
    p.ws();
    if (p.i != text.size()) { if (error) *error = "trailing characters"; return std::nullopt; }
    return out;
}

} // namespace al
