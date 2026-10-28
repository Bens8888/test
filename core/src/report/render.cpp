#include <al/report/render.hpp>

#include <al/gen/tiles.hpp>
#include <al/physics/sim.hpp>

#include <miniz.h>

#include <algorithm>
#include <cmath>

namespace al::report {

using level::Kind;

Image::Image(int w_, int h_, uint32_t rgba) : w(w_), h(h_), px(size_t(w_) * size_t(h_) * 4) {
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
        px[i * 4 + 0] = uint8_t(rgba >> 24);
        px[i * 4 + 1] = uint8_t(rgba >> 16);
        px[i * 4 + 2] = uint8_t(rgba >> 8);
        px[i * 4 + 3] = uint8_t(rgba);
    }
}

void Image::blend(int x, int y, uint32_t c, float a) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    a *= float(c & 0xff) / 255.0f;
    size_t i = (size_t(y) * size_t(w) + size_t(x)) * 4;
    uint8_t col[3] = {uint8_t(c >> 24), uint8_t(c >> 16), uint8_t(c >> 8)};
    for (int k = 0; k < 3; ++k) px[i + size_t(k)] = uint8_t(float(px[i + size_t(k)]) * (1 - a) + float(col[k]) * a);
    px[i + 3] = 255;
}

void Image::fillRect(float x0, float y0, float x1, float y1, uint32_t c, float a) {
    int ix0 = std::max(0, int(std::floor(std::min(x0, x1)))), ix1 = std::min(w - 1, int(std::ceil(std::max(x0, x1))));
    int iy0 = std::max(0, int(std::floor(std::min(y0, y1)))), iy1 = std::min(h - 1, int(std::ceil(std::max(y0, y1))));
    for (int y = iy0; y <= iy1; ++y)
        for (int x = ix0; x <= ix1; ++x) blend(x, y, c, a);
}

void Image::line(float x0, float y0, float x1, float y1, uint32_t c, float width) {
    float len = std::hypot(x1 - x0, y1 - y0);
    int n = std::max(1, int(len * 2));
    for (int i = 0; i <= n; ++i) {
        float t = float(i) / float(n);
        float x = x0 + (x1 - x0) * t, y = y0 + (y1 - y0) * t;
        fillRect(x - width * 0.5f, y - width * 0.5f, x + width * 0.5f, y + width * 0.5f, c);
    }
}

void Image::triangle(float ax, float ay, float bx, float by, float cx, float cy, uint32_t c) {
    int minx = std::max(0, int(std::floor(std::min({ax, bx, cx})))), maxx = std::min(w - 1, int(std::ceil(std::max({ax, bx, cx}))));
    int miny = std::max(0, int(std::floor(std::min({ay, by, cy})))), maxy = std::min(h - 1, int(std::ceil(std::max({ay, by, cy}))));
    auto edge = [](float x0, float y0, float x1, float y1, float x, float y) { return (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0); };
    float area = edge(ax, ay, bx, by, cx, cy);
    if (std::fabs(area) < 1e-6f) return;
    for (int y = miny; y <= maxy; ++y)
        for (int x = minx; x <= maxx; ++x) {
            float px_ = float(x) + 0.5f, py_ = float(y) + 0.5f;
            float w0 = edge(bx, by, cx, cy, px_, py_), w1 = edge(cx, cy, ax, ay, px_, py_), w2 = edge(ax, ay, bx, by, px_, py_);
            if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)) blend(x, y, c);
        }
}

void Image::circle(float cx, float cy, float r, uint32_t c, float a) {
    for (int y = int(cy - r - 1); y <= int(cy + r + 1); ++y)
        for (int x = int(cx - r - 1); x <= int(cx + r + 1); ++x) {
            float d = std::hypot(float(x) + 0.5f - cx, float(y) + 0.5f - cy);
            if (d <= r) blend(x, y, c, a * std::clamp(r - d + 0.5f, 0.0f, 1.0f));
        }
}

std::string Image::png() const {
    size_t len = 0;
    void* data = tdefl_write_image_to_png_file_in_memory(px.data(), w, h, 4, &len);
    if (!data) return {};
    std::string out(static_cast<const char*>(data), len);
    mz_free(data);
    return out;
}

std::string renderPng(const level::Level& lvl, const bot::InputTrack* inputs, const RenderOptions& opt) {
    float x0 = opt.x0, x1 = opt.x1;
    if (x1 < 0) {
        x1 = 0;
        for (auto& o : lvl.objects) if (o.info().kind != Kind::Trigger) x1 = std::max(x1, float(o.x));
        x1 += 120;
    }
    const float s = opt.scale;
    const int rowW = std::max(64, std::min(opt.maxWidth, int((x1 - x0) * s) + 1));
    const float rowSpan = float(rowW) / s;
    const int rows = std::max(1, int(std::ceil((x1 - x0) / rowSpan)));
    const int rowH = int((opt.y1 - opt.y0) * s) + 1;
    const int gap = 8;
    Image img(rowW, rows * rowH + (rows - 1) * gap, 0x0a0a20ff);
    auto toPx = [&](float x, float y, int& row, float& px_, float& py_) {
        row = std::clamp(int((x - x0) / rowSpan), 0, rows - 1);
        px_ = (x - x0 - float(row) * rowSpan) * s;
        py_ = float(row * (rowH + gap)) + (opt.y1 - y) * s;
    };
    // editor-like background: blue with a faint 30-unit grid, the ground below y = 0
    for (int r = 0; r < rows; ++r) {
        float top = float(r * (rowH + gap));
        img.fillRect(0, top, float(rowW), top + float(rowH), 0x1f3cb4ff);
        const float firstX = std::ceil((x0 + float(r) * rowSpan) / 30.0f) * 30.0f;
        for (float gx = firstX; gx < x0 + float(r + 1) * rowSpan; gx += 30.0f) {
            float px_ = (gx - x0 - float(r) * rowSpan) * s;
            img.fillRect(px_, top, px_, top + float(rowH), 0x14288cff, 0.6f);
        }
        for (float gy = 0; gy <= opt.y1; gy += 30.0f) {
            float py_ = top + (opt.y1 - gy) * s;
            img.fillRect(0, py_, float(rowW), py_, 0x14288cff, 0.6f);
        }
        float gy = top + (opt.y1 - 0) * s;
        img.fillRect(0, gy, float(rowW), top + float(rowH), 0x2a6ef0ff); // ground
        img.line(0, gy, float(rowW), gy, 0xffffffff, 1.0f);
    }
    auto drawRect = [&](float cx, float cy, float hw, float hh, uint32_t c, float a = 1.0f) {
        int r;
        float px_, py_;
        toPx(cx, cy, r, px_, py_);
        img.fillRect(px_ - hw * s, py_ - hh * s, px_ + hw * s, py_ + hh * s, c, a);
    };
    for (auto& o : lvl.objects) {
        if (o.x < x0 - 60 || o.x > x1 + 60) continue;
        const auto& info = o.info();
        phys::SimObject so;
        bool sim = phys::World::makeSimObject(o, 0, so);
        int r;
        float px_, py_;
        toPx(float(o.x), float(o.y), r, px_, py_);
        switch (info.kind) {
            case Kind::Solid: case Kind::Breakable: {
                const uint8_t edges = gen::drawnEdges(o);
                if (o.id == level::id::OutlineTop) {
                    drawRect(so.x, so.y, std::max(so.hw, 0.75f), std::max(so.hh, 0.75f), 0xffffffff); // a white line
                } else if (edges) {
                    const float hw = 15 * s, l = std::max(1.0f, 1.5f * s);
                    if (edges & gen::EdgeT) img.fillRect(px_ - hw, py_ - hw, px_ + hw, py_ - hw + l, 0xffffffff);
                    if (edges & gen::EdgeB) img.fillRect(px_ - hw, py_ + hw - l, px_ + hw, py_ + hw, 0xffffffff);
                    if (edges & gen::EdgeL) img.fillRect(px_ - hw, py_ - hw, px_ - hw + l, py_ + hw, 0xffffffff);
                    if (edges & gen::EdgeR) img.fillRect(px_ + hw - l, py_ - hw, px_ + hw, py_ + hw, 0xffffffff);
                } else {
                    drawRect(so.x, so.y, so.hw, so.hh, 0xffffffff);
                    drawRect(so.x, so.y, so.hw - 1.5f, so.hh - 1.5f, 0x0c1238ff);
                }
                break;
            }
            case Kind::Hazard: {
                float hw = std::max(so.hw, 12.0f), hh = std::max(so.hh, 12.0f);
                bool down = std::fabs(std::fmod(o.rotation + 360.0, 360.0) - 180) < 1 || o.flipY;
                float base = down ? py_ - hh * s : py_ + hh * s, tip = down ? py_ + hh * s : py_ - hh * s;
                if (o.id == level::id::HalfSpike) { base = down ? py_ - 6 * s : py_ + 6 * s; tip = down ? py_ + 6 * s : py_ - 6 * s; }
                img.triangle(px_ - hw * s, base, px_ + hw * s, base, px_, tip, 0xffffffff);
                const float in = 2.0f * s;
                img.triangle(px_ - hw * s + in * 1.5f, base + (down ? in : -in), px_ + hw * s - in * 1.5f, base + (down ? in : -in), px_, tip + (down ? -in * 2 : in * 2), 0x0a0a28ff);
                break;
            }
            case Kind::Saw:
                img.circle(px_, py_, so.hw * s * 1.15f, 0x050a30ff);
                img.circle(px_, py_, so.hw * s * 0.55f, 0x1840a0ff);
                break;
            case Kind::Orb: {
                uint32_t c = 0xffe040ff;
                switch (o.id) {
                    case level::id::OrbPink: c = 0xff60e0ff; break;
                    case level::id::OrbRed: c = 0xff5020ff; break;
                    case level::id::OrbBlue: c = 0x40e8ffff; break;
                    case level::id::OrbGreen: c = 0x40ff50ff; break;
                    case level::id::OrbBlack: c = 0x181818ff; break;
                    case level::id::OrbDash: c = 0x60ff90ff; break;
                    case level::id::OrbDashPink: c = 0xff50c0ff; break;
                    case level::id::OrbSpider: c = 0xb050ffff; break;
                    default: break;
                }
                img.circle(px_, py_, 15 * s, 0xffffffff);
                img.circle(px_, py_, 12 * s, c);
                break;
            }
            case Kind::Pad: {
                uint32_t c = 0xffe040ff;
                switch (o.id) {
                    case level::id::PadPink: c = 0xff60e0ff; break;
                    case level::id::PadRed: c = 0xff5020ff; break;
                    case level::id::PadBlue: c = 0x40e8ffff; break;
                    case level::id::PadSpider: c = 0xb050ffff; break;
                    default: break;
                }
                drawRect(so.x, so.y, so.hw, std::max(so.hh, 3.0f), c);
                break;
            }
            case Kind::ModePortal: case Kind::GravityPortal: case Kind::SizePortal: case Kind::SpeedPortal: case Kind::DualPortal:
                drawRect(so.x, so.y, so.hw * 0.5f, so.hh, info.kind == Kind::ModePortal ? 0x40ff90ff : (info.kind == Kind::SpeedPortal ? 0xffb030ff : 0x40c0ffff), 0.8f);
                break;
            case Kind::Slope: case Kind::HazardSlope: {
                float hw = so.hw * s, hh = so.hh * s;
                uint32_t c = info.kind == Kind::Slope ? 0x0c1238ff : 0xff4060ff;
                if (o.id == 1338 || o.id == 1339 || o.id == 665 || o.id == 666) {
                    // outline slope: only the white diagonal
                    const bool rising = so.slopeOrient == 0 || so.slopeOrient == 2;
                    if (rising) img.line(px_ - hw, py_ + hh, px_ + hw, py_ - hh, 0xffffffff, std::max(1.0f, 1.5f * s));
                    else img.line(px_ - hw, py_ - hh, px_ + hw, py_ + hh, 0xffffffff, std::max(1.0f, 1.5f * s));
                    break;
                }
                switch (so.slopeOrient) {
                    case 0: img.triangle(px_ - hw, py_ + hh, px_ + hw, py_ + hh, px_ + hw, py_ - hh, c); break;
                    case 1: img.triangle(px_ - hw, py_ + hh, px_ + hw, py_ + hh, px_ - hw, py_ - hh, c); break;
                    case 2: img.triangle(px_ - hw, py_ - hh, px_ + hw, py_ - hh, px_ - hw, py_ + hh, c); break;
                    default: img.triangle(px_ - hw, py_ - hh, px_ + hw, py_ - hh, px_ + hw, py_ + hh, c); break;
                }
                break;
            }
            case Kind::Deco:
                if (opt.drawDeco && o.y < opt.y1) drawRect(float(o.x), float(o.y), 6, 6, 0x9090ff40, 0.4f);
                break;
            default: break;
        }
    }
    if (opt.drawPath && inputs) {
        phys::World W(lvl);
        phys::GameState g = phys::initialState(W);
        size_t ei = 0;
        bool held = false;
        float lx = g.p[0].x, ly = g.p[0].y;
        float lx2 = 0, ly2 = 0;
        bool haveP2 = false;
        int guard = 0;
        while (!g.dead && !g.won && g.p[0].x < x1 && guard++ < 240 * 900) {
            while (ei < inputs->events.size() && inputs->events[ei].tick <= g.tick + 1) {
                held = inputs->events[ei].press;
                if (held) {
                    int r;
                    float px_, py_;
                    toPx(g.p[0].x, g.p[0].y, r, px_, py_);
                    if (g.p[0].x >= x0) img.circle(px_, py_, 3, 0x00ff80ff);
                }
                ++ei;
            }
            phys::step(W, g, held);
            if (g.p[0].x >= x0 && g.p[0].bounded) {
                // the floor and ceiling of a flying/ball/spider stretch (GD draws them as ground)
                int rr;
                float bx, by0, by1;
                toPx(g.p[0].x, g.p[0].floorY, rr, bx, by0);
                toPx(g.p[0].x, g.p[0].ceilY, rr, bx, by1);
                img.fillRect(bx - 1, by0, bx + 1, by0 + 3 * s, 0x2a6ef0ff);
                img.fillRect(bx - 1, by0 - 1, bx + 1, by0, 0xffffffff);
                img.fillRect(bx - 1, by1 - 3 * s, bx + 1, by1, 0x2a6ef0ff);
                img.fillRect(bx - 1, by1, bx + 1, by1 + 1, 0xffffffff);
            }
            if (g.p[0].x >= x0) {
                int r1, r2;
                float ax, ay, bx, by;
                toPx(lx, ly, r1, ax, ay);
                toPx(g.p[0].x, g.p[0].y, r2, bx, by);
                if (r1 == r2) img.line(ax, ay, bx, by, held ? 0x60ffa0ff : 0xffffffd0, 1.5f);
                if (g.dual) {
                    if (haveP2) {
                        toPx(lx2, ly2, r1, ax, ay);
                        toPx(g.p[1].x, g.p[1].y, r2, bx, by);
                        if (r1 == r2) img.line(ax, ay, bx, by, 0xffa0ffd0, 1.5f);
                    }
                    lx2 = g.p[1].x;
                    ly2 = g.p[1].y;
                    haveP2 = true;
                }
            }
            lx = g.p[0].x;
            ly = g.p[0].y;
        }
        if (g.dead) {
            int r;
            float px_, py_;
            toPx(g.p[0].x, g.p[0].y, r, px_, py_);
            img.circle(px_, py_, 6, 0xff0000ff);
        }
    }
    return img.png();
}

} // namespace al::report
