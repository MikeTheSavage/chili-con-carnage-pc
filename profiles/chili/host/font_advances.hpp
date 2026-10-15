#pragma once
// The letter spacing of a replaced text font (mods/TEXTURES.md, "The letter spacing of a replaced font").
//
// The game moves its pen by (the glyph's advance + the text box's spacing) x scale. Its glyph table holds
// advance = cell width for every glyph and x offset 0, and every text box has spacing -5: a letter steps by
// its cell's width less 5 whatever a replacement sheet draws in the cell, so a typeface whose f, r, t or j is
// narrower than the game's stands loose. A pack that replaces the font's sheet says what its typeface wants
// in a text file beside the sheet:
//
//   mods\<pack>\textures\5d0c6d1a663a8515.advances.txt
//
//   chili-font-advances 1
//   sheet 5d0c6d1a663a8515 0123456789abcdef     the XXH64 of the bytes of the PNG file beside this file
//   face Saira                                  optional, for the log
//   glyph U+0066 11.42 -4.79                    code point, pen step, cell offset
//
// Numbers are texels of the game's 256 x 512 sheet, which are PSP pixels at text scale 1. The pen step is
// how far the pen moves after the letter in a box with the game's spacing: the letter's own advance. The cell
// offset is where the left edge of the letter's cell lies from the start of that advance: (step - cell width)
// / 2 for a letter drawn in the middle of its cell. `#` starts a comment; fields are separated by spaces or tabs.
// A file that does not follow this, or names a number outside (3, 48) for a step or (-24, 8) for an offset,
// is refused whole.
//
// Nothing here touches the game or a file. The game reads the table in place on every width and draw call
// (the font module at 0x08808000: glyph lookup 0x0880B6A0, advance 0x08808C00, string width 0x08808CCC, word
// wrap 0x08809438, the quad builder 0x08809F84, which applies the x offset as page_draw.hpp does), so the two
// tables of todpsp_game.main, the text font's and its outline twin's, are served with
//
//   advance  = step + 5      a box with spacing -5 then steps by `step`; a box with another spacing changes
//                            the step by what it changes the game's own
//   x offset = offset + 2.5  the game lets neighbouring cells overlap by those 5 texels, half on each side: a
//                            letter's own advance begins 2.5 texels behind the game's pen. So a line keeps
//                            its place between a box's edges and beside a button picture, and a file with
//                            the game's own numbers (step = cell width - 5, offset = -2.5) is the game's table
//
// for every letter the file names (file_runtime.cpp serves the two ranges from memory). Code points, cells
// (u, v, u width), the y offset, the picture flag and the pictures themselves (buttons, locks, medals) stay
// as they are, and a block whose tables are not the game's own is left alone.
#include "menu_entry_blocks.hpp"
#include "mod_files.hpp"
#include "page_draw.hpp"
#include "state_hash.hpp"
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
namespace chili::font_advances {
inline constexpr unsigned version = 1u;
// What every text box of the game adds to a glyph's advance.
inline constexpr float box_spacing = -5.0f;
// A step or an offset must lie between these, ends excluded (the game's own steps are 5 to 37).
inline constexpr float least_step = 3.0f, most_step = 48.0f, least_offset = -24.0f, most_offset = 8.0f;
// The game's tables: 181 glyphs in cells 30 rows high, in both fonts.
inline constexpr std::size_t table_glyphs = 181u;
inline constexpr float table_cell_rows = 30.0f;
// The block that holds the two fonts; the per-language blocks hold none.
inline constexpr char block_name[] = "todpsp_game.main";
static_assert(text_font_sheet_key == page_texture::font_fill);

struct Glyph { float step{}, offset{}; };
struct FontAdvances {
    std::string face;
    std::uint64_t sheet_key{}, sheet_hash{};
    std::map<std::uint16_t, Glyph> glyphs;   // by code point
};

inline std::string hex16(std::uint64_t value) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

// The file's text as a FontAdvances. False with the line and the reason in `problem`; `out` is then empty.
inline bool parse(std::string_view text, FontAdvances &out, std::string &problem) {
    out = {};
    unsigned line_number = 0u;
    const auto fail = [&](const std::string &what) {
        out = {};
        problem = (line_number ? "line " + std::to_string(line_number) + ": " : std::string()) + what;
        return false;
    };
    const auto hex = [](std::string_view word, std::size_t most, std::uint64_t &value) {
        if (word.empty() || word.size() > most) return false;
        value = 0u;
        for (const char c : word) {
            if (c >= '0' && c <= '9') value = (value << 4u) | static_cast<std::uint64_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value = (value << 4u) | static_cast<std::uint64_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value = (value << 4u) | static_cast<std::uint64_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    };
    const auto number = [](std::string_view word, float &value) {
        const auto result = std::from_chars(word.data(), word.data() + word.size(), value, std::chars_format::fixed);
        return result.ec == std::errc() && result.ptr == word.data() + word.size() && std::isfinite(value);
    };
    // A word of the file for a message: what a log line can carry.
    const auto shown = [](std::string_view word) {
        std::string plain;
        for (const char c : word.substr(0u, 24u)) plain.push_back(c >= 32 && c < 127 ? c : '?');
        return plain;
    };
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3u);
    bool versioned = false, sheet = false, face = false;
    std::vector<std::string_view> fields;
    while (!text.empty()) {
        const auto end = std::min(text.find('\n'), text.size());
        auto line = text.substr(0u, end);
        text.remove_prefix(std::min(end + 1u, text.size()));
        ++line_number;
        line = line.substr(0u, line.find('#'));
        fields.clear();
        for (auto rest = line;;) {
            const auto start = rest.find_first_not_of(" \t\r");
            if (start == std::string_view::npos) break;
            rest.remove_prefix(start);
            const auto stop = std::min(rest.find_first_of(" \t\r"), rest.size());
            fields.push_back(rest.substr(0u, stop));
            rest.remove_prefix(stop);
        }
        if (fields.empty()) continue;
        if (!versioned) {
            if (fields[0] != "chili-font-advances" || fields.size() != 2u) return fail("the first line must be chili-font-advances 1");
            if (fields[1] != "1") return fail("version " + shown(fields[1]) + " is not known to this build");
            versioned = true;
        } else if (fields[0] == "sheet") {
            if (sheet) return fail("a second sheet line");
            if (fields.size() != 3u || fields[1].size() != 16u || !hex(fields[1], 16u, out.sheet_key) || fields[2].size() != 16u || !hex(fields[2], 16u, out.sheet_hash))
                return fail("expected sheet <key> <hash>, 16 hex digits each");
            if (out.sheet_key != text_font_sheet_key) return fail("the sheet " + shown(fields[1]) + " is not the text font's, " + hex16(text_font_sheet_key));
            sheet = true;
        } else if (fields[0] == "face") {
            if (face) return fail("a second face line");
            if (fields.size() < 2u) return fail("expected face <name>");
            for (std::size_t i = 1; i < fields.size(); ++i) {
                for (const char c : fields[i]) if (c < 32 || c > 126) return fail("the face's name is not plain ASCII");
                out.face += (i > 1u ? " " : "") + std::string(fields[i]);
            }
            if (out.face.size() > 48u) return fail("the face's name is longer than 48 characters");
            face = true;
        } else if (fields[0] == "glyph") {
            std::uint64_t code = 0u;
            Glyph glyph;
            if (fields.size() != 4u || !fields[1].starts_with("U+") || !hex(fields[1].substr(2u), 4u, code) || !code)
                return fail("expected glyph U+XXXX <step> <offset>");
            if (!number(fields[2], glyph.step) || !number(fields[3], glyph.offset)) return fail("a step or an offset is not a decimal number");
            if (!(glyph.step > least_step && glyph.step < most_step)) return fail("the step " + shown(fields[2]) + " is not between 3 and 48");
            if (!(glyph.offset > least_offset && glyph.offset < most_offset)) return fail("the offset " + shown(fields[3]) + " is not between -24 and 8");
            if (!out.glyphs.emplace(static_cast<std::uint16_t>(code), glyph).second) return fail("a second line for " + shown(fields[1]));
        } else return fail("unknown line '" + shown(fields[0]) + "'");
    }
    line_number = 0u;
    if (!versioned) return fail("the first line must be chili-font-advances 1");
    if (!sheet) return fail("no sheet line");
    if (out.glyphs.empty()) return fail("no glyph line");
    return true;
}

// The whole file; false when it cannot be read or is larger than `most`.
inline bool read_file(const std::filesystem::path &file, std::size_t most, std::vector<std::uint8_t> &bytes) {
    std::error_code status;
    if (!std::filesystem::is_regular_file(file, status)) return false;
    const auto size = std::filesystem::file_size(file, status);
    if (status || size > most) return false;
    bytes.resize(static_cast<std::size_t>(size));
    std::ifstream stream(file, std::ios::binary);
    stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return stream.is_open() && stream.gcount() == static_cast<std::streamsize>(bytes.size());
}

// Whether `png` is the picture the file was made for: the XXH64 of its bytes is the sheet line's.
inline bool matches_sheet(const FontAdvances &advances, const std::filesystem::path &png, std::string &problem) {
    std::vector<std::uint8_t> bytes;
    if (!read_file(png, std::size_t{512u} << 20u, bytes)) { problem = "the sheet beside it cannot be read"; return false; }
    const auto hash = xxh64(bytes);
    if (hash == advances.sheet_hash) return true;
    problem = "it was made for another picture (its sheet line says " + hex16(advances.sheet_hash) + ", the PNG beside it is " + hex16(hash) + ")";
    return false;
}

// A glyph table of the block as it is served: `bytes.size()` bytes at `offset`.
struct TableEdit { std::size_t offset{}; std::vector<std::uint8_t> bytes; };
struct Tables {
    std::vector<TableEdit> edits;        // two, by offset: the text font's table and its outline twin's
    unsigned patched{};                  // letters given the file's values, in each table
    unsigned unknown{};                  // glyphs of the file the table has no letter for
    double game_steps{}, face_steps{};   // the pen steps of the patched letters added up, the game's and the file's
    std::size_t read_bytes{};
    std::string problem;                 // why nothing is changed
};

// The two glyph tables of todpsp_game.main with the file's values. Reads the block's head and its info buffer
// (the asset records, where both fonts lie; 4 MB of the game's 6 MB block) through `read`. Empty with
// `problem` set when the block is not laid out as the game's, a table is not found with its font record, or
// the tables are not the game's own: 181 glyphs in both, the same cells, advance = cell width, x offset 0.
inline Tables edit_tables(const menu_entry::Read &read, std::uint64_t whole, const FontAdvances &advances) {
    Tables result;
    const auto fail = [&](std::string why) { result.edits.clear(); result.patched = result.unknown = 0u; result.problem = std::move(why); return result; };
    constexpr std::size_t head_size = 37u, most_info = std::size_t{64u} << 20u;
    if (whole < head_size + 4u || whole > 0x7FFFFFFFu) return fail("not a block");
    const auto size = static_cast<std::size_t>(whole);
    std::array<std::uint8_t, head_size> head{};
    if (read(0u, head) != head.size()) return fail("cannot be read");
    const std::size_t count = props::word(head, 16u), info_size = props::word(head, 20u);
    const std::size_t stream_start = props::word(head, 29u), stream_end = props::word(head, 33u);
    if (stream_end < stream_start || stream_end - stream_start > size - head_size) return fail("bad stream range");
    std::size_t at = head_size + (stream_end - stream_start), total = 0u;
    for (unsigned runs = 0u; total < count; ++runs) {
        std::array<std::uint8_t, 4u> run{};
        if (runs > 4096u || size - at < 4u || read(at, run) != run.size()) return fail("the run-length words run past the file");
        total += props::word(run, 0u) & 0x7FFFFFFFu;
        at += 4u;
    }
    if (total != count || info_size > size - at || info_size > most_info) return fail("the header does not describe this file");
    std::vector<std::uint8_t> info(info_size);
    if (read(at, info) != info.size()) return fail("cannot be read");
    result.read_bytes = at + info.size();

    const std::span<const std::uint8_t> bytes(info);
    constexpr std::size_t table_size = table_glyphs * PageFont::entry_size;
    const auto whole_table = [](const PageFont &font) {
        return font.header_found() && font.glyphs().size() == table_glyphs && font.cell_height() == table_cell_rows;
    };
    PageFont fill, outline;
    if (!fill.scan(bytes) || !whole_table(fill)) return fail("the text font's glyph table was not found");
    const std::size_t behind = fill.table_offset() + table_size;
    if (!outline.scan(bytes.subspan(behind)) || !whole_table(outline)) return fail("the outline font's glyph table was not found");
    const auto own = [](const PageGlyph &glyph) { return glyph.x_offset == 0.0f && std::fabs(glyph.advance - glyph.width()) <= 0.01f; };
    for (std::size_t i = 0; i < table_glyphs; ++i) {
        const auto &a = fill.glyphs()[i], &b = outline.glyphs()[i];
        if (a.code != b.code || a.u != b.u || a.v != b.v || a.u_width != b.u_width || a.picture != b.picture)
            return fail("the two font tables do not hold the same cells");
        if (!own(a) || !own(b)) {
            char code[8];
            std::snprintf(code, sizeof(code), "U+%04X", static_cast<unsigned>(a.code));
            return fail(std::string("the font tables are not the game's own (") + code + " does not step by its cell's width)");
        }
    }
    std::map<std::uint16_t, bool> used;
    for (const auto &[code, glyph] : advances.glyphs) used[code] = false;
    for (const std::size_t table : {fill.table_offset(), behind + outline.table_offset()}) {
        std::vector<std::uint8_t> copy(info.begin() + static_cast<std::ptrdiff_t>(table), info.begin() + static_cast<std::ptrdiff_t>(table + table_size));
        for (std::size_t i = 0; i < table_glyphs; ++i) {
            const auto &glyph = fill.glyphs()[i];
            const auto named = advances.glyphs.find(glyph.code);
            if (glyph.picture || named == advances.glyphs.end()) continue;
            const float advance = named->second.step - box_spacing, x_offset = named->second.offset - box_spacing / 2.0f;
            std::memcpy(copy.data() + i * PageFont::entry_size + 0x0Cu, &advance, 4u);
            std::memcpy(copy.data() + i * PageFont::entry_size + 0x04u, &x_offset, 4u);
            if (table != fill.table_offset()) continue;
            used[glyph.code] = true;
            ++result.patched;
            result.game_steps += static_cast<double>(glyph.advance + box_spacing);
            result.face_steps += static_cast<double>(named->second.step);
        }
        result.edits.push_back({at + table, std::move(copy)});
    }
    for (const auto &[code, taken] : used) if (!taken) ++result.unknown;
    return result;
}

// The block's bytes with the edits (tests).
inline std::vector<std::uint8_t> apply(std::span<const std::uint8_t> block, const std::vector<TableEdit> &edits) {
    std::vector<std::uint8_t> out(block.begin(), block.end());
    for (const auto &edit : edits) std::copy(edit.bytes.begin(), edit.bytes.end(), out.begin() + static_cast<std::ptrdiff_t>(edit.offset));
    return out;
}
}
