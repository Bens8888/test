#pragma once
// Byte-level codecs used by the level format: base64 (standard and URL-safe), gzip/zlib
// via miniz, CRC32, and small string helpers.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace al {

std::string base64Encode(std::string_view data, bool urlSafe);
// Accepts both alphabets, tolerates missing padding and whitespace.
std::optional<std::string> base64Decode(std::string_view text);

std::string gzipCompress(std::string_view data, int level = 6);
// Auto-detects gzip (1f 8b) and zlib headers; raw deflate as a fallback.
std::optional<std::string> inflateAny(std::string_view data);

uint32_t crc32(std::string_view data);

// GD "compressed string" = base64url(gzip(data)).
std::string gdCompress(std::string_view plain);
// Returns the plain level string; accepts already-plain strings (returned unchanged).
std::optional<std::string> gdDecompress(std::string_view text);

std::vector<std::string_view> split(std::string_view s, char sep);
std::string_view trim(std::string_view s);
bool parseDouble(std::string_view s, double& out);
bool parseInt(std::string_view s, int& out);
std::string fmtNum(double v); // shortest round-trippable-ish form, no trailing zeros
std::string xmlEscape(std::string_view s);
std::string xmlUnescape(std::string_view s);

bool readFile(const std::string& path, std::string& out);
bool writeFile(const std::string& path, std::string_view data);

} // namespace al
