#pragma once
// Minimal JSON value with a strict parser and a pretty/compact writer. Used for presets,
// debug exports of recordings, reports and model metadata. Object key order is preserved.
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace al {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : m_type(Type::Bool), m_bool(b) {}
    Json(int v) : m_type(Type::Number), m_num(v) {}
    Json(int64_t v) : m_type(Type::Number), m_num(double(v)) {}
    Json(uint32_t v) : m_type(Type::Number), m_num(double(v)) {}
    Json(uint64_t v) : m_type(Type::Number), m_num(double(v)) {}
    Json(double v) : m_type(Type::Number), m_num(v) {}
    Json(float v) : m_type(Type::Number), m_num(v) {}
    Json(const char* s) : m_type(Type::String), m_str(s) {}
    Json(std::string s) : m_type(Type::String), m_str(std::move(s)) {}
    Json(std::string_view s) : m_type(Type::String), m_str(s) {}

    static Json array() { Json j; j.m_type = Type::Array; return j; }
    static Json object() { Json j; j.m_type = Type::Object; return j; }

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isString() const { return m_type == Type::String; }
    bool isArray() const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    bool asBool(bool def = false) const { return m_type == Type::Bool ? m_bool : (m_type == Type::Number ? m_num != 0 : def); }
    double asNumber(double def = 0) const { return m_type == Type::Number ? m_num : (m_type == Type::Bool ? (m_bool ? 1 : 0) : def); }
    int asInt(int def = 0) const { return m_type == Type::Number ? int(m_num) : def; }
    const std::string& asString() const { static const std::string empty; return m_type == Type::String ? m_str : empty; }
    std::string asString(const std::string& def) const { return m_type == Type::String ? m_str : def; }

    // Array access
    size_t size() const { return m_type == Type::Array ? m_arr.size() : (m_type == Type::Object ? m_obj.size() : 0); }
    const Json& operator[](size_t i) const { static const Json nul; return (m_type == Type::Array && i < m_arr.size()) ? m_arr[i] : nul; }
    Json& push(Json v) { if (m_type != Type::Array) { *this = array(); } m_arr.push_back(std::move(v)); return m_arr.back(); }
    const std::vector<Json>& items() const { return m_arr; }

    // Object access
    bool has(std::string_view k) const;
    const Json& operator[](std::string_view k) const;
    Json& operator[](std::string_view k); // creates (converts to object if needed)
    Json& set(std::string_view k, Json v) { (*this)[k] = std::move(v); return *this; }
    const std::vector<std::pair<std::string, Json>>& members() const { return m_obj; }

    double get(std::string_view k, double def) const { const Json& j = (*this)[k]; return j.isNumber() ? j.m_num : def; }
    int get(std::string_view k, int def) const { const Json& j = (*this)[k]; return j.isNumber() ? int(j.m_num) : def; }
    bool get(std::string_view k, bool def) const { const Json& j = (*this)[k]; return j.isBool() ? j.m_bool : (j.isNumber() ? j.m_num != 0 : def); }
    std::string get(std::string_view k, const char* def) const { const Json& j = (*this)[k]; return j.isString() ? j.m_str : std::string(def); }

    std::string dump(int indent = -1) const;
    static std::optional<Json> parse(std::string_view text, std::string* error = nullptr);

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type m_type = Type::Null;
    bool m_bool = false;
    double m_num = 0;
    std::string m_str;
    std::vector<Json> m_arr;
    std::vector<std::pair<std::string, Json>> m_obj;
};

} // namespace al
