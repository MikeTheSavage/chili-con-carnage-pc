// On-screen button prompts: the glyph painter (every picture of every style at scales 1, 2 and 4, written
// as PNG sheets for eyes and checked for size, for staying in its box and for not being empty), the
// pictures of one style as overlays of the game's seven textures, their composition over a picture, the
// overlay table the renderer reads, and the rules that pick the style and the binding to show.
//
//   chili_prompt_glyphs_test [output folder]
//
// No game data is used: the pictures are composed over made-up sheets.
#include "prompt_style.hpp"
#include "prompt_glyphs.hpp"
#include "gpu/ge_gpu_log.hpp"
#include "gpu/ge_texture_mods.hpp"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>

// ge_texture_mods.cpp reports through the renderer's log, which this tool does not link.
namespace vcs {
void runtime_log_error(std::string_view category, std::string_view message) {
    std::fprintf(stderr, "[gpu] %.*s: %.*s\n", static_cast<int>(category.size()), category.data(), static_cast<int>(message.size()), message.data());
}
}

namespace {
using namespace chili;
unsigned checks, failures;
// CHILI_TEST_KEEP_GOING=1: a check that fails is printed and the run goes on, so one run shows every check that fails.
const bool keep_going = [] { const char *value = std::getenv("CHILI_TEST_KEEP_GOING"); return value && *value == '1'; }();
void require(bool ok, const std::string &what) {
    ++checks;
    if (ok) return;
    if (!keep_going) throw std::runtime_error(what);
    ++failures;
    std::cerr << "FAILED CHECK: " << what << '\n';
}
double ms_since(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}
void write_png(const std::filesystem::path &path, std::uint32_t width, std::uint32_t height, const std::uint8_t *rgba) {
    std::string error;
    const bool written = vcs::ge_texture_write_png(path, width, height,
        std::span<const std::byte>(reinterpret_cast<const std::byte *>(rgba), static_cast<std::size_t>(width) * height * 4u), error);
    require(written, "cannot write " + path.string() + ": " + error);
}
std::size_t inked(const GlyphPicture &picture, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1) {
    std::size_t count = 0u;
    for (std::uint32_t y = y0; y < y1 && y < picture.height; ++y)
        for (std::uint32_t x = x0; x < x1 && x < picture.width; ++x)
            if (picture.rgba[(static_cast<std::size_t>(y) * picture.width + x) * 4u + 3u] != 0u) ++count;
    return count;
}
Glyph mouse_glyph(MousePart part) { Glyph g; g.kind = GlyphKind::Mouse; g.index = static_cast<std::uint8_t>(part); return g; }
Glyph pad_glyph(GlyphKind kind, PadLook look, std::uint8_t index = 0u, std::uint8_t directions = 0u) {
    Glyph g; g.kind = kind; g.look = look; g.index = index; g.directions = directions; return g;
}
Glyph cluster(int up, int left, int down, int right) {
    Glyph g; g.kind = GlyphKind::KeyCluster;
    g.keys = {key_glyph(up).keys[0], key_glyph(left).keys[0], key_glyph(down).keys[0], key_glyph(right).keys[0]};
    return g;
}
// With the gamepad code's separate PlayStation layout ([Input.Gamepad.PlayStation], PcInputConfig with
// playstation_named): the PlayStation pictures follow that section, the Xbox ones the common table. A
// configuration without the section's fields has nothing to check here.
template <typename Config>
void separate_playstation_layout(Config config) {
    if constexpr (requires { config.playstation_named; }) {
        (void)config.apply("[Input.Gamepad.PlayStation]\nX=Square\nY=Triangle\nLB=L\n");
        require(prompt_glyph(config, PromptStyle::PlayStation, PadTarget::Triangle) == pad_glyph(GlyphKind::PlayStationFace, PadLook::PlayStation, ps_triangle),
            "separate PlayStation layout: Triangle is on Triangle");
        require(prompt_glyph(config, PromptStyle::PlayStation, PadTarget::Square) == pad_glyph(GlyphKind::PlayStationFace, PadLook::PlayStation, ps_square),
            "separate PlayStation layout: Square is on Square, before R2");
        require(prompt_glyph(config, PromptStyle::PlayStation, PadTarget::L) == pad_glyph(GlyphKind::Stick, PadLook::PlayStation, 0u) ||
            prompt_glyph(config, PromptStyle::PlayStation, PadTarget::L) == pad_glyph(GlyphKind::Shoulder, PadLook::PlayStation, 0u), "separate PlayStation layout: L");
        require(prompt_glyph(config, PromptStyle::Xbox, PadTarget::Triangle) == pad_glyph(GlyphKind::XboxFace, PadLook::Xbox, xbox_x),
            "the Xbox pictures keep the common table");
        std::printf("separate PlayStation layout: checked\n");
    }
}

// Every picture of a style.
std::vector<Glyph> glyphs_of(PromptStyle style) {
    std::vector<Glyph> list;
    if (style == PromptStyle::Keyboard) {
        for (int key = 'A'; key <= 'Z'; ++key) list.push_back(key_glyph(key));
        for (int key = '0'; key <= '9'; ++key) list.push_back(key_glyph(key));
        for (const int key : {VK_SPACE, VK_RETURN, VK_LSHIFT, VK_LCONTROL, VK_LMENU, VK_TAB, VK_ESCAPE, VK_BACK, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
                 VK_F1, VK_F12, VK_INSERT, VK_DELETE, VK_HOME, VK_END, VK_PRIOR, VK_NEXT, VK_CAPITAL, VK_NUMPAD7, VK_OEM_COMMA, VK_OEM_PERIOD,
                 VK_OEM_2, VK_OEM_1, VK_OEM_7, VK_OEM_4, VK_OEM_6, VK_OEM_5, VK_OEM_MINUS, VK_OEM_PLUS, VK_OEM_3})
            list.push_back(key_glyph(key));
        list.push_back(cluster('W', 'A', 'S', 'D'));
        list.push_back(cluster(VK_UP, VK_LEFT, VK_DOWN, VK_RIGHT));
        for (const MousePart part : {MousePart::Left, MousePart::Right, MousePart::Middle, MousePart::WheelUp, MousePart::WheelDown,
                 MousePart::Side4, MousePart::Side5, MousePart::Move})
            list.push_back(mouse_glyph(part));
        list.push_back(Glyph{});
        return list;
    }
    const PadLook look = style == PromptStyle::PlayStation ? PadLook::PlayStation : PadLook::Xbox;
    for (std::uint8_t which = 0u; which < 4u; ++which)
        list.push_back(pad_glyph(look == PadLook::PlayStation ? GlyphKind::PlayStationFace : GlyphKind::XboxFace, look, which));
    for (std::uint8_t side = 0u; side < 2u; ++side) list.push_back(pad_glyph(GlyphKind::Shoulder, look, side));
    for (std::uint8_t side = 0u; side < 2u; ++side) list.push_back(pad_glyph(GlyphKind::Trigger, look, side));
    for (std::uint8_t side = 0u; side < 2u; ++side) list.push_back(pad_glyph(GlyphKind::Stick, look, side));
    for (const std::uint8_t directions : {glyph_all_directions, glyph_up, glyph_right, glyph_down, glyph_left})
        list.push_back(pad_glyph(GlyphKind::Stick, look, 0u, directions));
    list.push_back(pad_glyph(GlyphKind::Stick, look, 1u, glyph_all_directions));
    for (const std::uint8_t directions : {glyph_up, glyph_right, glyph_down, glyph_left, std::uint8_t{0u}, static_cast<std::uint8_t>(glyph_left | glyph_right)})
        list.push_back(pad_glyph(GlyphKind::Dpad, look, 0u, directions));
    list.push_back(pad_glyph(GlyphKind::Menu, look));
    list.push_back(pad_glyph(GlyphKind::View, look));
    list.push_back(Glyph{});
    return list;
}
const char *file_name(PromptStyle style) {
    return style == PromptStyle::Keyboard ? "keyboard" : style == PromptStyle::Xbox ? "xbox" : "playstation";
}

// One sheet: every glyph three times, in a font cell 20 PSP pixels wide, in one 28 wide, and as a sprite.
void sheet(GlyphPainter &painter, PromptStyle style, unsigned scale, const std::filesystem::path &folder) {
    const auto glyphs = glyphs_of(style);
    constexpr std::uint32_t columns = 14u, pitch = 34u, block = 100u;
    const std::uint32_t blocks = static_cast<std::uint32_t>((glyphs.size() + columns - 1u) / columns);
    const std::uint32_t width = columns * pitch * scale, height = blocks * block * scale;
    std::string error;
    const auto started = std::chrono::steady_clock::now();
    require(painter.begin(width, height, error), "begin: " + error);
    struct Box { std::uint32_t x0, y0, x1, y1; };
    std::vector<Box> boxes;
    const float unit = static_cast<float>(scale);
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        const std::uint32_t left = static_cast<std::uint32_t>(i % columns) * pitch, top = static_cast<std::uint32_t>(i / columns) * block;
        const auto draw = [&](float x, float y, float w, float h) {
            painter.draw(glyphs[i], GlyphBox{x * unit, y * unit, w * unit, h * unit});
            boxes.push_back({static_cast<std::uint32_t>(x * unit), static_cast<std::uint32_t>(y * unit),
                static_cast<std::uint32_t>((x + w) * unit + 0.999f), static_cast<std::uint32_t>((y + h) * unit + 0.999f)});
        };
        // As in the game's font: a pixel in from the sides of the cell, rows 4.5 to 24.5 of its 30.
        draw(static_cast<float>(left) + prompt_cell_inset, static_cast<float>(top) + prompt_cell_top, 20.0f - 2.0f * prompt_cell_inset, prompt_cell_rows);
        draw(static_cast<float>(left) + prompt_cell_inset, static_cast<float>(top + 32u) + prompt_cell_top, 28.0f - 2.0f * prompt_cell_inset, prompt_cell_rows);
        draw(static_cast<float>(left) + 1.5f, static_cast<float>(top + 66u) + 1.5f, 29.0f, 29.0f);
    }
    GlyphPicture picture;
    require(painter.end(picture, error), "end: " + error);
    const double paint_ms = ms_since(started);
    require(picture.width == width && picture.height == height && picture.rgba.size() == static_cast<std::size_t>(width) * height * 4u,
        "sheet size");
    // Every picture shows something, and nothing is drawn outside the boxes.
    std::vector<std::uint8_t> allowed(static_cast<std::size_t>(width) * height);
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        const Box &box = boxes[i];
        const std::size_t ink = inked(picture, box.x0, box.y0, box.x1, box.y1);
        const std::size_t area = static_cast<std::size_t>(box.x1 - box.x0) * (box.y1 - box.y0);
        require(ink * 8u >= area, std::string(file_name(style)) + " glyph " + std::to_string(i / 3u) + " is nearly empty at scale " + std::to_string(scale));
        for (std::uint32_t y = box.y0; y < box.y1; ++y) for (std::uint32_t x = box.x0; x < box.x1; ++x) allowed[static_cast<std::size_t>(y) * width + x] = 1u;
    }
    std::size_t outside = 0u;
    for (std::size_t i = 0; i < allowed.size(); ++i) if (!allowed[i] && picture.rgba[i * 4u + 3u] != 0u) ++outside;
    require(outside == 0u, std::string(file_name(style)) + ": " + std::to_string(outside) + " pixels outside the glyph boxes at scale " + std::to_string(scale));
    const auto path = folder / (std::string("glyphs_") + file_name(style) + "_" + std::to_string(scale) + "x.png");
    write_png(path, picture.width, picture.height, picture.rgba.data());
    std::printf("sheet %s: %zu glyphs, %ux%u, painted in %.1f ms\n", path.filename().string().c_str(), glyphs.size(), width, height, paint_ms);
}

// A made-up font sheet: white bars where letters would be, and something in every cell a button picture
// goes into, so a cell that was not cleared shows.
std::vector<std::byte> made_up_sheet(std::uint32_t width, std::uint32_t height) {
    std::vector<std::byte> sheet(static_cast<std::size_t>(width) * height * 4u, std::byte{0});
    const auto put = [&](std::uint32_t x, std::uint32_t y, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
        auto *px = sheet.data() + (static_cast<std::size_t>(y) * width + x) * 4u;
        px[0] = std::byte{r}; px[1] = std::byte{g}; px[2] = std::byte{b}; px[3] = std::byte{a};
    };
    for (std::uint32_t y = 0u; y < height; ++y)
        for (std::uint32_t x = 0u; x < width; ++x) {
            const std::uint32_t sx = x * 256u / width, sy = y * 512u / height;
            if (sy % 30u >= 8u && sy % 30u < 22u && sx % 21u >= 4u && sx % 21u < 15u) put(x, y, 255u, 255u, 255u, 255u);
            else if ((sx + sy) % 16u == 0u) put(x, y, 255u, 0u, 255u, 128u);
        }
    return sheet;
}
const std::uint8_t *level0(const vcs::GeTextureReplacement &picture) { return reinterpret_cast<const std::uint8_t *>(picture.rgba8.data()); }

void overlays(GlyphPainter &painter, const std::filesystem::path &folder) {
    const auto dual = PcInputConfig::defaults(InputScheme::DualAnalog);
    const auto sheet256 = made_up_sheet(256u, 512u);
    require(prompt_sheet_scale(1u) == 1u && prompt_sheet_scale(2u) == 2u && prompt_sheet_scale(3u) == 2u && prompt_sheet_scale(4u) == 4u &&
        prompt_sheet_scale(7u) == 4u && prompt_sheet_scale(8u) == 4u, "font sheets at one, two or four times their size");
    require(prompt_level_scales(4u) == std::vector<unsigned>{4u, 2u, 1u} && prompt_level_scales(1u) == std::vector<unsigned>{1u} &&
        prompt_level_scales(6u) == std::vector<unsigned>{6u} && prompt_level_scales(8u).size() == 4u, "a picture per level of a power of two");
    // Bytes of a cell's pictures: every level a scale has.
    const auto cell_bytes = [](std::uint32_t width, std::uint32_t height, unsigned scale) {
        std::size_t bytes = 0u;
        for (const unsigned level : prompt_level_scales(scale)) bytes += static_cast<std::size_t>(width) * level * height * level * 4u;
        return bytes;
    };
    for (const PromptStyle style : {PromptStyle::Keyboard, PromptStyle::Xbox, PromptStyle::PlayStation}) {
        for (const unsigned scale : {1u, 2u, 3u, 4u, 8u}) {
            PromptOverlays set;
            std::string error;
            const unsigned sheet_scale = prompt_sheet_scale(scale);
            const auto started = std::chrono::steady_clock::now();
            require(paint_prompt_overlays(painter, dual, style, sheet_scale, scale, set, error), "paint: " + error);
            const double paint_ms = ms_since(started);
            for (std::size_t sheet_index = 0; sheet_index < 2u; ++sheet_index) {
                const auto &overlay = *set[sheet_index];
                require(overlay.width == 256u && overlay.height == 512u && overlay.scale == sheet_scale, "sheet overlay shape");
                require(overlay.cells.size() == prompt_font_cells.size(), "one cell per button picture");
                for (std::size_t i = 0; i < overlay.cells.size(); ++i) {
                    const auto &cell = overlay.cells[i];
                    require(cell.x == prompt_font_cells[i].x && cell.y == prompt_font_cells[i].y && cell.width == prompt_font_cells[i].width && cell.height == 30u, "cell place");
                    require(cell.picture_width == cell.width * sheet_scale && cell.picture_height == 30u * sheet_scale, "cell picture size");
                    require(cell.picture_levels == prompt_level_scales(sheet_scale).size() && cell.rgba8.size() == cell_bytes(cell.width, 30u, sheet_scale), "cell picture levels");
                    std::size_t ink = 0u;
                    for (std::size_t at = 3u; at < cell.rgba8.size(); at += 4u) if (cell.rgba8[at] != std::byte{0}) ++ink;
                    require(ink * 10u >= cell.rgba8.size() / 4u, "cell picture is nearly empty");
                }
            }
            // The outline twin covers the fill and reaches further, in every level.
            for (std::size_t i = 0; i < prompt_font_cells.size(); ++i) {
                const auto &fill = set[0]->cells[i].rgba8, &outline = set[1]->cells[i].rgba8;
                std::size_t fill_ink = 0u, outline_ink = 0u;
                for (std::size_t at = 3u; at < fill.size(); at += 4u) {
                    require(outline[at] >= fill[at], "the outline covers the fill");
                    if (fill[at] != std::byte{0}) ++fill_ink;
                    if (outline[at] != std::byte{0}) ++outline_ink;
                }
                require(outline_ink > fill_ink, "the outline is larger than the fill");
            }
            for (std::size_t i = 2u; i < set.size(); ++i) {
                const auto &overlay = *set[i];
                require(overlay.width == 32u && overlay.height == 32u && overlay.scale == scale && overlay.cells.size() == 1u, "sprite overlay shape");
                const auto &cell = overlay.cells[0];
                require(cell.x == 0u && cell.y == 0u && cell.width == 32u && cell.height == 32u, "sprite cell");
                require(cell.picture_width == 32u * scale && cell.picture_height == 32u * scale && cell.rgba8.size() == cell_bytes(32u, 32u, scale), "sprite picture size");
                std::size_t ink = 0u;
                for (std::size_t at = 3u; at < cell.rgba8.size(); at += 4u) if (cell.rgba8[at] != std::byte{0}) ++ink;
                require(ink * 4u >= cell.rgba8.size() / 4u, "sprite picture is nearly empty");
            }
            // Composition over the made-up sheet: the game's size times the scale, a level per halving down to
            // the game's own size.
            const auto compose_started = std::chrono::steady_clock::now();
            const auto fill = vcs::ge_texture_compose_overlay(*set[0], 256u, 512u, sheet256);
            const double compose_ms = ms_since(compose_started);
            const auto outline = vcs::ge_texture_compose_overlay(*set[1], 256u, 512u, sheet256);
            require(fill && outline, "composed");
            require(fill->width == 256u * sheet_scale && fill->height == 512u * sheet_scale, "composed size");
            const auto sheet_scales = prompt_level_scales(sheet_scale);
            require(fill->exact_levels && fill->mip_levels == sheet_scales.size() && fill->rgba8.size() == cell_bytes(256u, 512u, sheet_scale), "composed levels");
            // In every level, inside a cell: the cell's picture for that level and nothing of what was there.
            std::size_t level_offset = 0u, cell_offset = 0u;
            const auto &cell = set[0]->cells[3];
            for (const unsigned level_scale : sheet_scales) {
                const std::uint32_t w = 256u * level_scale;
                for (std::uint32_t y = 0u; y < cell.height * level_scale; ++y)
                    require(std::memcmp(level0(*fill) + level_offset + ((static_cast<std::size_t>(cell.y) * level_scale + y) * w + static_cast<std::size_t>(cell.x) * level_scale) * 4u,
                        cell.rgba8.data() + cell_offset + static_cast<std::size_t>(y) * cell.width * level_scale * 4u, static_cast<std::size_t>(cell.width) * level_scale * 4u) == 0,
                        "the cell's picture is in the sheet, level by level");
                level_offset += static_cast<std::size_t>(w) * 512u * level_scale * 4u;
                cell_offset += static_cast<std::size_t>(cell.width) * level_scale * cell.height * level_scale * 4u;
            }
            // The last level is the game's own sheet outside the cells, texel for texel.
            {
                const std::uint8_t *last = level0(*fill) + fill->rgba8.size() - sheet256.size();
                std::size_t differing = 0u;
                for (std::uint32_t y = 0u; y < 512u; ++y) for (std::uint32_t x = 0u; x < 256u; ++x) {
                    bool in_cell = false;
                    for (const auto &c : prompt_font_cells) in_cell = in_cell || (x >= c.x && x < std::uint32_t{c.x} + c.width && y >= c.y && y < std::uint32_t{c.y} + 30u);
                    if (!in_cell && std::memcmp(last + (static_cast<std::size_t>(y) * 256u + x) * 4u, sheet256.data() + (static_cast<std::size_t>(y) * 256u + x) * 4u, 4u) != 0) ++differing;
                }
                require(differing == 0u, "the sheet outside the cells is the game's own in the last level");
            }
            if (sheet_scale != 1u) {
                // Enlarged: a texel in the middle of a white bar is still white, one in an empty place still empty.
                const auto at = [&](std::uint32_t sx, std::uint32_t sy) { return level0(*fill) + ((static_cast<std::size_t>(sy) * sheet_scale + sheet_scale / 2u) * fill->width + static_cast<std::size_t>(sx) * sheet_scale + sheet_scale / 2u) * 4u; };
                require(at(9u, 14u)[3] == 255u && at(9u, 14u)[0] == 255u, "enlarged sheet keeps its letters");
                require(at(1u, 3u)[3] == 0u, "enlarged sheet keeps its empty places");
            }
            if (scale <= 4u && scale != 3u) {
                write_png(folder / (std::string("sheet_") + file_name(style) + "_" + std::to_string(scale) + "x_fill.png"), fill->width, fill->height, level0(*fill));
                write_png(folder / (std::string("sheet_") + file_name(style) + "_" + std::to_string(scale) + "x_outline.png"), outline->width, outline->height, level0(*outline));
            }
            // The five sprites side by side, over a made-up sprite. A scale that is no power of two has the
            // smaller levels down to 1x1, filtered from the first.
            const std::uint32_t side = 32u * scale;
            std::vector<std::uint8_t> strip(static_cast<std::size_t>(side) * 5u * side * 4u);
            const std::vector<std::byte> grey(32u * 32u * 4u, std::byte{0x80});
            for (std::size_t i = 0; i < 5u; ++i) {
                const auto sprite = vcs::ge_texture_compose_overlay(*set[2u + i], 32u, 32u, grey);
                require(sprite && sprite->width == side && sprite->height == side, "composed sprite size");
                require(sprite->exact_levels == ((scale & (scale - 1u)) == 0u), "exact levels for a power of two");
                if (sprite->exact_levels) require(sprite->mip_levels == prompt_level_scales(scale).size() && sprite->rgba8 == set[2u + i]->cells[0].rgba8, "a sprite is its pictures, level by level");
                else require(sprite->mip_levels > 5u && sprite->rgba8.size() > static_cast<std::size_t>(side) * side * 4u, "a sprite at another scale has filtered levels");
                for (std::uint32_t y = 0u; y < side; ++y)
                    std::memcpy(strip.data() + (static_cast<std::size_t>(y) * side * 5u + i * side) * 4u, level0(*sprite) + static_cast<std::size_t>(y) * side * 4u, static_cast<std::size_t>(side) * 4u);
                require(std::memcmp(level0(*sprite), set[2u + i]->cells[0].rgba8.data(), static_cast<std::size_t>(side) * side * 4u) == 0, "a sprite is the whole picture");
            }
            if (scale != 3u) write_png(folder / (std::string("sprites_") + file_name(style) + "_" + std::to_string(scale) + "x.png"), side * 5u, side, strip.data());
            std::printf("overlays %s scale %u: painted in %.1f ms, a font sheet composed in %.2f ms (%ux%u, %.1f MB with its levels)\n", file_name(style), scale,
                paint_ms, compose_ms, fill->width, fill->height, static_cast<double>(fill->rgba8.size()) / (1024.0 * 1024.0));
        }
    }
    // What an 8x sheet would cost, for the notes: not what the game does.
    {
        PromptOverlays set;
        std::string error;
        require(paint_prompt_overlays(painter, dual, PromptStyle::Xbox, 8u, 8u, set, error), "paint 8x: " + error);
        const auto started = std::chrono::steady_clock::now();
        const auto fill = vcs::ge_texture_compose_overlay(*set[0], 256u, 512u, sheet256);
        std::printf("for comparison, a font sheet at 8x: composed in %.2f ms (%.1f MB with its levels)\n", ms_since(started),
            fill ? static_cast<double>(fill->rgba8.size()) / (1024.0 * 1024.0) : 0.0);
    }
    // A mod's larger picture as the base: the composed picture keeps its size and the cells are fitted to it.
    {
        PromptOverlays set;
        std::string error;
        require(paint_prompt_overlays(painter, dual, PromptStyle::Xbox, 2u, 2u, set, error), "paint: " + error);
        const auto big = made_up_sheet(768u, 1536u);
        const auto composed = vcs::ge_texture_compose_overlay(*set[0], 768u, 1536u, big);
        require(composed && composed->width == 768u && composed->height == 1536u && !composed->exact_levels && composed->mip_levels > 5u, "a larger base keeps its size and gets filtered levels");
        const auto &cell = set[0]->cells[0];
        std::size_t ink = 0u;
        for (std::uint32_t y = cell.y * 3u; y < (cell.y + cell.height) * 3u; ++y)
            for (std::uint32_t x = cell.x * 3u; x < (cell.x + cell.width) * 3u; ++x) if (level0(*composed)[(static_cast<std::size_t>(y) * 768u + x) * 4u + 3u] != 0u) ++ink;
        const std::size_t area = static_cast<std::size_t>(cell.width) * cell.height * 9u;
        require(ink * 100u >= area * 20u && ink * 100u <= area * 80u, "the cell is painted into the larger base");
        require(level0(*composed)[(static_cast<std::size_t>(cell.y) * 3u * 768u + cell.x * 3u) * 4u + 3u] == 0u, "and its corner is empty");
        // A sprite replaced by a mod at 64x64 and a prompt picture painted at 32: the result is 64x64.
        const std::vector<std::byte> mod_sprite(64u * 64u * 4u, std::byte{0x40});
        PromptOverlays small_set;
        require(paint_prompt_overlays(painter, dual, PromptStyle::Xbox, 1u, 1u, small_set, error), "paint: " + error);
        const auto sprite = vcs::ge_texture_compose_overlay(*small_set[2], 64u, 64u, mod_sprite);
        require(sprite && sprite->width == 64u && sprite->height == 64u, "a larger sprite keeps its size");
        // A picture of another size than the overlay was measured on is refused by the table (below), not here.
    }
}

// The font's padlocks and chili, drawn new for a mod's larger font sheet (prompt_style.hpp, PromptMarks).
void marks(GlyphPainter &painter, const std::filesystem::path &folder) {
    const auto dual = PcInputConfig::defaults(InputScheme::DualAnalog);
    std::string error;
    PromptMarks made;
    require(paint_prompt_marks(painter, prompt_mark_scale, made, error), "marks: " + error);
    require(made.fill.size() == prompt_mark_cells.size() && made.outline.size() == prompt_mark_cells.size() && !made.empty(), "a fill and an outline picture per mark");
    const unsigned k = prompt_mark_scale;
    // A picture's pixel at a place of the cell given in PSP pixels.
    const auto pixel = [&](const vcs::GeTextureOverlayCell &cell, float x, float y) {
        return reinterpret_cast<const std::uint8_t *>(cell.rgba8.data()) + (static_cast<std::size_t>(y * static_cast<float>(k)) * cell.picture_width + static_cast<std::size_t>(x * static_cast<float>(k))) * 4u;
    };
    std::vector<std::uint8_t> strip;
    std::uint32_t strip_width = 0u;
    for (std::size_t i = 0; i < made.fill.size(); ++i) {
        const auto &fill = made.fill[i], &outline = made.outline[i];
        const auto &cell = prompt_mark_cells[i];
        require(fill.x == cell.x && fill.y == cell.y && fill.width == cell.width && fill.height == 30u && outline.x == cell.x && outline.width == cell.width, "a mark's cell is the font's");
        require(fill.picture_width == cell.width * k && fill.picture_height == 30u * k && fill.picture_levels == 4u, "a mark is painted at eight times the cell, with three smaller pictures");
        const std::size_t level0_pixels = static_cast<std::size_t>(fill.picture_width) * fill.picture_height;
        std::size_t ink = 0u, edge = 0u, soft = 0u;
        for (std::size_t p = 0; p < level0_pixels; ++p) {
            const auto a = static_cast<std::uint8_t>(fill.rgba8[p * 4u + 3u]), b = static_cast<std::uint8_t>(outline.rgba8[p * 4u + 3u]);
            if (a) ++ink;
            if (a > 0u && a < 255u) ++soft;
            if (b > a) ++edge;
            require(b >= a, "a mark's outline covers its fill");
        }
        require(ink * 100u >= level0_pixels * 14u && ink * 100u <= level0_pixels * 75u, "mark " + std::to_string(i) + " covers a sensible share of its cell: " + std::to_string(ink * 100u / level0_pixels) + " %");
        require(edge > 0u && soft * 4u < ink, "a mark has an outline round it, and its own edge is a thin line of soft pixels (it is drawn, not enlarged): " + std::to_string(soft) + " of " + std::to_string(ink));
        // Nothing in the cell's corners, nothing in its last row (the game draws 29 of 30), nothing at its side columns.
        require(pixel(outline, 0.2f, 0.2f)[3] == 0u && pixel(outline, static_cast<float>(cell.width) - 0.2f, 0.2f)[3] == 0u && pixel(outline, static_cast<float>(cell.width) * 0.5f, 29.5f)[3] == 0u &&
                pixel(outline, 0.1f, 15.0f)[3] == 0u && pixel(outline, static_cast<float>(cell.width) - 0.1f, 15.0f)[3] == 0u, "a mark stays inside its cell");
        const std::uint32_t old = strip_width;
        strip_width += fill.picture_width;
        std::vector<std::uint8_t> wider(static_cast<std::size_t>(strip_width) * fill.picture_height * 8u, 0u);
        // The strip for a look: the outline twins above, the fills below, on the menu wall's brown.
        for (std::uint32_t y = 0; y < fill.picture_height * 2u; ++y)
            for (std::uint32_t x = 0; x < strip_width; ++x) {
                std::uint8_t *to = wider.data() + (static_cast<std::size_t>(y) * strip_width + x) * 4u;
                if (x < old) { std::memcpy(to, strip.data() + (static_cast<std::size_t>(y) * old + x) * 4u, 4u); continue; }
                const auto &from = y < fill.picture_height ? outline : fill;
                const auto *in = reinterpret_cast<const std::uint8_t *>(from.rgba8.data()) + (static_cast<std::size_t>(y % fill.picture_height) * fill.picture_width + (x - old)) * 4u;
                const unsigned a = in[3];
                const std::uint8_t wall[3]{0x8Au, 0x4Au, 0x1Eu};
                for (int c = 0; c < 3; ++c) to[c] = static_cast<std::uint8_t>((in[c] * a + wall[c] * (255u - a)) / 255u);
                to[3] = 255u;
            }
        strip = std::move(wider);
    }
    write_png(folder / "marks_8x.png", strip_width, 30u * k * 2u, strip.data());
    // The open lock (first), the closed one, the chili: gold in the locks' bodies, the keyhole dark, red in the chili.
    const auto gold = [](const std::uint8_t *p) { return p[3] == 255u && p[0] > 170u && p[1] > 110u && p[2] < 190u && p[0] > p[2] + 40u; };
    require(gold(pixel(made.fill[0], 6.0f, 20.0f)) && gold(pixel(made.fill[1], 6.0f, 20.0f)) && gold(pixel(made.fill[1], 20.0f, 24.0f)), "the locks' bodies are gold");
    { const auto *hole = pixel(made.fill[1], 13.0f, 18.5f); require(hole[3] == 255u && hole[0] < 110u && hole[1] < 70u, "the keyhole is dark"); }
    { const auto *red = pixel(made.fill[2], 12.5f, 17.5f); require(red[3] == 255u && red[0] > 130u && red[1] < 90u && red[2] < 110u, "the chili is red"); }
    require(made.fill[0].rgba8 != made.fill[1].rgba8, "the open lock is not the closed one");
    // The closed lock's shackle goes into the body on both sides; the open one's right leg ends above it.
    require(pixel(made.fill[1], 18.6f, 12.6f)[3] == 255u && pixel(made.fill[0], 18.6f, 12.6f)[3] == 0u && pixel(made.outline[0], 17.2f, 11.7f)[3] == 0u, "the open lock's right leg is out of the body");
    // In the overlays: after the button pictures in every style, and all the PSP style has.
    for (const PromptStyle style : {PromptStyle::Keyboard, PromptStyle::Xbox, PromptStyle::PlayStation, PromptStyle::Psp}) {
        PromptOverlays set;
        require(paint_prompt_overlays(painter, dual, style, 4u, 8u, set, error, &made), "paint with marks: " + error);
        const std::size_t buttons = style == PromptStyle::Psp ? 0u : prompt_font_cells.size();
        require(set[0] && set[1] && set[0]->cells.size() == buttons + 3u && set[1]->cells.size() == buttons + 3u, "the marks are in both sheets of every style");
        require(set[0]->cells.back().rgba8 == made.fill.back().rgba8 && set[1]->cells[buttons].rgba8 == made.outline.front().rgba8, "as they were painted");
        require((set[2] != nullptr) == (style != PromptStyle::Psp), "the PSP style has no sprite pictures");
        // Over a mod's sheet at eight times the game's size a mark is in its cell texel for texel; over the game's
        // own sheet (the mod's picture not decoded yet) the picture of half the size is.
        const auto big = made_up_sheet(2048u, 4096u);
        const auto composed = vcs::ge_texture_compose_overlay(*set[0], 2048u, 4096u, big);
        require(composed && composed->width == 2048u && composed->height == 4096u, "composed over a mod's sheet at its size");
        const auto &lock = set[0]->cells[buttons + 1u];
        bool same = true;
        for (std::uint32_t y = 0; same && y < lock.picture_height; ++y)
            same = std::memcmp(level0(*composed) + ((static_cast<std::size_t>(lock.y) * k + y) * 2048u + static_cast<std::size_t>(lock.x) * k) * 4u,
                               reinterpret_cast<const std::uint8_t *>(lock.rgba8.data()) + static_cast<std::size_t>(y) * lock.picture_width * 4u, static_cast<std::size_t>(lock.picture_width) * 4u) == 0;
        require(same, "the closed lock in the mod's sheet is the painted picture, texel for texel");
        const auto own_sheet = vcs::ge_texture_compose_overlay(*set[0], 256u, 512u, made_up_sheet(256u, 512u));
        require(own_sheet && own_sheet->width == 1024u && own_sheet->height == 2048u, "composed over the game's own sheet at four times its size");
        const std::size_t first_level = static_cast<std::size_t>(lock.picture_width) * lock.picture_height * 4u;
        same = true;
        for (std::uint32_t y = 0; same && y < lock.picture_height / 2u; ++y)
            same = std::memcmp(level0(*own_sheet) + ((static_cast<std::size_t>(lock.y) * 4u + y) * 1024u + static_cast<std::size_t>(lock.x) * 4u) * 4u,
                               reinterpret_cast<const std::uint8_t *>(lock.rgba8.data()) + first_level + static_cast<std::size_t>(y) * (lock.picture_width / 2u) * 4u, static_cast<std::size_t>(lock.picture_width / 2u) * 4u) == 0;
        require(same, "and in the game's own sheet at 4x the picture painted for that size");
    }
    // Without marks nothing changes: the PSP style has no overlay at all.
    PromptOverlays plain;
    require(paint_prompt_overlays(painter, dual, PromptStyle::Psp, 4u, 8u, plain, error) && !plain[0] && !plain[1], "the PSP style without marks paints nothing");
    std::printf("marks: %zu pictures at %ux in %s\n", made.fill.size(), k, (folder / "marks_8x.png").string().c_str());
}

// The renderer's path keeps the enlarged picture with its levels and paints only the cells on a change of
// style: the result must be the picture made from nothing, byte for byte, levels included.
void kept_base(GlyphPainter &painter) {
    using namespace vcs;
    const auto dual = PcInputConfig::defaults(InputScheme::DualAnalog);
    const auto sheet256 = made_up_sheet(256u, 512u);
    ge_texture_mods_start({});
    ge_texture_mods_watch_overlays(prompt_overlay_keys);
    for (const unsigned scale : {1u, 2u, 4u}) {
        double first_ms = 0.0, again_ms = 0.0;
        for (const PromptStyle style : {PromptStyle::Keyboard, PromptStyle::Xbox, PromptStyle::PlayStation, PromptStyle::Keyboard}) {
            PromptOverlays set;
            std::string error;
            require(paint_prompt_overlays(painter, dual, style, scale, scale, set, error), "paint: " + error);
            const GeTextureOverlaySet entries[]{{prompt_font_fill_key, set[0]}, {prompt_font_outline_key, set[1]}, {prompt_sprites[4].key, set[6]}};
            ge_texture_mods_set_overlays(entries, true);
            const auto started = std::chrono::steady_clock::now();
            const auto fill = ge_texture_mods_compose_overlay(prompt_font_fill_key, nullptr, 256u, 512u, sheet256);
            (style == PromptStyle::Keyboard && first_ms == 0.0 ? first_ms : again_ms) = ms_since(started);
            const auto outline = ge_texture_mods_compose_overlay(prompt_font_outline_key, nullptr, 256u, 512u, sheet256);
            const auto expected_fill = ge_texture_compose_overlay(*set[0], 256u, 512u, sheet256);
            const auto expected_outline = ge_texture_compose_overlay(*set[1], 256u, 512u, sheet256);
            require(fill && expected_fill && fill->width == expected_fill->width && fill->mip_levels == expected_fill->mip_levels &&
                fill->rgba8 == expected_fill->rgba8, "the fill sheet from the kept picture is the sheet made from nothing");
            require(outline && expected_outline && outline->rgba8 == expected_outline->rgba8, "the outline sheet too");
            const std::vector<std::byte> game(32u * 32u * 4u, std::byte{0x33});
            const auto sprite = ge_texture_mods_compose_overlay(prompt_sprites[4].key, nullptr, 32u, 32u, game);
            const auto expected_sprite = ge_texture_compose_overlay(*set[6], 32u, 32u, game);
            require(sprite && expected_sprite && sprite->rgba8 == expected_sprite->rgba8 && sprite->mip_levels == expected_sprite->mip_levels, "a sprite too");
        }
        std::printf("renderer's path at scale %u: a font sheet the first time %.2f ms, on a change of style %.2f ms\n", scale, first_ms, again_ms);
    }
    // A mod's picture arriving later is another base: the kept picture is made again.
    {
        PromptOverlays set;
        std::string error;
        require(paint_prompt_overlays(painter, dual, PromptStyle::Xbox, 2u, 2u, set, error), "paint: " + error);
        const GeTextureOverlaySet entries[]{{prompt_font_fill_key, set[0]}};
        ge_texture_mods_set_overlays(entries, true);
        auto mod = std::make_shared<GeTextureReplacement>();
        mod->width = 512u; mod->height = 1024u; mod->mip_levels = 1u;
        mod->rgba8 = made_up_sheet(512u, 1024u);
        for (auto &value : mod->rgba8) value = value == std::byte{0xFF} ? std::byte{0xEE} : value;
        const auto over_mod = ge_texture_mods_compose_overlay(prompt_font_fill_key, mod, 256u, 512u, sheet256);
        const auto expected = ge_texture_compose_overlay(*set[0], 512u, 1024u, mod->rgba8);
        require(over_mod && expected && over_mod->rgba8 == expected->rgba8, "over a mod's picture once it is there");
        const auto over_game = ge_texture_mods_compose_overlay(prompt_font_fill_key, nullptr, 256u, 512u, sheet256);
        require(over_game && over_game->rgba8 == ge_texture_compose_overlay(*set[0], 256u, 512u, sheet256)->rgba8, "and over the game's again");
    }
    ge_texture_mods_stop();
}

void table() {
    using namespace vcs;
    GeTextureModsSetup setup;
    ge_texture_mods_start(setup);
    require(!ge_texture_mods_keying() && !ge_texture_mods_keying_for(5u, 256u, 512u), "nothing is keyed without mods and overlays");
    // The keys a host page watches (the overlay pass finds its textures by key) are a table of their own.
    {
        const GeTextureOverlayKey page_keys[]{{0xa247e7634946f676ull, 8u, 256u, 256u}, {0x5d0c6d1a663a8515ull, 5u, 256u, 512u}};
        ge_texture_mods_watch_keys(page_keys);
        require(ge_texture_mods_keying_for(8u, 256u, 256u) && ge_texture_mods_keying_for(5u, 256u, 512u), "a page's shapes are keyed without any overlay");
        require(!ge_texture_mods_keying_for(4u, 256u, 512u) && !ge_texture_mods_keying_for(5u, 32u, 32u) && !ge_texture_mods_keying(), "other shapes are not, and it is not a texture mod");
        require(!ge_texture_mods_has_overlay(0xa247e7634946f676ull), "a watched key has no overlay");
        ge_texture_mods_watch_overlays(prompt_overlay_keys);
        require(ge_texture_mods_keying_for(8u, 256u, 256u) && ge_texture_mods_keying_for(5u, 32u, 32u), "both tables count");
        ge_texture_mods_watch_overlays({});
        require(ge_texture_mods_keying_for(8u, 256u, 256u) && !ge_texture_mods_keying_for(5u, 32u, 32u), "ending the overlays leaves the page's keys");
        ge_texture_mods_watch_keys({});
        require(!ge_texture_mods_keying_for(8u, 256u, 256u) && !ge_texture_mods_keying_for(5u, 256u, 512u), "an empty list ends the page's keys");
    }
    ge_texture_mods_watch_overlays(prompt_overlay_keys);
    require(!ge_texture_mods_keying(), "watching overlays is not a texture mod");
    require(ge_texture_mods_keying_for(5u, 256u, 512u) && ge_texture_mods_keying_for(4u, 256u, 512u), "the font sheets' shapes are keyed");
    require(ge_texture_mods_keying_for(5u, 32u, 32u) && ge_texture_mods_keying_for(9u, 32u, 32u), "the sprites' shapes are keyed");
    require(!ge_texture_mods_keying_for(5u, 64u, 64u) && !ge_texture_mods_keying_for(8u, 32u, 32u) && !ge_texture_mods_keying_for(3u, 512u, 512u) &&
        !ge_texture_mods_keying_for(5u, 512u, 256u), "other shapes are not");
    auto first = std::make_shared<GeTextureOverlay>();
    first->width = 32u; first->height = 32u; first->scale = 2u;
    GeTextureOverlayCell cell;
    cell.width = cell.height = 32u; cell.picture_width = cell.picture_height = 64u;
    cell.rgba8.assign(64u * 64u * 4u, std::byte{0xFF});
    first->cells.push_back(cell);
    const std::uint64_t key = prompt_sprites[0].key, other = prompt_sprites[1].key;
    const std::vector<std::byte> game(32u * 32u * 4u, std::byte{0x20});
    std::vector<std::uint64_t> changed;
    // At once: in force when the call returns, and nothing for the renderer to take.
    const GeTextureOverlaySet at_start[]{{key, first}};
    ge_texture_mods_set_overlays(at_start, true);
    require(ge_texture_mods_has_overlay(key) && !ge_texture_mods_has_overlay(other), "an overlay set at once is in force");
    require(!ge_texture_mods_take_overlay_changes(changed) && changed.empty(), "nothing to take after a set at once");
    auto composed = ge_texture_mods_compose_overlay(key, nullptr, 32u, 32u, game);
    require(composed && composed->width == 64u && composed->rgba8[0] == std::byte{0xFF}, "the picture of a key with an overlay");
    require(ge_texture_mods_compose_overlay(other, nullptr, 32u, 32u, game) == nullptr, "a key without an overlay keeps its picture");
    require(ge_texture_mods_compose_overlay(key, nullptr, 64u, 64u, std::vector<std::byte>(64u * 64u * 4u)) == nullptr, "a texture of another size keeps its picture");
    // A mod's replacement is the base.
    auto replacement = std::make_shared<GeTextureReplacement>();
    replacement->width = replacement->height = 128u; replacement->mip_levels = 1u;
    replacement->rgba8.assign(128u * 128u * 4u, std::byte{0x10});
    composed = ge_texture_mods_compose_overlay(key, replacement, 32u, 32u, game);
    require(composed && composed != replacement && composed->width == 128u && composed->rgba8[0] == std::byte{0xFF}, "composed over a mod's picture at its size");
    require(ge_texture_mods_compose_overlay(other, replacement, 32u, 32u, game) == replacement, "a mod's picture passes through without an overlay");
    // During play: in force only when the renderer takes it.
    auto second = std::make_shared<GeTextureOverlay>(*first);
    second->cells[0].rgba8.assign(64u * 64u * 4u, std::byte{0x7F});
    const GeTextureOverlaySet change[]{{key, second}, {other, first}};
    ge_texture_mods_set_overlays(change, false);
    require(ge_texture_mods_compose_overlay(key, nullptr, 32u, 32u, game)->rgba8[0] == std::byte{0xFF}, "a change waits for the renderer");
    require(!ge_texture_mods_has_overlay(other), "a new key waits for the renderer");
    require(ge_texture_mods_take_overlay_changes(changed) && changed.size() == 2u && changed[0] == key && changed[1] == other, "the renderer takes the changed keys");
    require(ge_texture_mods_compose_overlay(key, nullptr, 32u, 32u, game)->rgba8[0] == std::byte{0x7F} && ge_texture_mods_has_overlay(other), "then it is in force");
    changed.clear();
    require(!ge_texture_mods_take_overlay_changes(changed), "taken once");
    // The same overlay again changes nothing; none takes the texture back to the game's.
    ge_texture_mods_set_overlays(change, false);
    require(!ge_texture_mods_take_overlay_changes(changed) && changed.empty(), "the same overlay is no change");
    const GeTextureOverlaySet clear[]{{key, nullptr}, {other, nullptr}};
    ge_texture_mods_set_overlays(clear, false);
    require(ge_texture_mods_take_overlay_changes(changed) && changed.size() == 2u, "clearing is a change");
    require(!ge_texture_mods_has_overlay(key) && ge_texture_mods_compose_overlay(key, nullptr, 32u, 32u, game) == nullptr, "cleared");
    require(ge_texture_mods_keying_for(5u, 32u, 32u), "the keys stay watched without overlays");
    changed.clear();
    ge_texture_mods_set_overlays(clear, false);
    require(!ge_texture_mods_take_overlay_changes(changed), "clearing twice is no change");
    const auto report = ge_texture_mods_report();
    require(report.overlay_composed == 4u && report.overlay_bytes != 0u, "compositions are counted");
    ge_texture_mods_note_overlay_evictions(3u);
    require(ge_texture_mods_report().overlay_evicted == 3u, "evictions are counted");
    const auto tick = ge_texture_mods_clock_ns();
    ge_texture_mods_note_keyed(tick);
    require(ge_texture_mods_report().keyed_textures == 1u, "keyed textures are counted");
    const GeTextureOverlayKey page_key[]{{0xa247e7634946f676ull, 8u, 256u, 256u}};
    ge_texture_mods_watch_keys(page_key);
    ge_texture_mods_stop();
    require(!ge_texture_mods_keying_for(5u, 32u, 32u) && !ge_texture_mods_has_overlay(key), "stop ends it");
    require(!ge_texture_mods_keying_for(8u, 256u, 256u), "and a page's keys with it");
}

void styles() {
    using P = PromptsSetting; using G = GlyphsSetting; using D = InputDevice; using K = PadKind; using S = PromptStyle;
    // The style the settings and the input ask for.
    require(resolve_prompt_style(P::Auto, G::Auto, D::KeyboardMouse, K::None) == S::Keyboard, "auto starts as keyboard and mouse");
    require(resolve_prompt_style(P::Auto, G::Auto, D::KeyboardMouse, K::Xbox) == S::Keyboard, "a pad that is connected but not used does not change it");
    require(resolve_prompt_style(P::Auto, G::Auto, D::Gamepad, K::Xbox) == S::Xbox, "auto follows an Xbox pad");
    require(resolve_prompt_style(P::Auto, G::Auto, D::Gamepad, K::PlayStation) == S::PlayStation, "auto follows a PlayStation pad");
    require(resolve_prompt_style(P::Auto, G::Auto, D::Gamepad, K::None) == S::Xbox, "a pad of unknown kind looks like an Xbox pad");
    require(resolve_prompt_style(P::Auto, G::PlayStation, D::Gamepad, K::Xbox) == S::PlayStation, "Glyphs=playstation overrides the kind");
    require(resolve_prompt_style(P::Auto, G::Xbox, D::Gamepad, K::PlayStation) == S::Xbox, "Glyphs=xbox overrides the kind");
    require(resolve_prompt_style(P::Auto, G::PlayStation, D::KeyboardMouse, K::PlayStation) == S::Keyboard, "Glyphs does not make a pad the device in use");
    require(resolve_prompt_style(P::Keyboard, G::Auto, D::Gamepad, K::Xbox) == S::Keyboard, "Prompts=keyboard stays");
    require(resolve_prompt_style(P::Gamepad, G::Auto, D::KeyboardMouse, K::None) == S::Xbox, "Prompts=gamepad without a pad");
    require(resolve_prompt_style(P::Gamepad, G::Auto, D::KeyboardMouse, K::PlayStation) == S::PlayStation, "Prompts=gamepad with the pad seen last");
    require(resolve_prompt_style(P::Gamepad, G::PlayStation, D::KeyboardMouse, K::None) == S::PlayStation, "Prompts=gamepad Glyphs=playstation");
    require(resolve_prompt_style(P::Psp, G::Xbox, D::Gamepad, K::Xbox) == S::Psp, "Prompts=psp stays");
    require(prompt_style_from_name("XBOX") == S::Xbox && prompt_style_from_name("playstation") == S::PlayStation && prompt_style_from_name("keyboard") == S::Keyboard &&
        prompt_style_from_name("psp") == S::Psp && !prompt_style_from_name("gamepad") && !prompt_style_from_name(""), "style names");

    // Debounce: 250 ms without a break, and two switches a second at most.
    PromptDebounce debounce(S::Keyboard);
    require(debounce.update(S::Keyboard, 1000u) == S::Keyboard, "steady");
    require(debounce.update(S::Xbox, 1010u) == S::Keyboard && debounce.update(S::Xbox, 1259u) == S::Keyboard, "a new style waits 250 ms");
    require(debounce.update(S::Xbox, 1260u) == S::Xbox, "then it shows");
    require(debounce.update(S::Keyboard, 1300u) == S::Xbox && debounce.update(S::Keyboard, 1550u) == S::Xbox && debounce.update(S::Keyboard, 1759u) == S::Xbox,
        "the next switch waits half a second after the last");
    require(debounce.update(S::Keyboard, 1760u) == S::Keyboard, "then it shows");
    // A mouse nudged again and again while the pad is in use: the pictures stay.
    PromptDebounce busy(S::Xbox);
    PromptStyle shown = S::Xbox; unsigned switches = 0u;
    for (std::uint64_t t = 0u; t < 10000u; t += 16u) {
        const auto now = busy.update((t / 200u) % 2u ? S::Keyboard : S::Xbox, t);
        if (now != shown) { ++switches; shown = now; }
    }
    require(switches == 0u, "two devices taking turns every 200 ms do not flicker");
    // Taking turns every 300 ms: never more than two switches in any second.
    PromptDebounce slow(S::Xbox);
    shown = S::Xbox; std::vector<std::uint64_t> at;
    for (std::uint64_t t = 0u; t < 10000u; t += 16u) {
        const auto now = slow.update((t / 300u) % 2u ? S::Keyboard : S::Xbox, t);
        if (now != shown) { at.push_back(t); shown = now; }
    }
    require(!at.empty(), "slower turns do switch");
    for (std::size_t i = 2u; i < at.size(); ++i) require(at[i] - at[i - 2u] >= 1000u, "at most two switches a second");

    // Key caps.
    const auto cap_text = [](int vk) { const auto g = key_glyph(vk); return std::string(g.keys[0].text.data()); };
    require(key_glyph('W').kind == GlyphKind::Key && cap_text('W') == "W" && cap_text('7') == "7", "letters and digits");
    require(cap_text(VK_F1) == "F1" && cap_text(VK_F12) == "F12" && cap_text(VK_NUMPAD7) == "N7", "function and number pad keys");
    require(cap_text(VK_ESCAPE) == "Esc" && cap_text(VK_TAB) == "Tab" && cap_text(VK_LCONTROL) == "Ctrl" && cap_text(VK_RMENU) == "Alt" &&
        cap_text(VK_PRIOR) == "PgUp" && cap_text(VK_NEXT) == "PgDn" && cap_text(VK_INSERT) == "Ins" && cap_text(VK_DELETE) == "Del" &&
        cap_text(VK_CAPITAL) == "Caps" && cap_text(VK_HOME) == "Home" && cap_text(VK_END) == "End", "shortened names");
    require(key_glyph(VK_SPACE).keys[0].sign == KeySign::Space && key_glyph(VK_RETURN).keys[0].sign == KeySign::Enter &&
        key_glyph(VK_RSHIFT).keys[0].sign == KeySign::Shift && key_glyph(VK_BACK).keys[0].sign == KeySign::Backspace &&
        key_glyph(VK_LEFT).keys[0].sign == KeySign::Left && key_glyph(VK_UP).keys[0].sign == KeySign::Up, "signs for keys whose names do not fit");
    require(cap_text(0xE8) == "?", "an unknown key");

    // The default bindings (dual_analog), keyboard and mouse.
    const auto dual = PcInputConfig::defaults(InputScheme::DualAnalog);
    const auto keyboard = [&](PadTarget target) { return prompt_glyph(dual, S::Keyboard, target); };
    const auto is_mouse = [](const Glyph &g, MousePart part) { return g.kind == GlyphKind::Mouse && g.index == static_cast<std::uint8_t>(part); };
    require(keyboard(PadTarget::Cross) == key_glyph(VK_SPACE), "Cross: Space, the first of Space, E, Enter");
    require(keyboard(PadTarget::Circle) == key_glyph(PcInputConfig::key_from_name("F")), "Circle: F before Backspace and the middle button");
    require(is_mouse(keyboard(PadTarget::Square), MousePart::Left), "Square: the left mouse button before G");
    require(keyboard(PadTarget::Triangle) == key_glyph(PcInputConfig::key_from_name("R")), "Triangle: R");
    require(keyboard(PadTarget::L) == key_glyph(PcInputConfig::key_from_name("Q")), "L: Q");
    require(is_mouse(keyboard(PadTarget::R), MousePart::Right), "R: the right mouse button before V");
    require(keyboard(PadTarget::DpadLeft) == key_glyph(VK_LEFT) && keyboard(PadTarget::DpadRight) == key_glyph(VK_RIGHT), "D-pad left and right: the arrow keys before 1, 2 and the wheel");
    require(keyboard(PadTarget::DpadDown) == key_glyph(VK_DOWN) && keyboard(PadTarget::DpadUp) == key_glyph(VK_UP), "D-pad down and up: the arrow keys");
    require(keyboard(PadTarget::Start) == key_glyph(VK_ESCAPE) && keyboard(PadTarget::Select) == key_glyph(VK_TAB), "Start and Select");
    require(keyboard(PadTarget::None).kind == GlyphKind::Unbound, "no target, no picture");
    require(prompt_move_glyph(dual, S::Keyboard, glyph_all_directions) == cluster(PcInputConfig::key_from_name("W"), PcInputConfig::key_from_name("A"),
        PcInputConfig::key_from_name("S"), PcInputConfig::key_from_name("D")), "the analog stick: the four move keys");
    require(prompt_move_glyph(dual, S::Keyboard, glyph_up) == key_glyph(PcInputConfig::key_from_name("W")) &&
        prompt_move_glyph(dual, S::Keyboard, glyph_left) == key_glyph(PcInputConfig::key_from_name("A")) &&
        prompt_move_glyph(dual, S::Keyboard, glyph_down) == key_glyph(PcInputConfig::key_from_name("S")) &&
        prompt_move_glyph(dual, S::Keyboard, glyph_right) == key_glyph(PcInputConfig::key_from_name("D")), "the analog stick in one direction: that move key");
    // The priority rule on other bindings.
    {
        auto config = dual;
        (void)config.apply("[Input.Keyboard]\nDpadLeft=1,Left\nSquare=\nCircle=\n[Input.Mouse]\nMouse1=None\nMouse3=Square\nMouse4=Circle\nWheelUp=Triangle\n");
        require(prompt_glyph(config, S::Keyboard, PadTarget::DpadLeft) == key_glyph(VK_LEFT), "an arrow key wins for a D-pad target wherever it stands");
        require(is_mouse(prompt_glyph(config, S::Keyboard, PadTarget::Square), MousePart::Middle), "the middle button when no key is bound");
        require(is_mouse(prompt_glyph(config, S::Keyboard, PadTarget::Circle), MousePart::Side4), "a thumb button when no key is bound");
        require(prompt_glyph(config, S::Keyboard, PadTarget::Triangle) == key_glyph(PcInputConfig::key_from_name("R")), "a key before the wheel");
        (void)config.apply("[Input.Keyboard]\nTriangle=\nDpadRight=\nL=\n[Input.Mouse]\nWheelUp=Triangle\nWheelDown=DpadRight\nMouse5=None\n");
        require(is_mouse(prompt_glyph(config, S::Keyboard, PadTarget::Triangle), MousePart::WheelUp), "the wheel when nothing else is bound");
        require(is_mouse(prompt_glyph(config, S::Keyboard, PadTarget::DpadRight), MousePart::WheelDown), "the wheel down");
        require(prompt_glyph(config, S::Keyboard, PadTarget::L).kind == GlyphKind::Unbound, "nothing bound: the grey cap");
        (void)config.apply("[Input.Keyboard]\nCross=E\nR=V\n[Input.Mouse]\nEnabled=false\nMouse2=R\n");
        require(prompt_glyph(config, S::Keyboard, PadTarget::R) == key_glyph(PcInputConfig::key_from_name("V")), "a mouse that is switched off is not pictured");
        require(prompt_glyph(config, S::Keyboard, PadTarget::Cross) == key_glyph(PcInputConfig::key_from_name("E")), "the key the ini names first");
        (void)config.apply("[Input.Keyboard]\nMoveLeft=\n");
        const auto moves = prompt_move_glyph(config, S::Keyboard, glyph_all_directions);
        require(moves.kind == GlyphKind::KeyCluster && std::string(moves.keys[1].text.data()) == "-", "a move key that is not bound shows as a dash");
        require(prompt_move_glyph(config, S::Keyboard, glyph_left).kind == GlyphKind::Unbound, "and alone as the grey cap");
    }
    // The pad, dual_analog defaults: A Cross, B Circle, X Triangle, Y Cross, LB/RB D-pad left/right, LT R, RT Square, LS L.
    const auto xbox = [&](PadTarget target) { return prompt_glyph(dual, S::Xbox, target); };
    const auto ps = [&](PadTarget target) { return prompt_glyph(dual, S::PlayStation, target); };
    require(xbox(PadTarget::Cross) == pad_glyph(GlyphKind::XboxFace, PadLook::Xbox, xbox_a), "Cross: A (Y is bound too)");
    require(xbox(PadTarget::Circle) == pad_glyph(GlyphKind::XboxFace, PadLook::Xbox, xbox_b), "Circle: B");
    require(xbox(PadTarget::Triangle) == pad_glyph(GlyphKind::XboxFace, PadLook::Xbox, xbox_x), "Triangle: X");
    require(xbox(PadTarget::Square) == pad_glyph(GlyphKind::Trigger, PadLook::Xbox, 1u), "Square: RT");
    require(xbox(PadTarget::R) == pad_glyph(GlyphKind::Trigger, PadLook::Xbox, 0u), "R: LT");
    require(xbox(PadTarget::L) == pad_glyph(GlyphKind::Stick, PadLook::Xbox, 0u), "L: the left stick pressed");
    require(xbox(PadTarget::DpadLeft) == pad_glyph(GlyphKind::Dpad, PadLook::Xbox, 0u, glyph_left), "D-pad left: the D-pad, though LB is bound too");
    require(xbox(PadTarget::DpadRight) == pad_glyph(GlyphKind::Dpad, PadLook::Xbox, 0u, glyph_right), "D-pad right: the D-pad, though RB is bound too");
    require(xbox(PadTarget::DpadDown) == pad_glyph(GlyphKind::Dpad, PadLook::Xbox, 0u, glyph_down), "D-pad down");
    require(xbox(PadTarget::Start) == pad_glyph(GlyphKind::Menu, PadLook::Xbox) && xbox(PadTarget::Select) == pad_glyph(GlyphKind::View, PadLook::Xbox), "Start and Back");
    require(ps(PadTarget::Cross) == pad_glyph(GlyphKind::PlayStationFace, PadLook::PlayStation, ps_cross), "PlayStation Cross: Cross");
    require(ps(PadTarget::Circle) == pad_glyph(GlyphKind::PlayStationFace, PadLook::PlayStation, ps_circle), "PlayStation Circle: Circle");
    require(ps(PadTarget::Triangle) == pad_glyph(GlyphKind::PlayStationFace, PadLook::PlayStation, ps_square), "PlayStation: the PSP's Triangle is on Square");
    require(ps(PadTarget::Square) == pad_glyph(GlyphKind::Trigger, PadLook::PlayStation, 1u), "PlayStation Square: R2");
    require(ps(PadTarget::R) == pad_glyph(GlyphKind::Trigger, PadLook::PlayStation, 0u) && ps(PadTarget::L) == pad_glyph(GlyphKind::Stick, PadLook::PlayStation, 0u), "PlayStation R: L2, L: L3");
    require(ps(PadTarget::Start) == pad_glyph(GlyphKind::Menu, PadLook::PlayStation), "PlayStation Start: Options");
    require(prompt_move_glyph(dual, S::Xbox, glyph_all_directions) == pad_glyph(GlyphKind::Stick, PadLook::Xbox, 0u, glyph_all_directions) &&
        prompt_move_glyph(dual, S::PlayStation, glyph_down) == pad_glyph(GlyphKind::Stick, PadLook::PlayStation, 0u, glyph_down), "the analog stick: the left stick");
    // The psp scheme's pad: X Square, Y Triangle, LB L, RB R (LT R and RT Square too).
    const auto psp = PcInputConfig::defaults(InputScheme::Psp);
    require(prompt_glyph(psp, S::Xbox, PadTarget::Square) == pad_glyph(GlyphKind::XboxFace, PadLook::Xbox, xbox_x), "psp scheme, Square: X before RT");
    require(prompt_glyph(psp, S::Xbox, PadTarget::Triangle) == pad_glyph(GlyphKind::XboxFace, PadLook::Xbox, xbox_y), "psp scheme, Triangle: Y");
    require(prompt_glyph(psp, S::Xbox, PadTarget::L) == pad_glyph(GlyphKind::Shoulder, PadLook::Xbox, 0u), "psp scheme, L: LB");
    require(prompt_glyph(psp, S::Xbox, PadTarget::R) == pad_glyph(GlyphKind::Shoulder, PadLook::Xbox, 1u), "psp scheme, R: RB before LT");
    require(prompt_glyph(psp, S::PlayStation, PadTarget::Square) == pad_glyph(GlyphKind::PlayStationFace, PadLook::PlayStation, ps_square), "psp scheme on a PlayStation pad: Square is Square");
    require(is_mouse(prompt_glyph(psp, S::Keyboard, PadTarget::Square), MousePart::Left) && is_mouse(prompt_glyph(psp, S::Keyboard, PadTarget::R), MousePart::Right), "psp scheme: the mouse buttons");
    {
        auto config = dual;
        (void)config.apply("[Input.Gamepad]\nDpadLeft=None\nStart=Select\nBack=Start\nLB=DpadLeft\nA=None\nY=None\nRS=Cross\n");
        require(prompt_glyph(config, S::Xbox, PadTarget::DpadLeft) == pad_glyph(GlyphKind::Shoulder, PadLook::Xbox, 0u), "the D-pad arm unbound: the first button in table order");
        require(prompt_glyph(config, S::Xbox, PadTarget::Start) == pad_glyph(GlyphKind::View, PadLook::Xbox), "Start moved to Back");
        require(prompt_glyph(config, S::Xbox, PadTarget::Select) == pad_glyph(GlyphKind::Menu, PadLook::Xbox), "Select moved to Start");
        require(prompt_glyph(config, S::Xbox, PadTarget::Cross) == pad_glyph(GlyphKind::Stick, PadLook::Xbox, 1u), "Cross on the right stick's click");
        (void)config.apply("[Input.Gamepad]\nRS=None\n");
        require(prompt_glyph(config, S::Xbox, PadTarget::Cross).kind == GlyphKind::Unbound, "nothing bound on the pad: the grey cap");
    }
    // A configuration that knows a separate PlayStation layout (the gamepad code's pad_target): the
    // PlayStation pictures are read from it, the Xbox ones from the common table.
    {
        struct TwoTables {
            std::array<PadTarget, 16> pad_buttons{}, playstation{};
            PadTarget pad_target(std::size_t button, PadKind kind) const noexcept { return kind == PadKind::PlayStation ? playstation[button] : pad_buttons[button]; }
        } two;
        two.pad_buttons = dual.pad_buttons;
        two.playstation = dual.pad_buttons;
        std::swap(two.playstation[prompt_pad_index("X")], two.playstation[prompt_pad_index("Y")]);
        require(prompt_pad_table(two, PadLook::Xbox) == dual.pad_buttons && prompt_pad_table(two, PadLook::PlayStation) == two.playstation, "the table of the pad's kind");
        require(pad_prompt_glyph(prompt_pad_table(two, PadLook::PlayStation), PadTarget::Triangle, PadLook::PlayStation) ==
            pad_glyph(GlyphKind::PlayStationFace, PadLook::PlayStation, ps_triangle), "a separate PlayStation layout puts the PSP's Triangle on Triangle");
        require(prompt_pad_table(dual, PadLook::Xbox) == dual.pad_buttons, "the common table for an Xbox pad");
    }
    separate_playstation_layout(dual);
    // The menu's left/right sprite.
    require(prompt_dpad_pair(xbox(PadTarget::DpadLeft), xbox(PadTarget::DpadRight)), "a pad's own D-pad: one picture with both arms lit");
    require(!prompt_dpad_pair(keyboard(PadTarget::DpadLeft), keyboard(PadTarget::DpadRight)), "keys: two caps side by side");
    // The table of the font's pictures and the seven keys.
    for (std::size_t i = 0; i < prompt_font_cells.size(); ++i)
        for (std::size_t j = i + 1u; j < prompt_font_cells.size(); ++j) {
            const auto &a = prompt_font_cells[i], &b = prompt_font_cells[j];
            require(a.y != b.y || a.x + a.width <= b.x || b.x + b.width <= a.x, "font cells do not overlap");
        }
    for (const auto &cell : prompt_font_cells) require(std::uint32_t{cell.x} + cell.width <= 256u && std::uint32_t{cell.y} + 30u <= 512u && (cell.move != 0u) != (cell.target != PadTarget::None), "a font cell is a button or a stick direction");
    require(prompt_overlay_keys[0].key == 0x5d0c6d1a663a8515ull && prompt_overlay_keys[1].key == 0x002d10b31e312e9dull && prompt_overlay_keys[6].format == 9u, "the seven keys");
    require(prompt_pad_index("A") < 16u && prompt_pad_index("LT") == PcInputConfig::pad_left_trigger && prompt_pad_index("nothing") == 16u, "pad button places");
}

void controller() {
    const auto dual = PcInputConfig::defaults(InputScheme::DualAnalog);
    auto &state = input_device_state();
    const auto reset = [&] {
        for (const char *name : {"CHILI_PROMPTS", "CHILI_PROMPTS_AT", "CHILI_PROMPT_DEVICE_AT", "CHILI_PROMPT_KEY_ALL", "CHILI_PROMPT_PAINTER"}) _putenv_s(name, "");
        state.device.store(InputDevice::KeyboardMouse); state.pad_kind.store(PadKind::None);
        state.prompts.store(PromptsSetting::Auto); state.glyphs.store(GlyphsSetting::Auto);
        vcs::ge_texture_mods_stop();
    };
    std::vector<std::uint64_t> changed;
    const auto take = [&] { changed.clear(); (void)vcs::ge_texture_mods_take_overlay_changes(changed); return changed.size(); };
    // A scripted run, the software renderer and Prompts=psp leave the game's pictures alone.
    {
        reset();
        PromptController prompts;
        const auto lines = prompts.start(dual, 2u, true);
        require(!prompts.active() && lines.size() == 1u && lines[0].find("scripted") != std::string::npos, "a scripted run shows the PSP pictures");
        require(!vcs::ge_texture_mods_keying_for(5u, 256u, 512u), "and keys nothing");
        prompts.frame(10u);
    }
    {
        reset();
        PromptController prompts;
        require(!prompts.start(dual, 0u, false).empty() && !prompts.active() && !vcs::ge_texture_mods_keying_for(5u, 32u, 32u), "the software renderer shows the PSP pictures");
    }
    {
        reset();
        state.prompts.store(PromptsSetting::Psp);
        PromptController prompts;
        require(!prompts.start(dual, 2u, false).empty() && !prompts.active() && !vcs::ge_texture_mods_keying_for(5u, 32u, 32u), "Prompts=psp shows the PSP pictures");
    }
    // Without the painter (Direct2D or DirectWrite missing): the PSP pictures and one line that says why.
    {
        reset();
        _putenv_s("CHILI_PROMPT_PAINTER", "off");
        PromptController prompts;
        const auto lines = prompts.start(dual, 2u, false);
        require(!prompts.active() && lines.size() == 1u && lines[0].find("PSP pictures") != std::string::npos && !vcs::ge_texture_mods_keying_for(5u, 256u, 512u),
            "no painter: the PSP pictures, one log line");
        _putenv_s("CHILI_PROMPT_PAINTER", "");
    }
    // A player's run: keyboard and mouse at the start, the pad once it is touched, after the debounce.
    {
        reset();
        PromptController prompts;
        const auto lines = prompts.start(dual, 4u, false);
        require(prompts.active() && prompts.shown() == PromptStyle::Keyboard && lines.size() == 1u, "auto starts with keyboard and mouse: " + (lines.empty() ? std::string() : lines[0]));
        std::printf("log line: [prompts] %s\n", lines[0].c_str());
        require(vcs::ge_texture_mods_keying_for(5u, 256u, 512u) && !vcs::ge_texture_mods_keying(), "the seven shapes are keyed");
        for (const auto &key : prompt_overlay_keys) require(vcs::ge_texture_mods_has_overlay(key.key), "all seven textures have their pictures from the start");
        require(take() == 0u, "nothing for the renderer to take at the start");
        prompts.tick(1u, 5000u);
        state.device.store(InputDevice::Gamepad); state.pad_kind.store(PadKind::PlayStation);
        prompts.tick(2u, 5016u);
        prompts.tick(3u, 5200u);
        require(prompts.shown() == PromptStyle::Keyboard && take() == 0u, "a pad touched: nothing changes for 250 ms");
        prompts.tick(4u, 5270u);
        require(prompts.shown() == PromptStyle::PlayStation && prompts.switches() == 1u && take() == 7u, "then the seven textures get the pad's pictures");
        state.device.store(InputDevice::KeyboardMouse);
        prompts.tick(5u, 5300u);
        prompts.tick(6u, 5600u);
        require(prompts.shown() == PromptStyle::PlayStation, "back to the keyboard: not within half a second of the last switch");
        prompts.tick(7u, 5780u);
        require(prompts.shown() == PromptStyle::Keyboard && take() == 7u, "then it switches back");
        state.glyphs.store(GlyphsSetting::Xbox); state.device.store(InputDevice::Gamepad);
        prompts.tick(8u, 6000u); prompts.tick(9u, 6300u);
        require(prompts.shown() == PromptStyle::Xbox && take() == 7u, "Glyphs=xbox shows a PlayStation pad as an Xbox pad");
    }
    // CHILI_PROMPTS wins over the scripted run and over the settings.
    {
        reset();
        _putenv_s("CHILI_PROMPTS", "playstation");
        state.prompts.store(PromptsSetting::Psp);
        PromptController prompts;
        const auto lines = prompts.start(dual, 2u, true);
        require(prompts.active() && prompts.shown() == PromptStyle::PlayStation && vcs::ge_texture_mods_has_overlay(prompt_font_fill_key), "CHILI_PROMPTS=playstation in a scripted run");
        state.device.store(InputDevice::KeyboardMouse);
        for (std::uint64_t frame = 1u; frame < 40u; ++frame) prompts.frame(frame);
        require(prompts.shown() == PromptStyle::PlayStation && prompts.switches() == 0u && take() == 0u, "and it stays");
    }
    {
        reset();
        _putenv_s("CHILI_PROMPTS", "psp");
        PromptController prompts;
        prompts.start(dual, 2u, false);
        require(prompts.active() && prompts.shown() == PromptStyle::Psp && !vcs::ge_texture_mods_has_overlay(prompt_font_fill_key) && vcs::ge_texture_mods_keying_for(5u, 256u, 512u),
            "CHILI_PROMPTS=psp: the PSP pictures, the textures still watched");
    }
    {
        reset();
        _putenv_s("CHILI_PROMPTS", "nonsense");
        PromptController prompts;
        const auto lines = prompts.start(dual, 2u, true);
        require(!prompts.active() && lines.size() == 2u && lines[0].find("nonsense") != std::string::npos, "a wrong CHILI_PROMPTS is reported and ignored");
    }
    // The style changes at given frames of a scripted run.
    {
        reset();
        _putenv_s("CHILI_PROMPTS", "keyboard");
        _putenv_s("CHILI_PROMPTS_AT", "30:xbox, 20:psp,40:playstation,bad,50:keyboard");
        PromptController prompts;
        const auto lines = prompts.start(dual, 1u, true);
        require(prompts.active() && prompts.shown() == PromptStyle::Keyboard && lines.size() == 2u && lines[0].find("bad") != std::string::npos, "CHILI_PROMPTS_AT is read, a wrong item reported");
        prompts.frame(19u);
        require(prompts.shown() == PromptStyle::Keyboard && take() == 0u, "not before its frame");
        prompts.frame(20u);
        require(prompts.shown() == PromptStyle::Psp && take() == 7u && !vcs::ge_texture_mods_has_overlay(prompt_sprites[0].key), "frame 20: the PSP pictures");
        prompts.frame(30u);
        require(prompts.shown() == PromptStyle::Xbox && take() == 7u && vcs::ge_texture_mods_has_overlay(prompt_sprites[0].key), "frame 30: Xbox");
        prompts.frame(45u);
        require(prompts.shown() == PromptStyle::PlayStation && take() == 7u, "frame 40, seen at 45: PlayStation");
        prompts.frame(50u);
        require(prompts.shown() == PromptStyle::Keyboard && prompts.switches() == 4u && take() == 7u, "frame 50: keyboard and mouse");
    }
    // The device used last changes at given frames: the style follows after the debounce, on the frame clock.
    {
        reset();
        _putenv_s("CHILI_PROMPT_DEVICE_AT", "30:xbox,90:keyboard,150:playstation");
        PromptController prompts;
        prompts.start(dual, 1u, true);
        require(prompts.active() && prompts.shown() == PromptStyle::Keyboard, "CHILI_PROMPT_DEVICE_AT makes a scripted run follow the device");
        std::uint64_t to_xbox = 0u, to_keyboard = 0u, to_playstation = 0u;
        for (std::uint64_t frame = 1u; frame <= 200u; ++frame) {
            prompts.frame(frame);
            if (!to_xbox && prompts.shown() == PromptStyle::Xbox) to_xbox = frame;
            if (to_xbox && !to_keyboard && prompts.shown() == PromptStyle::Keyboard) to_keyboard = frame;
            if (!to_playstation && prompts.shown() == PromptStyle::PlayStation) to_playstation = frame;
        }
        // 250 ms are 8 frames of 33.3 ms.
        require(to_xbox == 38u && to_keyboard == 98u && to_playstation == 158u, "the pictures follow 8 frames after the device: " +
            std::to_string(to_xbox) + " " + std::to_string(to_keyboard) + " " + std::to_string(to_playstation));
        require(state.pad_kind.load() == PadKind::PlayStation && state.device.load() == InputDevice::Gamepad, "the device state was set as the input code sets it");
    }
    // A screen that reads the pad without ending a game frame (the briefing's Continue): no frame start comes
    // there, so the pad-sample path looks at the style too, at most every 50 ms of the real clock.
    {
        reset();
        PromptController prompts;
        prompts.start(dual, 2u, false);
        prompts.tick(1u, 9000u);   // the last frame start before that screen
        state.device.store(InputDevice::Gamepad); state.pad_kind.store(PadKind::Xbox);
        unsigned looked = 0u;
        std::uint64_t shown_at = 0u;
        for (std::uint64_t ms = 9004u; ms <= 10000u; ms += 4u) {   // a pad sample every 4 ms and no frame
            if (prompts.sample_tick(1u, ms)) ++looked;
            if (!shown_at && prompts.shown() == PromptStyle::Xbox) shown_at = ms;
        }
        require(shown_at >= 9250u && shown_at <= 9360u, "no frame ends: the pad's pictures come from the sample path, a quarter of a second after the pad was touched: at " + std::to_string(shown_at));
        require(looked >= 18u && looked <= 20u, "the sample path looks at the style at most every 50 ms: " + std::to_string(looked) + " times in a second");
        require(prompts.switches() == 1u && take() == 7u, "and the seven textures get the pad's pictures there");
        // Frames end again: the frame start decides, and the samples between two frames find nothing to do.
        std::uint64_t frame = 1u;
        unsigned between = 0u;
        state.device.store(InputDevice::KeyboardMouse);
        for (std::uint64_t ms = 10016u; ms <= 11000u; ms += 33u) {
            prompts.tick(++frame, ms);
            for (const std::uint64_t later : {8u, 16u, 24u}) if (prompts.sample_tick(frame, ms + later)) ++between;
        }
        require(between == 0u && prompts.shown() == PromptStyle::Keyboard && prompts.switches() == 2u, "while frames end, the frame start decides: " + std::to_string(between) + " looks from the sample path");
    }
    // A scripted run's clock is its frame count: the sample path leaves it alone, whatever the real clock says.
    {
        reset();
        _putenv_s("CHILI_PROMPT_DEVICE_AT", "5:xbox");
        PromptController prompts;
        prompts.start(dual, 1u, true);
        prompts.frame(5u);
        bool looked = false;
        for (std::uint64_t ms = 0u; ms < 3000u; ms += 10u) looked = prompts.sample_tick(5u, ms) || looked;
        require(prompts.active() && !looked && prompts.shown() == PromptStyle::Keyboard && prompts.switches() == 0u, "a scripted run is not looked at from the sample path");
        for (std::uint64_t frame = 6u; frame <= 13u; ++frame) prompts.frame(frame);
        require(prompts.shown() == PromptStyle::Xbox && prompts.switches() == 1u, "its frames decide, eight frames after the device as before");
    }
    {
        reset();
        PromptController prompts;
        (void)prompts.start(dual, 2u, true);
        require(!prompts.active() && !prompts.sample_tick(1u, 123456u), "nor is a run that shows the PSP pictures");
    }
    // A scripted run in real time that is made to follow the input (CHILI_PROMPTS=auto with a script session): the
    // real clock, like a player's, so the samples the player's devices own are looked at there too.
    {
        reset();
        _putenv_s("CHILI_PROMPTS", "auto");
        PromptController prompts;
        prompts.start(dual, 2u, true, false);
        require(prompts.active() && prompts.shown() == PromptStyle::Keyboard, "a real-time scripted run with CHILI_PROMPTS=auto follows the input");
        prompts.tick(40u, 70000u);
        state.device.store(InputDevice::Gamepad); state.pad_kind.store(PadKind::PlayStation);
        for (std::uint64_t ms = 70010u; ms <= 70400u; ms += 10u) (void)prompts.sample_tick(40u, ms);
        require(prompts.shown() == PromptStyle::PlayStation && prompts.switches() == 1u, "by the real clock: the pad's pictures from the sample path while frame 40 lasts");
    }
    {
        reset();
        _putenv_s("CHILI_PROMPTS", "auto");
        PromptController prompts;
        prompts.start(dual, 2u, true, true);
        prompts.frame(40u);
        state.device.store(InputDevice::Gamepad);
        bool looked = false;
        for (std::uint64_t ms = 0u; ms <= 5000u; ms += 10u) looked = prompts.sample_tick(40u, ms) || looked;
        require(prompts.active() && !looked && prompts.shown() == PromptStyle::Keyboard, "a deterministic run with CHILI_PROMPTS=auto goes by its frames only");
    }
    reset();
}

// The two halves together: the input code (a PcInput of its own with made-up keys and pads) writes the device
// used last and the pad's kind, the controller reads them and shows the style. Nothing here sets the shared
// state by hand; the clock is the input code's.
struct JoinedHost {
    std::set<int> keys;
    std::array<std::optional<XINPUT_STATE>, 4> slots;
    bool focused{true};
    double now{500.0};
    std::uint64_t frame{};
    PcInput input;
    explicit JoinedHost(PcInputConfig config) {
        input.configure(std::move(config));
        PcInputHost host;
        host.focused = [this] { return focused; };
        host.key_down = [this](int key) { return keys.count(key) != 0; };
        host.pad = [this](unsigned slot, XINPUT_STATE &state) { if (!slots[slot]) return false; state = *slots[slot]; return true; };
        host.seconds = [this] { return now; };
        host.pad_worker = false;
        host.vibrate = [](unsigned, std::uint16_t, std::uint16_t) { return true; };
        host.log = [](const std::string &) {};
        input.set_host(std::move(host));
    }
    // One controller sample and the frame start after it, a 60 Hz frame after the last.
    void step(PromptController &prompts) {
        now += 1.0 / 60.0;
        (void)input.sample();
        prompts.tick(++frame, static_cast<std::uint64_t>(now * 1000.0));
    }
    void run(PromptController &prompts, double seconds) { for (const double until = now + seconds; now < until;) step(prompts); }
    // One controller sample on a screen that ends no game frame: the sample path is all there is.
    void sample_only(PromptController &prompts) {
        now += 1.0 / 60.0;
        (void)input.sample();
        (void)prompts.sample_tick(frame, static_cast<std::uint64_t>(now * 1000.0));
    }
};
XINPUT_STATE xinput_buttons(WORD buttons) { XINPUT_STATE state{}; state.Gamepad.wButtons = buttons; return state; }
void joined() {
    auto &state = input_device_state();
    const auto reset = [&] {
        for (const char *name : {"CHILI_PROMPTS", "CHILI_PROMPTS_AT", "CHILI_PROMPT_DEVICE_AT", "CHILI_PROMPT_KEY_ALL", "CHILI_PROMPT_PAINTER"}) _putenv_s(name, "");
        state.device.store(InputDevice::KeyboardMouse); state.pad_kind.store(PadKind::None);
        state.prompts.store(PromptsSetting::Auto); state.glyphs.store(GlyphsSetting::Auto);
        vcs::ge_texture_mods_stop();
    };
    const int key_w = PcInputConfig::key_from_name("W");
    // No pad: keyboard and mouse from the start, and they stay.
    {
        reset();
        JoinedHost host(PcInputConfig::defaults(InputScheme::DualAnalog));
        PromptController prompts;
        prompts.start(host.input.config(), 2u, false);
        host.run(prompts, 1.0);
        require(prompts.active() && prompts.shown() == PromptStyle::Keyboard && prompts.switches() == 0u, "joined: no pad, keyboard and mouse pictures");
    }
    // An Xbox-style pad that is connected before anything is touched: its pictures a quarter of a second
    // after the first sample; a key brings the keyboard's, a button the pad's again.
    {
        reset();
        JoinedHost host(PcInputConfig::defaults(InputScheme::DualAnalog));
        host.slots[2] = xinput_buttons(0u);
        host.input.scan_pads();
        PromptController prompts;
        prompts.start(host.input.config(), 2u, false);
        require(prompts.shown() == PromptStyle::Keyboard, "joined: before the first sample the pad is not known");
        host.run(prompts, 0.2);
        require(state.device.load() == InputDevice::Gamepad && state.pad_kind.load() == PadKind::Xbox && prompts.shown() == PromptStyle::Keyboard,
            "joined: the first sample makes a connected pad the device; the pictures wait for the debounce");
        host.run(prompts, 0.1);
        require(prompts.shown() == PromptStyle::Xbox && prompts.switches() == 1u, "joined: then the Xbox pictures, with nothing touched");
        host.keys.insert(key_w);
        host.run(prompts, 0.2);
        require(state.device.load() == InputDevice::KeyboardMouse && prompts.shown() == PromptStyle::Xbox, "joined: a bound key held: the device changes at once, the pictures not yet");
        host.run(prompts, 0.4);
        require(prompts.shown() == PromptStyle::Keyboard && prompts.switches() == 2u, "joined: then the keyboard's pictures");
        host.keys.clear();
        host.run(prompts, 1.0);
        require(prompts.shown() == PromptStyle::Keyboard && prompts.switches() == 2u, "joined: nothing touched, nothing changes");
        host.slots[2] = xinput_buttons(XINPUT_GAMEPAD_A);
        host.step(prompts);
        const double pressed_at = host.now;
        require(state.device.load() == InputDevice::Gamepad, "joined: a pad button makes the pad the device in that sample");
        while (prompts.shown() != PromptStyle::Xbox && host.now < pressed_at + 2.0) host.step(prompts);
        const double took = host.now - pressed_at;
        require(prompts.shown() == PromptStyle::Xbox && took >= 0.25 - 1.0e-6 && took < 0.25 + 2.5 / 60.0, "joined: the pad's pictures 250 ms after the button: " + std::to_string(took));
        std::printf("joined: pad button to pad pictures in %.0f ms\n", took * 1000.0);
        // Both in the same samples: as it was. Then a key alone, in a window without the focus: nothing.
        host.keys.insert(key_w);
        host.run(prompts, 1.0);
        require(prompts.shown() == PromptStyle::Xbox, "joined: key and pad button together leave the pictures alone");
        host.slots[2] = xinput_buttons(0u);
        host.focused = false;
        host.run(prompts, 1.0);
        require(prompts.shown() == PromptStyle::Xbox && state.device.load() == InputDevice::Gamepad, "joined: without the focus nothing is read and nothing changes");
        host.focused = true;
        // A replay or a script command owns the samples: the held key is set aside.
        host.input.set_scripted(true);
        host.run(prompts, 1.0);
        require(prompts.shown() == PromptStyle::Xbox && state.device.load() == InputDevice::Gamepad, "joined: scripted samples do not change the device");
        host.input.set_scripted(false);
        host.run(prompts, 1.0);
        require(prompts.shown() == PromptStyle::Keyboard, "joined: the player's samples again: the held key shows the keyboard's pictures");
        host.keys.clear();
        // The pad is unplugged while it is the device: its pictures stay, by the kind it had.
        host.slots[2] = xinput_buttons(XINPUT_GAMEPAD_B);
        host.run(prompts, 1.0);
        host.slots[2].reset();
        host.run(prompts, 1.0);
        require(prompts.shown() == PromptStyle::Xbox && state.pad_kind.load() == PadKind::Xbox && host.input.active_pad() == nullptr, "joined: unplugged, the pad's pictures stay");
        // A PlayStation pad arrives and is touched: its pictures.
        HidPadInfo info;
        info.vid = hid_sony_vendor; info.pid = 0x0CE6u; info.model = hid_pad_model(info.vid, info.pid); info.name = "Wireless Controller"; info.buttons = 15u; info.axes = 6u; info.hat = true;
        require(host.input.hid_arrived(0x51u, info), "joined: a PlayStation pad arrives");
        PadSample cross; cross.buttons = PadSample::bit(PadSample::A);
        for (const double until = host.now + 1.0; host.now < until;) { host.input.hid_report(0x51u, cross); host.step(prompts); }
        require(state.pad_kind.load() == PadKind::PlayStation && prompts.shown() == PromptStyle::PlayStation, "joined: the PlayStation pad's pictures");
        // The Xbox-style pad comes back and is touched while the PlayStation pad rests: Xbox pictures again.
        host.slots[1] = xinput_buttons(XINPUT_GAMEPAD_X);
        host.input.scan_pads();
        for (const double until = host.now + 1.5; host.now < until;) { host.input.hid_report(0x51u, PadSample{}); host.step(prompts); }
        require(state.pad_kind.load() == PadKind::Xbox && prompts.shown() == PromptStyle::Xbox, "joined: the pad touched last decides the kind");
        std::printf("joined: %llu style changes, all from the input code's samples\n", static_cast<unsigned long long>(prompts.switches()));
    }
    // The briefing's Continue: the game reads the pad there and ends no frame. A pad button pressed on that
    // screen shows the pad's pictures all the same, and a key the keyboard's.
    {
        reset();
        JoinedHost host(PcInputConfig::defaults(InputScheme::DualAnalog));
        host.slots[0] = xinput_buttons(0u);
        host.input.scan_pads();
        PromptController prompts;
        prompts.start(host.input.config(), 2u, false);
        host.keys.insert(key_w);
        host.run(prompts, 1.0);
        host.keys.clear();
        require(prompts.shown() == PromptStyle::Keyboard && prompts.switches() == 0u, "joined: a key held from the start, the keyboard's pictures");
        host.slots[0] = xinput_buttons(XINPUT_GAMEPAD_A);
        const double pressed_at = host.now;
        while (prompts.shown() != PromptStyle::Xbox && host.now < pressed_at + 2.0) host.sample_only(prompts);
        const double took = host.now - pressed_at;
        require(prompts.shown() == PromptStyle::Xbox && took >= 0.25 && took < 0.45, "joined: on a screen that ends no frame a pad button brings the pad's pictures: after " + std::to_string(took) + " s");
        std::printf("joined: no frame ends, pad button to pad pictures in %.0f ms\n", took * 1000.0);
        host.slots[0] = xinput_buttons(0u);
        host.keys.insert(key_w);
        const double key_at = host.now;
        while (prompts.shown() != PromptStyle::Keyboard && host.now < key_at + 2.0) host.sample_only(prompts);
        require(prompts.shown() == PromptStyle::Keyboard && prompts.switches() == 2u && host.now - key_at < 0.65, "joined: and a key the keyboard's, not sooner than half a second after the switch before");
    }
    // Glyphs names a kind: an Xbox-style pad is shown with PlayStation pictures. Prompts=keyboard and
    // Prompts=gamepad do not follow the device.
    {
        reset();
        auto config = PcInputConfig::defaults(InputScheme::DualAnalog);
        (void)config.apply("[Input.Gamepad]\nGlyphs=playstation\n");
        JoinedHost host(config);
        host.slots[0] = xinput_buttons(XINPUT_GAMEPAD_A);
        host.input.scan_pads();
        PromptController prompts;
        prompts.start(host.input.config(), 2u, false);
        host.run(prompts, 0.5);
        require(state.pad_kind.load() == PadKind::Xbox && prompts.shown() == PromptStyle::PlayStation, "joined: Glyphs=playstation over an Xbox-style pad");
    }
    {
        reset();
        auto config = PcInputConfig::defaults(InputScheme::DualAnalog);
        (void)config.apply("[Input]\nPrompts=keyboard\n");
        JoinedHost host(config);
        host.slots[0] = xinput_buttons(XINPUT_GAMEPAD_A);
        host.input.scan_pads();
        PromptController prompts;
        prompts.start(host.input.config(), 2u, false);
        host.run(prompts, 1.0);
        require(state.device.load() == InputDevice::Gamepad && prompts.shown() == PromptStyle::Keyboard && prompts.switches() == 0u, "joined: Prompts=keyboard with a pad in use");
    }
    {
        reset();
        auto config = PcInputConfig::defaults(InputScheme::DualAnalog);
        (void)config.apply("[Input]\nPrompts=gamepad\n");
        JoinedHost host(config);
        host.keys.insert(key_w);
        PromptController prompts;
        prompts.start(host.input.config(), 2u, false);
        require(prompts.shown() == PromptStyle::Xbox, "joined: Prompts=gamepad without a pad starts with Xbox pictures");
        host.run(prompts, 1.0);
        require(state.device.load() == InputDevice::KeyboardMouse && prompts.shown() == PromptStyle::Xbox && prompts.switches() == 0u, "joined: Prompts=gamepad with the keyboard in use");
    }
    {
        reset();
        auto config = PcInputConfig::defaults(InputScheme::DualAnalog);
        (void)config.apply("[Input]\nPrompts=psp\n");
        JoinedHost host(config);
        host.slots[0] = xinput_buttons(XINPUT_GAMEPAD_A);
        host.input.scan_pads();
        PromptController prompts;
        prompts.start(host.input.config(), 2u, false);
        host.run(prompts, 1.0);
        require(!prompts.active() && !vcs::ge_texture_mods_keying_for(5u, 256u, 512u), "joined: Prompts=psp from the ini: the game's pictures, nothing keyed");
    }
    reset();
}
}

int main(int argc, char **argv) {
    try {
        std::filesystem::path folder = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::temp_directory_path() / "chili_prompt_glyphs";
        std::filesystem::create_directories(folder);
        GlyphPainter painter;
        std::string error;
        require(painter.start(error), "the glyph painter starts: " + error);
        require(!painter.font_name().empty(), "a font was found");
        std::printf("font: %s\n", painter.font_name().c_str());
        for (const PromptStyle style : {PromptStyle::Keyboard, PromptStyle::Xbox, PromptStyle::PlayStation})
            for (const unsigned scale : {1u, 2u, 4u}) sheet(painter, style, scale, folder);
        overlays(painter, folder);
        marks(painter, folder);
        kept_base(painter);
        table();
        styles();
        controller();
        joined();
        if (failures) { std::cerr << "prompt glyph test failed: " << failures << " of " << checks << " checks\n"; return 1; }
        std::cout << "prompt glyph checks passed: " << checks << " (pictures in " << folder.string() << ")\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "prompt glyph test failed after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
