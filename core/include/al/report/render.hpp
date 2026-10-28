#pragma once
// Software renderer for level previews (gallery images, debugging): objects as simple shapes,
// the bot's path as a line, clicks as markers. Output: PNG bytes.
#include <al/bot/bot.hpp>
#include <al/level/level.hpp>

#include <string>
#include <vector>

namespace al::report {

struct RenderOptions {
    float x0 = 0, x1 = -1;      // level x range (x1 < 0 = whole level)
    float y0 = -30, y1 = 480;   // level y range
    float scale = 0.5f;         // pixels per unit
    int maxWidth = 8000;        // split into rows when wider
    bool drawPath = true;
    bool drawDeco = true;
};

// Renders the level (and the path of `inputs`, if given) to PNG bytes.
std::string renderPng(const level::Level& lvl, const bot::InputTrack* inputs, const RenderOptions& opt = {});

// Minimal RGBA image + PNG encoding helpers (also used by the waveform/timeline previews).
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px; // RGBA
    Image(int w_, int h_, uint32_t rgba = 0xff000000);
    void blend(int x, int y, uint32_t rgba, float a = 1.0f);
    void fillRect(float x0, float y0, float x1, float y1, uint32_t rgba, float a = 1.0f);
    void line(float x0, float y0, float x1, float y1, uint32_t rgba, float width = 1.0f);
    void triangle(float ax, float ay, float bx, float by, float cx, float cy, uint32_t rgba);
    void circle(float cx, float cy, float r, uint32_t rgba, float a = 1.0f);
    std::string png() const;
};

} // namespace al::report
