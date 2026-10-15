#pragma once
// Run-time texture dump and replacement (mods/TEXTURES.md).
//
// A texture is identified by its content key (ge_texture_key.hpp). A file
// mods/<name>/textures/<key>.png replaces it; with dumping on, every keyed
// texture is written once as <dump folder>/<key>.png. PNG files are read and
// written by Windows Imaging Component on worker threads, so the thread that
// executes GE lists never touches the disk.
//
// Every replacement picture is decoded when the game starts, before a draw
// asks for it (two workers; the pictures of what the game draws from its start
// to the main menu first, in that order and at normal priority, then the other
// screen-shaped pictures, then the largest first, below normal priority), as
// far as the store of decoded pictures goes. A texture whose picture is in the store is drawn
// with it from its first frame. One that is not (the plan has not reached it,
// or the store was too small for it) is drawn with the game's own pixels
// until its picture has been decoded (it goes to the front of the queue),
// then swapped.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace vcs {

struct GeTextureModsSetup {
    // mods/<name>/textures folders, lowest priority first.
    std::vector<std::filesystem::path> replacement_folders;
    // Empty: no dumping.
    std::filesystem::path dump_folder;
    // The most memory the decoded pictures may take, smaller levels included ([Mods] PictureStoreMB,
    // host/mod_files.hpp). 0: 256 MiB.
    std::uint64_t store_bytes{};
};

// A decoded replacement: 8-bit RGBA, level 0 followed by every smaller level
// down to 1x1, each half the one before (rounded down, at least 1).
struct GeTextureReplacement {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t mip_levels{};
    std::vector<std::byte> rgba8;
    // Height of the file's own picture. `height` is larger when rows were added
    // below it for a screen image (272 of 512 texture rows hold the picture).
    std::uint32_t source_height{};
    // An overlay's picture over the game's own (below): the levels are the
    // game's picture at exactly that size times 2, 4, ... and they end at the
    // game's own size, with the game's own texels. It is sampled without the
    // game's level bias, so each draw takes the level that has one texel per
    // pixel and text drawn at the size the game meant looks as it did.
    bool exact_levels{};
    // A mod's picture, or cells painted over one: several times the size of the
    // game's texture, with every level down to 1x1. In a 3D draw it is sampled
    // without the game's level bias and with a linear filter between levels, so
    // the draw reads the levels that have about one texel per screen pixel; in
    // a sprite it keeps the sampler of a replaced texture (ge_sampler_rule.hpp).
    // Cells painted over the game's own picture are not.
    bool mod_picture{};
};
using GeTextureReplacementPtr = std::shared_ptr<const GeTextureReplacement>;

struct GeTextureReplacementReady {
    std::uint64_t key{};
    GeTextureReplacementPtr picture;   // null: the file could not be used
};

struct GeTextureModsReport {
    std::uint64_t replacement_files{};
    std::uint64_t replaced{};          // pictures handed to the renderer (each counted once)
    std::uint64_t failed{};            // files that could not be read
    // Decoding at the start, ahead of the draws.
    std::uint64_t decoded_ahead{};        // pictures decoded before a draw asked for them
    std::uint64_t decoded_ahead_bytes{};  // their bytes, smaller levels included
    std::uint64_t decoded_ahead_ms{};     // from the start to the last of them
    std::uint64_t waiting_first_use{};    // files left for their first use: the store has no room for them
    std::uint64_t late{};                 // pictures decoded after a draw asked: the game's own was drawn first
    std::uint64_t requeued{};             // pictures decoded again because the texture's rows were not the file's shape
    // The pictures of the start (ge_texture_start_keys) among them.
    std::uint64_t start_files{};          // in the plan: the packs replace that many of them
    std::uint64_t start_decoded_ms{};     // from the start to the last of them decoded (ahead or on first use)
    std::uint64_t start_late{};           // of `late`: drawn before they were decoded
    std::uint64_t start_others_before{};  // other pictures decoded before the last of them (tests)
    std::uint32_t workers{};
    std::uint64_t dumped{};            // PNG files written by this run
    std::uint64_t keyed{};             // textures keyed
    std::uint64_t unstable_addresses{};
    std::uint64_t store_bytes{};       // the limit in force for decoded pictures
    std::uint64_t kept_bytes{};        // decoded pictures in memory now
    // Overlays (below).
    std::uint64_t overlay_composed{};        // textures uploaded with cells painted in
    std::uint64_t overlay_compose_us{};      // time that took, all of them
    std::uint64_t overlay_compose_max_us{};  // and the slowest one
    std::uint64_t overlay_bytes{};           // bytes of those pictures, smaller levels included
    std::uint64_t overlay_evicted{};         // cached textures taken out because their cells changed
    // What computing content keys cost (every texture with a mod or the dump, else the watched shapes).
    std::uint64_t keyed_textures{};
    std::uint64_t keying_ns{};
    std::uint64_t keying_max_ns{};
};

// Scans the folders and starts the worker. Call before the first draw. With no
// replacement file and no dump folder nothing is started and
// ge_texture_mods_keying() stays false.
void ge_texture_mods_start(const GeTextureModsSetup &setup);
void ge_texture_mods_stop() noexcept;

// True when textures need a key at all. Plain load, safe per upload.
[[nodiscard]] bool ge_texture_mods_keying() noexcept;
[[nodiscard]] bool ge_texture_mods_dumping() noexcept;

// A texture at one address whose content keeps changing (a picture the game
// draws into, an animated palette) gets a new key every time: it must be
// neither dumped nor matched. Returns false while `address` is in that state.
// `tick` counts presented frames.
[[nodiscard]] bool ge_texture_mods_admit(std::uint32_t address, std::uint64_t key, std::uint64_t tick);

// True when a replacement file exists for `key`.
[[nodiscard]] bool ge_texture_mods_has(std::uint64_t key) noexcept;

// The decoded replacement when it is in memory. Otherwise null, and the file
// goes to the front of the decoding queue; it then arrives through
// ge_texture_mods_take_ready(). `rows` of `height` texture rows are covered
// by the picture (272 of 512 for a screen image, else rows == height). A
// picture decoded ahead for other rows than these is decoded again.
[[nodiscard]] GeTextureReplacementPtr ge_texture_mods_find(std::uint64_t key, std::uint32_t rows,
                                                           std::uint32_t height);

// Pictures a draw asked for (ge_texture_mods_find returned null) that have
// been decoded since the last call.
void ge_texture_mods_take_ready(std::vector<GeTextureReplacementReady> &out);

// Queues <key>.png unless this run already wrote it or the file exists.
// `rgba8` is level 0, `width` x `rows`, straight alpha.
void ge_texture_mods_dump(std::uint64_t key, std::uint32_t width, std::uint32_t rows,
                          std::span<const std::byte> rgba8);

// Blocks until the workers have nothing left to do: every picture of the plan
// is decoded and every dump written (tools, tests, and a deterministic run
// before its first frame, so that its frame pictures are the same every time).
void ge_texture_mods_wait_idle();

// The plan of the start, apart from the workers so that it can be checked
// without them. A picture file is its key and the size its PNG header names
// (0 x 0 when the header cannot be read).
struct GeTextureAheadFile {
    std::uint64_t key{};
    std::uint32_t width{}, height{};
};
// A picture with the shape of the game's screen images, k x 512 by k x 272:
// only the 57 standalone boot and loading pictures have it. Such a file is
// decoded first and with the rows a screen image needs.
[[nodiscard]] bool ge_texture_screen_shaped(std::uint32_t width, std::uint32_t height) noexcept;
// Bytes of a decoded picture with its smaller levels (and the rows added
// below a screen-shaped one). 0 for a size that cannot be decoded.
[[nodiscard]] std::uint64_t ge_texture_decoded_bytes(std::uint32_t width, std::uint32_t height) noexcept;
// The textures the game draws from its start to the main menu (the boot screen,
// the first loading screen, the two font sheets, the title, the menus' walls,
// logos and buttons, and what the boot uploads with them), in the order it
// first draws them: 48 keys, taken from texture dumps of the start.
[[nodiscard]] std::span<const std::uint64_t> ge_texture_start_keys() noexcept;
// Puts `files` (given mod folder by mod folder, by file name) into the order
// they are decoded in: the pictures of the start first, in the order of
// ge_texture_start_keys; then the other screen-shaped pictures (the loading
// pictures of the missions, the trophies' pictures: drawn only when a mission
// loads or a trophy is looked at), then the largest first (a large picture is
// the one a draw would wait for longest), pictures of one size as given. Returns how many of them are decoded ahead: up to the first
// whose picture no longer fits into `store_bytes` with those before it. The
// others wait for their first use.
[[nodiscard]] std::size_t ge_texture_ahead_plan(std::vector<GeTextureAheadFile> &files, std::uint64_t store_bytes);

// ---------------------------------------------------------------------------
// Overlays: pictures held in memory and painted over parts of a texture when
// it is uploaded. The on-screen button prompts are made this way
// (host/prompt_style.hpp): the pictures of the keys or pad buttons in use go
// over the PSP button pictures of the game's two font sheets and five menu
// sprites, and change while the game runs.
//
// The host names the textures once (watch) and then sets or clears the
// overlay of each key from any thread. The renderer's thread takes a change
// between two submissions, takes the cached textures with those keys out of
// its cache, and the next draw uploads them again: an overlay's cells are
// painted over the picture in force (a mod's replacement when one is in
// memory, else the game's own level 0) in every level. No frame shows half of
// a change.
struct GeTextureOverlayCell {
    // The part of the texture the picture takes the place of, in texels of
    // the game's own level 0.
    std::uint32_t x{}, y{}, width{}, height{};
    // The picture: straight alpha RGBA8 of any size, and after it
    // `picture_levels` - 1 smaller ones, each half the one before, painted for
    // the smaller levels of the texture. A level of the texture takes the
    // picture that has the cell's size there; without one, the first is fitted.
    std::uint32_t picture_width{}, picture_height{};
    std::uint32_t picture_levels{1u};
    std::vector<std::byte> rgba8;
};
struct GeTextureOverlay {
    // Size of the game's own level 0, which the cells are measured on.
    std::uint32_t width{}, height{};
    // The composed picture is at least this many times that size, so a cell
    // painted for a high rendering resolution keeps its detail.
    std::uint32_t scale{1u};
    std::vector<GeTextureOverlayCell> cells;
};
// A texture that may get an overlay: its content key and what the GE state
// says about it (format, level 0 size).
struct GeTextureOverlayKey {
    std::uint64_t key{};
    std::uint32_t format{};
    std::uint32_t width{}, height{};
};
struct GeTextureOverlaySet {
    std::uint64_t key{};
    std::shared_ptr<const GeTextureOverlay> overlay;   // null: the texture as it is
};

// Names the textures that may get overlays (at most 16) and switches keying on
// for textures of their formats and sizes; with `key_every_texture` for all
// textures, which is only good for measuring what that costs. Call after
// ge_texture_mods_start() and before the first draw; an empty list ends it.
void ge_texture_mods_watch_overlays(std::span<const GeTextureOverlayKey> keys, bool key_every_texture = false);
// Names textures whose content key is wanted although nothing is painted over them: the overlay pass
// (ge_gpu_backend.hpp) finds the textures a host page draws with by key. Textures of these formats and
// level 0 sizes are keyed too, beside the ones watch_overlays names. At most 16; call after
// ge_texture_mods_start() and before the first draw; an empty list ends it.
void ge_texture_mods_watch_keys(std::span<const GeTextureOverlayKey> keys);
// Sets the overlays of these keys in one step. `at_once`: they are in force
// when the call returns, for the start, while nothing is being drawn.
// Otherwise the renderer takes them at its next submission.
void ge_texture_mods_set_overlays(std::span<const GeTextureOverlaySet> entries, bool at_once);
// True when a texture of this format and level 0 size needs its content key:
// always with a texture mod or the dump, otherwise for the watched shapes.
[[nodiscard]] bool ge_texture_mods_keying_for(std::uint32_t format, std::uint32_t width,
                                              std::uint32_t height) noexcept;
// Renderer thread, between two submissions: puts the overlays set since the
// last call in force and appends the keys whose cached textures must go.
// Returns false when nothing changed.
bool ge_texture_mods_take_overlay_changes(std::vector<std::uint64_t> &keys);
[[nodiscard]] bool ge_texture_mods_has_overlay(std::uint64_t key);
// Renderer thread, at upload: the picture to upload for `key`. `base` is the
// mod's replacement or null, `rgba8` the game's level 0 (`width` x `height`).
// Returns `base` unchanged when the key has no overlay in force.
[[nodiscard]] GeTextureReplacementPtr ge_texture_mods_compose_overlay(
    std::uint64_t key, const GeTextureReplacementPtr &base, std::uint32_t width, std::uint32_t height,
    std::span<const std::byte> rgba8);
// The same without the table: `level0` (`width` x `height`, the game's or a
// mod's picture) with the overlay's cells, and its smaller levels.
[[nodiscard]] GeTextureReplacementPtr ge_texture_compose_overlay(
    const GeTextureOverlay &overlay, std::uint32_t width, std::uint32_t height,
    std::span<const std::byte> level0);
void ge_texture_mods_note_overlay_evictions(std::uint64_t textures) noexcept;
// Cost of computing content keys: a tick before, and the note after.
[[nodiscard]] std::uint64_t ge_texture_mods_clock_ns() noexcept;
void ge_texture_mods_note_keyed(std::uint64_t started_ns) noexcept;

[[nodiscard]] GeTextureModsReport ge_texture_mods_report();

// Box-filtered mip chain of an RGBA8 picture, colour weighted by alpha so the
// edge of a cut-out does not darken. Returns the level count; `rgba8` holds
// level 0 on entry and the whole chain on return.
std::uint32_t ge_texture_build_mip_chain(std::uint32_t width, std::uint32_t height,
                                         std::vector<std::byte> &rgba8);

// PNG through Windows Imaging Component. The caller's thread must have COM
// initialised. Pixels are straight-alpha RGBA8.
[[nodiscard]] bool ge_texture_read_png(const std::filesystem::path &path, std::uint32_t &width,
                                       std::uint32_t &height, std::vector<std::byte> &rgba8,
                                       std::string &error);
[[nodiscard]] bool ge_texture_write_png(const std::filesystem::path &path, std::uint32_t width,
                                        std::uint32_t height, std::span<const std::byte> rgba8,
                                        std::string &error);

} // namespace vcs
