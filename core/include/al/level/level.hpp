#pragma once
// In-memory GD level: header (level start object) + objects, with parsing and serialization of
// the inner level string and .gmd (GDShare plist) files.
#include <al/level/objects.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace al::level {

struct Color { uint8_t r = 255, g = 255, b = 255; };

struct ColorChannel {
    int id = 0;
    Color color;
    bool blending = false;
    float opacity = 1.0f;
    int copyFrom = 0;          // 0 = none
    std::string copyHsv;       // e.g. "0a1a0.6a0a0"
};

struct LevelObject {
    int id = 0;
    double x = 0, y = 0;
    bool flipX = false, flipY = false;
    double rotation = 0;       // degrees, clockwise positive
    double scale = 1;          // key 32
    double scaleX = 1, scaleY = 1; // keys 128/129
    std::vector<int> groups;   // key 57
    int editorLayer = 0;       // key 20
    int colorMain = 0, colorDetail = 0; // keys 21/22 (0 = default)
    int zLayer = 0, zOrder = 0;         // keys 24/25 (0 = default)
    // Any other key (trigger properties etc.), stored verbatim as strings, ordered by key.
    std::map<int, std::string> props;
    // Non-numeric keys (start-pos objects carry kA2/kA3/... inline), in file order.
    std::vector<std::pair<std::string, std::string>> strProps;

    const ObjectInfo& info() const { return objectInfo(id); }
    void setProp(int key, const std::string& value) { props[key] = value; }
    void setProp(int key, double value);
    std::string prop(int key, const std::string& def = "") const;
    double propNum(int key, double def = 0) const;
};

struct LevelSettings {
    Mode startMode = Mode::Cube;
    bool startMini = false;
    int startSpeed = 1;          // speed index (0 = 0.5x ... 4 = 4x)
    bool startDual = false;
    bool startFlipped = false;
    bool twoPlayer = false;
    bool platformer = false;
    double songOffset = 0;       // kA13 seconds
    bool fadeIn = false, fadeOut = false;
    int background = 1, ground = 1, groundLine = 1, font = 0, middleground = 0;
    bool fixGravityBug = true;   // kA32
    bool fixRobotJump = true;    // kA34
    bool fixRadius = true;       // kA39
    std::vector<ColorChannel> colors;
    // Unknown header keys preserved verbatim.
    std::vector<std::pair<std::string, std::string>> extra;
};

struct Level {
    std::string name = "AutoLayout";
    std::string description;
    std::string creator;
    int customSongId = 0;        // k45 (Newgrounds / library id)
    int officialSong = 0;        // k8
    LevelSettings settings;
    std::vector<LevelObject> objects;

    // Inner (decompressed) level string.
    std::string toLevelString() const;
    static std::optional<Level> fromLevelString(const std::string& inner, std::string* error = nullptr);

    // .gmd (GDShare plist) with gzip+base64 level data.
    std::string toGmd() const;
    static std::optional<Level> fromGmd(const std::string& xml, std::string* error = nullptr);

    double lastObjectX() const;
};

// Serialization helpers exposed for tests and the editor bridge.
std::string serializeObject(const LevelObject& o);
std::optional<LevelObject> parseObject(const std::string& s);
std::string serializeHeader(const LevelSettings& s);
LevelSettings parseHeader(const std::string& s);
ColorChannel makeChannel(int id, Color c, bool blending = false);

} // namespace al::level
