// Where the game's glow is in a frame's draws (host/gpu/ge_glow_blocks.hpp), without a graphics card:
//
//   made-up frames: a mission frame's order of draws (3D scene, the copy onto the composite, the tints, the 27
//       draws of the glow, the sprites that add it to the picture, the HUD) gives one block, from the sprite
//       that shrinks the composite to the last sprite into the sheet; the copy to the screen, a menu and a
//       movie give none
//   the rule's edges: a run without a draw that reads its own surface, a run of two draws, a copy that does
//       not shrink or shrinks along one axis only, rectangles that are not known, a run into the displayed
//       surface, a 3D draw or a clear inside the run
//   captured frames, when the folder with them is given (profiles/chili/captures/ge, kept out of the
//       repository): the draws of two mission frames and of two copies to the screen, read from the per-draw
//       trace, their sprites' rectangles from the captures' memory
//
//   chili_glow_blocks_test [<folder with mission_9450_9453_trace.jsonl and ge_00945*.gecap>]
#include "gpu/ge_glow_blocks.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using vcs::find_glow_blocks;
using vcs::GeGlowBlock;
using vcs::GeGlowDraw;
unsigned checks, failures;
const bool keep_going = [] { const char *value = std::getenv("CHILI_TEST_KEEP_GOING"); return value && *value == '1'; }();
void require(bool ok, const std::string &what) {
    ++checks;
    if (ok) return;
    if (!keep_going) throw std::runtime_error(what);
    ++failures;
    std::cerr << "FAILED CHECK: " << what << '\n';
}

// The game's surfaces (VRAM offsets): the screen, the glow's sheet, the composite, the scene.
constexpr std::uint32_t kScreen = 0x000000u, kSheet = 0x088000u, kComposite = 0x0D8000u, kScene = 0x0E8000u;

// A 3D draw, or any other draw that is not a sprite of the effect.
GeGlowDraw other(std::uint32_t target) { GeGlowDraw d; d.target = target; return d; }
// A sprite without a surface for a texture (a fill, a tint, a HUD picture).
GeGlowDraw plain(std::uint32_t target) { GeGlowDraw d; d.target = target; d.sprite = true; return d; }
// A sprite that copies `texels` of `source` (left, top, right, bottom) onto `pixels` of `target`.
GeGlowDraw copy(std::uint32_t target, std::uint32_t source, std::array<float, 4> texels, std::array<float, 4> pixels) {
    GeGlowDraw d;
    d.target = target; d.sprite = true; d.samples_surface = true; d.source = source;
    d.rectangles_known = true; d.texels = texels; d.pixels = pixels;
    return d;
}
// A sprite that reads the surface it draws into.
GeGlowDraw self(std::uint32_t target) { return copy(target, target, {0.0f, 0.0f, 120.0f, 68.0f}, {240.0f, 10.0f, 360.0f, 78.0f}); }

// The glow's sprites as the renderer gets them, a draw per sprite where a surface is sampled: eight strips
// shrink the composite to half, four the sheet to a quarter, a white fill, the threshold, the doubling, a
// black fill, 21 additions.
void append_glow(std::vector<GeGlowDraw> &draws, std::uint32_t sheet = kSheet, std::uint32_t composite = kComposite) {
    for (int i = 0; i < 8; ++i) {
        const float x = static_cast<float>(i);
        draws.push_back(copy(sheet, composite, {x * 64.0f, 0.0f, x * 64.0f + 64.0f, 272.0f}, {x * 32.0f, 0.0f, x * 32.0f + 32.0f, 136.0f}));
    }
    for (int i = 0; i < 4; ++i) {
        const float x = static_cast<float>(i);
        draws.push_back(copy(sheet, sheet, {x * 64.0f, 0.0f, x * 64.0f + 64.0f, 136.0f}, {240.0f + x * 32.0f, 0.0f, 272.0f + x * 32.0f, 68.0f}));
    }
    draws.push_back(plain(sheet));
    draws.push_back(self(sheet));
    draws.push_back(self(sheet));
    draws.push_back(plain(sheet));
    for (int i = 0; i < 21; ++i) draws.push_back(self(sheet));
}
// The eight sprites that add the sheet to the composite, four times enlarged.
void append_readers(std::vector<GeGlowDraw> &draws) {
    for (int i = 0; i < 8; ++i) {
        const float x = static_cast<float>(i);
        draws.push_back(copy(kComposite, kSheet, {240.0f + x * 16.0f, 10.0f, 256.0f + x * 16.0f, 78.0f}, {x * 64.0f, 0.0f, x * 64.0f + 64.0f, 272.0f}));
    }
}
// A mission frame up to the glow: the scene, its copy onto the composite in 135 sprites of 34x37 texels on
// 32x32 pixels, two tints.
std::vector<GeGlowDraw> scene_and_composite() {
    std::vector<GeGlowDraw> draws;
    for (int i = 0; i < 480; ++i) draws.push_back(other(kScene));
    for (int i = 0; i < 135; ++i) {
        const float x = static_cast<float>(i % 15), y = static_cast<float>(i / 15);
        draws.push_back(copy(kComposite, kScene, {x * 34.0f, y * 37.0f, x * 34.0f + 34.0f, y * 37.0f + 37.0f}, {x * 32.0f, y * 32.0f, x * 32.0f + 32.0f, y * 32.0f + 32.0f}));
    }
    draws.push_back(plain(kComposite));
    draws.push_back(plain(kComposite));
    return draws;
}

void made_up_frames() {
    // A mission frame.
    auto frame = scene_and_composite();
    const std::size_t first = frame.size();
    append_glow(frame);
    const std::size_t last = frame.size() - 1u;
    append_readers(frame);
    for (int i = 0; i < 40; ++i) frame.push_back(plain(kComposite));
    auto blocks = find_glow_blocks(frame, kScreen);
    require(blocks.size() == 1u && blocks[0].first == first && blocks[0].last == last && blocks[0].target == kSheet,
            "a mission frame has one block: from the first sprite that shrinks the composite to the last sprite into the sheet");
    require(last - first + 1u == 37u, "the block's 27 draws are 37 for the renderer");
    // The same with the scene shown early: the composite is the displayed surface then.
    blocks = find_glow_blocks(frame, kComposite);
    require(blocks.size() == 1u && blocks[0].first == first && blocks[0].target == kSheet, "the block is found when the composite is the displayed surface");

    // Two frames in one submission.
    auto two = frame;
    const std::size_t second = two.size() + first;
    two.insert(two.end(), frame.begin(), frame.end());
    blocks = find_glow_blocks(two, kScreen);
    require(blocks.size() == 2u && blocks[0].first == first && blocks[1].first == second && blocks[1].last == second + 36u, "two frames, two blocks");

    // A submission that ends inside the block: what is there is still a block from its fourteenth sprite on.
    std::vector<GeGlowDraw> cut(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(first + 14u));
    blocks = find_glow_blocks(cut, kScreen);
    require(blocks.size() == 1u && blocks[0].first == first && blocks[0].last == first + 13u, "a frame cut short inside the block");
    cut.resize(first + 8u);
    require(find_glow_blocks(cut, kScreen).empty(), "cut before the first sprite that reads the sheet: nothing yet");

    // The copy to the screen, a menu, a movie: no sheet, no block.
    std::vector<GeGlowDraw> screen{copy(kScreen, kComposite, {0.0f, 0.0f, 480.0f, 272.0f}, {0.0f, 0.0f, 480.0f, 272.0f})};
    require(find_glow_blocks(screen, kScreen).empty(), "the copy to the screen is no block");
    std::vector<GeGlowDraw> menu;
    for (int i = 0; i < 60; ++i) menu.push_back(plain(kScreen));
    require(find_glow_blocks(menu, kScreen).empty(), "a menu's sprites are no block");
    require(find_glow_blocks({}, kScreen).empty(), "no draws, no block");
    // A mission frame without the glow's sprites.
    auto bare = scene_and_composite();
    for (int i = 0; i < 40; ++i) bare.push_back(plain(kComposite));
    require(find_glow_blocks(bare, kScreen).empty(), "the scene's copy onto the composite (34 texels on 32 pixels) is no block");
}

void rule_edges() {
    const auto shrink = [](std::uint32_t target, float texels_x, float texels_y) {
        return copy(target, kComposite, {0.0f, 0.0f, texels_x, texels_y}, {0.0f, 0.0f, 100.0f, 100.0f});
    };
    // The smallest block: a shrinking copy and two more sprites, one of which reads the sheet.
    std::vector<GeGlowDraw> draws{shrink(kSheet, 200.0f, 200.0f), plain(kSheet), self(kSheet)};
    auto blocks = find_glow_blocks(draws, kScreen);
    require(blocks.size() == 1u && blocks[0].first == 0u && blocks[0].last == 2u, "three sprites, one reads the sheet: a block");
    draws = {shrink(kSheet, 200.0f, 200.0f), self(kSheet)};
    require(find_glow_blocks(draws, kScreen).empty(), "two sprites are no block");
    draws = {shrink(kSheet, 200.0f, 200.0f), plain(kSheet), plain(kSheet), plain(kSheet)};
    require(find_glow_blocks(draws, kScreen).empty(), "a run in which nothing reads the sheet is no block");
    // A second copy of another surface inside the run does not read the sheet either.
    draws = {shrink(kSheet, 200.0f, 200.0f), shrink(kSheet, 200.0f, 200.0f), plain(kSheet)};
    require(find_glow_blocks(draws, kScreen).empty(), "copies of another surface alone are no block");
    draws = {shrink(kSheet, 200.0f, 200.0f), shrink(kSheet, 300.0f, 300.0f), self(kSheet)};
    blocks = find_glow_blocks(draws, kScreen);
    require(blocks.size() == 1u && blocks[0].last == 2u, "a second shrinking copy inside the run belongs to the one block");

    // How much the first sprite must shrink.
    draws = {shrink(kSheet, 150.0f, 150.0f), plain(kSheet), self(kSheet)};
    require(find_glow_blocks(draws, kScreen).size() == 1u, "1.5 to 1 along both axes begins a block");
    draws = {shrink(kSheet, 149.0f, 200.0f), plain(kSheet), self(kSheet)};
    require(find_glow_blocks(draws, kScreen).empty(), "less than 1.5 to 1 along x does not");
    draws = {shrink(kSheet, 200.0f, 149.0f), plain(kSheet), self(kSheet)};
    require(find_glow_blocks(draws, kScreen).empty(), "nor along y");
    draws = {shrink(kSheet, 100.0f, 100.0f), plain(kSheet), self(kSheet)};
    require(find_glow_blocks(draws, kScreen).empty(), "a copy at the same size begins no block");
    draws = {shrink(kSheet, 50.0f, 50.0f), plain(kSheet), self(kSheet)};
    require(find_glow_blocks(draws, kScreen).empty(), "an enlarging copy begins no block");
    GeGlowDraw unknown = shrink(kSheet, 200.0f, 200.0f);
    unknown.rectangles_known = false;
    draws = {unknown, plain(kSheet), self(kSheet)};
    require(find_glow_blocks(draws, kScreen).empty(), "rectangles that are not known begin no block");
    GeGlowDraw flat = copy(kSheet, kComposite, {0.0f, 0.0f, 200.0f, 200.0f}, {0.0f, 0.0f, 0.0f, 100.0f});
    draws = {flat, plain(kSheet), self(kSheet)};
    require(find_glow_blocks(draws, kScreen).empty(), "a sprite without width begins no block");
    // A run that begins with a sprite reading the sheet, or with a fill, has no beginning.
    draws = {self(kSheet), plain(kSheet), self(kSheet)};
    require(find_glow_blocks(draws, kScreen).empty(), "a run that does not begin with a shrinking copy is no block");

    // Never the displayed surface.
    draws = {shrink(kSheet, 200.0f, 200.0f), plain(kSheet), self(kSheet)};
    require(find_glow_blocks(draws, kSheet).empty(), "a run into the displayed surface is no block");

    // What ends the run: another surface, a draw that is not a sprite of the effect.
    draws = {shrink(kSheet, 200.0f, 200.0f), plain(kSheet), self(kSheet), plain(kComposite), self(kSheet)};
    blocks = find_glow_blocks(draws, kScreen);
    require(blocks.size() == 1u && blocks[0].last == 2u, "the run ends before a draw into another surface");
    draws = {shrink(kSheet, 200.0f, 200.0f), plain(kSheet), self(kSheet), other(kSheet), self(kSheet), self(kSheet)};
    blocks = find_glow_blocks(draws, kScreen);
    require(blocks.size() == 1u && blocks[0].last == 2u, "the run ends before a 3D draw or a clear into the sheet");
    draws = {shrink(kSheet, 200.0f, 200.0f), other(kSheet), self(kSheet), self(kSheet)};
    require(find_glow_blocks(draws, kScreen).empty(), "a 3D draw right after the copy: no block, and the sprites after it begin none");
    // A failed beginning does not hide a block that begins inside its run.
    draws = {shrink(kSheet, 200.0f, 200.0f), shrink(kComposite, 200.0f, 200.0f)};
    draws[1].source = kScene;
    draws.push_back(plain(kComposite));
    draws.push_back(self(kComposite));
    blocks = find_glow_blocks(draws, kScreen);
    require(blocks.size() == 1u && blocks[0].first == 1u && blocks[0].last == 3u && blocks[0].target == kComposite, "the search goes on after a beginning that led nowhere");
    // A copy of the surface onto itself is no beginning.
    draws = {copy(kSheet, kSheet, {0.0f, 0.0f, 200.0f, 200.0f}, {0.0f, 0.0f, 100.0f, 100.0f}), plain(kSheet), self(kSheet)};
    require(find_glow_blocks(draws, kScreen).empty(), "a sprite that shrinks its own surface begins no block");
}

// ---- captured frames ---------------------------------------------------------------------------------

// The number after `key` in a line of the trace ("n":632), searched from `from`; -1 when it is not there.
long long number_after(std::string_view line, std::string_view key, std::size_t from = 0u) {
    const auto at = line.find(key, from);
    if (at == std::string_view::npos) return -1;
    return std::strtoll(std::string(line.substr(at + key.size(), 24u)).c_str(), nullptr, 0);
}
// The text between quotes after `key` ("fb":"0x040D8000").
std::string text_after(std::string_view line, std::string_view key, std::size_t from = 0u) {
    const auto at = line.find(key, from);
    if (at == std::string_view::npos) return {};
    const auto begin = at + key.size(), end = line.find('"', begin);
    return end == std::string_view::npos ? std::string() : std::string(line.substr(begin, end - begin));
}

// Main memory of a capture (host/ge_frame_capture.hpp: "CHILIGE1", version, slot count, slots of tag, offset, size).
std::vector<unsigned char> capture_ram(const std::filesystem::path &file) {
    std::ifstream in(file, std::ios::binary);
    char magic[8]{};
    std::uint32_t version{}, slots{};
    in.read(magic, 8).read(reinterpret_cast<char *>(&version), 4).read(reinterpret_cast<char *>(&slots), 4);
    if (!in || std::memcmp(magic, "CHILIGE1", 8) != 0 || slots > 256u) throw std::runtime_error("not a capture: " + file.string());
    for (std::uint32_t i = 0; i < slots; ++i) {
        char tag[8]{};
        std::uint64_t offset{}, size{};
        in.read(tag, 8).read(reinterpret_cast<char *>(&offset), 8).read(reinterpret_cast<char *>(&size), 8);
        if (!in) break;
        if (std::strncmp(tag, "RAM", 8) != 0) continue;
        std::vector<unsigned char> ram(static_cast<std::size_t>(size));
        in.seekg(static_cast<std::streamoff>(offset));
        in.read(reinterpret_cast<char *>(ram.data()), static_cast<std::streamsize>(ram.size()));
        if (!in) throw std::runtime_error("capture cut short: " + file.string());
        return ram;
    }
    throw std::runtime_error("no main memory in " + file.string());
}

void captured_frames(const std::filesystem::path &folder) {
    const auto trace = folder / "mission_9450_9453_trace.jsonl";
    std::ifstream in(trace);
    if (!in) throw std::runtime_error("cannot read " + trace.string());
    struct Row { long long n{}; bool clear{}; GeGlowDraw draw; std::string vtype; std::uint32_t vertices{}, count{}; };
    std::map<long long, std::vector<Row>> lists;
    std::set<std::uint32_t> surfaces;
    std::vector<std::pair<long long, std::uint32_t>> textures;   // (list, index of the row) with the texture's address in `source`
    for (std::string line; std::getline(in, line);) {
        if (text_after(line, "\"ev\":\"") != "draw") continue;
        Row row;
        const long long list = number_after(line, "\"list\":");
        row.n = number_after(line, "\"n\":");
        row.vtype = text_after(line, "\"vtype\":\"");
        row.count = static_cast<std::uint32_t>(number_after(line, "\"count\":"));
        row.vertices = static_cast<std::uint32_t>(std::strtoul(text_after(line, "\"vaddr\":\"").c_str(), nullptr, 16));
        const auto fb = static_cast<std::uint32_t>(std::strtoul(text_after(line, "\"fb\":\"").c_str(), nullptr, 16));
        row.draw.target = fb & 0x001FFFF0u;
        surfaces.insert(row.draw.target);
        const auto depth = line.find("\"depth\":{"), clear = line.find("\"clear\":{"), tex = line.find("\"tex\":{");
        require(depth != std::string::npos && clear != std::string::npos && tex != std::string::npos, "a draw of the trace has its depth, clear and texture state");
        row.clear = number_after(line, "\"on\":", clear) == 1;
        row.draw.sprite = text_after(line, "\"prim\":\"") == "sprites" && number_after(line, "\"through\":") == 1 &&
                          number_after(line, "\"test\":", depth) == 0 && !row.clear;
        // A clear does not sample; whether the texture is a surface is known once every draw's target is.
        if (number_after(line, "\"on\":", tex) == 1 && !row.clear) {
            const auto address = static_cast<std::uint32_t>(std::strtoul(text_after(line, "\"addr\":\"", tex).c_str(), nullptr, 16));
            if ((address & 0x0F800000u) == 0x04000000u) { row.draw.source = address & 0x001FFFF0u; row.draw.samples_surface = true; }
        }
        lists[list].push_back(row);
    }
    require(lists.size() == 4u && lists.count(9450) && lists.count(9451) && lists.count(9452) && lists.count(9453), "the trace has the four lists 9450 to 9453");
    require(surfaces == std::set<std::uint32_t>{kScreen, kSheet, kComposite, kScene}, "they draw into the game's four surfaces");
    for (auto &[list, rows] : lists) {
        std::filesystem::path capture;
        for (const auto &entry : std::filesystem::directory_iterator(folder)) {
            const auto name = entry.path().filename().string();
            if (name.rfind("ge_00" + std::to_string(list) + "_", 0u) == 0u && entry.path().extension() == ".gecap") capture = entry.path();
        }
        require(!capture.empty(), "the capture of list " + std::to_string(list) + " is in the folder");
        const auto ram = capture_ram(capture);
        std::vector<GeGlowDraw> draws;
        for (auto &row : rows) {
            GeGlowDraw &draw = row.draw;
            // A texture in VRAM that is no surface (a picture the game keeps there) is an ordinary texture.
            if (draw.samples_surface && !surfaces.count(draw.source)) draw.samples_surface = false;
            // The sprites' rectangles: 16-bit texture coordinates in texels, 16-bit positions in pixels.
            if (draw.sprite && draw.samples_surface && row.vtype == "0x800102" && row.count >= 2u) {
                const std::size_t offset = row.vertices - 0x08000000u;
                require(row.vertices >= 0x08000000u && offset + row.count * 10u <= ram.size(), "the sprites' vertices lie in main memory");
                draw.rectangles_known = true;
                draw.texels = draw.pixels = {1.0e9f, 1.0e9f, -1.0e9f, -1.0e9f};
                for (std::uint32_t i = 0; i < row.count; ++i) {
                    std::uint16_t uv[2];
                    std::int16_t xy[2];
                    std::memcpy(uv, ram.data() + offset + i * 10u, 4u);
                    std::memcpy(xy, ram.data() + offset + i * 10u + 4u, 4u);
                    draw.texels = {std::min(draw.texels[0], float(uv[0])), std::min(draw.texels[1], float(uv[1])), std::max(draw.texels[2], float(uv[0])), std::max(draw.texels[3], float(uv[1]))};
                    draw.pixels = {std::min(draw.pixels[0], float(xy[0])), std::min(draw.pixels[1], float(xy[1])), std::max(draw.pixels[2], float(xy[0])), std::max(draw.pixels[3], float(xy[1]))};
                }
            }
            draws.push_back(draw);
        }
        const auto blocks = find_glow_blocks(draws, kScreen);
        const std::string name = "list " + std::to_string(list);
        if (list == 9451 || list == 9453) {
            require(rows.size() == 1u && blocks.empty(), name + " (the copy to the screen) has no block");
            require(rows[0].draw.sprite && rows[0].draw.samples_surface && rows[0].draw.source == kComposite && !vcs::ge_glow_shrinks(rows[0].draw), "its one sprite copies the composite without shrinking it");
            continue;
        }
        require(blocks.size() == 1u && blocks[0].target == kSheet, name + " (a mission frame) has one block, on the sheet at 0x04088000");
        const GeGlowBlock &block = blocks[0];
        require(block.last - block.first == 26u, name + ": the block is 27 draws");
        if (list == 9450) require(rows[block.first].n == 632 && rows[block.last].n == 658, "in list 9450 they are draws 632 to 658");
        const GeGlowDraw &first = draws[block.first];
        require(first.source == kComposite && first.texels == std::array<float, 4>{0.0f, 0.0f, 480.0f, 272.0f} && first.pixels == std::array<float, 4>{0.0f, 0.0f, 240.0f, 136.0f},
                name + ": the block begins with the composite's 480x272 copied onto 240x136");
        unsigned reads_itself = 0u, fills = 0u;
        for (std::size_t i = block.first + 1u; i <= block.last; ++i) {
            require(draws[i].sprite && draws[i].target == kSheet && (!draws[i].samples_surface || draws[i].source == kSheet), name + ": every draw of the block is a sprite on the sheet that samples nothing else");
            if (draws[i].samples_surface) ++reads_itself; else ++fills;
        }
        require(reads_itself == 24u && fills == 2u, name + ": 24 of them read the sheet, two fill a rectangle");
        // The draw after the block adds the sheet to the composite, four times enlarged.
        require(block.last + 1u < draws.size(), name + ": a draw follows the block");
        const GeGlowDraw &reader = draws[block.last + 1u];
        require(reader.sprite && reader.target == kComposite && reader.samples_surface && reader.source == kSheet && reader.rectangles_known &&
                reader.texels == std::array<float, 4>{240.0f, 10.0f, 360.0f, 78.0f} && reader.pixels == std::array<float, 4>{0.0f, 0.0f, 480.0f, 272.0f},
                name + ": the next draw puts the sheet's 120x68 at (240, 10) onto the composite's 480x272");
        unsigned readers = 0u;
        for (std::size_t i = 0; i < draws.size(); ++i)
            if (draws[i].samples_surface && draws[i].source == kSheet && draws[i].target != kSheet) ++readers;
        require(readers == 1u, name + ": it is the only draw that reads the sheet from another surface");
        // Before the block nothing draws into the sheet; the composite's own copies are there and begin no block.
        unsigned composite_copies = 0u;
        for (std::size_t i = 0; i < block.first; ++i) {
            require(draws[i].target != kSheet, name + ": nothing is drawn into the sheet before the block");
            if (draws[i].sprite && draws[i].samples_surface && draws[i].target == kComposite && draws[i].source == kScene) { ++composite_copies; require(!vcs::ge_glow_shrinks(draws[i]), name + ": the scene's copy onto the composite does not shrink it"); }
        }
        require(composite_copies == 9u, name + ": nine draws copy the scene onto the composite");
    }
}
}

int main(int argc, char **argv) {
    try {
        made_up_frames();
        rule_edges();
        if (argc > 1) captured_frames(argv[1]);
        else std::cout << "no folder of captured frames given: the made-up frames only\n";
    } catch (const std::exception &failure) {
        std::cerr << "FAILED: " << failure.what() << " (after " << checks << " checks)\n";
        return 1;
    }
    if (failures) { std::cerr << failures << " of " << checks << " checks failed\n"; return 1; }
    std::cout << "glow blocks: " << checks << " checks passed\n";
    return 0;
}
