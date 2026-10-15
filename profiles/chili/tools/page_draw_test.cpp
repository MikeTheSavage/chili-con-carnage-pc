// The host page's drawing (host/page_draw.hpp) and the picture rectangle (host/picture_rect.hpp), without the
// game and without a graphics card:
//
//   the picture's rectangle at window sizes with bars beside and above it, with whole multiples, stretched;
//       a client pixel as a place in the picture and back
//   the glyph table found in a made-up memory image: behind a font record that names it by a relative and
//       by an absolute pointer, without a record, among decoys, and not found where there is none
//   a line of text set as the game sets it. The places are the game's own, read from a capture of Profile
//       Options (the first and last pixel columns of every glyph of `Settings`, `Back` and the headline, the
//       four corners of the tilted `P`): the same boxes must give the same rectangles
//   the draw list: three copies in the game's order, runs shared by neighbours, colours, clipping
//
// No game data is used: the glyph table here is made up from the widths of the letters the capture shows.
#include "page_draw.hpp"
#include "picture_rect.hpp"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
using namespace chili;
unsigned checks, failures;
const bool keep_going = [] { const char *value = std::getenv("CHILI_TEST_KEEP_GOING"); return value && *value == '1'; }();
void require(bool ok, const std::string &what) {
    ++checks;
    if (ok) return;
    if (!keep_going) throw std::runtime_error(what);
    ++failures;
    std::cerr << "FAILED CHECK: " << what << '\n';
}
template <class A, class B> void is(const A &a, const B &b, const std::string &what) {
    require(a == b, what + ": " + std::to_string(a) + " is not " + std::to_string(b));
}

// A glyph as the font record has it: a cell `width` wide at (x, y) of the 256 x 512 sheet.
PageGlyph cell(char32_t code, float x, float y, float width, bool picture = false) {
    PageGlyph glyph;
    glyph.code = static_cast<std::uint16_t>(code);
    glyph.y_offset = 9.0f; glyph.advance = width;
    glyph.u = x / 256.0f; glyph.v = y / 512.0f; glyph.u_width = width / 256.0f;
    glyph.picture = picture;
    return glyph;
}
// The ten digits first, as in the game's table, then the letters the capture's words need, with the widths
// the capture shows, and two pictures.
std::vector<PageGlyph> made_up_table() {
    std::vector<PageGlyph> glyphs;
    for (unsigned digit = 0; digit < 10u; ++digit) glyphs.push_back(cell(U'0' + digit, 22.0f * static_cast<float>(digit), 0.0f, 21.0f));
    static constexpr struct { char32_t code; float x, y, width; } letters[]{
        {U' ', 20, 30, 14}, {U'a', 35, 30, 20}, {U'c', 76, 30, 19}, {U'e', 117, 30, 20}, {U'f', 138, 30, 21}, {U'g', 160, 30, 20},
        {U'i', 201, 30, 13}, {U'k', 233, 30, 21}, {U'l', 0, 60, 13}, {U'n', 43, 60, 20}, {U'o', 64, 60, 19}, {U'p', 84, 60, 21},
        {U'r', 127, 60, 20}, {U's', 148, 60, 19}, {U't', 168, 60, 18}, {U'B', 108, 90, 20}, {U'O', 125, 120, 20}, {U'P', 146, 120, 21},
        {U'S', 211, 120, 20}, {U'D', 150, 90, 20}, {U'E', 171, 90, 20}, {U'G', 214, 90, 20}, {U'L', 58, 120, 18}, {U'M', 77, 120, 24},
        {U'h', 181, 30, 19}, {U'm', 14, 60, 28}, {U'v', 207, 60, 18}};
    for (const auto &letter : letters) glyphs.push_back(cell(letter.code, letter.x, letter.y, letter.width));
    glyphs.push_back(cell(U'}', 54.0f, 420.0f, 26.0f, true));
    glyphs.push_back(cell(0x00A5, 63.0f, 330.0f, 20.0f, true));
    return glyphs;
}
PageFont made_up_font() { PageFont font; font.assign(made_up_table(), 30.0f); return font; }

void store(std::vector<std::uint8_t> &memory, std::size_t at, std::uint32_t value) { std::memcpy(memory.data() + at, &value, 4u); }
// A font record at `record` whose table is at `table`: the cell height 4 bytes in, the pointer and the count
// 0x40 and 0x44 bytes in.
void put_record(std::vector<std::uint8_t> &memory, std::size_t record, std::size_t table, std::uint32_t count, std::uint32_t pointer) {
    store(memory, record + 4u, 30u);
    store(memory, record + 0x40u, pointer);
    store(memory, record + 0x44u, count);
    (void)table;
}
void put_table(std::vector<std::uint8_t> &memory, std::size_t table, const std::vector<PageGlyph> &glyphs) {
    for (std::size_t i = 0; i < glyphs.size(); ++i) PageFont::write(glyphs[i], memory.data() + table + i * PageFont::entry_size);
}

void test_picture_rect() {
    const auto same = [](PictureRect rect, int x, int y, int width, int height, const std::string &what) {
        require(rect.x == x && rect.y == y && rect.width == width && rect.height == height,
            what + ": (" + std::to_string(rect.x) + ", " + std::to_string(rect.y) + ") " + std::to_string(rect.width) + "x" + std::to_string(rect.height));
    };
    same(picture_rect(960u, 544u), 0, 0, 960, 544, "a window of twice the PSP's size is all picture");
    same(picture_rect(1440u, 816u), 0, 0, 1440, 816, "three times");
    same(picture_rect(3840u, 2160u), 14, 0, 3812, 2160, "a 2160p screen: bars of 14 pixels beside the picture");
    same(picture_rect(2560u, 1440u), 9, 0, 2541, 1440, "a 1440p screen");
    same(picture_rect(1920u, 1080u), 7, 0, 1906, 1080, "a 1080p screen");
    same(picture_rect(1600u, 1000u), 0, 46, 1600, 907, "a tall client area: bars above and below");
    same(picture_rect(1000u, 600u, true, true), 20, 28, 960, 544, "whole multiples only");
    same(picture_rect(300u, 200u, true, true), 0, 15, 300, 170, "smaller than the PSP's screen: no whole multiple, fitted");
    same(picture_rect(1000u, 600u, false), 0, 0, 1000, 600, "stretched over the client area");
    same(picture_rect(0u, 600u), 0, 0, 0, 0, "no client area");
    require(picture_rect(0u, 0u).empty(), "an empty rectangle says so");

    const PictureRect rect = picture_rect(3840u, 2160u);
    float x = 0.0f, y = 0.0f;
    require(rect.to_picture(14, 0, x, y) && x >= 0.0f && x < 0.1f && y >= 0.0f && y < 0.1f, "the picture's first pixel is its top left corner");
    require(rect.to_picture(14 + 3811, 2159, x, y) && x > 479.9f && x < 480.0f && y > 271.9f && y < 272.0f, "its last pixel is the bottom right corner");
    require(!rect.to_picture(13, 100, x, y) && x < 0.0f, "a pixel in the left bar is outside, and left of the picture");
    require(!rect.to_picture(14 + 3812, 100, x, y) && x > 480.0f, "a pixel in the right bar is outside");
    require(rect.to_picture(14 + 1906, 1080, x, y) && std::abs(x - 240.0f) < 0.1f && std::abs(y - 136.0f) < 0.1f, "the middle is (240, 136)");
    float cx = 0.0f, cy = 0.0f;
    rect.to_client(240.0f, 136.0f, cx, cy);
    require(std::abs(cx - 1920.0f) < 0.01f && std::abs(cy - 1080.0f) < 0.01f, "and (240, 136) is the middle of the client area");
    require(!PictureRect{}.to_picture(0, 0, x, y), "nothing is inside an empty rectangle");
}

void test_font_scan() {
    const auto glyphs = made_up_table();
    const std::size_t record = 0x2000u, table = 0x2A00u;
    // A relative pointer, as the block's own records have them; the bytes behind the table look like more
    // glyphs, so only the record says where it ends.
    {
        std::vector<std::uint8_t> memory(0x8000u);
        auto longer = glyphs;
        for (unsigned extra = 0; extra < 5u; ++extra) longer.push_back(cell(U'A' + extra, 10.0f, 200.0f, 12.0f));
        put_table(memory, table, longer);
        put_record(memory, record, table, static_cast<std::uint32_t>(glyphs.size()), static_cast<std::uint32_t>(table - (record + 0x40u)));
        // Decoys: a lone '0', and digits that do not go on to ten.
        PageFont::write(cell(U'0', 0.0f, 0.0f, 21.0f), memory.data() + 0x400u);
        for (unsigned digit = 0; digit < 6u; ++digit) PageFont::write(cell(U'0' + digit, 0.0f, 0.0f, 21.0f), memory.data() + 0x800u + digit * PageFont::entry_size);
        PageFont font;
        require(font.scan(memory, 0x08800000u), "the table behind a font record is found");
        is(font.table_offset(), table, "where the table is");
        require(font.header_found(), "the font record was recognised");
        is(font.glyphs().size(), glyphs.size(), "the record's count ends the table, not the look of the bytes behind it");
        is(font.cell_height(), 30.0f, "the cell height comes from the record");
        const PageGlyph *s = font.find(U'S');
        require(s && s->width() == 20.0f && s->y_offset == 9.0f && !s->picture && std::abs(s->u * 256.0f - 211.0f) < 0.001f, "a letter's cell");
        const PageGlyph *lock = font.find(U'}');
        require(lock && lock->picture, "a picture is marked");
        require(font.find(0x00A5) && font.find(0x00A5)->picture, "a picture above ASCII");
        require(!font.find(U'<') && !font.find(0x2026), "a code point the font lacks");
    }
    // An absolute guest address in the record.
    {
        std::vector<std::uint8_t> memory(0x8000u);
        put_table(memory, table, glyphs);
        put_record(memory, record, table, static_cast<std::uint32_t>(glyphs.size()), 0x08800000u + static_cast<std::uint32_t>(table));
        PageFont font;
        require(font.scan(memory, 0x08800000u) && font.header_found() && font.glyphs().size() == glyphs.size(), "a record with a guest address is recognised too");
    }
    // No record: as long as the entries look like glyphs.
    {
        std::vector<std::uint8_t> memory(0x8000u);
        put_table(memory, table, glyphs);
        PageFont font;
        require(font.scan(memory, 0x08800000u) && !font.header_found(), "without a record the table is still found");
        is(font.glyphs().size(), glyphs.size(), "and ends where the bytes stop being glyphs");
    }
    // Nothing there.
    {
        std::vector<std::uint8_t> memory(0x4000u);
        PageFont::write(cell(U'0', 0.0f, 0.0f, 21.0f), memory.data() + 0x400u);
        PageFont font;
        require(!font.scan(memory) && !font.loaded() && font.find(U'0') == nullptr, "no table: nothing is found and nothing can be looked up");
        require(!font.scan(std::span<const std::uint8_t>{}), "an empty memory");
    }
    // The table at the very end of the memory.
    {
        std::vector<std::uint8_t> memory(0x1000u + glyphs.size() * PageFont::entry_size);
        put_table(memory, 0x1000u, glyphs);
        PageFont font;
        require(font.scan(memory) && font.glyphs().size() == glyphs.size(), "a table that ends with the memory");
    }
}

// The six vertices of the quad at index `quad` of the list, as the rectangle they cover and the first vertex.
struct Quad { float x0, y0, x1, y1; vcs::GeGpuOverlayVertex first; std::array<PagePoint, 4> corners; };
Quad quad_at(const vcs::GeGpuOverlay &list, std::size_t quad) {
    const auto *v = list.vertices.data() + quad * 6u;
    // top left, top right, bottom right, top left, bottom right, bottom left
    return {std::min(v[0].x, v[5].x), std::min(v[0].y, v[1].y), std::max(v[1].x, v[2].x), std::max(v[2].y, v[5].y), v[0],
            {{{v[0].x, v[0].y}, {v[1].x, v[1].y}, {v[2].x, v[2].y}, {v[5].x, v[5].y}}}};
}

void test_text() {
    const PageFont font = made_up_font();
    // `Settings`, a menu line in Profile Options: the box at (130, 208).
    {
        PageDraw draw;
        const PageBox box = draw.text(font, "Settings", 130.0f, 208.0f, PageTextStyle::line());
        const auto list = draw.finish();
        is(list->vertices.size(), std::size_t{8u * 6u * 3u}, "eight glyphs in three copies");
        is(list->runs.size(), std::size_t{3u}, "three runs: shadow, outline, fill");
        require(list->runs[0].texture == page_texture::font_fill && list->runs[1].texture == page_texture::font_outline &&
                list->runs[2].texture == page_texture::font_fill, "the shadow is the fill sheet, then the outline sheet, then the fill sheet");
        for (const auto &run : list->runs) require(run.alpha_reference == 12u && run.vertex_count == 48u && !run.clipped && run.flags == 0u, "the game's alpha test in every run");
        static constexpr int left[]{130, 142, 154, 164, 174, 181, 193, 205}, right[]{146, 158, 168, 178, 185, 197, 209, 220};
        for (std::size_t i = 0; i < 8u; ++i) {
            const Quad shadow = quad_at(*list, i), outline = quad_at(*list, 8u + i), fill = quad_at(*list, 16u + i);
            const std::string glyph = "glyph " + std::to_string(i) + " of Settings";
            is(fill.x0, static_cast<float>(left[i]), glyph + ": first column");
            is(fill.x1, static_cast<float>(right[i]), glyph + ": the column after the last");
            is(fill.y0, 207.0f, glyph + ": top row");
            is(fill.y1, 230.0f, glyph + ": the row after the last");
            require(outline.x0 == fill.x0 && outline.x1 == fill.x1 && outline.y0 == fill.y0 && outline.y1 == fill.y1, glyph + ": the outline is the same rectangle");
            require(shadow.x0 == fill.x0 + 2.0f && shadow.y0 == fill.y0 + 2.0f && shadow.x1 == fill.x1 + 2.0f && shadow.y1 == fill.y1 + 2.0f, glyph + ": the shadow two pixels right and down");
            is(fill.first.rgba, 0xFF00A0FFu, glyph + ": the line's orange, as the game's vertex colour");
            is(outline.first.rgba, 0xFF000050u, glyph + ": the dark red outline");
            is(shadow.first.rgba, 0xFF000000u, glyph + ": the black shadow");
        }
        // The first glyph's texels: the cell of `S` at (211, 120), 20 wide and 29 of its 30 rows.
        const auto *v = list->vertices.data() + 16u * 6u;
        require(std::abs(v[0].u * 256.0f - 211.0f) < 0.001f && std::abs(v[0].v * 512.0f - 120.0f) < 0.001f &&
                std::abs(v[2].u * 256.0f - 231.0f) < 0.001f && std::abs(v[2].v * 512.0f - 149.0f) < 0.001f, "the cell's texels: 20 columns and 29 rows");
        require(box.x0 == 130.0f && std::abs(box.x1 - 220.4f) < 0.01f && std::abs(box.y0 - 207.2f) < 0.01f && std::abs(box.y1 - 230.4f) < 0.01f, "the returned box is the text's own");
        require(std::abs(page_text_width(font, "Settings", PageTextStyle::line()) - 90.4f) < 0.01f, "the width: every advance less 5, times 0.8, the last glyph whole");
    }
    // The headline `Profile Options`: the box at (120, 20), 350 wide, the text ends at its right edge.
    {
        PageDraw draw;
        draw.text(font, "Profile Options", 120.0f, 20.0f, PageTextStyle::headline());
        const auto list = draw.finish();
        const Quad shadow = quad_at(*list, 0u), first = quad_at(*list, 30u), last = quad_at(*list, 44u);
        is(first.x0, 269.0f, "the headline's first column");
        is(first.x1, 290.0f, "its first glyph is 21 wide at scale 1");
        is(last.x1, 470.0f, "the headline ends at the box's right edge");
        is(first.y0, 25.0f, "the headline's top row");
        is(first.y1, 54.0f, "29 rows at scale 1");
        require(shadow.x0 == 272.0f && shadow.y0 == 28.0f, "the headline's shadow is three pixels off");
        is(first.first.rgba, 0xFF0050FFu, "the headline's hot orange");
        is(quad_at(*list, 15u).first.rgba, 0xFF00003Fu, "the headline's outline");
        static constexpr int steps[]{16, 15, 14, 16, 8, 8, 15, 9, 15, 16, 13, 8, 14, 15};
        for (std::size_t i = 0; i < 14u; ++i)
            is(quad_at(*list, 31u + i).x0 - quad_at(*list, 30u + i).x0, static_cast<float>(steps[i]), "the step after glyph " + std::to_string(i) + " of the headline");
    }
    // `Back`, a legend at scale 0.7: the box at (44, 239).
    {
        PageDraw draw;
        draw.text(font, "Back", 44.0f, 239.0f, PageTextStyle::legend());
        const auto list = draw.finish();
        static constexpr int left[]{44, 54, 65, 74}, right[]{58, 68, 78, 89};
        for (std::size_t i = 0; i < 4u; ++i) {
            const Quad fill = quad_at(*list, 8u + i);
            is(fill.x0, static_cast<float>(left[i]), "glyph " + std::to_string(i) + " of Back: first column");
            is(fill.x1, static_cast<float>(right[i]), "glyph " + std::to_string(i) + " of Back: the column after the last");
            require(fill.y0 == 238.0f && fill.y1 == 258.0f, "a legend's rows");
        }
    }
    // Three lines whose columns only the game's single-precision pen gives: a pen kept in double precision
    // puts a glyph or two of each a pixel to the right.
    {
        static constexpr struct { const char *text; float x, y; int top; std::size_t count; int left[17], right[17]; } lines[]{
            {"El Gringo Loco", 195.0f, 87.0f, 86, 14u, {195, 207, 213, 220, 232, 244, 250, 263, 275, 286, 293, 303, 315, 326},
                {211, 217, 224, 236, 248, 254, 267, 279, 290, 297, 307, 319, 330, 341}},
            {"El Macho", 182.0f, 112.0f, 111, 8u, {182, 194, 200, 207, 222, 234, 245, 257}, {198, 204, 211, 226, 238, 249, 261, 272}},
            {"Delete save games", 169.0f, 133.0f, 132, 17u, {169, 181, 193, 199, 211, 221, 233, 240, 252, 264, 274, 286, 293, 305, 317, 336, 348},
                {185, 197, 203, 215, 225, 237, 244, 256, 268, 278, 290, 297, 309, 321, 340, 352, 363}}};
        for (const auto &line : lines) {
            PageDraw draw;
            draw.text(font, line.text, line.x, line.y, PageTextStyle::line());
            const auto list = draw.finish();
            is(list->vertices.size(), line.count * 6u * 3u, std::string("every glyph of ") + line.text + " is set");
            for (std::size_t i = 0; i < line.count; ++i) {
                const Quad shadow = quad_at(*list, i), fill = quad_at(*list, 2u * line.count + i);
                const std::string glyph = "glyph " + std::to_string(i) + " of " + line.text;
                is(fill.x0, static_cast<float>(line.left[i]), glyph + ": first column");
                is(fill.x1, static_cast<float>(line.right[i]), glyph + ": the column after the last");
                is(fill.y0, static_cast<float>(line.top), glyph + ": top row");
                require(shadow.x0 == fill.x0 + 2.0f && shadow.x1 == fill.x1 + 2.0f && shadow.y0 == fill.y0 + 2.0f, glyph + ": the shadow two pixels right and down");
            }
        }
    }
    // The tilted category line: `P` of the box (20, 50), 300 x 30, turned 8 degrees about its middle.
    {
        PageDraw draw;
        draw.text(font, "P", 20.0f, 50.0f, PageTextStyle::category());
        const auto list = draw.finish();
        const Quad fill = quad_at(*list, 2u), shadow = quad_at(*list, 0u);
        static constexpr int corners[4][2]{{20, 75}, {36, 73}, {39, 96}, {23, 98}};   // top left, top right, bottom right, bottom left
        for (std::size_t i = 0; i < 4u; ++i)
            require(fill.corners[i].x == static_cast<float>(corners[i][0]) && fill.corners[i].y == static_cast<float>(corners[i][1]),
                "corner " + std::to_string(i) + " of the tilted P: (" + std::to_string(fill.corners[i].x) + ", " + std::to_string(fill.corners[i].y) + ")");
        require(shadow.corners[0].x == 22.0f && shadow.corners[0].y == 77.0f, "its shadow: the box two pixels right and down, turned about the same point");
    }
    // Alignment, pictures, unknown code points, UTF-8.
    {
        PageDraw draw;
        PageTextStyle right = PageTextStyle::value();
        right.align = PageAlign::Right; right.width = 100.0f;
        draw.text(font, "on", 291.0f, 72.0f, right);
        const auto list = draw.finish();
        is(list->runs.size(), std::size_t{1u}, "a value has no outline: shadow and fill are one run of the fill sheet");
        is(quad_at(*list, 3u).x1, 391.0f, "a value ends at the right edge it was given");
        is(quad_at(*list, 3u).first.rgba, 0xFFFFFFFFu, "a value is white");
        PageTextStyle centre = PageTextStyle::line();
        centre.align = PageAlign::Centre; centre.width = 200.0f; centre.whole_pixels = false;
        PageDraw middle;
        const PageBox box = middle.text(font, "Back", 100.0f, 50.0f, centre);
        require(std::abs((box.x0 + box.x1) * 0.5f - 200.0f) < 0.01f, "centred text has its middle in the middle of the box");
    }
    {
        PageDraw draw;
        PageTextStyle locked = PageTextStyle::line();
        locked.fill = locked.fill.dimmed(0.5f); locked.outline = locked.outline.dimmed(0.5f); locked.picture_tint = 0.5f;
        std::string text = "a}";
        page_append(text, 0x00A5);
        text += "<";                       // not in the font: left out
        page_append(text, 0x2026);         // neither
        draw.text(font, text, 10.0f, 10.0f, locked);
        const auto list = draw.finish();
        is(list->vertices.size(), std::size_t{3u * 6u * 3u}, "three glyphs are set, the two the font lacks are left out");
        is(quad_at(*list, 6u).first.rgba, 0xFF005080u, "a locked line's letter: half the orange");
        is(quad_at(*list, 7u).first.rgba, 0xFF808080u, "a picture keeps its colours, at half brightness on a locked line");
        is(quad_at(*list, 8u).first.rgba, 0xFF808080u, "the picture above ASCII too");
        is(quad_at(*list, 4u).first.rgba, 0xFF808080u, "the picture's outline twin keeps its colours as well");
        is(quad_at(*list, 1u).first.rgba, 0xFF000000u, "the picture's shadow is its shape in black");
        std::string_view rest = text;
        require(page_next_code(rest) == U'a' && page_next_code(rest) == U'}' && page_next_code(rest) == 0x00A5 && page_next_code(rest) == U'<' &&
                page_next_code(rest) == 0x2026 && rest.empty(), "UTF-8 is read back as it was written");
        std::string_view latin = "\xA5";
        require(page_next_code(latin) == 0x00A5 && latin.empty(), "a lone byte above ASCII is taken as Latin-1");
        require(page_text(page_glyph::cross) == "\xC2\xA5", "the Cross picture as text");
    }
}

void test_list() {
    const PageFont font = made_up_font();
    PageDraw draw;
    require(draw.empty(), "a new list is empty");
    draw.sprite(page_texture::background_page, {0.0f, 0.0f, 480.0f, 272.0f});
    draw.rect({10.0f, 10.0f, 20.0f, 20.0f}, {1.0f, 0.0f, 0.0f, 0.5f});
    draw.rect({30.0f, 10.0f, 40.0f, 20.0f}, page_colour::black);
    draw.triangle({0.0f, 0.0f}, {4.0f, 0.0f}, {2.0f, 4.0f}, page_colour::white);
    draw.text(font, "Back", 44.0f, 239.0f, PageTextStyle::legend());
    draw.text(font, "Back", 44.0f, 139.0f, PageTextStyle::legend());
    draw.clip({100.0f, 0.0f, 200.0f, 272.0f});
    draw.rect({90.0f, 0.0f, 210.0f, 10.0f}, page_colour::white);
    draw.rect({90.0f, 20.0f, 210.0f, 30.0f}, page_colour::white);
    draw.unclip();
    draw.rect({90.0f, 40.0f, 210.0f, 50.0f}, page_colour::white);
    draw.sprite(page_texture::button_cross, {8.0f, 232.0f, 40.0f, 264.0f}, {}, {0.0f, 0.0f, 1.0f, 1.0f}, vcs::kGeGpuOverlayNearest);
    const PageBox placed = draw.glyph(font, 0x00A5, {200.0f, 236.0f, 232.0f, 260.0f});
    is(draw.quads(), std::size_t{1u + 2u + 24u + 3u + 1u + 2u}, "rectangles counted");
    const auto list = draw.finish();
    require(draw.empty() && draw.quads() == 0u, "finish starts a new list");
    // background | plain (two rectangles and a triangle) | shadow | outline | fill + next shadow | outline | fill |
    // plain clipped | plain | sprite nearest | outline cell | fill cell
    is(list->runs.size(), std::size_t{12u}, "neighbours with the same texture and state share a run");
    require(list->runs[0].texture == page_texture::background_page && list->runs[0].vertex_count == 6u && list->runs[0].alpha_reference == 0u, "the background");
    require(list->runs[1].texture == 0u && list->runs[1].vertex_count == 15u, "two rectangles and a triangle in one plain run");
    require(list->runs[4].texture == page_texture::font_fill && list->runs[4].vertex_count == 48u, "a text's fill and the next text's shadow share a run");
    require(list->runs[7].clipped && list->runs[7].vertex_count == 12u && list->runs[7].clip[0] == 100.0f && list->runs[7].clip[2] == 200.0f, "the clipped rectangles");
    require(!list->runs[8].clipped && list->runs[8].texture == 0u, "after unclip a new run");
    require(list->runs[9].flags == vcs::kGeGpuOverlayNearest, "the flag of a sprite");
    require(list->runs[10].texture == page_texture::font_outline && list->runs[11].texture == page_texture::font_fill, "a single cell: the outline twin under the picture");
    std::uint32_t next = 0u;
    for (const auto &run : list->runs) { is(run.first_vertex, next, "runs follow each other without a gap"); require(run.vertex_count % 3u == 0u, "whole triangles"); next += run.vertex_count; }
    is(static_cast<std::size_t>(next), list->vertices.size(), "the runs cover every vertex");
    is(list->vertices[6].rgba, 0x800000FFu, "half transparent red: bytes R G B A");
    // A cell fitted by its height: 29 rows in 24 pixels, 20 columns centred in the box.
    require(std::abs(placed.height() - 24.0f) < 0.001f && std::abs(placed.width() - 20.0f * 24.0f / 29.0f) < 0.001f &&
            std::abs((placed.x0 + placed.x1) * 0.5f - 216.0f) < 0.001f, "a font picture fitted into a box");
    require(page_textures.size() <= 16u, "the page's textures fit the watch table");
    for (std::size_t i = 0; i < page_textures.size(); ++i)
        for (std::size_t j = i + 1u; j < page_textures.size(); ++j) require(page_textures[i].key != page_textures[j].key, "every texture is named once");
}
}

int main() {
    try {
        test_picture_rect();
        test_font_scan();
        test_text();
        test_list();
    } catch (const std::exception &error) {
        std::cerr << "page draw test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "page draw test: " << checks << " checks, " << failures << " failed\n";
    return failures ? 1 : 0;
}
