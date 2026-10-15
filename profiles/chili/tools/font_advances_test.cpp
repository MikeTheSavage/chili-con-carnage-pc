// The letter spacing of a replaced text font (host/font_advances.hpp).
//
// Without the game: the file's grammar and every way it is refused, the tie to the font sheet by the hash of
// the PNG file, and the two glyph tables of a block made here in the game's layout: which words change, which
// never do, and the blocks that are left alone.
//
// With the game's disc folder as second argument: the tables of the real todpsp_game.main, where they lie and
// what they hold, edited and compared byte for byte.
//
//   chili_font_advances_test <scratch folder> [disc0 folder]
#include "../host/font_advances.hpp"
#include "../host/vfs.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
namespace {
namespace fa = chili::font_advances;
using chili::PageFont;
using chili::PageGlyph;
unsigned checks = 0u;
void require(bool value, const std::string &message) { ++checks; if (!value) throw std::runtime_error(message); }
void put(std::vector<std::uint8_t> &out, std::uint32_t value) { for (unsigned i = 0; i < 4u; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8u * i))); }
void store(std::vector<std::uint8_t> &out, std::size_t at, std::uint32_t value) { for (unsigned i = 0; i < 4u; ++i) out[at + i] = static_cast<std::uint8_t>(value >> (8u * i)); }
float number(std::span<const std::uint8_t> data, std::size_t at) { float out; std::memcpy(&out, data.data() + at, 4u); return out; }
std::vector<std::uint8_t> pattern(std::size_t size, unsigned seed) {
    std::vector<std::uint8_t> out(size);
    for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<std::uint8_t>((i * 131u + seed * 17u + (i >> 8u)) & 0xFFu);
    return out;
}
chili::menu_entry::Read reader(const std::vector<std::uint8_t> &block) {
    return [&block](std::uint64_t offset, std::span<std::uint8_t> out) -> std::size_t {
        if (offset >= block.size()) return 0u;
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), block.size() - offset));
        std::memcpy(out.data(), block.data() + offset, count);
        return count;
    };
}

// ---- The file ---------------------------------------------------------------------------------------------
const std::string head = "chili-font-advances 1\nsheet 5d0c6d1a663a8515 0123456789abcdef\n";
void file_grammar() {
    fa::FontAdvances out;
    std::string problem;
    const std::string good = "# made by a test\n\n  chili-font-advances 1   # the version\nsheet 5d0c6d1a663a8515 0123456789ABCDEF\t# the picture\n"
                             "face Saira  Condensed\nglyph U+0066 11.42 -4.79\r\nglyph\tU+30\t16.38\t-2.31\n\n# end";
    require(fa::parse(good, out, problem) && problem.empty(), "a good file was refused: " + problem);
    require(out.face == "Saira Condensed" && out.sheet_key == chili::text_font_sheet_key && out.sheet_hash == 0x0123456789abcdefull && out.glyphs.size() == 2u,
        "the lines of a good file");
    require(out.glyphs.at(0x66).step == 11.42f && out.glyphs.at(0x66).offset == -4.79f && out.glyphs.at(0x30).step == 16.38f && out.glyphs.at(0x30).offset == -2.31f,
        "the numbers of a good file");
    require(fa::parse("\xEF\xBB\xBF" + good, out, problem) && out.glyphs.size() == 2u, "a byte order mark in front");
    require(fa::parse(head + "glyph U+0041 3.01 7.99\nglyph U+0042 47.99 -23.99\n", out, problem) && out.face.empty() && out.glyphs.size() == 2u, "the ends of the ranges, no face line");
    require(std::string(chili::font_advances_file_name) == fa::hex16(chili::text_font_sheet_key) + ".advances.txt", "the file's name is the sheet's key");

    struct Bad { std::string text, why; };
    const std::vector<Bad> refused{
        {"", "the first line must be chili-font-advances 1"},
        {"# nothing but a comment\n\n", "the first line must be chili-font-advances 1"},
        {"glyph U+0066 11.42 -4.79\n" + head, "line 1: the first line must be chili-font-advances 1"},
        {"chili-font-advances\nsheet 5d0c6d1a663a8515 0123456789abcdef\n", "line 1: the first line must be"},
        {"chili-font-advances 2\nsheet 5d0c6d1a663a8515 0123456789abcdef\nglyph U+0066 11.42 -4.79\n", "line 1: version 2 is not known to this build"},
        {"chili-font-advances 1\nglyph U+0066 11.42 -4.79\n", "no sheet line"},
        {head + "sheet 5d0c6d1a663a8515 0123456789abcdef\nglyph U+0066 11.42 -4.79\n", "line 3: a second sheet line"},
        {"chili-font-advances 1\nsheet 002d10b31e312e9d 0123456789abcdef\nglyph U+0066 11.42 -4.79\n", "line 2: the sheet 002d10b31e312e9d is not the text font's"},
        {"chili-font-advances 1\nsheet 5d0c6d1a663a8515 0123456789abcde\nglyph U+0066 11.42 -4.79\n", "line 2: expected sheet <key> <hash>"},
        {"chili-font-advances 1\nsheet 5d0c6d1a663a8515 0123456789abcdeg\nglyph U+0066 11.42 -4.79\n", "line 2: expected sheet <key> <hash>"},
        {"chili-font-advances 1\nsheet 5d0c6d1a663a8515\nglyph U+0066 11.42 -4.79\n", "line 2: expected sheet <key> <hash>"},
        {head, "no glyph line"},
        {head + "face\n", "line 3: expected face <name>"},
        {head + "face Saira\nface Tektur\nglyph U+0066 11.42 -4.79\n", "line 4: a second face line"},
        {head + "face Sa\xC3\xAFra\nglyph U+0066 11.42 -4.79\n", "line 3: the face's name is not plain ASCII"},
        {head + "face " + std::string(49u, 'x') + "\n", "line 3: the face's name is longer than 48 characters"},
        {head + "glyph U+0066 abc -4.79\n", "line 3: a step or an offset is not a decimal number"},
        {head + "glyph U+0066 11.42 -4.79x\n", "line 3: a step or an offset is not a decimal number"},
        {head + "glyph U+0066 1e1 -4.79\n", "line 3: a step or an offset is not a decimal number"},
        {head + "glyph U+0066 nan -4.79\n", "line 3: a step or an offset is not a decimal number"},
        {head + "glyph U+0066 11.42 inf\n", "line 3: a step or an offset is not a decimal number"},
        {head + "glyph U+0066 3 -4.79\n", "line 3: the step 3 is not between 3 and 48"},
        {head + "glyph U+0066 48 -4.79\n", "line 3: the step 48 is not between 3 and 48"},
        {head + "glyph U+0066 -11.42 -4.79\n", "line 3: the step -11.42 is not between 3 and 48"},
        {head + "glyph U+0066 11.42 -24\n", "line 3: the offset -24 is not between -24 and 8"},
        {head + "glyph U+0066 11.42 8\n", "line 3: the offset 8 is not between -24 and 8"},
        {head + "glyph 0066 11.42 -4.79\n", "line 3: expected glyph U+XXXX <step> <offset>"},
        {head + "glyph U+12345 11.42 -4.79\n", "line 3: expected glyph U+XXXX <step> <offset>"},
        {head + "glyph U+0000 11.42 -4.79\n", "line 3: expected glyph U+XXXX <step> <offset>"},
        {head + "glyph U+00G6 11.42 -4.79\n", "line 3: expected glyph U+XXXX <step> <offset>"},
        {head + "glyph U+0066 11.42\n", "line 3: expected glyph U+XXXX <step> <offset>"},
        {head + "glyph U+0066 11.42 -4.79 1\n", "line 3: expected glyph U+XXXX <step> <offset>"},
        {head + "glyph U+0066 11.42 -4.79\n\nglyph U+0066 12 -4\n", "line 5: a second line for U+0066"},
        {head + "glyph U+0066 11.42 -4.79\nkern U+0041 U+0056 -1.5\n", "line 4: unknown line 'kern'"},
        {head + "glyph U+0066 11.42 -4.79\nchili-font-advances 1\n", "line 4: unknown line 'chili-font-advances'"},
    };
    for (const auto &bad : refused) {
        problem.clear();
        const bool taken = fa::parse(bad.text, out, problem);
        require(!taken && problem.find(bad.why) == 0u && out.glyphs.empty() && out.face.empty() && !out.sheet_hash,
            "expected '" + bad.why + "', got '" + (taken ? std::string("accepted") : problem) + "'");
    }
    // A word of the file reaches a message cut short and without control characters.
    require(!fa::parse(head + std::string(60u, '\x01') + " 1\n", out, problem) && problem == "line 3: unknown line '" + std::string(24u, '?') + "'", "a message with a strange word");
    std::cout << "PASS: the file: a good one read, " << refused.size() + 1u << " kinds of bad file refused with the line and the reason\n";
}

// ---- The tie to the sheet -----------------------------------------------------------------------------------
void sheet_hash(const std::filesystem::path &root) {
    // The algorithm's own answers, so the hash is the one tools\extract_textures.py computes.
    const std::string abc = "abc";
    require(chili::xxh64({}) == 0xEF46DB3751D8E999ull &&
        chili::xxh64(std::span(reinterpret_cast<const std::uint8_t *>(abc.data()), abc.size())) == 0x44BC2CF5AD770999ull, "XXH64 of nothing and of abc");
    std::filesystem::create_directories(root);
    const auto png = root / "5d0c6d1a663a8515.png";
    auto bytes = pattern(70001u, 3u);
    const auto write = [&] { std::ofstream stream(png, std::ios::binary); stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())); };
    write();
    fa::FontAdvances advances;
    advances.sheet_hash = chili::xxh64(bytes);
    std::string problem;
    require(fa::matches_sheet(advances, png, problem) && problem.empty(), "the picture the file was made for");
    bytes[40000] ^= 1u;
    write();
    require(!fa::matches_sheet(advances, png, problem) && problem.find("made for another picture") != std::string::npos &&
        problem.find(fa::hex16(advances.sheet_hash)) != std::string::npos && problem.find(fa::hex16(chili::xxh64(bytes))) != std::string::npos,
        "another picture under the same name was accepted");
    require(!fa::matches_sheet(advances, root / "missing.png", problem) && problem == "the sheet beside it cannot be read", "a missing picture");
    std::vector<std::uint8_t> read;
    require(fa::read_file(png, 70001u, read) && read == bytes && !fa::read_file(png, 70000u, read) && !fa::read_file(root, 70000u, read), "read_file and its limit");
    std::cout << "PASS: the tie to the sheet: the hash of the PNG file, another picture and a missing one refused\n";
}

// ---- A block in the game's layout with two font tables ------------------------------------------------------
struct Made {
    std::vector<std::uint8_t> block;
    std::size_t info{}, fill{}, outline{};   // file offsets: the info buffer, the two tables
    std::vector<PageGlyph> glyphs;
};
// 181 glyphs as the game's table has them: the ten digits first, letters, and `pictures` cells that keep their
// colours; advance = cell width, x offset 0, y offset 9.
std::vector<PageGlyph> game_like_glyphs(unsigned pictures = 30u) {
    std::vector<PageGlyph> out;
    float x = 0.0f, row = 0.0f;
    for (std::size_t i = 0; i < fa::table_glyphs; ++i) {
        PageGlyph glyph;
        const bool picture = i >= fa::table_glyphs - pictures;
        glyph.code = static_cast<std::uint16_t>(i < 10u ? u'0' + i : picture ? 0x2190u + i : i < 36u ? u'A' + (i - 10u) : i < 62u ? u'a' + (i - 36u) : 0x00C0u + (i - 62u));
        const float width = static_cast<float>(10u + (i * 7u) % 23u);
        if (x + width > 256.0f) { x = 0.0f; row += 30.0f; }
        glyph.y_offset = 9.0f; glyph.advance = width;
        glyph.u = x / 256.0f; glyph.v = row / 512.0f; glyph.u_width = width / 256.0f;
        glyph.picture = picture;
        x += width;
        out.push_back(glyph);
    }
    return out;
}
// A font record in front of its table: the cell height 4 bytes in, the table's place relative to the word
// that holds it 0x40 in, the glyph count behind that.
void add_font(std::vector<std::uint8_t> &info, const std::vector<PageGlyph> &glyphs, std::uint32_t named, std::uint32_t rows, std::size_t gap, std::size_t &table) {
    const auto record = info.size();
    info.resize(record + 0x60u + gap);
    table = info.size();
    store(info, record + 4u, rows);
    store(info, record + 0x40u, static_cast<std::uint32_t>(table - (record + 0x40u)));
    store(info, record + 0x44u, named);
    info.resize(table + glyphs.size() * PageFont::entry_size);
    for (std::size_t i = 0; i < glyphs.size(); ++i) PageFont::write(glyphs[i], info.data() + table + i * PageFont::entry_size);
    // Every entry has 116 in the word after the code point; nobody knows what it is, and it must stay.
    for (std::size_t i = 0; i < glyphs.size(); ++i) info[table + i * PageFont::entry_size + 2u] = 116u;
}
struct Shape { std::uint32_t named{181u}, rows{30u}; bool second{true}; std::size_t stream{}; };
Made make_block(const std::vector<PageGlyph> &fill_glyphs, const std::vector<PageGlyph> &outline_glyphs, const Shape &shape = {}) {
    Made made;
    made.glyphs = fill_glyphs;
    std::vector<std::uint8_t> info(256u);   // an asset record's room
    std::size_t fill = 0u, outline = 0u;
    add_font(info, fill_glyphs, shape.named, shape.rows, 0x1F0u, fill);
    info.resize(info.size() + 0x70u);
    if (shape.second) add_font(info, outline_glyphs, shape.named, shape.rows, 0x230u, outline);
    info.resize(info.size() + 0x400u);
    auto &out = made.block;
    put(out, 0x0DA24EFAu); put(out, 0x5E6233D4u); put(out, 0x27403369u); put(out, 0u);
    put(out, 3u); put(out, static_cast<std::uint32_t>(info.size())); put(out, 5000u);
    out.push_back(1u); put(out, 4096u); put(out, 4096u + static_cast<std::uint32_t>(shape.stream));
    const auto stream = pattern(shape.stream, 9u);
    out.insert(out.end(), stream.begin(), stream.end());
    put(out, 0x80000001u); put(out, 2u);   // two run-length words that add up to the three assets
    made.info = out.size();
    out.insert(out.end(), info.begin(), info.end());
    for (const std::uint32_t size : {5000u, 100u, 0u}) put(out, size);
    const auto payload = pattern(5100u, 21u);
    out.insert(out.end(), payload.begin(), payload.end());
    made.fill = made.info + fill;
    made.outline = shape.second ? made.info + outline : 0u;
    return made;
}

fa::FontAdvances some_advances() {
    fa::FontAdvances advances;
    advances.face = "Test Face";
    advances.sheet_key = chili::text_font_sheet_key;
    advances.glyphs = {{u'0', {16.38f, -2.31f}}, {u'f', {11.42f, -4.79f}}, {u'r', {11.5f, -4.25f}}, {u'O', {17.05f, 0.5f}}, {0x00C9, {13.0f, -1.0f}}};
    return advances;
}

void made_up_block() {
    const auto glyphs = game_like_glyphs();
    constexpr std::size_t table_size = fa::table_glyphs * PageFont::entry_size;
    for (const std::size_t stream : {std::size_t{0}, std::size_t{4444}}) {
        const auto made = make_block(glyphs, glyphs, {.stream = stream});
        auto advances = some_advances();
        advances.glyphs[static_cast<std::uint16_t>(0x2190u + 180u)] = {12.0f, -1.0f};   // a picture of the table: never changed
        advances.glyphs[0x0416u] = {14.0f, -2.0f};          // a letter the table does not have
        const auto tables = fa::edit_tables(reader(made.block), made.block.size(), advances);
        require(tables.problem.empty(), "the made-up block was refused: " + tables.problem);
        require(tables.edits.size() == 2u && tables.edits[0].offset == made.fill && tables.edits[1].offset == made.outline &&
            tables.edits[0].bytes.size() == table_size && tables.edits[1].bytes.size() == table_size, "the two edits are the two tables");
        require(tables.patched == 5u && tables.unknown == 2u && tables.read_bytes == made.info + (made.block.size() - made.info - 12u - 5100u), "what was counted");
        // The game's steps of the five letters (cell width less 5) and the file's.
        double game = 0.0, face = 0.0;
        for (const auto &glyph : glyphs) if (const auto named = advances.glyphs.find(glyph.code); !glyph.picture && named != advances.glyphs.end()) {
            game += static_cast<double>(glyph.advance) - 5.0; face += static_cast<double>(named->second.step);
        }
        require(std::fabs(tables.game_steps - game) < 1e-6 && std::fabs(tables.face_steps - face) < 1e-6 && game > 0.0, "the pen steps added up");
        const auto after = fa::apply(made.block, tables.edits);
        require(after.size() == made.block.size(), "the block changed its size");
        std::size_t differing = 0u;
        for (std::size_t at = 0; at < after.size(); ++at) {
            if (after[at] == made.block[at]) continue;
            ++differing;
            const bool in_fill = at >= made.fill && at < made.fill + table_size, in_outline = at >= made.outline && at < made.outline + table_size;
            require(in_fill || in_outline, "a byte outside the two tables changed");
            const std::size_t inside = (at - (in_fill ? made.fill : made.outline)) % PageFont::entry_size;
            require((inside >= 4u && inside < 8u) || (inside >= 12u && inside < 16u), "a word other than the x offset and the advance changed");
        }
        require(differing > 0u, "nothing changed");
        for (const std::size_t table : {made.fill, made.outline})
            for (std::size_t i = 0; i < fa::table_glyphs; ++i) {
                const auto *before = made.block.data() + table + i * PageFont::entry_size, *now = after.data() + table + i * PageFont::entry_size;
                const auto was = PageFont::read(before), is = PageFont::read(now);
                const auto named = advances.glyphs.find(was.code);
                require(is.code == was.code && now[2] == 116u && now[3] == 0u && is.y_offset == 9.0f && is.u == was.u && is.v == was.v && is.u_width == was.u_width && is.picture == was.picture,
                    "the code point, the cell, the y offset or the picture flag changed");
                if (was.picture || named == advances.glyphs.end()) require(std::memcmp(before, now, PageFont::entry_size) == 0, "a picture or an unnamed letter changed");
                else require(is.advance == named->second.step + 5.0f && is.x_offset == named->second.offset + 2.5f, "a named letter does not carry step + 5 and offset + 2.5");
            }
        // What the game and the settings page then read: the same table, found the same way, with the new numbers.
        PageFont found;
        require(found.scan(std::span<const std::uint8_t>(after).subspan(made.info)) && found.header_found() && found.glyphs().size() == fa::table_glyphs &&
            found.table_offset() == made.fill - made.info && found.find(u'f')->advance == 11.42f + 5.0f && found.find(u'f')->x_offset == -4.79f + 2.5f &&
            found.find(u'g')->advance == found.find(u'g')->width() && found.find(u'g')->x_offset == 0.0f, "the served table as a reader of the game's memory finds it");
        // A box with the game's spacing steps by the file's step, to the last bit a float can hold.
        require(std::fabs((found.find(u'f')->advance + fa::box_spacing) - 11.42f) < 1e-5f, "advance + spacing is not the step");

        // Over a block another edit has changed elsewhere (a property line's value): the same two edits, and the other change stays.
        auto tuned = made.block;
        tuned[made.block.size() - 300u] ^= 0x5Au;
        const auto again = fa::edit_tables(reader(tuned), tuned.size(), advances);
        require(again.problem.empty() && again.edits.size() == 2u && again.edits[0].bytes == tables.edits[0].bytes && again.edits[1].bytes == tables.edits[1].bytes &&
            fa::apply(tuned, again.edits)[made.block.size() - 300u] == tuned[made.block.size() - 300u], "the edit over a block changed elsewhere");
        // A file with the game's own numbers (step = cell width - 5, the cell 2.5 before the letter's advance) is the game's table.
        fa::FontAdvances same;
        for (const auto &glyph : glyphs) if (!glyph.picture) same.glyphs[glyph.code] = {glyph.width() - 5.0f, -2.5f};
        const auto unchanged = fa::edit_tables(reader(made.block), made.block.size(), same);
        require(unchanged.problem.empty() && unchanged.patched == 151u && fa::apply(made.block, unchanged.edits) == made.block &&
            std::fabs(unchanged.game_steps - unchanged.face_steps) < 1e-9, "the game's own numbers do not give the game's own table");
        // Twice is refused: the tables are then no longer the game's own.
        const auto twice = fa::edit_tables(reader(after), after.size(), advances);
        require(twice.edits.empty() && twice.problem.find("not the game's own") != std::string::npos, "an edited block was edited again");
    }

    // Blocks that are left alone, each with its reason.
    const auto advances = some_advances();
    const auto refused = [&](const Made &made, const std::string &why, const std::string &what) {
        const auto tables = fa::edit_tables(reader(made.block), made.block.size(), advances);
        require(tables.edits.empty() && !tables.patched && tables.problem.find(why) != std::string::npos, what + ": expected '" + why + "', got '" + tables.problem + "'");
    };
    auto changed = glyphs;
    changed[40].advance += 1.0f;
    refused(make_block(changed, glyphs), "the font tables are not the game's own (U+0065 does not step by its cell's width)", "an advance that is not the cell's width");
    refused(make_block(glyphs, changed), "the font tables are not the game's own (U+0065", "the same in the outline font");
    changed = glyphs;
    changed[12].x_offset = -1.0f;
    refused(make_block(changed, glyphs), "the font tables are not the game's own (U+0043", "an x offset");
    changed = glyphs;
    changed[100].u_width += 1.0f / 256.0f; changed[100].advance += 1.0f;
    refused(make_block(glyphs, changed), "the two font tables do not hold the same cells", "an outline font with another cell");
    changed = glyphs;
    changed[70].code = 0x0152u;
    refused(make_block(glyphs, changed), "the two font tables do not hold the same cells", "an outline font with another letter");
    refused(make_block(glyphs, glyphs, {.named = 180u}), "the text font's glyph table was not found", "a font record that names 180 glyphs");
    refused(make_block(glyphs, glyphs, {.rows = 32u}), "the text font's glyph table was not found", "cells 32 rows high");
    refused(make_block(glyphs, glyphs, {.second = false}), "the outline font's glyph table was not found", "a block with one font");
    refused(make_block(glyphs, glyphs, {.named = 0u}), "the text font's glyph table was not found", "a table without its font record");
    {
        // Not a block, a cut block, a head that lies, a reader that fails.
        const auto made = make_block(glyphs, glyphs);
        const auto tiny = pattern(40u, 1u);
        require(fa::edit_tables(reader(tiny), tiny.size(), advances).problem == "not a block", "40 bytes");
        auto cut = made.block;
        cut.resize(made.fill + 100u);
        require(fa::edit_tables(reader(cut), cut.size(), advances).problem == "the header does not describe this file", "a cut block");
        auto lying = made.block;
        store(lying, 16u, 9u);
        require(!fa::edit_tables(reader(lying), lying.size(), advances).problem.empty(), "an asset count that the run-length words do not reach");
        store(lying, 16u, 3u); store(lying, 33u, 0x7FFFFFFFu);
        require(fa::edit_tables(reader(lying), lying.size(), advances).problem == "bad stream range", "a stream longer than the file");
        const auto failing = [&](std::uint64_t offset, std::span<std::uint8_t> out) -> std::size_t { return offset >= made.info ? 0u : reader(made.block)(offset, out); };
        require(fa::edit_tables(failing, made.block.size(), advances).problem == "cannot be read", "a reader that gives nothing of the info buffer");
        const auto noise = pattern(200000u, 77u);
        require(!fa::edit_tables(reader(noise), noise.size(), advances).problem.empty(), "noise");
    }
    std::cout << "PASS: a block in the game's layout: the two tables served with step + 5 and the offset for the named letters, every other byte as it was; "
                 "15 kinds of block left alone\n";
}

// ---- The game's own block -----------------------------------------------------------------------------------
void actual_block(const std::filesystem::path &disc) {
    constexpr char name[] = "data_psp/maps/todpsp_the_game/todpsp_game/shared/todpsp_game.main";
    constexpr std::size_t table_size = fa::table_glyphs * PageFont::entry_size;
    chili::vfs::HostFileSource archive(disc / "PSP_GAME" / "USRDIR" / "assets00.naz");
    const auto directory = chili::naz::read_directory(archive);
    std::vector<std::uint8_t> block;
    for (const auto &entry : directory.entries) if (entry.name == name) { block.resize(entry.size); archive.read(entry.data_offset(), block); }
    require(block.size() == 6334181u, "the archive's todpsp_game.main is not the 6,334,181 bytes this test knows");
    constexpr std::size_t info = 0x29u, info_size = 0x3D0980u, fill = 0x1643E9u, outline = 0x175BE9u;
    require(chili::props::word(block, 20u) == info_size, "the info buffer's size");

    // What the game's tables hold.
    PageFont game;
    require(game.scan(std::span<const std::uint8_t>(block).subspan(info, info_size)) && game.header_found() && game.table_offset() == fill - info &&
        game.glyphs().size() == fa::table_glyphs && game.cell_height() == 30.0f, "the text font's table is not at info offset 0x1643C0 with 181 glyphs");
    unsigned pictures = 0u;
    for (const auto &glyph : game.glyphs()) {
        require(glyph.x_offset == 0.0f && glyph.y_offset == 9.0f && glyph.advance == glyph.width(), "a glyph of the game that does not step by its cell's width");
        pictures += glyph.picture ? 1u : 0u;
    }
    require(pictures == 30u && game.find(u' ') && game.find(u' ')->advance == 14.0f && game.find(u'f') && game.find(0x00A5) && game.find(0x00A5)->picture, "30 pictures, the space 14 wide");
    require(std::memcmp(block.data() + fill, block.data() + outline, table_size) == 0, "the outline font's table is not the text font's, byte for byte");

    // Every letter but the space named, as a typeface's file names them, plus one the game does not have.
    fa::FontAdvances advances;
    advances.sheet_key = chili::text_font_sheet_key;
    for (const auto &glyph : game.glyphs())
        if (!glyph.picture && glyph.code != u' ') advances.glyphs[glyph.code] = {std::clamp(glyph.width() - 5.0f - 0.25f * static_cast<float>(glyph.code % 7u), 3.5f, 47.0f), -0.5f - 0.125f * static_cast<float>(glyph.code % 5u)};
    const auto letters = static_cast<unsigned>(advances.glyphs.size());
    advances.glyphs[0x0416u] = {14.0f, -2.0f};
    const auto started = std::chrono::steady_clock::now();
    const auto tables = fa::edit_tables(reader(block), block.size(), advances);
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    require(tables.problem.empty(), "the game's block was refused: " + tables.problem);
    require(tables.edits.size() == 2u && tables.edits[0].offset == fill && tables.edits[1].offset == outline && tables.edits[0].bytes == tables.edits[1].bytes &&
        tables.edits[0].bytes.size() == table_size, "the edits are not the two tables at 0x1643E9 and 0x175BE9");
    require(letters == 150u && tables.patched == letters && tables.unknown == 1u && tables.read_bytes == info + info_size, "150 letters besides the space, one unknown");
    const auto after = fa::apply(block, tables.edits);
    std::size_t outside = 0u, words = 0u;
    for (std::size_t at = 0; at < block.size(); ++at) {
        if (after[at] == block[at]) continue;
        const bool in_fill = at >= fill && at < fill + table_size, in_outline = at >= outline && at < outline + table_size;
        if (!in_fill && !in_outline) { ++outside; continue; }
        const std::size_t inside = (at - (in_fill ? fill : outline)) % PageFont::entry_size;
        require((inside >= 4u && inside < 8u) || (inside >= 12u && inside < 16u), "a word other than the x offset and the advance changed");
        ++words;
    }
    require(!outside && words > 0u, "bytes outside the two tables changed");
    for (std::size_t i = 0; i < fa::table_glyphs; ++i) {
        const auto *before = block.data() + fill + i * PageFont::entry_size, *now = after.data() + fill + i * PageFont::entry_size;
        const auto was = PageFont::read(before), is = PageFont::read(now);
        const auto named = advances.glyphs.find(was.code);
        if (was.picture || named == advances.glyphs.end()) require(std::memcmp(before, now, PageFont::entry_size) == 0, "a picture or the space changed");
        else require(is.advance == named->second.step + 5.0f && is.x_offset == named->second.offset + 2.5f && is.u == was.u && is.v == was.v && is.u_width == was.u_width &&
            is.y_offset == was.y_offset && std::memcmp(before, now, 4u) == 0 && std::memcmp(before + 0x1Cu, now + 0x1Cu, 4u) == 0, "a letter of the game's table");
    }
    require(number(after, fill + 4u) == advances.glyphs.at(u'0').offset + 2.5f && number(after, outline + 12u) == advances.glyphs.at(u'0').step + 5.0f, "the digit 0, first in both tables");
    // The game's own numbers for every letter give the game's own block, byte for byte.
    fa::FontAdvances same;
    for (const auto &glyph : game.glyphs()) if (!glyph.picture) same.glyphs[glyph.code] = {glyph.width() - 5.0f, -2.5f};
    const auto unchanged = fa::edit_tables(reader(block), block.size(), same);
    require(unchanged.problem.empty() && unchanged.patched == 151u && fa::apply(block, unchanged.edits) == block, "the game's own numbers do not give the game's own block");
    const auto twice = fa::edit_tables(reader(after), after.size(), advances);
    require(twice.edits.empty() && twice.problem.find("not the game's own") != std::string::npos, "the edited block was edited again");
    char line[240];
    std::snprintf(line, sizeof(line), "PASS: the game's todpsp_game.main: tables at file offsets 0x%zX and 0x%zX, 181 glyphs (30 pictures), %u letters served with other "
        "advances, %zu bytes differ, all inside the two tables; %.1f MB read in %.0f ms", fill, outline, tables.patched, words + outside, static_cast<double>(tables.read_bytes) / 1048576.0, ms);
    std::cout << line << '\n';
}
}
int main(int argc, char **argv) {
    try {
        if (argc < 2 || argc > 3) throw std::runtime_error("Usage: chili_font_advances_test <scratch folder> [actual disc0]");
        const auto root = std::filesystem::absolute(argv[1]) / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        file_grammar();
        sheet_hash(root);
        made_up_block();
        if (argc == 3) actual_block(argv[2]);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cout << "PASS: font advances (" << checks << " checks)\n";
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
