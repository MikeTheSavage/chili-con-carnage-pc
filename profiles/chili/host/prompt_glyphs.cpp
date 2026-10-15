#include "prompt_glyphs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace chili {

namespace {

using Microsoft::WRL::ComPtr;

// The look: the game's menu buttons are near-black glossy discs with a grey rim and a light legend.
constexpr std::uint32_t kBodyTop = 0x40404Au;
constexpr std::uint32_t kBodyBottom = 0x0C0C10u;
constexpr std::uint32_t kRim = 0xE6E6E6u;
constexpr std::uint32_t kLegend = 0xFFFFFFu;
constexpr std::uint32_t kLit = 0xFF3A2Eu;      // the PSP pictures light a D-pad arm and the stick's arrows in red
constexpr std::uint32_t kDim = 0x8E8E98u;
constexpr std::uint32_t kEdge = 0x08080Au;     // the outline twin's edge
// The game's padlock and chili (the pictures '{', '}' and '~' of its text font), as its own texels have them.
constexpr std::uint32_t kLockLight = 0xF8EC93u, kLockGold = 0xE4B831u, kLockDeep = 0xB9800Fu, kLockEdge = 0x3B2604u;
constexpr std::uint32_t kLockShackle = 0xF7E976u, kLockHole = 0x3A1408u;
constexpr std::uint32_t kChiliLight = 0xFF3040u, kChili = 0xE10124u, kChiliDeep = 0x7C0A14u, kChiliShine = 0xFF8A8Au;
constexpr std::uint32_t kChiliStem = 0x44170Eu, kChiliEdge = 0x000000u;
constexpr std::uint32_t kXboxFace[4]{0x86E05Au, 0xFF5E55u, 0x62AEFFu, 0xFFD640u};   // A B X Y
constexpr std::uint32_t kPsFace[4]{0x86BCFFu, 0xFF6E6Eu, 0xF492DCu, 0x62E4B4u};     // Cross Circle Square Triangle

std::string hr_text(const char *what, HRESULT hr) {
    char buffer[112];
    std::snprintf(buffer, sizeof(buffer), "%s (HRESULT 0x%08lX)", what, static_cast<unsigned long>(hr));
    return buffer;
}

} // namespace

struct GlyphPainter::State {
    bool com{};
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IDWriteFontFace> font_face;
    ComPtr<ID2D1StrokeStyle> round;
    DWRITE_FONT_METRICS metrics{};
    std::string font;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush, fill_brush;
    ComPtr<ID2D1GradientStopCollection> body_stops;
    std::uint32_t canvas_width{}, canvas_height{};

    ~State() {
        drop_canvas();
        round.Reset();
        font_face.Reset();
        dwrite.Reset();
        d2d.Reset();
        wic.Reset();
        if (com) CoUninitialize();
    }
    void drop_canvas() {
        body_stops.Reset();
        brush.Reset();
        fill_brush.Reset();
        target.Reset();
        bitmap.Reset();
    }

    // The named family's heaviest upright face up to `weight`, if the system has it.
    bool pick_font(const wchar_t *family, DWRITE_FONT_WEIGHT weight) {
        ComPtr<IDWriteFontCollection> fonts;
        if (FAILED(dwrite->GetSystemFontCollection(&fonts, FALSE))) return false;
        UINT32 index = 0u;
        BOOL exists = FALSE;
        if (family != nullptr) {
            if (FAILED(fonts->FindFamilyName(family, &index, &exists)) || !exists) return false;
        } else if (fonts->GetFontFamilyCount() == 0u) return false;
        ComPtr<IDWriteFontFamily> found;
        if (FAILED(fonts->GetFontFamily(index, &found))) return false;
        ComPtr<IDWriteFont> font_file;
        if (FAILED(found->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font_file)))
            return false;
        ComPtr<IDWriteFontFace> made;
        if (FAILED(font_file->CreateFontFace(&made))) return false;
        font_face = made;
        font_face->GetMetrics(&metrics);
        font.clear();
        ComPtr<IDWriteLocalizedStrings> names;
        if (SUCCEEDED(found->GetFamilyNames(&names)) && names->GetCount() != 0u) {
            wchar_t wide[64]{};
            if (SUCCEEDED(names->GetString(0u, wide, 64u)))
                for (const wchar_t *c = wide; *c; ++c) font.push_back(*c < 128 ? static_cast<char>(*c) : '?');
        }
        font += weight >= DWRITE_FONT_WEIGHT_BOLD ? " Bold" : " Semibold";
        return metrics.designUnitsPerEm != 0u;
    }

    ID2D1SolidColorBrush *solid(std::uint32_t rgb, float alpha = 1.0f) {
        brush->SetColor(D2D1::ColorF(rgb, alpha));
        return brush.Get();
    }
    // A second colour for a call that fills with one and strokes with another.
    ID2D1SolidColorBrush *solid_fill(std::uint32_t rgb) {
        fill_brush->SetColor(D2D1::ColorF(rgb));
        return fill_brush.Get();
    }
    // Where a shape of about this place and size goes: its sides on pixel borders, so that a rim one pixel
    // wide is one pixel and not two grey ones.
    struct Place { float cx, cy, w, h; };
    static Place place(float cx, float cy, float w, float h) noexcept {
        const float rw = std::max(1.0f, std::round(w)), rh = std::max(1.0f, std::round(h));
        const float left = std::round(cx - rw * 0.5f), top = std::round(cy - rh * 0.5f);
        return {left + rw * 0.5f, top + rh * 0.5f, rw, rh};
    }
    // The dark body colour, lighter at `top` than at `bottom`.
    ComPtr<ID2D1LinearGradientBrush> body(float top, float bottom) {
        ComPtr<ID2D1LinearGradientBrush> gradient;
        (void)target->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, top), D2D1::Point2F(0.0f, bottom)),
            body_stops.Get(), &gradient);
        return gradient;
    }
    static float rim_width(float size) noexcept { return size < 50.0f ? std::max(1.0f, std::round(0.06f * size)) : 0.06f * size; }
    static float stroke_width(float size, float share) noexcept { return std::max(1.0f, share * size); }

    void polygon(const D2D1_POINT_2F *points, std::size_t count, ID2D1Brush *fill, ID2D1Brush *stroke = nullptr,
                 float width = 0.0f, bool closed = true) {
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(d2d->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
        sink->BeginFigure(points[0], fill != nullptr ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLines(points + 1, static_cast<UINT32>(count - 1u));
        sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        if (FAILED(sink->Close())) return;
        if (fill != nullptr) target->FillGeometry(path.Get(), fill);
        if (stroke != nullptr) target->DrawGeometry(path.Get(), stroke, width, round.Get());
    }
    void line(float x0, float y0, float x1, float y1, float width, std::uint32_t color) {
        target->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), solid(color), width, round.Get());
    }
    // A filled triangle pointing in `direction` (one of the glyph_* bits), `size` from base to tip.
    void arrow_head(float cx, float cy, float size, std::uint8_t direction, std::uint32_t color) {
        const float half = size * 0.62f, h = size * 0.5f;
        D2D1_POINT_2F p[3];
        switch (direction) {
        case glyph_up:    p[0] = {cx, cy - h}; p[1] = {cx + half, cy + h}; p[2] = {cx - half, cy + h}; break;
        case glyph_down:  p[0] = {cx, cy + h}; p[1] = {cx - half, cy - h}; p[2] = {cx + half, cy - h}; break;
        case glyph_left:  p[0] = {cx - h, cy}; p[1] = {cx + h, cy - half}; p[2] = {cx + h, cy + half}; break;
        default:          p[0] = {cx + h, cy}; p[1] = {cx - h, cy + half}; p[2] = {cx - h, cy - half}; break;
        }
        polygon(p, 3u, solid(color));
    }
    // An arrow with a shaft, `length` long, centred.
    void arrow(float cx, float cy, float length, std::uint8_t direction, float width, std::uint32_t color) {
        const float h = length * 0.5f, head = length * 0.36f;
        float dx = 0.0f, dy = 0.0f;
        if (direction == glyph_up) dy = -1.0f; else if (direction == glyph_down) dy = 1.0f;
        else if (direction == glyph_left) dx = -1.0f; else dx = 1.0f;
        const float tx = cx + dx * h, ty = cy + dy * h;
        line(cx - dx * h, cy - dy * h, tx, ty, width, color);
        const D2D1_POINT_2F p[3]{{tx - dx * head - dy * head, ty - dy * head - dx * head}, {tx, ty},
                                 {tx - dx * head + dy * head, ty - dy * head + dx * head}};
        polygon(p, 3u, nullptr, solid(color), width, false);
    }

    // The outline of `text` with capitals `cap` pixels high, its baseline at y = 0.
    ComPtr<ID2D1PathGeometry> outline(std::string_view text, float cap, D2D1_RECT_F &bounds) {
        UINT32 codes[8]{};
        UINT16 indices[8]{};
        const UINT32 count = static_cast<UINT32>(std::min<std::size_t>(text.size(), 8u));
        if (count == 0u || !font_face) return nullptr;
        for (UINT32 i = 0u; i < count; ++i) codes[i] = static_cast<unsigned char>(text[i]);
        if (FAILED(font_face->GetGlyphIndices(codes, count, indices))) return nullptr;
        const float cap_units = metrics.capHeight != 0u ? static_cast<float>(metrics.capHeight)
                                                        : static_cast<float>(metrics.ascent) * 0.7f;
        const float em = cap * static_cast<float>(metrics.designUnitsPerEm) / cap_units;
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(d2d->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return nullptr;
        const HRESULT hr = font_face->GetGlyphRunOutline(em, indices, nullptr, nullptr, count, FALSE, FALSE, sink.Get());
        if (FAILED(sink->Close()) || FAILED(hr)) return nullptr;
        if (FAILED(path->GetBounds(nullptr, &bounds))) return nullptr;
        return path;
    }
    // `text` centred on (cx, cy) with capitals `cap` high, pressed together and then made smaller when it
    // is wider than `max_width`. On a small canvas the capitals stand on whole pixel rows.
    void label(std::string_view text, float cx, float cy, float cap, float max_width, std::uint32_t color) {
        if (cap < 12.0f) cap = std::max(3.0f, std::round(cap));
        D2D1_RECT_F bounds{};
        const auto path = outline(text, cap, bounds);
        const float width = bounds.right - bounds.left;
        if (!path || !(width > 0.0f)) return;
        float squeeze = 1.0f, scale = 1.0f;
        if (width > max_width) {
            squeeze = std::max(0.66f, max_width / width);
            if (width * squeeze > max_width) scale = max_width / (width * squeeze);
        }
        float baseline = cy + cap * scale * 0.5f, left = cx - width * squeeze * scale * 0.5f;
        if (cap < 12.0f) { baseline = std::round(baseline); left = std::round(left); }
        target->SetTransform(D2D1::Matrix3x2F::Translation(-bounds.left, 0.0f) *
                             D2D1::Matrix3x2F::Scale(squeeze * scale, scale) *
                             D2D1::Matrix3x2F::Translation(left, baseline));
        target->FillGeometry(path.Get(), solid(color));
        target->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    // The dark disc with its light rim. (Callers place it first.)
    void disc(float cx, float cy, float radius, std::uint32_t rim = kRim) {
        const auto fill = body(cy - radius, cy + radius);
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), fill.Get());
        const float width = rim_width(radius * 2.0f);
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius - width * 0.5f, radius - width * 0.5f),
                            solid(rim), width);
    }
    // A dark rounded shape with the light rim.
    void slab(float cx, float cy, float w, float h, float radius, std::uint32_t rim = kRim) {
        const D2D1_RECT_F outer = D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f);
        const auto fill = body(outer.top, outer.bottom);
        target->FillRoundedRectangle(D2D1::RoundedRect(outer, radius, radius), fill.Get());
        const float width = rim_width(std::min(w, h));
        const D2D1_RECT_F inner = D2D1::RectF(outer.left + width * 0.5f, outer.top + width * 0.5f,
                                              outer.right - width * 0.5f, outer.bottom - width * 0.5f);
        const float r = std::max(0.0f, radius - width * 0.5f);
        target->DrawRoundedRectangle(D2D1::RoundedRect(inner, r, r), solid(rim), width);
    }
    // A key cap: the light base shows as rim and as a thicker lip at the bottom. Returns the middle of its face.
    Place key_body(float at_x, float at_y, float about_w, float about_h, std::uint32_t base = kRim) {
        const auto [cx, cy, w, h] = place(at_x, at_y, about_w, about_h);
        const float size = std::min(w, h);
        const float rim = rim_width(size), lip = rim + std::max(1.0f, std::round(0.07f * size));
        const float radius = 0.2f * size;
        const D2D1_RECT_F outer = D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f);
        target->FillRoundedRectangle(D2D1::RoundedRect(outer, radius, radius), solid(base));
        const D2D1_RECT_F inner = D2D1::RectF(outer.left + rim, outer.top + rim, outer.right - rim, outer.bottom - lip);
        const auto fill = body(inner.top, inner.bottom);
        const float r = std::max(0.5f, radius - rim);
        target->FillRoundedRectangle(D2D1::RoundedRect(inner, r, r), fill.Get());
        return {cx, (inner.top + inner.bottom) * 0.5f, w, h};
    }
    void key_sign(const KeyCap &cap, float cx, float cy, float w, float h, std::uint32_t color) {
        const float size = std::min(w, h);
        const float pen = stroke_width(size, 0.085f);
        switch (cap.sign) {
        case KeySign::Text: {
            const std::string_view text(cap.text.data(), ::strnlen(cap.text.data(), cap.text.size()));
            const float room = w - 2.0f * rim_width(size) - std::max(2.0f, 0.2f * size);
            const float share = text.size() <= 1u ? 0.56f : text.size() == 2u ? 0.46f : 0.38f;
            label(text, cx, cy, h * share, room, color);
            break;
        }
        case KeySign::Space: {
            // The mark printed on a space bar: a wide open bracket.
            const float half = w * 0.30f, drop = h * 0.13f, y = cy + h * 0.08f;
            const D2D1_POINT_2F p[4]{{cx - half, y - drop}, {cx - half, y + drop}, {cx + half, y + drop}, {cx + half, y - drop}};
            polygon(p, 4u, nullptr, solid(color), pen, false);
            break;
        }
        case KeySign::Enter: {
            // Down the right side, then left to an arrow head.
            const float half = w * 0.26f, top = cy - h * 0.20f, y = cy + h * 0.10f, head = size * 0.14f;
            const D2D1_POINT_2F p[3]{{cx + half, top}, {cx + half, y}, {cx - half, y}};
            polygon(p, 3u, nullptr, solid(color), pen, false);
            const D2D1_POINT_2F a[3]{{cx - half + head, y - head}, {cx - half, y}, {cx - half + head, y + head}};
            polygon(a, 3u, nullptr, solid(color), pen, false);
            break;
        }
        case KeySign::Shift: {
            // A hollow arrow pointing up.
            const float wide = size * 0.26f, narrow = size * 0.12f, top = cy - size * 0.25f, mid = cy, bottom = cy + size * 0.22f;
            const D2D1_POINT_2F p[7]{{cx, top}, {cx + wide, mid}, {cx + narrow, mid}, {cx + narrow, bottom},
                                     {cx - narrow, bottom}, {cx - narrow, mid}, {cx - wide, mid}};
            polygon(p, 7u, nullptr, solid(color), pen, true);
            break;
        }
        case KeySign::Tab: {
            // An arrow running into a bar.
            const float half = w * 0.27f;
            arrow(cx - pen, cy, half * 2.0f - pen * 2.0f, glyph_right, pen, color);
            line(cx + half, cy - size * 0.17f, cx + half, cy + size * 0.17f, pen, color);
            break;
        }
        case KeySign::Backspace: {
            // The erase mark: a box with a point to the left and a cross in it.
            const float left = cx - w * 0.31f, right = cx + w * 0.29f, half = size * 0.19f, neck = left + size * 0.2f;
            const D2D1_POINT_2F p[5]{{left, cy}, {neck, cy - half}, {right, cy - half}, {right, cy + half}, {neck, cy + half}};
            polygon(p, 5u, nullptr, solid(color), pen, true);
            const float mx = (neck + right) * 0.5f, x = size * 0.085f;
            line(mx - x, cy - x, mx + x, cy + x, pen, color);
            line(mx - x, cy + x, mx + x, cy - x, pen, color);
            break;
        }
        case KeySign::Up: arrow(cx, cy, size * 0.46f, glyph_up, pen, color); break;
        case KeySign::Down: arrow(cx, cy, size * 0.46f, glyph_down, pen, color); break;
        case KeySign::Left: arrow(cx, cy, size * 0.46f, glyph_left, pen, color); break;
        case KeySign::Right: arrow(cx, cy, size * 0.46f, glyph_right, pen, color); break;
        }
    }
    void key(const KeyCap &cap, float cx, float cy, float w, float h) {
        const Place face = key_body(cx, cy, w, h);
        key_sign(cap, face.cx, face.cy, face.w, face.h, kLegend);
    }

    void draw_key(const Glyph &glyph, const GlyphBox &box) {
        const KeyCap &cap = glyph.keys[0];
        const float size = std::min(box.width, box.height);
        const float h = size * 0.94f;
        // One character sits on a square cap; a word or a sign gets the width the box has, up to a key and a half.
        const bool single = cap.sign == KeySign::Text && ::strnlen(cap.text.data(), cap.text.size()) <= 1u;
        const bool arrow_key = cap.sign >= KeySign::Up;
        const float w = single || arrow_key ? h : std::min(box.width, h * 1.5f);
        key(cap, box.x + box.width * 0.5f, box.y + box.height * 0.5f, w, h);
    }
    void draw_cluster(const Glyph &glyph, const GlyphBox &box) {
        // Up in the middle of the top row; left, down, right below.
        const float side = std::floor(std::min(box.width / 3.0f, box.height / 2.0f));
        const float cx = box.x + box.width * 0.5f, cy = box.y + box.height * 0.5f;
        const float gap = side >= 12.0f ? std::round(side * 0.06f) : 0.0f;
        const float cap = side - gap;
        key(glyph.keys[0], cx, cy - side * 0.5f, cap, cap);
        key(glyph.keys[1], cx - side, cy + side * 0.5f, cap, cap);
        key(glyph.keys[2], cx, cy + side * 0.5f, cap, cap);
        key(glyph.keys[3], cx + side, cy + side * 0.5f, cap, cap);
    }
    void draw_unbound(const GlyphBox &box) {
        const float size = std::min(box.width, box.height) * 0.94f;
        const float cx = box.x + box.width * 0.5f, cy = box.y + box.height * 0.5f;
        const Place face = key_body(cx, cy, size, size, kDim);
        line(face.cx - face.w * 0.18f, face.cy, face.cx + face.w * 0.18f, face.cy, stroke_width(face.w, 0.085f), kDim);
    }

    void draw_mouse(const Glyph &glyph, const GlyphBox &box) {
        const auto part = static_cast<MousePart>(glyph.index);
        const bool wheel_turn = part == MousePart::WheelUp || part == MousePart::WheelDown;
        const float cx = box.x + box.width * 0.5f;
        float h = box.height * (part == MousePart::Move ? 0.58f : wheel_turn ? 0.76f : 0.98f);
        float w = h * 0.64f;
        if (w > box.width * (part == MousePart::Move ? 0.58f : 1.0f)) {
            w = box.width * (part == MousePart::Move ? 0.58f : 1.0f);
            h = w / 0.64f;
        }
        float cy = box.y + box.height * 0.5f;
        if (part == MousePart::WheelUp) cy = box.y + box.height - h * 0.5f;
        if (part == MousePart::WheelDown) cy = box.y + h * 0.5f;
        const Place at = place(cx, cy, w, h);
        w = at.w; h = at.h; cy = at.cy;
        const float body_cx = at.cx;
        const float left = body_cx - w * 0.5f, top = cy - h * 0.5f, radius = w * 0.46f;
        const float rim = rim_width(w), pen = std::max(1.0f, 0.05f * w);
        const float split = top + h * 0.44f;
        slab(body_cx, cy, w, h, radius);
        const D2D1_ROUNDED_RECT inside = D2D1::RoundedRect(
            D2D1::RectF(left + rim, top + rim, left + w - rim, top + h - rim), radius - rim, radius - rim);
        const auto light = [&](float x0, float x1) {
            target->PushAxisAlignedClip(D2D1::RectF(x0, top, x1, split), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            target->FillRoundedRectangle(inside, solid(kLit));
            target->PopAxisAlignedClip();
        };
        if (part == MousePart::Left) light(left, body_cx);
        if (part == MousePart::Right) light(body_cx, left + w);
        // The line between the buttons and the line under them.
        line(left + rim, split, left + w - rim, split, pen, kRim);
        line(body_cx, top + rim, body_cx, split, pen, kRim);
        // The wheel.
        const bool wheel_lit = part == MousePart::Middle || wheel_turn;
        const float wheel_w = std::max(2.0f, w * (wheel_lit ? 0.26f : 0.18f)), wheel_h = h * (wheel_lit ? 0.27f : 0.2f);
        const float wheel_y = top + h * 0.24f;
        const D2D1_RECT_F wheel = D2D1::RectF(body_cx - wheel_w * 0.5f, wheel_y - wheel_h * 0.5f, body_cx + wheel_w * 0.5f, wheel_y + wheel_h * 0.5f);
        if (wheel_lit) {
            const D2D1_RECT_F around = D2D1::RectF(wheel.left - pen, wheel.top - pen, wheel.right + pen, wheel.bottom + pen);
            target->FillRoundedRectangle(D2D1::RoundedRect(around, wheel_w * 0.5f + pen, wheel_w * 0.5f + pen), solid(kBodyBottom));
        }
        target->FillRoundedRectangle(D2D1::RoundedRect(wheel, wheel_w * 0.5f, wheel_w * 0.5f), solid(wheel_lit ? kLit : kRim));
        if (wheel_turn) {
            const float size = box.height * 0.17f;
            if (part == MousePart::WheelUp) arrow_head(cx, box.y + size * 0.5f, size, glyph_up, kLegend);
            else arrow_head(cx, box.y + box.height - size * 0.5f, size, glyph_down, kLegend);
        }
        if (part == MousePart::Side4 || part == MousePart::Side5) {
            // The thumb buttons on the left side: 5 is the front one.
            const float bar_h = h * 0.15f, bar_w = std::max(2.0f, w * 0.17f);
            const float y = top + h * (part == MousePart::Side5 ? 0.53f : 0.71f);
            const D2D1_RECT_F bar = D2D1::RectF(left - bar_w * 0.25f, y - bar_h * 0.5f, left + bar_w * 0.75f, y + bar_h * 0.5f);
            target->FillRoundedRectangle(D2D1::RoundedRect(bar, bar_w * 0.4f, bar_w * 0.4f), solid(kLit));
            label(part == MousePart::Side5 ? "5" : "4", cx + w * 0.08f, top + h * 0.72f, h * 0.25f, w * 0.5f, kLegend);
        }
        if (part == MousePart::Move) {
            const float size = box.height * 0.15f, reach_y = h * 0.5f + size * 0.78f, reach_x = w * 0.5f + size * 0.78f;
            arrow_head(cx, cy - reach_y, size, glyph_up, kLegend);
            arrow_head(cx, cy + reach_y, size, glyph_down, kLegend);
            arrow_head(cx - reach_x, cy, size, glyph_left, kLegend);
            arrow_head(cx + reach_x, cy, size, glyph_right, kLegend);
        }
    }

    void draw_face(const Glyph &glyph, const GlyphBox &box) {
        const float size = std::min(box.width, box.height);
        const Place at = place(box.x + box.width * 0.5f, box.y + box.height * 0.5f, size, size);
        const float cx = at.cx, cy = at.cy, d = at.w, radius = d * 0.5f;
        disc(cx, cy, radius);
        const unsigned which = glyph.index & 3u;
        if (glyph.kind == GlyphKind::XboxFace) {
            static constexpr const char *letters[4]{"A", "B", "X", "Y"};
            label(letters[which], cx, cy, d * 0.5f, d * 0.6f, kXboxFace[which]);
            return;
        }
        const std::uint32_t color = kPsFace[which];
        const float pen = stroke_width(d, 0.1f);
        if (which == ps_cross) {
            const float reach = d * 0.2f;
            line(cx - reach, cy - reach, cx + reach, cy + reach, pen, color);
            line(cx - reach, cy + reach, cx + reach, cy - reach, pen, color);
        } else if (which == ps_circle) {
            target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), d * 0.24f, d * 0.24f), solid(color), pen);
        } else if (which == ps_square) {
            const float reach = d * 0.2f;
            const D2D1_POINT_2F p[4]{{cx - reach, cy - reach}, {cx + reach, cy - reach}, {cx + reach, cy + reach}, {cx - reach, cy + reach}};
            polygon(p, 4u, nullptr, solid(color), pen, true);
        } else {
            const float reach = d * 0.27f, lift = d * 0.035f;
            const D2D1_POINT_2F p[3]{{cx, cy - reach - lift}, {cx + reach * 0.9f, cy + reach * 0.56f - lift},
                                     {cx - reach * 0.9f, cy + reach * 0.56f - lift}};
            polygon(p, 3u, nullptr, solid(color), pen, true);
        }
    }
    void draw_shoulder(const Glyph &glyph, const GlyphBox &box) {
        // A flat wide bar, as the bumper lies across the top of the pad.
        const float about_h = std::min(box.height * 0.7f, box.width * 0.78f);
        const Place at = place(box.x + box.width * 0.5f, box.y + box.height * 0.5f, std::min(box.width, about_h * 1.9f), about_h);
        const float cx = at.cx, cy = at.cy, w = at.w, h = at.h;
        slab(cx, cy, w, h, h * 0.42f);
        static constexpr const char *names[2][2]{{"LB", "RB"}, {"L1", "R1"}};
        label(names[glyph.look == PadLook::PlayStation ? 1 : 0][glyph.index & 1u], cx, cy, h * 0.5f,
              w - 2.0f * rim_width(h) - std::max(2.0f, 0.14f * h), kLegend);
    }
    void draw_trigger(const Glyph &glyph, const GlyphBox &box) {
        // Taller than wide and rounder at the top, as a trigger looks from behind.
        const Place at = place(box.x + box.width * 0.5f, box.y + box.height * 0.5f, std::min(box.width, box.height * 0.84f), box.height * 0.98f);
        const float cx = at.cx, cy = at.cy, w = at.w, h = at.h;
        const float left = cx - w * 0.5f, right = cx + w * 0.5f, top = cy - h * 0.5f, bottom = cy + h * 0.5f;
        const float round_top = w * 0.46f, round_bottom = w * 0.2f;
        const auto shape = [&](float inset) {
            ComPtr<ID2D1PathGeometry> path;
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(d2d->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return ComPtr<ID2D1PathGeometry>();
            const float l = left + inset, r = right - inset, t = top + inset, b = bottom - inset;
            const float rb = std::max(0.5f, round_top - inset), rs = std::max(0.5f, round_bottom - inset);
            sink->BeginFigure(D2D1::Point2F(l, t + rb), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(l + rb, t), D2D1::SizeF(rb, rb), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->AddLine(D2D1::Point2F(r - rb, t));
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(r, t + rb), D2D1::SizeF(rb, rb), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->AddLine(D2D1::Point2F(r, b - rs));
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(r - rs, b), D2D1::SizeF(rs, rs), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->AddLine(D2D1::Point2F(l + rs, b));
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(l, b - rs), D2D1::SizeF(rs, rs), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (FAILED(sink->Close())) return ComPtr<ID2D1PathGeometry>();
            return path;
        };
        const float rim = rim_width(w);
        if (const auto outer = shape(0.0f)) target->FillGeometry(outer.Get(), body(top, bottom).Get());
        if (const auto edge = shape(rim * 0.5f)) target->DrawGeometry(edge.Get(), solid(kRim), rim);
        static constexpr const char *names[2][2]{{"LT", "RT"}, {"L2", "R2"}};
        label(names[glyph.look == PadLook::PlayStation ? 1 : 0][glyph.index & 1u], cx, cy + h * 0.03f, h * 0.36f,
              w - 2.0f * rim - std::max(2.0f, 0.12f * w), kLegend);
    }
    void draw_stick(const Glyph &glyph, const GlyphBox &box) {
        const float size = std::min(box.width, box.height);
        const Place at = place(box.x + box.width * 0.5f, box.y + box.height * 0.5f, size, size);
        const float cx = at.cx, cy = at.cy, d = at.w, radius = d * 0.5f;
        disc(cx, cy, radius);
        const bool pushed = glyph.directions != 0u;
        // The cap of the stick: a lighter disc in the disc. Pressed in, it fills the stick and carries its name.
        const float cap = radius * (pushed ? 0.56f : 0.76f);
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), cap, cap), solid(0x5A5A66u));
        // Its edge, where there are pixels for one more ring.
        const float ring = std::max(1.0f, 0.045f * d);
        if (d >= 30.0f) target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), cap - ring * 0.5f, cap - ring * 0.5f), solid(kRim), ring);
        const bool right = (glyph.index & 1u) != 0u;
        if (pushed) {
            label(right ? "R" : "L", cx, cy, cap * 1.02f, cap * 1.3f, kLegend);
            const float mark = radius * 0.32f, reach = (cap + radius - rim_width(d)) * 0.5f;
            if (glyph.directions & glyph_up) arrow_head(cx, cy - reach, mark, glyph_up, kLit);
            if (glyph.directions & glyph_down) arrow_head(cx, cy + reach, mark, glyph_down, kLit);
            if (glyph.directions & glyph_left) arrow_head(cx - reach, cy, mark, glyph_left, kLit);
            if (glyph.directions & glyph_right) arrow_head(cx + reach, cy, mark, glyph_right, kLit);
        } else {
            static constexpr const char *names[2][2]{{"LS", "RS"}, {"L3", "R3"}};
            label(names[glyph.look == PadLook::PlayStation ? 1 : 0][right ? 1 : 0], cx, cy, cap * 0.9f, cap * 1.56f, kLegend);
        }
    }
    void draw_dpad(const Glyph &glyph, const GlyphBox &box) {
        const float size = std::min(box.width, box.height);
        const Place at = place(box.x + box.width * 0.5f, box.y + box.height * 0.5f, size, size);
        const float cx = at.cx, cy = at.cy, d = at.w;
        const float rim = rim_width(d);
        // The rim is stroked along the middle of its width. The arms are a whole number of pixels wide.
        const float reach = d * 0.5f - rim * 0.5f;
        const float arm = std::max(1.0f, std::round(d * 0.38f)) * 0.5f - rim * 0.5f + (static_cast<int>(std::round(d * 0.38f)) % 2 != static_cast<int>(d) % 2 ? 0.5f : 0.0f);
        const D2D1_POINT_2F p[12]{{cx - arm, cy - reach}, {cx + arm, cy - reach}, {cx + arm, cy - arm}, {cx + reach, cy - arm},
                                  {cx + reach, cy + arm}, {cx + arm, cy + arm}, {cx + arm, cy + reach}, {cx - arm, cy + reach},
                                  {cx - arm, cy + arm}, {cx - reach, cy + arm}, {cx - reach, cy - arm}, {cx - arm, cy - arm}};
        polygon(p, 12u, body(cy - reach, cy + reach).Get());
        // A lit arm is red from the middle of the pad to its end, which shows at any size; the other arms
        // have a small grey mark where there are pixels for one.
        const float inner = arm - rim * 0.5f, outer = reach - rim * 0.5f, start = inner * 0.6f;
        const auto lit_arm = [&](float x0, float y0, float x1, float y1) {
            target->FillRectangle(D2D1::RectF(std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)), solid(kLit));
        };
        if (glyph.directions & glyph_up) lit_arm(cx - inner, cy - outer, cx + inner, cy - start);
        if (glyph.directions & glyph_down) lit_arm(cx - inner, cy + start, cx + inner, cy + outer);
        if (glyph.directions & glyph_left) lit_arm(cx - outer, cy - inner, cx - start, cy + inner);
        if (glyph.directions & glyph_right) lit_arm(cx + start, cy - inner, cx + outer, cy + inner);
        polygon(p, 12u, nullptr, solid(kRim), rim, true);
        const float out = d * 0.33f;
        const auto arm_mark = [&](std::uint8_t direction, float x, float y) {
            if ((glyph.directions & direction) != 0u) { if (d >= 24.0f) arrow_head(x, y, d * 0.13f, direction, kLegend); }
            else if (d >= 24.0f) arrow_head(x, y, d * 0.12f, direction, kDim);
        };
        arm_mark(glyph_up, cx, cy - out);
        arm_mark(glyph_down, cx, cy + out);
        arm_mark(glyph_left, cx - out, cy);
        arm_mark(glyph_right, cx + out, cy);
    }
    // A brush that runs from one colour to others along a line; the plain first colour if it cannot be made.
    ComPtr<ID2D1Brush> blend(D2D1_POINT_2F from, D2D1_POINT_2F to, const D2D1_GRADIENT_STOP *stops, UINT32 count) {
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> made;
        if (SUCCEEDED(target->CreateGradientStopCollection(stops, count, &collection)) &&
            SUCCEEDED(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(from, to), collection.Get(), &made))) return made;
        ComPtr<ID2D1SolidColorBrush> plain;
        (void)target->CreateSolidColorBrush(stops[0].color, &plain);
        return plain;
    }
    // The padlock of the game's font: a gold body with a keyhole, a lighter shackle, dark brown edges.
    // Open: the shackle is swung up about its left leg, so its right end stands clear above the body.
    void draw_lock(bool open, const GlyphBox &box) {
        const float w = box.width, h = box.height, cx = box.x + w * 0.5f;
        const float edge = std::max(1.0f, 0.048f * w);
        const float body_top = box.y + h * 0.48f, body_bottom = box.y + h - edge * 0.5f;
        const float body_left = box.x + w * 0.06f + edge * 0.5f, body_right = box.x + w * 0.94f - edge * 0.5f;
        const float body_h = body_bottom - body_top;
        // The shackle first: its legs end inside the body, which is drawn over them.
        const float reach = w * 0.25f, thick = std::max(1.5f, w * 0.105f);
        const float crown = box.y + edge + thick * 0.5f + h * 0.07f;
        const float left_end = body_top + body_h * 0.2f, right_end = open ? crown + reach : left_end;
        {
            ComPtr<ID2D1PathGeometry> path;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(d2d->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
                sink->BeginFigure(D2D1::Point2F(cx - reach, left_end), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLine(D2D1::Point2F(cx - reach, crown + reach));
                sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + reach, crown + reach), D2D1::SizeF(reach, reach), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                if (!open) sink->AddLine(D2D1::Point2F(cx + reach, right_end));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                if (SUCCEEDED(sink->Close())) {
                    // Swung up by 20 degrees about the foot of the left leg (a negative angle turns against the clock).
                    const D2D1::Matrix3x2F swing = open ? D2D1::Matrix3x2F::Rotation(-20.0f, D2D1::Point2F(cx - reach, body_top)) : D2D1::Matrix3x2F::Identity();
                    target->SetTransform(swing);
                    target->DrawGeometry(path.Get(), solid(kLockEdge), thick + 2.0f * edge, round.Get());
                    target->DrawGeometry(path.Get(), solid(kLockShackle), thick, round.Get());
                    // The light along the shackle's outer side.
                    if (thick >= 6.0f) {
                        target->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, -thick * 0.2f) * swing);
                        target->DrawGeometry(path.Get(), solid(0xFFFFFFu, 0.5f), thick * 0.22f, round.Get());
                    }
                    target->SetTransform(D2D1::Matrix3x2F::Identity());
                }
            }
        }
        // The body: light down its middle, deeper gold at the sides and along the bottom.
        const D2D1_ROUNDED_RECT body_shape = D2D1::RoundedRect(D2D1::RectF(body_left, body_top, body_right, body_bottom), w * 0.09f, w * 0.09f);
        const D2D1_GRADIENT_STOP across[4]{{0.0f, D2D1::ColorF(kLockDeep)}, {0.38f, D2D1::ColorF(kLockLight)}, {0.62f, D2D1::ColorF(kLockLight)}, {1.0f, D2D1::ColorF(kLockGold)}};
        if (const auto fill = blend(D2D1::Point2F(body_left, 0.0f), D2D1::Point2F(body_right, 0.0f), across, 4u)) target->FillRoundedRectangle(body_shape, fill.Get());
        const D2D1_GRADIENT_STOP down[3]{{0.0f, D2D1::ColorF(kLockGold, 0.0f)}, {0.55f, D2D1::ColorF(kLockGold, 0.25f)}, {1.0f, D2D1::ColorF(kLockDeep, 0.9f)}};
        if (const auto shade = blend(D2D1::Point2F(0.0f, body_top), D2D1::Point2F(0.0f, body_bottom), down, 3u)) target->FillRoundedRectangle(body_shape, shade.Get());
        target->DrawRoundedRectangle(body_shape, solid(kLockEdge), edge);
        // The keyhole: a round head over a slot.
        const float head = std::max(1.0f, w * 0.075f), slot = std::max(0.75f, w * 0.042f), hole_y = body_top + body_h * 0.36f;
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, hole_y), head, head), solid(kLockHole));
        target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - slot, hole_y, cx + slot, body_top + body_h * 0.76f), slot * 0.6f, slot * 0.6f), solid(kLockHole));
    }
    // The chili of the game's font: its tip at the lower left, the stem at the upper right, a black edge.
    void draw_chili(const GlyphBox &box) {
        const float w = box.width, h = box.height;
        const auto at = [&](float u, float v) { return D2D1::Point2F(box.x + u * w, box.y + v * h); };
        const float edge = std::max(1.0f, 0.085f * w);
        // The stem under the body's edge: a short bent stalk.
        {
            ComPtr<ID2D1PathGeometry> path;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(d2d->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
                sink->BeginFigure(at(0.73f, 0.20f), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddBezier(D2D1::BezierSegment(at(0.74f, 0.11f), at(0.78f, 0.07f), at(0.90f, 0.075f)));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                if (SUCCEEDED(sink->Close())) {
                    target->DrawGeometry(path.Get(), solid(kChiliEdge), std::max(2.0f, 0.1f * w) + 2.0f * edge * 0.75f, round.Get());
                    target->DrawGeometry(path.Get(), solid(kChiliStem), std::max(1.0f, 0.1f * w), round.Get());
                }
            }
        }
        ComPtr<ID2D1PathGeometry> body_path;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(d2d->CreatePathGeometry(&body_path)) || FAILED(body_path->Open(&sink))) return;
        sink->BeginFigure(at(0.07f, 0.94f), D2D1_FIGURE_BEGIN_FILLED);                                             // the tip
        sink->AddBezier(D2D1::BezierSegment(at(0.25f, 0.85f), at(0.46f, 0.58f), at(0.57f, 0.25f)));               // the hollow side, up to the shoulder
        sink->AddBezier(D2D1::BezierSegment(at(0.60f, 0.13f), at(0.84f, 0.11f), at(0.90f, 0.25f)));               // round over the top
        sink->AddBezier(D2D1::BezierSegment(at(0.94f, 0.52f), at(0.57f, 0.90f), at(0.07f, 0.94f)));               // the full side, back to the tip
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(sink->Close())) return;
        const D2D1_GRADIENT_STOP red[3]{{0.0f, D2D1::ColorF(kChiliLight)}, {0.5f, D2D1::ColorF(kChili)}, {1.0f, D2D1::ColorF(kChiliDeep)}};
        if (const auto fill = blend(at(0.30f, 0.38f), at(0.82f, 0.86f), red, 3u)) target->FillGeometry(body_path.Get(), fill.Get());
        target->DrawGeometry(body_path.Get(), solid(kChiliEdge), edge, round.Get());
        // The shine along the hollow side.
        ComPtr<ID2D1PathGeometry> shine;
        ComPtr<ID2D1GeometrySink> shine_sink;
        if (FAILED(d2d->CreatePathGeometry(&shine)) || FAILED(shine->Open(&shine_sink))) return;
        shine_sink->BeginFigure(at(0.31f, 0.80f), D2D1_FIGURE_BEGIN_HOLLOW);
        shine_sink->AddBezier(D2D1::BezierSegment(at(0.44f, 0.68f), at(0.58f, 0.50f), at(0.67f, 0.30f)));
        shine_sink->EndFigure(D2D1_FIGURE_END_OPEN);
        if (SUCCEEDED(shine_sink->Close())) target->DrawGeometry(shine.Get(), solid(kChiliShine, 0.9f), std::max(1.0f, 0.06f * w), round.Get());
    }
    void draw_menu(const Glyph &glyph, const GlyphBox &box) {
        const bool view = glyph.kind == GlyphKind::View;
        const float d = std::min(box.width, box.height);
        float cx = box.x + box.width * 0.5f, cy = box.y + box.height * 0.5f;
        float mark = d * 0.2f;
        if (glyph.look == PadLook::PlayStation) {
            // Options and Share are small bars beside the touch pad.
            const Place at = place(cx, cy, std::min(box.width, d * 0.62f * 1.7f), d * 0.62f);
            cx = at.cx; cy = at.cy;
            slab(cx, cy, at.w, at.h, at.h * 0.5f);
            mark = at.h * 0.24f;
        } else {
            const Place at = place(cx, cy, d * 0.88f, d * 0.88f);
            cx = at.cx; cy = at.cy;
            disc(cx, cy, at.w * 0.5f);
        }
        const float pen = stroke_width(d, 0.07f);
        if (!view) {
            // Three lines.
            for (int row = -1; row <= 1; ++row)
                line(cx - mark, cy + static_cast<float>(row) * mark * 0.72f, cx + mark, cy + static_cast<float>(row) * mark * 0.72f, pen, kLegend);
        } else if (glyph.look == PadLook::PlayStation) {
            // Share: three dots joined, one to the left of two.
            const float dot = std::max(1.0f, mark * 0.3f);
            const D2D1_POINT_2F p[3]{{cx + mark * 0.8f, cy - mark * 0.8f}, {cx - mark * 0.8f, cy}, {cx + mark * 0.8f, cy + mark * 0.8f}};
            polygon(p, 3u, nullptr, solid(kLegend), std::max(1.0f, pen * 0.7f), false);
            for (const auto &point : p) target->FillEllipse(D2D1::Ellipse(point, dot, dot), solid(kLegend));
        } else {
            // View: one window in front of another.
            const float s = mark * 0.78f, shift = mark * 0.42f;
            const D2D1_POINT_2F back[4]{{cx - s + shift, cy - s - shift}, {cx + s + shift, cy - s - shift},
                                        {cx + s + shift, cy + s - shift}, {cx - s + shift, cy + s - shift}};
            polygon(back, 4u, nullptr, solid(kLegend), std::max(1.0f, pen * 0.8f), true);
            const D2D1_POINT_2F front[4]{{cx - s - shift, cy - s + shift}, {cx + s - shift, cy - s + shift},
                                         {cx + s - shift, cy + s + shift}, {cx - s - shift, cy + s + shift}};
            polygon(front, 4u, solid_fill(kBodyBottom), solid(kLegend), std::max(1.0f, pen * 0.8f), true);
        }
    }
};

GlyphPainter::GlyphPainter() : state_(std::make_unique<State>()) {}
GlyphPainter::~GlyphPainter() = default;

const std::string &GlyphPainter::font_name() const noexcept { return state_->font; }

bool GlyphPainter::start(std::string &error) {
    State &s = *state_;
    if (s.d2d && s.dwrite && s.wic && s.font_face) return true;
    // S_FALSE: this thread has COM already. RPC_E_CHANGED_MODE: it has it in the other mode, which serves as well.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(com)) s.com = true;
    else if (com != RPC_E_CHANGED_MODE) { error = hr_text("COM did not start", com); return false; }
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&s.wic));
    if (FAILED(hr)) { error = hr_text("Windows Imaging Component is not available", hr); return false; }
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                           reinterpret_cast<void **>(s.d2d.GetAddressOf()));
    if (FAILED(hr)) { error = hr_text("Direct2D is not available", hr); return false; }
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown **>(s.dwrite.GetAddressOf()));
    if (FAILED(hr)) { error = hr_text("DirectWrite is not available", hr); return false; }
    // Fonts every Windows has; the last resort is whatever family comes first.
    if (!s.pick_font(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD) && !s.pick_font(L"Arial", DWRITE_FONT_WEIGHT_BOLD) &&
        !s.pick_font(L"Tahoma", DWRITE_FONT_WEIGHT_BOLD) && !s.pick_font(nullptr, DWRITE_FONT_WEIGHT_BOLD)) {
        error = "DirectWrite has no usable font";
        return false;
    }
    hr = s.d2d->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
        nullptr, 0u, &s.round);
    if (FAILED(hr)) { error = hr_text("Direct2D stroke style", hr); return false; }
    return true;
}

bool GlyphPainter::begin(std::uint32_t width, std::uint32_t height, std::string &error) {
    State &s = *state_;
    s.drop_canvas();
    if (!s.d2d || !s.wic || !s.font_face) { error = "the glyph painter was not started"; return false; }
    if (width == 0u || height == 0u || width > 16384u || height > 16384u) { error = "bad canvas size"; return false; }
    HRESULT hr = s.wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &s.bitmap);
    if (FAILED(hr)) { error = hr_text("cannot create the canvas", hr); return false; }
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    hr = s.d2d->CreateWicBitmapRenderTarget(s.bitmap.Get(), properties, &s.target);
    if (FAILED(hr)) { error = hr_text("Direct2D cannot draw into the canvas", hr); s.drop_canvas(); return false; }
    hr = s.target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &s.brush);
    if (SUCCEEDED(hr)) hr = s.target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &s.fill_brush);
    const D2D1_GRADIENT_STOP stops[2]{{0.0f, D2D1::ColorF(kBodyTop)}, {1.0f, D2D1::ColorF(kBodyBottom)}};
    if (SUCCEEDED(hr)) hr = s.target->CreateGradientStopCollection(stops, 2u, &s.body_stops);
    if (FAILED(hr)) { error = hr_text("Direct2D brushes", hr); s.drop_canvas(); return false; }
    s.canvas_width = width;
    s.canvas_height = height;
    s.target->BeginDraw();
    s.target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    s.target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    return true;
}

void GlyphPainter::draw(const Glyph &glyph, const GlyphBox &box) {
    State &s = *state_;
    if (!s.target || !(box.width > 0.0f) || !(box.height > 0.0f)) return;
    switch (glyph.kind) {
    case GlyphKind::Unbound: s.draw_unbound(box); break;
    case GlyphKind::Key: s.draw_key(glyph, box); break;
    case GlyphKind::KeyCluster: s.draw_cluster(glyph, box); break;
    case GlyphKind::Mouse: s.draw_mouse(glyph, box); break;
    case GlyphKind::XboxFace:
    case GlyphKind::PlayStationFace: s.draw_face(glyph, box); break;
    case GlyphKind::Shoulder: s.draw_shoulder(glyph, box); break;
    case GlyphKind::Trigger: s.draw_trigger(glyph, box); break;
    case GlyphKind::Stick: s.draw_stick(glyph, box); break;
    case GlyphKind::Dpad: s.draw_dpad(glyph, box); break;
    case GlyphKind::Menu:
    case GlyphKind::View: s.draw_menu(glyph, box); break;
    case GlyphKind::Lock: s.draw_lock((glyph.index & 1u) != 0u, box); break;
    case GlyphKind::Chili: s.draw_chili(box); break;
    }
}

bool GlyphPainter::end(GlyphPicture &picture, std::string &error) {
    State &s = *state_;
    if (!s.target) { error = "no canvas"; return false; }
    HRESULT hr = s.target->EndDraw();
    if (FAILED(hr)) { error = hr_text("Direct2D drawing failed", hr); s.drop_canvas(); return false; }
    s.brush.Reset();
    s.fill_brush.Reset();
    s.body_stops.Reset();
    s.target.Reset();
    WICRect whole{0, 0, static_cast<INT>(s.canvas_width), static_cast<INT>(s.canvas_height)};
    ComPtr<IWICBitmapLock> lock;
    hr = s.bitmap->Lock(&whole, WICBitmapLockRead, &lock);
    UINT stride = 0u, size = 0u;
    BYTE *data = nullptr;
    if (SUCCEEDED(hr)) hr = lock->GetStride(&stride);
    if (SUCCEEDED(hr)) hr = lock->GetDataPointer(&size, &data);
    if (FAILED(hr) || data == nullptr || stride < s.canvas_width * 4u) {
        error = hr_text("cannot read the canvas", hr);
        lock.Reset();
        s.drop_canvas();
        return false;
    }
    picture.width = s.canvas_width;
    picture.height = s.canvas_height;
    picture.rgba.assign(static_cast<std::size_t>(s.canvas_width) * s.canvas_height * 4u, 0u);
    for (std::uint32_t y = 0u; y < s.canvas_height; ++y) {
        const BYTE *in = data + static_cast<std::size_t>(y) * stride;
        std::uint8_t *out = picture.rgba.data() + static_cast<std::size_t>(y) * s.canvas_width * 4u;
        for (std::uint32_t x = 0u; x < s.canvas_width; ++x, in += 4, out += 4) {
            // Premultiplied blue-first to straight red-first.
            const unsigned alpha = in[3];
            if (alpha == 0u) continue;
            out[0] = static_cast<std::uint8_t>(std::min(255u, (in[2] * 255u + alpha / 2u) / alpha));
            out[1] = static_cast<std::uint8_t>(std::min(255u, (in[1] * 255u + alpha / 2u) / alpha));
            out[2] = static_cast<std::uint8_t>(std::min(255u, (in[0] * 255u + alpha / 2u) / alpha));
            out[3] = static_cast<std::uint8_t>(alpha);
        }
    }
    lock.Reset();
    s.drop_canvas();
    return true;
}

GlyphPicture glyph_outline(const GlyphPicture &fill, std::uint32_t radius) {
    GlyphPicture out;
    out.width = fill.width;
    out.height = fill.height;
    out.rgba.assign(fill.rgba.size(), 0u);
    const std::uint32_t w = fill.width, h = fill.height;
    if (w == 0u || h == 0u || fill.rgba.size() < static_cast<std::size_t>(w) * h * 4u) return out;
    // The alpha grown by `radius` in every direction: the largest value in a disc, taken row by row with
    // the half-width the disc has at that row.
    std::vector<std::uint8_t> grown(static_cast<std::size_t>(w) * h);
    std::vector<std::uint32_t> reach(radius + 1u);
    for (std::uint32_t dy = 0u; dy <= radius; ++dy)
        reach[dy] = static_cast<std::uint32_t>(std::floor(std::sqrt(static_cast<double>(radius * radius - dy * dy) + 0.25)));
    for (std::uint32_t y = 0u; y < h; ++y) {
        const std::uint32_t y0 = y >= radius ? y - radius : 0u, y1 = std::min(h - 1u, y + radius);
        for (std::uint32_t x = 0u; x < w; ++x) {
            std::uint8_t best = 0u;
            for (std::uint32_t sy = y0; sy <= y1 && best != 255u; ++sy) {
                const std::uint32_t span = reach[sy > y ? sy - y : y - sy];
                const std::uint32_t x0 = x >= span ? x - span : 0u, x1 = std::min(w - 1u, x + span);
                const std::uint8_t *row = fill.rgba.data() + (static_cast<std::size_t>(sy) * w + x0) * 4u + 3u;
                for (std::uint32_t sx = x0; sx <= x1; ++sx, row += 4) best = std::max(best, *row);
            }
            grown[static_cast<std::size_t>(y) * w + x] = best;
        }
    }
    const unsigned edge[3]{(kEdge >> 16u) & 255u, (kEdge >> 8u) & 255u, kEdge & 255u};
    for (std::size_t i = 0u; i < grown.size(); ++i) {
        const unsigned under = grown[i];
        if (under == 0u) continue;
        const std::uint8_t *in = fill.rgba.data() + i * 4u;
        std::uint8_t *px = out.rgba.data() + i * 4u;
        // The picture over the edge.
        const unsigned top = in[3];
        const unsigned rest = under * (255u - top);              // edge share, times 255
        const unsigned alpha = top * 255u + rest;                 // result alpha, times 255
        for (int c = 0; c < 3; ++c) px[c] = static_cast<std::uint8_t>((in[c] * top * 255u + edge[c] * rest + alpha / 2u) / alpha);
        px[3] = static_cast<std::uint8_t>((alpha + 127u) / 255u);
    }
    return out;
}

GlyphPicture glyph_cut(const GlyphPicture &picture, std::uint32_t x, std::uint32_t y, std::uint32_t width,
                       std::uint32_t height) {
    GlyphPicture out;
    if (x >= picture.width || y >= picture.height) return out;
    out.width = std::min(width, picture.width - x);
    out.height = std::min(height, picture.height - y);
    out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4u);
    for (std::uint32_t row = 0u; row < out.height; ++row)
        std::memcpy(out.rgba.data() + static_cast<std::size_t>(row) * out.width * 4u,
                    picture.rgba.data() + (static_cast<std::size_t>(y + row) * picture.width + x) * 4u,
                    static_cast<std::size_t>(out.width) * 4u);
    return out;
}

} // namespace chili
