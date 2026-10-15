// The pictures of texture mods from their files to the renderer (host/gpu/ge_texture_mods.cpp), without the
// game and without a graphics card: small PNG files made up here in a scratch folder.
//
//   the plan of the start: screen-shaped pictures first, then the largest first; where it ends when the
//       store has no room for all; the bytes a picture takes with its smaller levels
//   the pictures of the game's start (boot to main menu) before all others, in the order the game draws them,
//       whatever the packs hold: the font sheets are not held up by 27 loading pictures of 2048x1088; the log's
//       count of them and of those a draw asked for before they were decoded
//   decoding ahead: every picture is in the store before anything asks for it, a later mod folder wins,
//       the first find gives the picture at once and nothing goes through the renderer's late path
//   a screen-shaped picture is decoded with the rows a screen image needs; a file whose shape suggested
//       the wrong rows is decoded once more with the texture's own, never handed over as it is
//   a store too small for all: the plan stops, the rest is decoded on first use and the oldest picture goes
//   a picture asked for before its turn overtakes the queue
//   a file that is no picture is reported once and left alone; the dump alone keeps one worker
//
//   chili_texture_store_test [scratch folder]
#include "gpu/ge_gpu_log.hpp"
#include "gpu/ge_texture_mods.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

// ge_texture_mods.cpp reports through the renderer's log, which this tool does not link.
namespace {
std::atomic<unsigned> errors_logged{0u};
}
namespace vcs {
void runtime_log_error(std::string_view, std::string_view) { ++errors_logged; }
}

namespace {
using namespace vcs;
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
    require(a == static_cast<A>(b), what + ": " + std::to_string(a) + " is not " + std::to_string(static_cast<A>(b)));
}

std::string key_name(std::uint64_t key) {
    char text[32];
    std::snprintf(text, sizeof(text), "%016llx.png", static_cast<unsigned long long>(key));
    return text;
}
// A picture of one colour per row, so that a row can be told from the next; `shade` tells pictures apart.
void write_picture(const std::filesystem::path &folder, std::uint64_t key, std::uint32_t width, std::uint32_t height, std::uint8_t shade) {
    std::vector<std::byte> rgba(static_cast<std::size_t>(width) * height * 4u);
    for (std::uint32_t y = 0u; y < height; ++y)
        for (std::uint32_t x = 0u; x < width; ++x) {
            std::byte *texel = rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4u;
            texel[0] = std::byte{shade};
            texel[1] = static_cast<std::byte>(y & 255u);
            texel[2] = static_cast<std::byte>((y >> 8u) & 255u);
            texel[3] = std::byte{255};
        }
    std::string error;
    require(ge_texture_write_png(folder / key_name(key), width, height, rgba, error), "cannot write a test picture: " + error);
}
std::filesystem::path fresh(const std::filesystem::path &folder) {
    std::error_code status;
    std::filesystem::remove_all(folder, status);
    std::filesystem::create_directories(folder);
    return folder;
}
std::uint64_t chain(std::uint32_t width, std::uint32_t height) {
    std::uint64_t bytes = 0u;
    for (;; width = std::max(1u, width >> 1u), height = std::max(1u, height >> 1u)) {
        bytes += static_cast<std::uint64_t>(width) * height * 4u;
        if (width == 1u && height == 1u) return bytes;
    }
}
// The pictures the renderer would be handed on its late path, all of them so far.
std::vector<GeTextureReplacementReady> taken() {
    std::vector<GeTextureReplacementReady> ready;
    ge_texture_mods_take_ready(ready);
    return ready;
}
unsigned shade_of(const GeTextureReplacementPtr &picture) { return std::to_integer<unsigned>(picture->rgba8[0]); }
unsigned row_mark(const GeTextureReplacementPtr &picture, std::uint32_t row) {
    const std::byte *texel = picture->rgba8.data() + static_cast<std::size_t>(row) * picture->width * 4u;
    return std::to_integer<unsigned>(texel[1]) | (std::to_integer<unsigned>(texel[2]) << 8u);
}

void plan() {
    require(ge_texture_screen_shaped(512u, 272u) && ge_texture_screen_shaped(1024u, 544u) && ge_texture_screen_shaped(4096u, 2176u), "k x 512 by k x 272 is a screen picture");
    require(!ge_texture_screen_shaped(3840u, 2176u) && !ge_texture_screen_shaped(1024u, 512u) && !ge_texture_screen_shaped(512u, 544u) && !ge_texture_screen_shaped(480u, 272u) &&
            !ge_texture_screen_shaped(0u, 0u) && !ge_texture_screen_shaped(1024u, 272u), "no other shape is");
    is(ge_texture_decoded_bytes(4u, 4u), 4u * 4u * 4u + 2u * 2u * 4u + 4u, "a 4x4 picture with its levels");
    is(ge_texture_decoded_bytes(1024u, 512u), chain(1024u, 512u), "a 1024x512 picture with its levels");
    is(ge_texture_decoded_bytes(512u, 272u), chain(512u, 512u), "a screen picture is decoded onto 512 rows");
    is(ge_texture_decoded_bytes(4096u, 2176u), chain(4096u, 4096u), "a 4096x2176 screen picture onto 4096 rows");
    is(ge_texture_decoded_bytes(0u, 0u), 0u, "a file whose header cannot be read counts nothing");
    is(ge_texture_decoded_bytes(8193u, 16u), 0u, "nor does a picture too large to decode");

    // Load order in, decoding order out: the screen-shaped ones first, then the largest first, the unreadable last.
    std::vector<GeTextureAheadFile> files{{1u, 64u, 64u}, {2u, 512u, 272u}, {3u, 128u, 64u}, {4u, 0u, 0u}, {5u, 1024u, 544u}, {6u, 32u, 32u}, {7u, 64u, 64u}};
    std::size_t ahead = ge_texture_ahead_plan(files, ~0ull);
    is(ahead, 7u, "with room for all, all are decoded ahead");
    require(files[0].key == 5u && files[1].key == 2u && files[2].key == 3u && files[3].key == 1u && files[4].key == 7u && files[5].key == 6u && files[6].key == 4u,
            "screen-shaped pictures first, then the largest first, pictures of one size in load order");
    // The store ends the plan at the first picture that no longer fits; nothing after it is decoded ahead.
    const std::uint64_t two_screens = chain(512u, 512u) + chain(1024u, 1024u);
    is(ge_texture_ahead_plan(files, two_screens + chain(128u, 64u) + chain(64u, 64u) - 1u), 3u, "the plan stops at the picture that does not fit");
    is(ge_texture_ahead_plan(files, two_screens + chain(128u, 64u) + chain(64u, 64u)), 4u, "a picture that just fits is in");
    is(ge_texture_ahead_plan(files, two_screens + chain(128u, 64u) + 2u * chain(64u, 64u) + chain(32u, 32u)), 7u, "and the unreadable one at the end costs nothing");
    is(ge_texture_ahead_plan(files, two_screens - 1u), 1u, "a store smaller than the screen pictures");
    is(ge_texture_ahead_plan(files, 0u), 0u, "no store, no plan");
    std::vector<GeTextureAheadFile> none;
    is(ge_texture_ahead_plan(none, 1000u), 0u, "no files");
}

// Keys of the game's own textures (mods/TEXTURES.md, index.csv of the extracted archive).
constexpr std::uint64_t kBoot = 0x724ca7d1300aa765ull, kLoadGame = 0x21c797cd712c11d9ull;
constexpr std::uint64_t kTextSheet = 0x5d0c6d1a663a8515ull, kOutlineSheet = 0x002d10b31e312e9dull;
constexpr std::uint64_t kWall03 = 0xa247e7634946f676ull, kWall01 = 0xe1215277c6aafa16ull, kTitleLogo = 0x048e4eecf23e28a1ull;
constexpr std::uint64_t kGameLogo = 0x854c5701a58fb9c3ull, kCharacter = 0x68b9cff5c3f6a8d4ull, kHealthLeft = 0x212d2a1a5fd6b847ull;
constexpr std::uint64_t kButtonCross = 0xdc8cc6b510da1d80ull, kDpad = 0xe8cc8b252a16c095ull;
// The 27 loading pictures of the missions (loadscreen1_1 .. 7_3, loadscreenmacho*), a trophy picture, and HUD
// pictures the start does not draw (Pistol, Rifle, fist).
constexpr std::uint64_t kMissionLoading[] = {
    0x104afe1711660084ull, 0x56741964fb4dbf8eull, 0xec68a053e0a9f47cull, 0xc24c96c4f1d20cc1ull, 0x0ab472675bcd3fefull,
    0x54ae25ca2bbd5d73ull, 0x95950b079c6bf672ull, 0xbbbe4e61fe12ffa9ull, 0x303430f0ca079283ull, 0xd735036354e896e8ull,
    0x77bfaf742d0457e8ull, 0x32c238a5e4a7116aull, 0x5ee9ef23c4a61206ull, 0x0a91a05ad2d3f081ull, 0x58634ac6722e2e6eull,
    0xc5ed1d703b083b8dull, 0x92d296ca98b32fceull, 0x3064e115cec1b623ull, 0x4f63d7dd4438672eull, 0x04a53e2de0f1ee93ull,
    0x5796ddb491d07983ull, 0x8dd50022bed7a3e8ull, 0xfa90aea8346ea7d8ull, 0x4567781265a31831ull, 0x597dad52ed4fc39cull,
    0x85dc5138355da398ull, 0xde2c6135dab9e737ull};
constexpr std::uint64_t kTrophy = 0x0f896f88d6fbc26bull, kRifle = 0x1fee6d6c66275610ull, kFist = 0x0096d5d5f2844c16ull;

std::size_t place(const std::vector<GeTextureAheadFile> &files, std::uint64_t key) {
    for (std::size_t i = 0u; i < files.size(); ++i) if (files[i].key == key) return i;
    return files.size();
}

void start_first() {
    const auto keys = ge_texture_start_keys();
    is(keys.size(), 48u, "48 textures from the start to the main menu");
    for (std::size_t i = 0u; i < keys.size(); ++i)
        for (std::size_t j = i + 1u; j < keys.size(); ++j) require(keys[i] != keys[j], "no key twice in the start list");
    const auto rank = [&](std::uint64_t key) { return static_cast<std::size_t>(std::find(keys.begin(), keys.end(), key) - keys.begin()); };
    for (const std::uint64_t key : {kBoot, kLoadGame, kTextSheet, kOutlineSheet, kWall03, kWall01, kTitleLogo, kGameLogo, kCharacter, kButtonCross, kDpad, kHealthLeft})
        require(rank(key) < keys.size(), "the boot, the first loading screen, both font sheets, walls, logos, buttons are in the start list");
    for (const std::uint64_t key : kMissionLoading) require(rank(key) == keys.size(), "no mission's loading picture is");
    require(rank(kTrophy) == keys.size() && rank(kRifle) == keys.size() && rank(kFist) == keys.size(), "nor a trophy or a HUD picture of the missions");
    require(rank(kBoot) == 0u && rank(kTextSheet) < rank(kLoadGame) && rank(kLoadGame) < rank(kWall03) && rank(kWall03) < rank(kOutlineSheet) &&
            rank(kWall01) < rank(kTitleLogo) && rank(kTitleLogo) < rank(kCharacter), "in the order the game draws them");

    // Five HD packs with 27 loading pictures at 2048x1088, in the order the mods' folders give them
    // (five packs, by their folder names): the start's pictures come first, the text sheet before the
    // first loading screen, then the 27 loading pictures, then the rest, the largest first.
    std::vector<GeTextureAheadFile> files;
    files.push_back({kRifle, 1024u, 512u});
    files.push_back({kHealthLeft, 512u, 256u});
    files.push_back({kFist, 1024u, 512u});
    for (const std::uint64_t key : kMissionLoading) files.push_back({key, 2048u, 1088u});
    files.push_back({kWall01, 3840u, 2176u});
    files.push_back({kWall03, 3840u, 2176u});
    files.push_back({kCharacter, 2176u, 2176u});
    files.push_back({kTitleLogo, 4096u, 2048u});
    files.push_back({kGameLogo, 2048u, 1024u});
    files.push_back({kTrophy, 2048u, 1088u});
    files.push_back({kOutlineSheet, 2048u, 4096u});
    files.push_back({kTextSheet, 2048u, 4096u});
    files.push_back({kLoadGame, 2048u, 1088u});
    const std::vector<GeTextureAheadFile> given = files;
    is(ge_texture_ahead_plan(files, ~0ull), files.size(), "all ahead");
    const std::uint64_t start_order[] = {kHealthLeft, kTextSheet, kLoadGame, kWall03, kGameLogo, kOutlineSheet, kWall01, kTitleLogo, kCharacter};
    for (std::size_t i = 0u; i < std::size(start_order); ++i) is(place(files, start_order[i]), i, "the start's pictures first, as the game draws them");
    for (std::size_t i = 0u; i < std::size(kMissionLoading) + 1u; ++i)
        require(ge_texture_screen_shaped(files[std::size(start_order) + i].width, files[std::size(start_order) + i].height), "then the other screen pictures");
    require(place(files, kMissionLoading[0]) == std::size(start_order) && place(files, kTrophy) == std::size(start_order) + std::size(kMissionLoading),
            "in load order among themselves");
    require(place(files, kRifle) == files.size() - 2u && place(files, kFist) == files.size() - 1u, "then the largest first, one size in load order");
    // Whatever order the folders give, the same plan.
    for (int round = 0; round < 8; ++round) {
        std::vector<GeTextureAheadFile> shuffled(given.rbegin(), given.rend());
        std::rotate(shuffled.begin(), shuffled.begin() + round * 4, shuffled.end());
        (void)ge_texture_ahead_plan(shuffled, ~0ull);
        for (std::size_t i = 0u; i < std::size(start_order); ++i) is(place(shuffled, start_order[i]), i, "the start's pictures first in any load order");
    }
    // A store with room for the start's pictures and a few more: the start's are all decoded ahead, loading pictures wait.
    std::uint64_t start_bytes = 0u;
    for (std::size_t i = 0u; i < std::size(start_order); ++i) start_bytes += ge_texture_decoded_bytes(files[i].width, files[i].height);
    files = given;
    const std::size_t ahead = ge_texture_ahead_plan(files, start_bytes + 3u * ge_texture_decoded_bytes(2048u, 1088u));
    is(ahead, std::size(start_order) + 3u, "the store ends the plan after three loading pictures");
}

// The workers take the plan's order: with 24 screen pictures and 8 other pictures in the folder ahead of them,
// the start's three pictures are decoded first, and a draw that asks for one before its turn is counted.
void start_decoding(const std::filesystem::path &root) {
    const auto first = fresh(root / "start" / "a_first"), last = fresh(root / "start" / "z_last");
    for (std::uint64_t i = 0u; i < 24u; ++i) write_picture(first, kMissionLoading[i], 1024u, 544u, static_cast<std::uint8_t>(i));
    for (std::uint64_t i = 0u; i < 8u; ++i) write_picture(first, 0x100u + i, 512u, 512u, 40u);
    write_picture(last, kTextSheet, 256u, 512u, 50u);
    write_picture(last, kLoadGame, 512u, 272u, 51u);
    write_picture(last, kOutlineSheet, 256u, 512u, 52u);
    GeTextureModsSetup setup;
    setup.replacement_folders = {first, last};
    setup.store_bytes = 1024ull * 1024ull * 1024ull;
    for (int round = 0; round < 5; ++round) {
        ge_texture_mods_start(setup);
        ge_texture_mods_wait_idle();
        auto report = ge_texture_mods_report();
        is(report.decoded_ahead, 35u, "all 35 decoded ahead");
        is(report.start_files, 3u, "three of them are the start's");
        // Two workers: one may finish another picture while the other still has the last of the start's in hand; a
        // busy PC can stretch that, so the check is "before most of the others" (in the earlier order, the
        // screen pictures and then the largest first, all 32 others came first).
        require(report.start_others_before <= 8u, "the start's pictures are decoded before the others (" + std::to_string(report.start_others_before) +
                                                      " of 32 other pictures were done first)");
        require(report.start_decoded_ms <= report.decoded_ahead_ms, "and are all there before the last picture");
        require(report.late == 0u && report.start_late == 0u, "nothing late");
        const auto sheet = ge_texture_mods_find(kTextSheet, 512u, 512u);
        require(sheet != nullptr && shade_of(sheet) == 50u, "the text sheet is ready at its first find");
        const auto loading = ge_texture_mods_find(kLoadGame, 272u, 512u);
        require(loading != nullptr && loading->height == 512u && loading->source_height == 272u, "the first loading screen with a screen image's rows");
        ge_texture_mods_stop();
    }
    // Asked for at once, before the plan has run: on the late path, and counted as a picture of the start that was late.
    ge_texture_mods_start(setup);
    const bool at_once = ge_texture_mods_find(kOutlineSheet, 512u, 512u) != nullptr;
    ge_texture_mods_wait_idle();
    auto report = ge_texture_mods_report();
    require(!at_once, "nothing is decoded the moment the workers start");
    require(report.late == 1u && report.start_late == 1u, "one picture late, and it is the start's");
    is(report.start_files, 3u, "still three of the start's in the plan");
    is(report.decoded_ahead, 34u, "the others ahead");
    const auto ready = taken();
    require(ready.size() == 1u && ready[0].key == kOutlineSheet && ready[0].picture && shade_of(ready[0].picture) == 52u, "it arrives through the late path");
    ge_texture_mods_stop();
    // Packs without a picture of the start: no count, nothing changes.
    GeTextureModsSetup others;
    others.replacement_folders = {first};
    others.store_bytes = setup.store_bytes;
    ge_texture_mods_start(others);
    ge_texture_mods_wait_idle();
    report = ge_texture_mods_report();
    require(report.start_files == 0u && report.start_decoded_ms == 0u && report.decoded_ahead == 32u, "without the start's pictures the plan is the old one");
    ge_texture_mods_stop();
}

void decoded_ahead(const std::filesystem::path &root) {
    const auto low = fresh(root / "ahead" / "low"), high = fresh(root / "ahead" / "high");
    // Seven pictures in the lower folder, three in the higher one; key 3 is in both and the higher wins.
    for (std::uint64_t key = 1u; key <= 7u; ++key) write_picture(low, key, 64u, 32u, 10u);
    for (std::uint64_t key : {3u, 8u, 9u}) write_picture(high, key, 32u, 32u, 20u);
    { std::ofstream note(low / "notes.txt"); note << "not a picture"; }
    GeTextureModsSetup setup;
    setup.replacement_folders = {low, high};
    setup.store_bytes = 64ull * 1024ull * 1024ull;
    errors_logged = 0u;
    ge_texture_mods_start(setup);
    require(ge_texture_mods_keying() && !ge_texture_mods_dumping(), "textures are keyed with a mod's pictures");
    ge_texture_mods_wait_idle();
    auto report = ge_texture_mods_report();
    is(report.replacement_files, 9u, "nine keys in two folders");
    is(report.decoded_ahead, 9u, "all of them decoded ahead");
    is(report.workers, 2u, "on two workers");
    require(report.waiting_first_use == 0u && report.late == 0u && report.requeued == 0u && report.failed == 0u && report.replaced == 0u, "nothing waits, nothing was late, nothing was drawn yet");
    const std::uint64_t all = 6u * chain(64u, 32u) + 3u * chain(32u, 32u);
    is(report.kept_bytes, all, "the store holds every picture with its levels");
    is(report.decoded_ahead_bytes, all, "and that is what was decoded ahead");
    is(report.store_bytes, setup.store_bytes, "the store's size is the setup's");
    require(taken().empty(), "a picture nobody asked for does not go through the late path");
    for (std::uint64_t key = 1u; key <= 9u; ++key) {
        const auto picture = ge_texture_mods_find(key, 32u, 32u);
        require(picture != nullptr, "the first find gives the picture at once");
        require(picture->mod_picture && !picture->exact_levels && picture->source_height == picture->height, "a mod's picture, not padded");
        is(shade_of(picture), key == 3u || key >= 8u ? 20u : 10u, "the higher folder's picture wins");
        is(picture->width, key == 3u || key >= 8u ? 32u : 64u, "its size");
        is(picture->mip_levels, key == 3u || key >= 8u ? 6u : 7u, "every level down to 1x1");
        is(picture->rgba8.size(), chain(picture->width, picture->height), "the chain's bytes");
    }
    require(ge_texture_mods_find(100u, 32u, 32u) == nullptr && !ge_texture_mods_has(100u) && ge_texture_mods_has(3u), "a key without a file");
    (void)ge_texture_mods_find(1u, 32u, 32u);
    report = ge_texture_mods_report();
    is(report.replaced, 9u, "nine pictures reached the renderer, each counted once");
    require(report.late == 0u && taken().empty(), "none of them late");
    is(errors_logged.load(), 0u, "a file that is no PNG by its name is not reported");
    ge_texture_mods_stop();
    require(!ge_texture_mods_keying() && ge_texture_mods_find(1u, 32u, 32u) == nullptr, "stopped");
}

void screen_rows(const std::filesystem::path &root) {
    const auto folder = fresh(root / "screens");
    write_picture(folder, 0x21u, 1024u, 544u, 30u);   // a screen picture at twice the size: right guess
    write_picture(folder, 0x22u, 512u, 272u, 31u);    // screen-shaped, but it stands for a square texture: wrong guess
    write_picture(folder, 0x23u, 64u, 34u, 32u);      // not screen-shaped, but it stands for a screen image: wrong guess
    write_picture(folder, 0x24u, 512u, 272u, 33u);    // wrong guess again, found out while a worker has it or after
    GeTextureModsSetup setup;
    setup.replacement_folders = {folder};
    setup.store_bytes = 256ull * 1024ull * 1024ull;
    ge_texture_mods_start(setup);
    ge_texture_mods_wait_idle();
    is(ge_texture_mods_report().decoded_ahead, 4u, "four pictures decoded ahead");

    // The screen image: 272 of 512 texture rows hold the picture, so 544 rows become 1024 and the row below repeats the last.
    auto screen = ge_texture_mods_find(0x21u, 272u, 512u);
    require(screen != nullptr, "the screen picture is ready at its first find");
    require(screen->width == 1024u && screen->height == 1024u && screen->source_height == 544u, "decoded with the rows a screen image needs");
    require(row_mark(screen, 543u) == 543u && row_mark(screen, 544u) == 543u, "the row below the picture repeats its last row");
    is(ge_texture_mods_report().requeued, 0u, "a right guess is not decoded again");

    // Screen-shaped file, square texture: the padded picture must not be handed over.
    require(ge_texture_mods_find(0x22u, 64u, 64u) == nullptr, "a picture padded for a screen is not given to a square texture");
    is(ge_texture_mods_report().requeued, 1u, "it is decoded again");
    ge_texture_mods_wait_idle();
    auto ready = taken();
    require(ready.size() == 1u && ready[0].key == 0x22u && ready[0].picture && ready[0].picture->height == 272u && ready[0].picture->source_height == 272u, "and arrives with the texture's own rows");
    auto again = ge_texture_mods_find(0x22u, 64u, 64u);
    require(again != nullptr && again->height == 272u, "after that the store has it as the texture needs it");

    // A file of another shape for a screen image: decoded ahead without rows, then with them.
    require(ge_texture_mods_find(0x23u, 272u, 512u) == nullptr, "a picture without the added rows is not given to a screen image");
    ge_texture_mods_wait_idle();
    ready = taken();
    require(ready.size() == 1u && ready[0].key == 0x23u && ready[0].picture && ready[0].picture->height == 64u && ready[0].picture->source_height == 34u, "it arrives on 64 rows");
    is(ge_texture_mods_report().requeued, 2u, "two wrong guesses, two pictures decoded again");
    is(ge_texture_mods_report().late, 2u, "both were late: the game's own picture was drawn first");
    ge_texture_mods_stop();

    // The same wrong guess found out before the plan's job is done: whichever way the race goes, the
    // picture that reaches the renderer has the texture's rows.
    for (int round = 0; round < 20; ++round) {
        ge_texture_mods_start(setup);
        if (round % 2 == 1) std::this_thread::yield();
        const auto early = ge_texture_mods_find(0x24u, 64u, 64u);
        require(early == nullptr || early->height == 272u, "never the padded picture");
        ge_texture_mods_wait_idle();
        for (const auto &item : taken()) require(item.key != 0x24u || (item.picture && item.picture->height == 272u), "the late path carries the texture's rows");
        const auto settled = ge_texture_mods_find(0x24u, 64u, 64u);
        require(settled != nullptr && settled->height == 272u, "and the store ends with them");
        ge_texture_mods_stop();
    }
}

void small_store(const std::filesystem::path &root) {
    const auto folder = fresh(root / "small");
    for (std::uint64_t key = 1u; key <= 8u; ++key) write_picture(folder, key, 64u, 64u, static_cast<std::uint8_t>(key));
    const std::uint64_t one = chain(64u, 64u);
    GeTextureModsSetup setup;
    setup.replacement_folders = {folder};
    setup.store_bytes = 5u * one + one / 2u;   // room for five
    ge_texture_mods_start(setup);
    ge_texture_mods_wait_idle();
    auto report = ge_texture_mods_report();
    is(report.decoded_ahead, 5u, "the plan stops where the store ends");
    is(report.waiting_first_use, 3u, "three wait for their first use");
    is(report.kept_bytes, 5u * one, "five pictures kept");
    for (std::uint64_t key = 1u; key <= 5u; ++key) require(ge_texture_mods_find(key, 64u, 64u) != nullptr, "the five are ready at their first find");
    // A picture beyond the plan: today's path, the game's own first, then the swap.
    require(ge_texture_mods_find(7u, 64u, 64u) == nullptr, "the sixth to be asked for is not there yet");
    ge_texture_mods_wait_idle();
    auto ready = taken();
    require(ready.size() == 1u && ready[0].key == 7u && ready[0].picture && shade_of(ready[0].picture) == 7u, "it arrives once through the late path");
    report = ge_texture_mods_report();
    is(report.late, 1u, "one picture was late");
    is(report.replaced, 6u, "six reached the renderer");
    is(report.kept_bytes, 5u * one, "the store stays inside its size");
    // The one found longest ago went: key 1.
    require(ge_texture_mods_find(2u, 64u, 64u) != nullptr && ge_texture_mods_find(7u, 64u, 64u) != nullptr, "the newer ones stay");
    require(ge_texture_mods_find(1u, 64u, 64u) == nullptr, "the oldest was dropped and is decoded again when asked for");
    ge_texture_mods_wait_idle();
    ready = taken();
    require(ready.size() == 1u && ready[0].key == 1u, "and comes back");
    is(ge_texture_mods_report().replaced, 6u, "a picture that comes back is not counted twice");
    ge_texture_mods_stop();
}

void overtaking(const std::filesystem::path &root) {
    const auto folder = fresh(root / "queue");
    constexpr std::uint64_t count = 40u;
    for (std::uint64_t key = 1u; key <= count; ++key) write_picture(folder, key, 1024u, 1024u, static_cast<std::uint8_t>(key));
    GeTextureModsSetup setup;
    setup.replacement_folders = {folder};
    setup.store_bytes = 1024ull * 1024ull * 1024ull;
    ge_texture_mods_start(setup);
    // The last file of the plan, asked for at once.
    require(ge_texture_mods_find(count, 1024u, 1024u) == nullptr, "nothing is decoded the moment the workers start");
    std::uint64_t others = 0u;
    bool arrived = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!arrived && std::chrono::steady_clock::now() < deadline) {
        for (const auto &item : taken()) if (item.key == count && item.picture) arrived = true;
        if (arrived) others = ge_texture_mods_report().decoded_ahead;
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(arrived, "the picture asked for arrives");
    require(others < count / 2u, "before most of the queue: " + std::to_string(others) + " of " + std::to_string(count - 1u) + " others were decoded first");
    ge_texture_mods_wait_idle();
    const auto report = ge_texture_mods_report();
    is(report.decoded_ahead, count - 1u, "the others are decoded ahead all the same");
    is(report.late, 1u, "one was asked for before it was decoded");
    is(report.kept_bytes, count * chain(1024u, 1024u), "all kept");
    ge_texture_mods_stop();
    // Stopping while the queue is full does not wait for it.
    ge_texture_mods_start(setup);
    const auto started = std::chrono::steady_clock::now();
    ge_texture_mods_stop();
    require(std::chrono::steady_clock::now() - started < std::chrono::seconds(20), "a stop lets the pictures still queued go");
}

void unusable_and_dump(const std::filesystem::path &root) {
    const auto folder = fresh(root / "bad");
    write_picture(folder, 1u, 16u, 16u, 5u);
    { std::ofstream bad(folder / key_name(2u), std::ios::binary); bad << "this is not a picture"; }
    { std::ofstream named(folder / "pistol.png", std::ios::binary); named << "nor has it a key for a name"; }
    GeTextureModsSetup setup;
    setup.replacement_folders = {folder};
    errors_logged = 0u;
    ge_texture_mods_start(setup);
    is(errors_logged.load(), 1u, "a .png whose name is no key is reported at the start");
    ge_texture_mods_wait_idle();
    auto report = ge_texture_mods_report();
    require(report.replacement_files == 2u && report.decoded_ahead == 1u && report.failed == 1u, "one picture decoded, one file unreadable");
    is(report.store_bytes, 256ull * 1024ull * 1024ull, "a setup without a size keeps the 256 MiB");
    is(errors_logged.load(), 2u, "the unreadable file is reported once");
    require(ge_texture_mods_find(2u, 16u, 16u) == nullptr && ge_texture_mods_find(2u, 16u, 16u) == nullptr, "and never handed over");
    ge_texture_mods_wait_idle();
    require(taken().empty() && errors_logged.load() == 2u && ge_texture_mods_report().failed == 1u, "nor decoded again");
    ge_texture_mods_stop();

    // The dump alone: one worker, no plan, nothing decoded.
    const auto dump = fresh(root / "dump");
    GeTextureModsSetup dumping;
    dumping.dump_folder = dump;
    ge_texture_mods_start(dumping);
    require(ge_texture_mods_keying() && ge_texture_mods_dumping(), "the dump alone keys textures");
    std::vector<std::byte> texels(8u * 8u * 4u, std::byte{200});
    ge_texture_mods_dump(0xABCDu, 8u, 8u, texels);
    ge_texture_mods_dump(0xABCDu, 8u, 8u, texels);
    ge_texture_mods_wait_idle();
    report = ge_texture_mods_report();
    require(report.workers == 1u && report.replacement_files == 0u && report.decoded_ahead == 0u && report.dumped == 1u, "one worker writes each texture once");
    require(std::filesystem::is_regular_file(dump / key_name(0xABCDu)), "the dumped picture is there");
    ge_texture_mods_stop();

    // Neither: nothing starts.
    ge_texture_mods_start(GeTextureModsSetup{});
    require(!ge_texture_mods_keying() && ge_texture_mods_report().workers == 0u, "without pictures and without a dump no worker starts");
    ge_texture_mods_wait_idle();
    ge_texture_mods_stop();
}
}

int main(int argc, char **argv) {
    const std::filesystem::path root = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::temp_directory_path() / "chili_texture_store";
    int result = 1;
    try {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) throw std::runtime_error("COM does not start");
        std::filesystem::create_directories(root);
        plan();
        start_first();
        decoded_ahead(root);
        start_decoding(root);
        screen_rows(root);
        small_store(root);
        overtaking(root);
        unusable_and_dump(root);
        if (failures) std::cerr << failures << " of " << checks << " checks FAILED\n";
        else { std::cout << "texture store: " << checks << " checks passed\n"; result = 0; }
    } catch (const std::exception &error) {
        std::cerr << "FAILED after " << checks << " checks: " << error.what() << '\n';
    }
    vcs::ge_texture_mods_stop();
    std::error_code status;
    std::filesystem::remove_all(root, status);
    return result;
}
