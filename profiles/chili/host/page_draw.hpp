#pragma once
// What a host page (settings inside the game) is drawn with: a list of textured and plain rectangles in PSP
// pixels for the renderer's overlay pass (gpu/ge_gpu_backend.hpp), and text set from the game's own font as
// the game sets its menu text. Nothing here touches Direct3D or the game: a page builds a list on the game
// thread and hands it over with vcs::ge_gpu_backend_set_overlay().
//
// How the game draws a menu text box, measured on its own draws (a capture of Profile Options, the positions
// of the scene's text boxes beside it):
//
//   glyph          a rectangle of the sheet: the cell's width and one row less than the cell's height (29 of
//                  30), drawn width * scale wide and 29 * scale high
//   pen            starts at the box's x (left), or so that the text ends at the box's right edge; moves by
//                  (advance + spacing) * scale, where spacing is the box's own (-5 in every menu box); the
//                  last glyph counts with its full width. It is a single-precision sum, one product added
//                  per glyph
//   top            box y + the glyph's y offset (9) * scale - the box's lift (8 for a line at scale 0.8, 7 for
//                  a legend at 0.7, 4 for a headline at 1, 2.5 for the tilted category line)
//   whole pixels   every corner is cut down to a whole PSP pixel (the game's vertices are 16-bit integers):
//                  the left edge from the pen, the right edge from the pen plus width * scale. With the
//                  single-precision pen this gives the columns of all 121 one-line texts in eight
//                  captured menu frames (1,124 glyphs, every copy counted); a pen in double precision
//                  misses a glyph or two in every fifth of them
//   tilt           the corners as computed, not yet cut, are turned about the middle of the box, then cut
//   three copies   a black copy of the fill sheet whose rectangles are the text's own moved by (+2, +2)
//                  (the headline (+3, +3)), then the same rectangles from the outline sheet in the outline
//                  colour, then the fill sheet in the text colour; straight alpha; texels with alpha 12 or
//                  less are not drawn (the sheets' empty texels have alpha 4)
//
// The glyph table is the game's own: PageFont::scan finds it in guest memory, where the loaded font record
// of todpsp_game.main keeps it (181 glyphs of 32 bytes: code point, x offset, y offset, advance, u, v, u
// width, and a word that is 1 for a picture that keeps its own colours). Nothing of it is in this file.
#include "gpu/ge_gpu_backend.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace chili {

// ---- Textures ----------------------------------------------------------------------------------------

// Content keys (mods/TEXTURES.md) and GE shapes of the textures a page draws with. All of them are in the
// renderer's cache only once the game has drawn with them: ask vcs::ge_gpu_backend_overlay_texture_ready().
namespace page_texture {
inline constexpr std::uint64_t font_fill = 0x5d0c6d1a663a8515ull;        // /data/Fonts/ODMidText_24.font
inline constexpr std::uint64_t font_outline = 0x002d10b31e312e9dull;     // ODMidText_24_Stroke3.font
inline constexpr std::uint64_t background_main = 0xe1215277c6aafa16ull;  // Menu/Backgrounds/BackGround01.bmp: the main menu's
inline constexpr std::uint64_t background_page = 0xa247e7634946f676ull;  // BackGround03.bmp: Profile Options and Settings
inline constexpr std::uint64_t logo = 0x854c5701a58fb9c3ull;             // Menu/GameLogo.bmp
inline constexpr std::uint64_t white = 0xee34d1637a740122ull;            // generic/white.bmp
inline constexpr std::uint64_t button_cross = 0xdc8cc6b510da1d80ull;     // Menu/ButtonCross.bmp
inline constexpr std::uint64_t button_circle = 0xebff042e1d0cf176ull;
inline constexpr std::uint64_t button_square = 0xe6fd3c31244ec517ull;
inline constexpr std::uint64_t button_triangle = 0xeabe4d818979a897ull;
inline constexpr std::uint64_t button_dpad_lr = 0xe8cc8b252a16c095ull;   // Menu/ButtonDpadLR.bmp
}
// For vcs::ge_gpu_backend_watch_overlay_textures(): GE format 4 is CLUT4, 5 CLUT8, 8 DXT1, 9 DXT3.
inline constexpr std::array<vcs::GeGpuOverlayTexture, 11> page_textures{{
    {page_texture::font_fill, 5u, 256u, 512u}, {page_texture::font_outline, 4u, 256u, 512u},
    {page_texture::background_main, 8u, 256u, 256u}, {page_texture::background_page, 8u, 256u, 256u},
    {page_texture::logo, 5u, 256u, 128u}, {page_texture::white, 4u, 16u, 16u},
    {page_texture::button_cross, 5u, 32u, 32u}, {page_texture::button_circle, 5u, 32u, 32u},
    {page_texture::button_square, 5u, 32u, 32u}, {page_texture::button_triangle, 5u, 32u, 32u},
    {page_texture::button_dpad_lr, 9u, 32u, 32u}}};

// ---- Colours -----------------------------------------------------------------------------------------

struct PageColour {
    float r{1.0f}, g{1.0f}, b{1.0f}, a{1.0f};
    // The vertex colour word: bytes R G B A in memory.
    [[nodiscard]] constexpr std::uint32_t pack() const noexcept {
        const auto byte = [](float value) {
            const float clamped = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
            return static_cast<std::uint32_t>(clamped * 255.0f + 0.5f);
        };
        return byte(r) | (byte(g) << 8u) | (byte(b) << 16u) | (byte(a) << 24u);
    }
    // Darker by `factor`: the game's look of a line that cannot be chosen is 0.5.
    [[nodiscard]] constexpr PageColour dimmed(float factor) const noexcept { return {r * factor, g * factor, b * factor, a}; }
    [[nodiscard]] constexpr PageColour faded(float alpha) const noexcept { return {r, g, b, a * alpha}; }
};
// The menu's colours, from the text boxes of the menu scene.
namespace page_colour {
inline constexpr PageColour line{1.0f, 0.6275f, 0.0f}, line_outline{0.3137f, 0.0f, 0.0f};
inline constexpr PageColour headline{1.0f, 0.3137f, 0.0f}, headline_outline{0.2471f, 0.0f, 0.0f};
inline constexpr PageColour pause_headline{1.0f, 0.9412f, 0.3922f};
inline constexpr PageColour selected_dark{0.5f, 0.0f, 0.0f}, selected_bright{1.0f, 0.0f, 0.0f};
inline constexpr PageColour white{1.0f, 1.0f, 1.0f}, black{0.0f, 0.0f, 0.0f};
}

// ---- The font ----------------------------------------------------------------------------------------

// The game's sheets are 256 x 512; a replacement keeps every cell at the same fraction of the sheet.
inline constexpr float page_font_sheet_width = 256.0f, page_font_sheet_height = 512.0f;
// Pictures of the font that a page uses as characters: the PSP's buttons (the button prompts paint the key
// or pad button in use over them, prompt_style.hpp) and the game's own marks.
namespace page_glyph {
inline constexpr char32_t square = 0x00A2, triangle = 0x00A3, circle = 0x00A4, cross = 0x00A5, start = 0x00A7, select = 0x00A8;
inline constexpr char32_t l = 0x00AF, r = 0x00B1, dpad_up = 0x00B3, dpad_right = 0x00BC, dpad_down = 0x00B5, dpad_left = 0x00B6;
inline constexpr char32_t lock_open = U'{', lock_closed = U'}', chili = U'~', tick = U'|', bronze = U'[', silver = U']', gold = U'@';
}
// Appends a code point to UTF-8 text.
inline void page_append(std::string &text, char32_t code) {
    if (code < 0x80u) text.push_back(static_cast<char>(code));
    else if (code < 0x800u) { text.push_back(static_cast<char>(0xC0u | (code >> 6u))); text.push_back(static_cast<char>(0x80u | (code & 0x3Fu))); }
    else if (code < 0x10000u) {
        text.push_back(static_cast<char>(0xE0u | (code >> 12u))); text.push_back(static_cast<char>(0x80u | ((code >> 6u) & 0x3Fu)));
        text.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    } else {
        text.push_back(static_cast<char>(0xF0u | (code >> 18u))); text.push_back(static_cast<char>(0x80u | ((code >> 12u) & 0x3Fu)));
        text.push_back(static_cast<char>(0x80u | ((code >> 6u) & 0x3Fu))); text.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    }
}
[[nodiscard]] inline std::string page_text(char32_t code) { std::string text; page_append(text, code); return text; }
// The next code point of UTF-8 text; a byte that starts nothing is taken as Latin-1.
inline char32_t page_next_code(std::string_view &text) {
    const auto byte = [&](std::size_t i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(text[i])); };
    const std::uint32_t first = byte(0u);
    std::size_t length = first < 0x80u ? 1u : (first >> 5u) == 6u ? 2u : (first >> 4u) == 14u ? 3u : (first >> 3u) == 30u ? 4u : 1u;
    if (length > text.size()) length = 1u;
    for (std::size_t i = 1; i < length; ++i) if ((byte(i) & 0xC0u) != 0x80u) { length = 1u; break; }
    char32_t code = length == 1u ? first : length == 2u ? first & 0x1Fu : length == 3u ? first & 0x0Fu : first & 0x07u;
    for (std::size_t i = 1; i < length; ++i) code = (code << 6u) | (byte(i) & 0x3Fu);
    text.remove_prefix(length);
    return code;
}

struct PageGlyph {
    std::uint16_t code{};
    float x_offset{}, y_offset{}, advance{};   // pixels at text scale 1
    float u{}, v{}, u_width{};                 // fractions of the sheet
    bool picture{};                            // keeps its own colours
    // The cell's width in texels of the game's sheet, which is its width in pixels at scale 1.
    [[nodiscard]] constexpr float width() const noexcept { return u_width * page_font_sheet_width; }
};

class PageFont {
public:
    static constexpr std::size_t entry_size = 0x20u;
    PageFont() { latin_.fill(-1); }
    [[nodiscard]] bool loaded() const noexcept { return !glyphs_.empty(); }
    [[nodiscard]] const std::vector<PageGlyph> &glyphs() const noexcept { return glyphs_; }
    // Rows of the sheet a cell takes; a glyph is drawn one row less.
    [[nodiscard]] float cell_height() const noexcept { return cell_height_; }
    // Where the table was found (an offset into the scanned memory) and whether the font record's header
    // confirmed its length.
    [[nodiscard]] std::size_t table_offset() const noexcept { return table_offset_; }
    [[nodiscard]] bool header_found() const noexcept { return header_found_; }
    [[nodiscard]] const PageGlyph *find(char32_t code) const noexcept {
        if (code < latin_.size()) return latin_[code] >= 0 ? &glyphs_[static_cast<std::size_t>(latin_[code])] : nullptr;
        for (const auto &glyph : glyphs_) if (glyph.code == code) return &glyph;
        return nullptr;
    }
    // From the glyphs themselves (tests, and a table made by hand).
    void assign(std::vector<PageGlyph> glyphs, float cell_height) {
        glyphs_ = std::move(glyphs); cell_height_ = cell_height;
        latin_.fill(-1);
        for (std::size_t i = 0; i < glyphs_.size(); ++i)
            if (glyphs_[i].code < latin_.size() && latin_[glyphs_[i].code] < 0) latin_[glyphs_[i].code] = static_cast<std::int16_t>(i);
    }
    // Finds the game's glyph table in `memory` (guest RAM from `base` on; the game's user memory is
    // 0x08800000 to 0x0A000000). The table starts with the ten digits, 32 bytes each, and is as long as the
    // font record in front of it says: the record has the table's address (relative to itself, or as a
    // guest address) and the glyph count 0x40 and 0x44 bytes in, and the cell height 4 bytes in. Without a
    // header it is as long as its entries look like glyphs. False when the game has not loaded the font
    // (or this is not the game). Reads only; about 10 ms for 24 MB.
    bool scan(std::span<const std::uint8_t> memory, std::uint32_t base = 0u) {
        const auto word = [&](std::size_t at) { std::uint32_t value; std::memcpy(&value, memory.data() + at, 4u); return value; };
        const auto code_at = [&](std::size_t at) { std::uint16_t value; std::memcpy(&value, memory.data() + at, 2u); return value; };
        if (memory.size() < entry_size * 10u) return false;
        for (std::size_t at = 0; at + entry_size * 10u <= memory.size(); at += 4u) {
            if (code_at(at) != u'0' || code_at(at + entry_size) != u'1') continue;
            std::size_t count = 0u;
            while (count < 1024u && at + (count + 1u) * entry_size <= memory.size() && plausible(memory.data() + at + count * entry_size)) ++count;
            bool digits = count >= 10u;
            for (std::size_t i = 0; digits && i < 10u; ++i) digits = code_at(at + i * entry_size) == u'0' + i;
            if (!digits) continue;
            // The font record: the pointer to the table and the count behind it.
            float cell = 30.0f;
            bool header = false;
            for (std::size_t back = 4u; back <= 0x80000u && back <= at && !header; back += 4u) {
                const std::size_t pointer = at - back;
                const std::uint32_t value = word(pointer);
                if (value != back && value != base + static_cast<std::uint32_t>(at)) continue;
                const std::uint32_t named = word(pointer + 4u);
                if (named < 10u || named > count) continue;
                count = named;
                header = true;
                if (pointer >= 0x3Cu)
                    if (const std::uint32_t rows = word(pointer - 0x3Cu); rows >= 8u && rows <= 128u) cell = static_cast<float>(rows);
            }
            std::vector<PageGlyph> glyphs(count);
            for (std::size_t i = 0; i < count; ++i) glyphs[i] = read(memory.data() + at + i * entry_size);
            assign(std::move(glyphs), cell);
            table_offset_ = at; header_found_ = header;
            return true;
        }
        return false;
    }
    [[nodiscard]] static PageGlyph read(const std::uint8_t *entry) noexcept {
        PageGlyph glyph;
        std::uint32_t flag;
        float numbers[6];
        std::memcpy(&glyph.code, entry, 2u);
        std::memcpy(numbers, entry + 4u, sizeof(numbers));
        std::memcpy(&flag, entry + 0x1Cu, 4u);
        glyph.x_offset = numbers[0]; glyph.y_offset = numbers[1]; glyph.advance = numbers[2];
        glyph.u = numbers[3]; glyph.v = numbers[4]; glyph.u_width = numbers[5];
        glyph.picture = flag != 0u;
        return glyph;
    }
    // The 32 bytes of a glyph as the font record keeps them (tests build a memory image with it).
    static void write(const PageGlyph &glyph, std::uint8_t *entry) noexcept {
        std::memset(entry, 0, entry_size);
        const float numbers[6]{glyph.x_offset, glyph.y_offset, glyph.advance, glyph.u, glyph.v, glyph.u_width};
        const std::uint32_t flag = glyph.picture ? 1u : 0u;
        std::memcpy(entry, &glyph.code, 2u);
        std::memcpy(entry + 4u, numbers, sizeof(numbers));
        std::memcpy(entry + 0x1Cu, &flag, 4u);
    }
private:
    // An entry that can be a glyph: written so that a NaN fails.
    static bool plausible(const std::uint8_t *entry) noexcept {
        std::uint32_t flag;
        std::memcpy(&flag, entry + 0x1Cu, 4u);
        const PageGlyph glyph = read(entry);
        return glyph.code != 0u && flag <= 1u && glyph.advance > 0.0f && glyph.advance <= 96.0f && glyph.u >= 0.0f && glyph.u < 1.0f &&
               glyph.v >= 0.0f && glyph.v < 1.0f && glyph.u_width > 0.0f && glyph.u_width <= 0.5f &&
               glyph.x_offset >= -64.0f && glyph.x_offset <= 64.0f && glyph.y_offset >= -64.0f && glyph.y_offset <= 64.0f;
    }
    std::vector<PageGlyph> glyphs_;
    std::array<std::int16_t, 256> latin_{};
    float cell_height_{30.0f};
    std::size_t table_offset_{};
    bool header_found_{};
};

// ---- Text --------------------------------------------------------------------------------------------

enum class PageAlign : std::uint8_t { Left, Right, Centre };

// How a line of text is set. The defaults are a menu line; the four kinds the menu scene has are below.
struct PageTextStyle {
    float scale{0.8f};
    // The box's own vertical offset (see the top of this file).
    float lift{8.0f};
    // Added to every glyph's advance, in pixels at scale 1.
    float spacing{-5.0f};
    PageColour fill{page_colour::line}, outline{page_colour::line_outline}, shadow{page_colour::black};
    bool outlined{true}, shadowed{true};
    float shadow_x{2.0f}, shadow_y{2.0f};
    // Right: the text ends at x + width. Centre: in the middle of x to x + width.
    PageAlign align{PageAlign::Left};
    float width{};
    // Degrees, turned about the middle of the box (x to x + width, y to y + height); negative rises to the right.
    float angle{};
    float height{};
    // The colour pictures of the font are multiplied with (they keep their own colours): 0.5 on a line that
    // cannot be chosen. Its alpha is the fill's.
    float picture_tint{1.0f};
    // Cut every corner down to a whole PSP pixel, as the game does. Off: placed as computed.
    bool whole_pixels{true};

    // A selectable menu line (`Fiesta`, `Settings`): scale 0.8 in orange with the dark red outline.
    [[nodiscard]] static constexpr PageTextStyle line() noexcept { return {}; }
    // A legend's word beside its button (`Back`, `Select`): scale 0.7.
    [[nodiscard]] static constexpr PageTextStyle legend() noexcept { PageTextStyle style; style.scale = 0.7f; style.lift = 7.0f; return style; }
    // A screen's headline (`Profile Options`): scale 1 in hot orange, ending at the box's right edge
    // (the box is 350 wide at (120, 20)), shadow at (+3, +3).
    [[nodiscard]] static constexpr PageTextStyle headline() noexcept {
        PageTextStyle style;
        style.scale = 1.0f; style.lift = 4.0f; style.fill = page_colour::headline; style.outline = page_colour::headline_outline;
        style.shadow_x = style.shadow_y = 3.0f; style.align = PageAlign::Right; style.width = 350.0f;
        return style;
    }
    // The tilted line under the logo (`Profile: RAM`): scale 0.8, 8 degrees, in a box 300 x 30 at (20, 50).
    [[nodiscard]] static constexpr PageTextStyle category() noexcept {
        PageTextStyle style;
        style.lift = 2.5f; style.angle = -8.0f; style.width = 300.0f; style.height = 30.0f;
        return style;
    }
    // A value at the right of a line: white with the shadow and no outline.
    [[nodiscard]] static constexpr PageTextStyle value() noexcept {
        PageTextStyle style;
        style.fill = page_colour::white; style.outlined = false;
        return style;
    }
};

// A rectangle in PSP pixels.
struct PageBox {
    float x0{}, y0{}, x1{}, y1{};
    [[nodiscard]] constexpr float width() const noexcept { return x1 - x0; }
    [[nodiscard]] constexpr float height() const noexcept { return y1 - y0; }
    [[nodiscard]] constexpr bool contains(float x, float y) const noexcept { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};
struct PagePoint { float x{}, y{}; };

// The width of a line as the game adds it up, in single precision: every advance but the last with the
// spacing, each as one product added to the sum, then the last glyph's whole width.
[[nodiscard]] inline float page_glyphs_width(std::span<const PageGlyph *const> glyphs, const PageTextStyle &style) {
    if (glyphs.empty()) return 0.0f;
    float total = 0.0f;
    for (std::size_t i = 0; i + 1u < glyphs.size(); ++i) {
        const float step = (glyphs[i]->advance + style.spacing) * style.scale;
        total = total + step;
    }
    const float last = (glyphs.back()->x_offset + glyphs.back()->width()) * style.scale;
    return total + last;
}
// The width of `text` in PSP pixels as it would be set. Code points the font lacks are left out, as in text().
[[nodiscard]] inline float page_text_width(const PageFont &font, std::string_view text, const PageTextStyle &style) {
    std::vector<const PageGlyph *> glyphs;
    while (!text.empty())
        if (const PageGlyph *glyph = font.find(page_next_code(text))) glyphs.push_back(glyph);
    return page_glyphs_width(glyphs, style);
}

// ---- The list ----------------------------------------------------------------------------------------

// Builds one page's draw list. Draws come out in the order they are made; consecutive ones with the same
// texture and state share a run.
class PageDraw {
public:
    PageDraw() : list_(std::make_shared<vcs::GeGpuOverlay>()) {}
    // Nothing is drawn outside this rectangle from here on, until unclip().
    void clip(const PageBox &box) noexcept { clipped_ = true; clip_ = box; }
    void unclip() noexcept { clipped_ = false; }

    // Four corners in the order top left, top right, bottom right, bottom left, with the texture's part
    // `uv` (fractions; x0, y0 at the top left corner). `texture` 0: the colour alone.
    void quad(std::uint64_t texture, const std::array<PagePoint, 4> &corners, const PageBox &uv, PageColour colour,
              std::uint8_t flags = 0u, std::uint8_t alpha_reference = 0u) {
        const std::uint32_t rgba = colour.pack();
        const vcs::GeGpuOverlayVertex v[4]{{corners[0].x, corners[0].y, uv.x0, uv.y0, rgba}, {corners[1].x, corners[1].y, uv.x1, uv.y0, rgba},
                                           {corners[2].x, corners[2].y, uv.x1, uv.y1, rgba}, {corners[3].x, corners[3].y, uv.x0, uv.y1, rgba}};
        auto &vertices = begin(texture, flags, alpha_reference, 6u);
        vertices.insert(vertices.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
        ++quads_;
    }
    // A plain rectangle.
    void rect(const PageBox &box, PageColour colour) { quad(0u, corners(box), {}, colour); }
    // A plain triangle.
    void triangle(PagePoint a, PagePoint b, PagePoint c, PageColour colour) {
        const std::uint32_t rgba = colour.pack();
        auto &vertices = begin(0u, 0u, 0u, 3u);
        vertices.insert(vertices.end(), {{a.x, a.y, 0.0f, 0.0f, rgba}, {b.x, b.y, 0.0f, 0.0f, rgba}, {c.x, c.y, 0.0f, 0.0f, rgba}});
    }
    // A whole texture (or the part `uv` of it) in a rectangle: a background, the logo, a button sprite.
    void sprite(std::uint64_t texture, const PageBox &box, PageColour colour = {}, const PageBox &uv = {0.0f, 0.0f, 1.0f, 1.0f},
                std::uint8_t flags = 0u) {
        quad(texture, corners(box), uv, colour, flags);
    }
    // One picture of the font (a button, a lock) fitted into `box` by its height and centred in it, with
    // its outline twin underneath: for a legend whose 32 x 32 sprite is not in the cache. Returns what it covers.
    // `from` and `to` (fractions of the picture's width) draw a part of it in its own place: the left half of
    // one D-pad picture and the right half of another make a D-pad with both arms lit.
    PageBox glyph(const PageFont &font, char32_t code, const PageBox &box, PageColour tint = {}, float from = 0.0f, float to = 1.0f) {
        const PageGlyph *glyph = font.find(code);
        if (!glyph) return {};
        const float rows = font.cell_height() - 1.0f, scale = box.height() / rows, width = glyph->width() * scale;
        const float left = box.x0 + (box.width() - width) * 0.5f;
        const PageBox place{left + width * from, box.y0, left + width * to, box.y1};
        const PageBox uv{glyph->u + glyph->u_width * from, glyph->v, glyph->u + glyph->u_width * to, glyph->v + rows / page_font_sheet_height};
        quad(page_texture::font_outline, corners(place), uv, tint, 0u, text_alpha_reference);
        quad(page_texture::font_fill, corners(place), uv, tint, 0u, text_alpha_reference);
        return place;
    }
    // Sets `text` (UTF-8) at the box position (x, y), which is what the game's scene data calls a text
    // box's position. Returns the rectangle of the fill copy before any tilt (for the pointer).
    PageBox text(const PageFont &font, std::string_view text, float x, float y, const PageTextStyle &style = {}) {
        std::vector<const PageGlyph *> glyphs;
        for (auto rest = text; !rest.empty();)
            if (const PageGlyph *glyph = font.find(page_next_code(rest))) glyphs.push_back(glyph);
        if (glyphs.empty()) return {x, y, x, y};
        const float scale = style.scale, rows = font.cell_height() - 1.0f;
        const float total = page_glyphs_width(glyphs, style);
        const float radians = style.angle * 3.14159265f / 180.0f, sine = std::sin(radians), cosine = std::cos(radians);
        // The pen is a single-precision sum that grows by one advance per glyph, and every corner is cut down
        // to a whole pixel from it (see page_text_width): that is the game's arithmetic, and only with it do
        // the glyphs of a line sit in the columns the game puts them in. A copy's offset (the shadow's two
        // pixels) is added to the corners after they are cut: the game's shadow is the fill's rectangles moved.
        const float pivot_x = x + style.width * 0.5f, pivot_y = y + style.height * 0.5f;
        const auto copy = [&](std::uint64_t sheet, float dx, float dy, PageColour colour, bool pictures_keep_colours) {
            const PageColour picture{style.picture_tint, style.picture_tint, style.picture_tint, colour.a};
            const auto place = [&](float px, float py) {
                if (style.angle != 0.0f) {
                    const float rx = px - pivot_x, ry = py - pivot_y;
                    const float turned_x = pivot_x + rx * cosine - ry * sine, turned_y = pivot_y + rx * sine + ry * cosine;
                    px = turned_x; py = turned_y;
                }
                return style.whole_pixels ? PagePoint{std::floor(px) + dx, std::floor(py) + dy} : PagePoint{px + dx, py + dy};
            };
            float pen = style.align == PageAlign::Right ? x + style.width - total : style.align == PageAlign::Centre ? x + (style.width - total) * 0.5f : x;
            for (const PageGlyph *glyph : glyphs) {
                const float left = pen + glyph->x_offset * scale, right = left + glyph->width() * scale;
                const float top = y + glyph->y_offset * scale - style.lift, bottom = top + rows * scale;
                quad(sheet, {place(left, top), place(right, top), place(right, bottom), place(left, bottom)},
                     {glyph->u, glyph->v, glyph->u + glyph->u_width, glyph->v + rows / page_font_sheet_height},
                     glyph->picture && pictures_keep_colours ? picture : colour, 0u, text_alpha_reference);
                const float step = (glyph->advance + style.spacing) * scale;
                pen = pen + step;
            }
        };
        // A picture's shadow is its shape in the shadow colour; its outline twin and the picture itself keep their colours.
        if (style.shadowed) copy(page_texture::font_fill, style.shadow_x, style.shadow_y, style.shadow, false);
        if (style.outlined) copy(page_texture::font_outline, 0.0f, 0.0f, style.outline, true);
        copy(page_texture::font_fill, 0.0f, 0.0f, style.fill, true);
        const float start = style.align == PageAlign::Right ? x + style.width - total : style.align == PageAlign::Centre ? x + (style.width - total) * 0.5f : x;
        const float top = y + glyphs.front()->y_offset * scale - style.lift;
        return {start, top, start + total, top + rows * scale};
    }

    [[nodiscard]] std::size_t quads() const noexcept { return quads_; }
    [[nodiscard]] bool empty() const noexcept { return list_->runs.empty(); }
    // The finished list; this object starts a new one.
    [[nodiscard]] std::shared_ptr<const vcs::GeGpuOverlay> finish() {
        auto done = std::move(list_);
        list_ = std::make_shared<vcs::GeGpuOverlay>();
        quads_ = 0u;
        return done;
    }
    // The game's alpha test for text: greater than 12.
    static constexpr std::uint8_t text_alpha_reference = 12u;
    [[nodiscard]] static constexpr std::array<PagePoint, 4> corners(const PageBox &box) noexcept {
        return {{{box.x0, box.y0}, {box.x1, box.y0}, {box.x1, box.y1}, {box.x0, box.y1}}};
    }
private:
    // The run the next `count` vertices go into.
    std::vector<vcs::GeGpuOverlayVertex> &begin(std::uint64_t texture, std::uint8_t flags, std::uint8_t alpha_reference, std::uint32_t count) {
        auto &runs = list_->runs;
        const bool same = !runs.empty() && runs.back().texture == texture && runs.back().flags == flags &&
            runs.back().alpha_reference == alpha_reference && runs.back().clipped == clipped_ &&
            (!clipped_ || (runs.back().clip[0] == clip_.x0 && runs.back().clip[1] == clip_.y0 && runs.back().clip[2] == clip_.x1 && runs.back().clip[3] == clip_.y1));
        if (!same) {
            vcs::GeGpuOverlayRun run;
            run.texture = texture; run.flags = flags; run.alpha_reference = alpha_reference; run.clipped = clipped_;
            run.clip[0] = clip_.x0; run.clip[1] = clip_.y0; run.clip[2] = clip_.x1; run.clip[3] = clip_.y1;
            run.first_vertex = static_cast<std::uint32_t>(list_->vertices.size());
            runs.push_back(run);
        }
        runs.back().vertex_count += count;
        return list_->vertices;
    }
    std::shared_ptr<vcs::GeGpuOverlay> list_;
    PageBox clip_{};
    bool clipped_{};
    std::size_t quads_{};
};
}
