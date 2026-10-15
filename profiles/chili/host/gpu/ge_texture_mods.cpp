#include "ge_texture_mods.hpp"
#include "ge_gpu_log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace vcs {

namespace {

using Microsoft::WRL::ComPtr;

// A replacement larger than this in either direction is refused.
constexpr std::uint32_t kMaxReplacementSize = 8192u;
// Decoded replacements kept in system memory for the next upload of the same
// texture (the same art loaded at another address, or after an eviction), when
// the setup names no size.
constexpr std::uint64_t kDefaultStoreBytes = 256ull * 1024ull * 1024ull;
// Threads that decode the pictures of texture mods. Two take four large packs
// (156 pictures, 420 MiB decoded) in 0.6 s, one in 1.1 s; a third
// would gain 0.13 s and take a core from the boot.
constexpr std::uint32_t kDecodeWorkers = 2u;
// The rows of a screen image that hold the picture, of the rows it declares.
constexpr std::uint32_t kScreenRows = 272u, kScreenHeight = 512u;
// Address guard: this many content changes inside the window stop the keying
// of an address until it has been quiet for the hold time. Ticks are presented
// frames (30 or 60 a second).
constexpr std::uint32_t kGuardChanges = 4u;
constexpr std::uint64_t kGuardWindow = 60u;
constexpr std::uint64_t kGuardHold = 600u;

// The textures the game draws from its start to the main menu, in the order it first draws them: the 47
// textures before Story 1-1's loading picture in a texture dump of replays\mission1_baseline.replay (boot,
// "Loading" screen, Load profile, the notice, the title, the main menu, Story), then the one more the other
// menus of the main menu draw (dump of the menu tour). A pack's picture for one of them is
// decoded before every other picture (ge_texture_ahead_plan): the first menu screen is drawn about four
// seconds after the start and the "Loading" line of the first loading screen 0.7 to 1.4 seconds after it,
// while the other pictures are needed only when a mission loads, a trophy is looked at, and so on. The keys
// are the game's own content (mods/TEXTURES.md, "The key"): they are the same in every run.
constexpr std::uint64_t kStartKeys[] = {
    0x724ca7d1300aa765ull,   // menu/loadScreens/bootscreen (512x272)
    0x5347885ba272bd5aull,   // an 8x8 white square the game makes itself
    0xdf7825a8b394fb75ull,   // effects/Blood/BloodStainBigSplat
    0x5bc4636e3ec3c6d2ull,   // Effects/Blood/BloodStain
    0x7b8b27754774084bull,   // effects/Blood/BloodSpray02
    0x9fe83dae778158c8ull,   // effects/Blood/BloodWallDripping
    0x8153d070034fc398ull,   // effects/Blood/BloodSplatter03
    0xfd5249568bc295c4ull,   // effects/Blood/BloodSplatter02
    0x240434a8b31619d8ull,   // effects/Blood/BloodSplatter01
    0x60f450b35c30c905ull,   // effects/BulletHoles/BulletHole_Shotgun04
    0x311f9c3f6dcdd3ccull,   // BulletHole_Shotgun03
    0xaf6c069e2d55af52ull,   // BulletHole_Shotgun02
    0xc42f6b368e6b019aull,   // BulletHole_Shotgun01
    0xe8cf98fea772d315ull,   // BulletHole_Metal04
    0xbc658983b98743a4ull,   // BulletHole_Metal03
    0x2a03c1a0fa82aca8ull,   // BulletHole_Metal02
    0x71a3bae8f6a1e6bcull,   // BulletHole_Metal01
    0x134c5b675dfadee0ull,   // BulletHole_Small04
    0x96371b2b13644320ull,   // BulletHole_Small03
    0x31ebc177e0f4d642ull,   // BulletHole_Small02
    0x460f85e7785c39dbull,   // BulletHole_Small01
    0x1461a3a337108aa8ull,   // BulletHole_Glass
    0x4a3efb58549fd6b6ull,   // effects/FireSmokeExplosions/Eksplosion_Decal
    0x4e7bb03efda51547ull,   // Sprites/CenterDot
    0xfc653389140c1e09ull,   // Hud/HUD_PSP/HealthbarRight
    0x212d2a1a5fd6b847ull,   // Hud/HUD_PSP/HealthbarLeft
    0x6aae00bba69bfb91ull,   // Hud/HUD_PSP/HealthbarMid
    0x5d0c6d1a663a8515ull,   // Fonts/ODMidText_24.font: the text font's sheet
    0xd2073badafb2a5d3ull,   // Hud/HUD_PSP/CrossHealth
    0x506188e7b1e166a8ull,   // Hud/HUD_PSP/HealthbarMidWhite
    0xe4703857f7c7e1d5ull,   // Hud/HUD_PSP/RewindBlack
    0x5aea4cc8e88f4649ull,   // Hud/HUD_PSP/HealthbarRightWhite
    0x4a8db24110b7592bull,   // Hud/HUD_PSP/HealthbarLeftWhite
    0x3df1d52e86d67e3full,   // Menu/LoadScreens/LoadBar
    0xee34d1637a740122ull,   // generic/white
    0x21c797cd712c11d9ull,   // menu/loadscreens/loadscreengame (512x272): the first loading screen
    0xa247e7634946f676ull,   // Menu/Backgrounds/BackGround03: the menus' wall
    0x854c5701a58fb9c3ull,   // Menu/GameLogo
    0xebff042e1d0cf176ull,   // Menu/ButtonCircle
    0x002d10b31e312e9dull,   // Fonts/ODMidText_24_Stroke3.font: the outline font's sheet
    0xdc8cc6b510da1d80ull,   // Menu/ButtonCross
    0xc05c9810670e40bcull,   // Hud/HUD_PSP/BackGroundRightCorner
    0xe594daa6d122fe70ull,   // Hud/HUD_PSP/GrenadeLauncher
    0xe1215277c6aafa16ull,   // Menu/Backgrounds/BackGround01: the title's and the notices' wall
    0x048e4eecf23e28a1ull,   // Menu/GameLogo_FrontScreen: the title's logo
    0xe6fd3c31244ec517ull,   // Menu/ButtonSquare
    0x68b9cff5c3f6a8d4ull,   // Menu/BGCharacterRam: the main menu's character
    0xe8cc8b252a16c095ull,   // Menu/ButtonDpadLR
};
constexpr std::size_t kStartCount = sizeof(kStartKeys) / sizeof(kStartKeys[0]);

// Place of `key` in kStartKeys, kStartCount for any other texture.
std::size_t start_rank(std::uint64_t key) noexcept {
    for (std::size_t i = 0u; i < kStartCount; ++i)
        if (kStartKeys[i] == key) return i;
    return kStartCount;
}

struct Job {
    // Plan: read the size of every picture file and queue the decoding ahead.
    enum class Kind { Decode, Dump, Plan } kind{Kind::Decode};
    std::uint64_t key{};
    std::filesystem::path path;
    std::uint32_t width{};
    std::uint32_t rows{};
    std::uint32_t height{};
    std::vector<std::byte> rgba8;
    // Queued by the plan, before any draw asked for the picture.
    bool planned{};
    // A picture of the start (kStartKeys): the game draws it within its first second or so.
    bool start{};
};

struct Guard {
    std::uint64_t last_key{};
    std::uint64_t window_start{};
    std::uint64_t blocked_until{};
    std::uint32_t changes{};
    bool reported{};
};

struct Cached {
    GeTextureReplacementPtr picture;
    std::list<std::uint64_t>::iterator position;
};

// The picture a texture's cells are painted into, kept from one change of the cells to the next: the
// picture in force at the composed size with all its levels. Painting the cells is then a copy and a few
// small rectangles instead of enlarging a sheet and filtering all its levels.
struct OverlayBase {
    bool mod{};                    // made from a mod's replacement, not from the game's picture
    std::uint32_t source_width{}, source_height{};
    std::uint32_t width{}, height{};
    std::vector<std::byte> rgba8;
};

struct Mods {
    std::atomic<bool> keying{false};
    std::atomic<bool> dumping{false};
    std::atomic<bool> ready_flag{false};
    // Immutable between start and stop.
    std::unordered_map<std::uint64_t, std::filesystem::path> files;
    std::vector<std::uint64_t> load_order;               // their keys, mod folder by mod folder, by file name
    std::filesystem::path dump_folder;
    std::vector<std::thread> workers;

    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    // A picture a draw waits for goes to the front; the plan's pictures and dumps to the back.
    std::deque<Job> jobs;
    std::uint32_t working{};                             // workers with a job in hand
    bool quit{};
    std::vector<GeTextureReplacementReady> ready;
    // Keys a draw asked for whose picture is not decoded yet, with the rows and height of the texture.
    std::unordered_map<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>> wanted;
    std::unordered_set<std::uint64_t> used;              // keys whose picture reached the renderer
    std::uint64_t planned_left{};                        // jobs of the plan not finished
    std::unordered_set<std::uint64_t> start_left;        // pictures of the start (kStartKeys) in the plan not decoded yet
    std::uint64_t others_done{};                         // other pictures decoded so far
    std::uint64_t started_ns{};
    std::list<std::uint64_t> order;                      // most recently used first
    std::unordered_map<std::uint64_t, Cached> decoded;
    std::uint64_t decoded_bytes{};
    std::uint64_t store_bytes{kDefaultStoreBytes};       // set at start
    std::unordered_set<std::uint64_t> in_flight;
    std::unordered_set<std::uint64_t> unusable;
    std::unordered_set<std::uint64_t> dumped;
    GeTextureModsReport report;

    // Thread that executes GE lists only.
    std::unordered_map<std::uint32_t, Guard> guards;

    // Overlays. The watched keys are written before the first draw and then
    // only read; the two tables are under `mutex`.
    std::atomic<bool> overlay_watch{false};
    std::atomic<bool> overlay_key_all{false};
    std::atomic<bool> overlay_changed{false};
    std::array<GeTextureOverlayKey, 16> overlay_keys{};
    std::atomic<std::uint32_t> overlay_key_count{0u};
    std::unordered_map<std::uint64_t, std::shared_ptr<const GeTextureOverlay>> overlays;   // in force for the renderer
    std::vector<GeTextureOverlaySet> overlay_pending;                                      // set since it last looked
    std::unordered_map<std::uint64_t, OverlayBase> overlay_bases;                          // renderer thread only
    // Shapes keyed for the overlay pass (ge_texture_mods_watch_keys): written before the first draw.
    std::array<GeTextureOverlayKey, 16> page_keys{};
    std::atomic<std::uint32_t> page_key_count{0u};
    std::atomic<std::uint64_t> keyed_textures{0u}, keying_ns{0u}, keying_max_ns{0u};

    // A process that leaves without ge_texture_mods_stop() must not take a
    // running worker down with it.
    ~Mods() {
        if (workers.empty()) return;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            quit = true;
        }
        wake.notify_all();
        for (std::thread &worker : workers) worker.join();
    }
};

Mods &mods() {
    static Mods instance;
    return instance;
}

std::string key_text(std::uint64_t key) {
    static const char digits[] = "0123456789abcdef";
    std::string text(16u, '0');
    for (int i = 15; i >= 0; --i) {
        text[static_cast<std::size_t>(i)] = digits[key & 15u];
        key >>= 4u;
    }
    return text;
}

// "<16 hex digits>.png", either case.
bool parse_key_name(const std::filesystem::path &path, std::uint64_t &key) {
    std::wstring extension = path.extension().wstring();
    for (auto &c : extension) if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    if (extension != L".png") return false;
    const std::wstring stem = path.stem().wstring();
    if (stem.size() != 16u) return false;
    key = 0u;
    for (const wchar_t c : stem) {
        std::uint64_t digit = 0u;
        if (c >= L'0' && c <= L'9') digit = static_cast<std::uint64_t>(c - L'0');
        else if (c >= L'a' && c <= L'f') digit = static_cast<std::uint64_t>(c - L'a') + 10u;
        else if (c >= L'A' && c <= L'F') digit = static_cast<std::uint64_t>(c - L'A') + 10u;
        else return false;
        key = (key << 4u) | digit;
    }
    return true;
}

std::string hr_text(const char *what, HRESULT hr) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%s (HRESULT 0x%08lX)", what, static_cast<unsigned long>(hr));
    return buffer;
}

bool make_factory(ComPtr<IWICImagingFactory> &factory, std::string &error) {
    const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { error = hr_text("Windows Imaging Component is not available", hr); return false; }
    return true;
}

// Bytes of a picture with every smaller level down to 1x1.
std::uint64_t chain_bytes(std::uint32_t width, std::uint32_t height) noexcept {
    std::uint64_t total = 0u;
    for (std::uint32_t w = width, h = height;; w = std::max(1u, w >> 1u), h = std::max(1u, h >> 1u)) {
        total += static_cast<std::uint64_t>(w) * h * 4u;
        if (w <= 1u && h <= 1u) break;
    }
    return total;
}

bool read_png(IWICImagingFactory *factory, const std::filesystem::path &path, std::uint32_t &width,
              std::uint32_t &height, std::vector<std::byte> &rgba8, std::string &error) {
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                    WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) { error = hr_text("cannot open the picture", hr); return false; }
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0u, &frame);
    if (FAILED(hr)) { error = hr_text("cannot read the picture", hr); return false; }
    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr))
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                   nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) { error = hr_text("cannot convert the picture to RGBA", hr); return false; }
    UINT w = 0u, h = 0u;
    hr = converter->GetSize(&w, &h);
    if (FAILED(hr) || w == 0u || h == 0u) { error = "the picture has no size"; return false; }
    if (w > kMaxReplacementSize || h > kMaxReplacementSize) {
        error = "the picture is larger than " + std::to_string(kMaxReplacementSize) + " pixels";
        return false;
    }
    try {
        // Room for the smaller levels at once: they are added to this buffer.
        rgba8.reserve(static_cast<std::size_t>(chain_bytes(w, h)));
        rgba8.resize(static_cast<std::size_t>(w) * h * 4u);
    } catch (...) { error = "out of memory"; return false; }
    hr = converter->CopyPixels(nullptr, w * 4u, static_cast<UINT>(rgba8.size()),
                               reinterpret_cast<BYTE *>(rgba8.data()));
    if (FAILED(hr)) { error = hr_text("cannot decode the picture", hr); return false; }
    width = w;
    height = h;
    return true;
}

bool write_png(IWICImagingFactory *factory, const std::filesystem::path &path, std::uint32_t width,
               std::uint32_t height, std::span<const std::byte> rgba8, std::string &error) {
    if (width == 0u || height == 0u || rgba8.size() != static_cast<std::size_t>(width) * height * 4u) {
        error = "bad picture size";
        return false;
    }
    // The PNG encoder takes blue first.
    std::vector<BYTE> bgra(rgba8.size());
    for (std::size_t at = 0u; at < rgba8.size(); at += 4u) {
        bgra[at] = static_cast<BYTE>(rgba8[at + 2u]);
        bgra[at + 1u] = static_cast<BYTE>(rgba8[at + 1u]);
        bgra[at + 2u] = static_cast<BYTE>(rgba8[at]);
        bgra[at + 3u] = static_cast<BYTE>(rgba8[at + 3u]);
    }
    ComPtr<IWICStream> stream;
    HRESULT hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) { error = hr_text("cannot create the file", hr); return false; }
    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame;
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr) && !IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) hr = E_FAIL;
    if (SUCCEEDED(hr))
        hr = frame->WritePixels(height, width * 4u, static_cast<UINT>(bgra.size()), bgra.data());
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    if (FAILED(hr)) { error = hr_text("cannot write the PNG", hr); return false; }
    return true;
}

std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The height a picture of `picture_height` rows gets when it covers `rows` of
// `height` texture rows (pad_rows below): its own when it covers them all.
std::uint32_t padded_height(std::uint32_t picture_height, std::uint32_t rows, std::uint32_t height) noexcept {
    if (rows == 0u || rows >= height) return picture_height;
    const std::uint64_t full = (static_cast<std::uint64_t>(picture_height) * height + rows - 1u) / rows;
    return full <= picture_height || full > kMaxReplacementSize ? picture_height : static_cast<std::uint32_t>(full);
}

// The picture of a screen image covers `rows` of `height` texture rows: put it
// at the top of a taller canvas so the texture coordinates still fit. The row
// below the picture repeats its last row, so filtering does not pull in black.
void pad_rows(std::uint32_t width, std::uint32_t &picture_height, std::uint32_t rows,
              std::uint32_t height, std::vector<std::byte> &rgba8) {
    const std::uint32_t full = padded_height(picture_height, rows, height);
    if (full == picture_height) return;
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4u;
    rgba8.resize(row_bytes * static_cast<std::size_t>(full));
    std::memcpy(rgba8.data() + row_bytes * picture_height,
                rgba8.data() + row_bytes * (picture_height - 1u), row_bytes);
    picture_height = full;
}

// Width and height from a PNG file's header, without decoding it.
bool read_png_size(const std::filesystem::path &path, std::uint32_t &width, std::uint32_t &height) {
    std::array<unsigned char, 24> head{};
    std::ifstream stream(path, std::ios::binary);
    if (!stream.read(reinterpret_cast<char *>(head.data()), static_cast<std::streamsize>(head.size()))) return false;
    static constexpr unsigned char signature[8]{0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    if (std::memcmp(head.data(), signature, 8u) != 0 || std::memcmp(head.data() + 12u, "IHDR", 4u) != 0) return false;
    const auto word = [&](std::size_t at) {
        return (static_cast<std::uint32_t>(head[at]) << 24u) | (static_cast<std::uint32_t>(head[at + 1u]) << 16u) |
               (static_cast<std::uint32_t>(head[at + 2u]) << 8u) | static_cast<std::uint32_t>(head[at + 3u]);
    };
    width = word(16u);
    height = word(20u);
    return width != 0u && height != 0u;
}

void finish_job(Mods &m, Job &job, IWICImagingFactory *factory) {
    std::string error;
    if (job.kind == Job::Kind::Dump) {
        std::error_code status;
        bool written = false;
        if (!std::filesystem::exists(job.path, status)) {
            // Written under another name first: a run that is stopped half-way
            // leaves no broken <key>.png behind.
            std::filesystem::path partial = job.path;
            partial += L".part";
            if (factory != nullptr && write_png(factory, partial, job.width, job.rows, job.rgba8, error)) {
                std::filesystem::rename(partial, job.path, status);
                written = !status;
                if (status) error = "cannot rename " + partial.string();
            }
            if (!written) {
                std::filesystem::remove(partial, status);
                runtime_log_error("texture dump", job.path.filename().string() + ": " +
                                                      (error.empty() ? "no image codec" : error));
            }
        }
        const std::lock_guard<std::mutex> lock(m.mutex);
        if (written) ++m.report.dumped;
        return;
    }

    auto picture = std::make_shared<GeTextureReplacement>();
    bool usable = false;
    if (factory == nullptr) error = "no image codec";
    else if (read_png(factory, job.path, picture->width, picture->height, picture->rgba8, error)) {
        try {
            picture->source_height = picture->height;
            pad_rows(picture->width, picture->height, job.rows, job.height, picture->rgba8);
            picture->mip_levels = ge_texture_build_mip_chain(picture->width, picture->height, picture->rgba8);
            picture->mod_picture = true;
            usable = true;
        } catch (...) { error = "out of memory"; }
    }
    if (!usable)
        runtime_log_error("texture replacement", job.path.string() + ": " + error);
    {
        const std::lock_guard<std::mutex> lock(m.mutex);
        if (job.planned && --m.planned_left == 0u) m.report.decoded_ahead_ms = (now_ns() - m.started_ns) / 1000000u;
        // A draw asked for the picture while it was queued or being decoded: it goes to the renderer.
        const auto wanted = m.wanted.find(job.key);
        const bool asked = wanted != m.wanted.end();
        if (usable && asked &&
            picture->height != padded_height(picture->source_height, wanted->second.first, wanted->second.second)) {
            // Decoded for the rows the file's shape suggested; the texture that draws it has others.
            Job again;
            again.key = job.key;
            again.path = std::move(job.path);
            again.rows = wanted->second.first;
            again.height = wanted->second.second;
            m.jobs.push_front(std::move(again));
            ++m.report.requeued;
        } else {
            m.in_flight.erase(job.key);
            if (asked) m.wanted.erase(wanted);
            if (m.start_left.erase(job.key) != 0u) {
                if (m.start_left.empty()) {
                    m.report.start_decoded_ms = (now_ns() - m.started_ns) / 1000000u;
                    m.report.start_others_before = m.others_done;
                }
            } else if (start_rank(job.key) == kStartCount) {
                ++m.others_done;
            }
            if (usable && asked && start_rank(job.key) < kStartCount) ++m.report.start_late;
            if (!usable) {
                m.unusable.insert(job.key);
                ++m.report.failed;
                if (asked) m.ready.push_back({job.key, nullptr});
            } else {
                const std::uint64_t bytes = picture->rgba8.size();
                if (const auto old = m.decoded.find(job.key); old != m.decoded.end()) {
                    m.decoded_bytes -= old->second.picture->rgba8.size();
                    m.order.erase(old->second.position);
                    m.decoded.erase(old);
                }
                m.order.push_front(job.key);
                m.decoded.emplace(job.key, Cached{picture, m.order.begin()});
                m.decoded_bytes += bytes;
                // Dropping the oldest only lets go of this module's reference; a picture
                // still waiting for its upload stays alive through the renderer's.
                while (m.decoded_bytes > m.store_bytes && m.order.size() > 1u) {
                    const std::uint64_t victim = m.order.back();
                    const auto found = m.decoded.find(victim);
                    m.decoded_bytes -= found->second.picture->rgba8.size();
                    m.decoded.erase(found);
                    m.order.pop_back();
                }
                if (asked) {
                    ++m.report.late;
                    if (m.used.insert(job.key).second) ++m.report.replaced;
                    m.ready.push_back({job.key, std::move(picture)});
                } else {
                    ++m.report.decoded_ahead;
                    m.report.decoded_ahead_bytes += bytes;
                }
            }
            if (asked) m.ready_flag.store(true, std::memory_order_release);
        }
    }
    m.wake.notify_one();
}

// The plan of the start: every picture file's size from its header, the order
// and the end of ge_texture_ahead_plan, and a job for each picture inside it.
// The pictures of the start (kStartKeys) go first, in the order the game draws
// them, then the others, the largest first because they are the ones whose
// decoding a draw would wait for longest (a 2048x4096 sheet 85 ms, a 512x512
// icon 7). In file order (the HUD's 86 small pictures first) the font sheets
// came last, 0.6 s after the start on a quiet PC and 1.8 s on a busy one, and
// the first text of the boot (1.3 s after the start) was drawn with the game's
// own letters; with 27 loading pictures of 2048x1088 decoded first because of
// their shape (the order before this plan) the same text was late in every real-time run.
void plan_ahead(Mods &m) {
    std::vector<GeTextureAheadFile> files;
    files.reserve(m.load_order.size());
    for (const std::uint64_t key : m.load_order) {
        GeTextureAheadFile file;
        file.key = key;
        if (!read_png_size(m.files.find(key)->second, file.width, file.height)) file.width = file.height = 0u;
        files.push_back(file);
    }
    const std::size_t ahead = ge_texture_ahead_plan(files, m.store_bytes);
    {
        const std::lock_guard<std::mutex> lock(m.mutex);
        m.report.waiting_first_use = files.size() - ahead;
        for (std::size_t i = 0u; i < ahead; ++i) {
            const GeTextureAheadFile &file = files[i];
            if (start_rank(file.key) < kStartCount) {
                ++m.report.start_files;
                // Counted until it is decoded, also when a draw has asked for it already (its job is on the way).
                if (m.decoded.count(file.key) == 0u && m.unusable.count(file.key) == 0u) m.start_left.insert(file.key);
            }
            // A draw that was quicker than the plan has queued the picture itself, or has it already.
            if (m.decoded.count(file.key) != 0u || m.unusable.count(file.key) != 0u ||
                !m.in_flight.insert(file.key).second) continue;
            Job job;
            job.key = file.key;
            job.path = m.files.find(file.key)->second;
            job.planned = true;
            job.start = start_rank(file.key) < kStartCount;
            if (ge_texture_screen_shaped(file.width, file.height)) {
                job.rows = kScreenRows;
                job.height = kScreenHeight;
            }
            m.jobs.push_back(std::move(job));
            ++m.planned_left;
        }
        if (m.report.start_files != 0u && m.start_left.empty())
            m.report.start_decoded_ms = (now_ns() - m.started_ns) / 1000000u;
    }
    m.wake.notify_all();
}

void worker_main() {
    Mods &m = mods();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> factory;
    std::string error;
    if (!make_factory(factory, error)) runtime_log_error("texture mods", error);
    bool in_hand = false;
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m.mutex);
            if (in_hand) --m.working;
            in_hand = false;
            if (m.jobs.empty() && m.working == 0u) m.idle.notify_all();
            m.wake.wait(lock, [&m] { return m.quit || !m.jobs.empty(); });
            if (m.quit) break;
            job = std::move(m.jobs.front());
            m.jobs.pop_front();
            ++m.working;
            in_hand = true;
            // Decoding ahead gives way to the game's own threads. While a draw waits for a picture
            // nothing does: ge_texture_mods_find raises every worker, under this lock. Nor do the plan
            // (a few milliseconds) and the pictures of the start, which the game draws within its first
            // second: on a PC whose other programs kept every core busy, a worker below normal priority
            // got the processor only every few seconds (Windows lifts a starved thread about every
            // 4 s), the start's pictures were all there 3.6 s after the start and one was drawn late.
            const bool ahead = job.kind == Job::Kind::Decode && job.planned && !job.start;
            SetThreadPriority(GetCurrentThread(), ahead && m.wanted.empty() ? THREAD_PRIORITY_BELOW_NORMAL
                                                                            : THREAD_PRIORITY_NORMAL);
        }
        if (job.kind == Job::Kind::Plan) plan_ahead(m);
        else finish_job(m, job, factory.Get());
    }
    factory.Reset();
    if (SUCCEEDED(com)) CoUninitialize();
}

} // namespace

void ge_texture_mods_start(const GeTextureModsSetup &setup) {
    ge_texture_mods_stop();
    Mods &m = mods();
    std::error_code status;
    for (const auto &folder : setup.replacement_folders) {
        if (!std::filesystem::is_directory(folder, status)) continue;
        // By name within a folder: pictures of one size are decoded in this order (plan_ahead).
        std::vector<std::filesystem::path> pictures;
        for (const auto &entry : std::filesystem::directory_iterator(folder, status))
            if (entry.is_regular_file(status)) pictures.push_back(entry.path());
        std::sort(pictures.begin(), pictures.end());
        for (const auto &path : pictures) {
            std::uint64_t key = 0u;
            if (parse_key_name(path, key)) {
                // A later folder wins, and the picture takes its place in that folder's turn.
                if (!m.files.insert_or_assign(key, path).second) std::erase(m.load_order, key);
                m.load_order.push_back(key);
            } else if (path.extension() == L".png" || path.extension() == L".PNG")
                runtime_log_error("texture replacement",
                                  path.string() + ": the name is not a 16-digit key (mods/TEXTURES.md)");
        }
    }
    m.report = {};
    m.store_bytes = setup.store_bytes != 0u ? setup.store_bytes : kDefaultStoreBytes;
    m.keyed_textures.store(0u, std::memory_order_relaxed);
    m.keying_ns.store(0u, std::memory_order_relaxed);
    m.keying_max_ns.store(0u, std::memory_order_relaxed);
    m.report.replacement_files = m.files.size();
    if (!setup.dump_folder.empty()) {
        std::filesystem::create_directories(setup.dump_folder, status);
        if (std::filesystem::is_directory(setup.dump_folder, status)) {
            m.dump_folder = setup.dump_folder;
            for (const auto &entry : std::filesystem::directory_iterator(m.dump_folder, status)) {
                std::uint64_t key = 0u;
                if (entry.is_regular_file(status) && parse_key_name(entry.path(), key)) m.dumped.insert(key);
            }
        } else {
            runtime_log_error("texture dump", "cannot create " + setup.dump_folder.string());
        }
    }
    if (m.files.empty() && m.dump_folder.empty()) return;
    m.quit = false;
    // With pictures to decode: the plan first, on two workers. The dump alone keeps its one worker.
    m.started_ns = now_ns();
    if (!m.files.empty()) {
        Job plan;
        plan.kind = Job::Kind::Plan;
        m.jobs.push_back(std::move(plan));
    }
    m.report.workers = m.files.empty() ? 1u : kDecodeWorkers;
    for (std::uint32_t i = 0u; i < m.report.workers; ++i) m.workers.emplace_back(worker_main);
    m.dumping.store(!m.dump_folder.empty(), std::memory_order_release);
    m.keying.store(true, std::memory_order_release);
}

void ge_texture_mods_stop() noexcept {
    Mods &m = mods();
    m.keying.store(false, std::memory_order_release);
    m.dumping.store(false, std::memory_order_release);
    if (!m.workers.empty()) {
        {
            // Pictures nobody will draw any more are not decoded; dumps still
            // queued are written before the workers leave.
            std::unique_lock<std::mutex> lock(m.mutex);
            std::erase_if(m.jobs, [](const Job &job) { return job.kind != Job::Kind::Dump; });
            m.idle.wait(lock, [&m] { return m.jobs.empty() && m.working == 0u; });
            m.quit = true;
        }
        m.wake.notify_all();
        for (std::thread &worker : m.workers) worker.join();
        m.workers.clear();
    }
    const std::lock_guard<std::mutex> lock(m.mutex);
    m.files.clear();
    m.load_order.clear();
    m.wanted.clear();
    m.used.clear();
    m.planned_left = 0u;
    m.start_left.clear();
    m.others_done = 0u;
    m.working = 0u;
    m.dump_folder.clear();
    m.jobs.clear();
    m.ready.clear();
    m.order.clear();
    m.decoded.clear();
    m.decoded_bytes = 0u;
    m.in_flight.clear();
    m.unusable.clear();
    m.dumped.clear();
    m.guards.clear();
    m.ready_flag.store(false, std::memory_order_release);
    m.overlay_watch.store(false, std::memory_order_release);
    m.overlay_key_all.store(false, std::memory_order_release);
    m.overlay_changed.store(false, std::memory_order_release);
    m.overlay_key_count.store(0u, std::memory_order_release);
    m.page_key_count.store(0u, std::memory_order_release);
    m.overlays.clear();
    m.overlay_pending.clear();
    m.overlay_bases.clear();
}

bool ge_texture_mods_keying() noexcept { return mods().keying.load(std::memory_order_relaxed); }
bool ge_texture_mods_dumping() noexcept { return mods().dumping.load(std::memory_order_relaxed); }

bool ge_texture_mods_admit(std::uint32_t address, std::uint64_t key, std::uint64_t tick) {
    Mods &m = mods();
    Guard &guard = m.guards[address];
    if (guard.last_key == 0u) {
        guard.last_key = key;
        guard.window_start = tick;
        return true;
    }
    if (guard.last_key != key) {
        guard.last_key = key;
        if (tick - guard.window_start > kGuardWindow) {
            guard.window_start = tick;
            guard.changes = 0u;
        }
        if (++guard.changes >= kGuardChanges) {
            guard.blocked_until = tick + kGuardHold;
            if (!guard.reported) {
                guard.reported = true;
                char text[96];
                std::snprintf(text, sizeof(text),
                              "the texture at 0x%08X keeps changing: not dumped, not replaced", address);
                runtime_log_error("texture mods", text);
                const std::lock_guard<std::mutex> lock(m.mutex);
                ++m.report.unstable_addresses;
            }
        }
    }
    return tick >= guard.blocked_until;
}

bool ge_texture_mods_has(std::uint64_t key) noexcept {
    const Mods &m = mods();
    return m.files.find(key) != m.files.end();
}

GeTextureReplacementPtr ge_texture_mods_find(std::uint64_t key, std::uint32_t rows, std::uint32_t height) {
    Mods &m = mods();
    const auto file = m.files.find(key);
    if (file == m.files.end()) return nullptr;
    {
        const std::lock_guard<std::mutex> lock(m.mutex);
        if (const auto found = m.decoded.find(key); found != m.decoded.end()) {
            const GeTextureReplacementPtr picture = found->second.picture;
            if (picture->height == padded_height(picture->source_height, rows, height)) {
                m.order.splice(m.order.begin(), m.order, found->second.position);
                if (m.used.insert(key).second) ++m.report.replaced;
                return picture;
            }
            // Decoded ahead for the rows the file's shape suggested; this texture has others: once more.
            m.decoded_bytes -= picture->rgba8.size();
            m.order.erase(found->second.position);
            m.decoded.erase(found);
            ++m.report.requeued;
        }
        if (m.unusable.count(key) != 0u) return nullptr;
        // Asked for before (the same picture at another address): it is on its way.
        if (!m.wanted.emplace(key, std::pair{rows, height}).second) return nullptr;
        if (m.in_flight.insert(key).second) {
            Job job;
            job.key = key;
            job.path = file->second;
            job.rows = rows;
            job.height = height;
            m.jobs.push_front(std::move(job));
        } else {
            // Queued by the plan and not begun: to the front, with the texture's own rows. (One a
            // worker has in hand is checked against these rows when it is done.)
            for (auto it = m.jobs.begin(); it != m.jobs.end(); ++it) {
                if (it->kind != Job::Kind::Decode || it->key != key) continue;
                Job job = std::move(*it);
                m.jobs.erase(it);
                job.rows = rows;
                job.height = height;
                m.jobs.push_front(std::move(job));
                break;
            }
        }
        // The game's own picture is on the screen until this one is decoded.
        for (std::thread &worker : m.workers) SetThreadPriority(worker.native_handle(), THREAD_PRIORITY_NORMAL);
    }
    m.wake.notify_one();
    return nullptr;
}

void ge_texture_mods_take_ready(std::vector<GeTextureReplacementReady> &out) {
    Mods &m = mods();
    if (!m.ready_flag.load(std::memory_order_acquire)) return;
    const std::lock_guard<std::mutex> lock(m.mutex);
    out.insert(out.end(), std::make_move_iterator(m.ready.begin()), std::make_move_iterator(m.ready.end()));
    m.ready.clear();
    m.ready_flag.store(false, std::memory_order_release);
}

void ge_texture_mods_dump(std::uint64_t key, std::uint32_t width, std::uint32_t rows,
                          std::span<const std::byte> rgba8) {
    Mods &m = mods();
    if (!m.dumping.load(std::memory_order_relaxed) || width == 0u || rows == 0u ||
        rgba8.size() < static_cast<std::size_t>(width) * rows * 4u) return;
    {
        const std::lock_guard<std::mutex> lock(m.mutex);
        ++m.report.keyed;
        if (!m.dumped.insert(key).second) return;
        Job job;
        job.kind = Job::Kind::Dump;
        job.key = key;
        job.path = m.dump_folder / (key_text(key) + ".png");
        job.width = width;
        job.rows = rows;
        job.rgba8.assign(rgba8.begin(), rgba8.begin() + static_cast<std::ptrdiff_t>(
                                                            static_cast<std::size_t>(width) * rows * 4u));
        m.jobs.push_back(std::move(job));
    }
    m.wake.notify_one();
}

void ge_texture_mods_wait_idle() {
    Mods &m = mods();
    if (m.workers.empty()) return;
    std::unique_lock<std::mutex> lock(m.mutex);
    m.idle.wait(lock, [&m] { return m.jobs.empty() && m.working == 0u; });
}

bool ge_texture_screen_shaped(std::uint32_t width, std::uint32_t height) noexcept {
    return width != 0u && width % kScreenHeight == 0u && height % kScreenRows == 0u &&
           width / kScreenHeight == height / kScreenRows;
}

std::uint64_t ge_texture_decoded_bytes(std::uint32_t width, std::uint32_t height) noexcept {
    if (width == 0u || height == 0u || width > kMaxReplacementSize || height > kMaxReplacementSize) return 0u;
    return chain_bytes(width, ge_texture_screen_shaped(width, height) ? padded_height(height, kScreenRows, kScreenHeight)
                                                                      : height);
}

std::span<const std::uint64_t> ge_texture_start_keys() noexcept { return {kStartKeys, kStartCount}; }

std::size_t ge_texture_ahead_plan(std::vector<GeTextureAheadFile> &files, std::uint64_t store_bytes) {
    std::stable_sort(files.begin(), files.end(), [](const GeTextureAheadFile &a, const GeTextureAheadFile &b) {
        // The pictures of the start first, in the order the game draws them; every other picture has the same rank.
        const std::size_t rank_a = start_rank(a.key), rank_b = start_rank(b.key);
        if (rank_a != rank_b) return rank_a < rank_b;
        const bool screen_a = ge_texture_screen_shaped(a.width, a.height);
        const bool screen_b = ge_texture_screen_shaped(b.width, b.height);
        // Then the other screen pictures (a mission's loading picture is drawn before the mission's own
        // pictures), then the largest first.
        if (screen_a != screen_b) return screen_a;
        return ge_texture_decoded_bytes(a.width, a.height) > ge_texture_decoded_bytes(b.width, b.height);
    });
    std::uint64_t bytes = 0u;
    std::size_t ahead = 0u;
    for (; ahead < files.size(); ++ahead) {
        bytes += ge_texture_decoded_bytes(files[ahead].width, files[ahead].height);
        if (bytes > store_bytes) break;
    }
    return ahead;
}

GeTextureModsReport ge_texture_mods_report() {
    Mods &m = mods();
    const std::lock_guard<std::mutex> lock(m.mutex);
    GeTextureModsReport report = m.report;
    report.store_bytes = m.store_bytes;
    report.kept_bytes = m.decoded_bytes;
    report.keyed_textures = m.keyed_textures.load(std::memory_order_relaxed);
    report.keying_ns = m.keying_ns.load(std::memory_order_relaxed);
    report.keying_max_ns = m.keying_max_ns.load(std::memory_order_relaxed);
    return report;
}

// ---------------------------------------------------------------------------
// Overlays
// ---------------------------------------------------------------------------

namespace {

// `source` enlarged to `dw` x `dh` the way the graphics card enlarges a
// texture it samples with linear filtering: all four channels alike, texel
// centres half a texel in, the border repeated. A picture enlarged like this
// and then sampled looks as the small one did.
void enlarge_linear(const std::uint8_t *source, std::uint32_t sw, std::uint32_t sh, std::uint8_t *dest,
                    std::uint32_t dw, std::uint32_t dh) {
    if (sw == dw && sh == dh) {
        std::memcpy(dest, source, static_cast<std::size_t>(sw) * sh * 4u);
        return;
    }
    // Per destination column: the source column to its left and the weight of the next one, of 256.
    std::vector<std::uint32_t> column(dw);
    std::vector<std::uint32_t> weight(dw);
    for (std::uint32_t x = 0u; x < dw; ++x) {
        float at = (static_cast<float>(x) + 0.5f) * static_cast<float>(sw) / static_cast<float>(dw) - 0.5f;
        at = std::clamp(at, 0.0f, static_cast<float>(sw - 1u));
        const auto left = std::min(static_cast<std::uint32_t>(at), sw - 1u);
        column[x] = left;
        weight[x] = left + 1u < sw ? static_cast<std::uint32_t>((at - static_cast<float>(left)) * 256.0f + 0.5f) : 0u;
    }
    const std::size_t row_bytes = static_cast<std::size_t>(dw) * 4u;
    std::vector<std::uint8_t> above(row_bytes), below(row_bytes);
    std::uint32_t above_row = ~0u, below_row = ~0u;
    const auto widen = [&](std::uint32_t row, std::vector<std::uint8_t> &out) {
        const std::uint8_t *in = source + static_cast<std::size_t>(row) * sw * 4u;
        std::uint8_t *to = out.data();
        for (std::uint32_t x = 0u; x < dw; ++x, to += 4) {
            const std::uint8_t *a = in + static_cast<std::size_t>(column[x]) * 4u;
            const std::uint32_t w = weight[x];
            const std::uint8_t *b = w != 0u ? a + 4 : a;
            for (int c = 0; c < 4; ++c) to[c] = static_cast<std::uint8_t>((a[c] * (256u - w) + b[c] * w + 128u) >> 8u);
        }
    };
    for (std::uint32_t y = 0u; y < dh; ++y) {
        float at = (static_cast<float>(y) + 0.5f) * static_cast<float>(sh) / static_cast<float>(dh) - 0.5f;
        at = std::clamp(at, 0.0f, static_cast<float>(sh - 1u));
        const auto top = std::min(static_cast<std::uint32_t>(at), sh - 1u);
        const std::uint32_t bottom = std::min(top + 1u, sh - 1u);
        const std::uint32_t w = bottom != top ? static_cast<std::uint32_t>((at - static_cast<float>(top)) * 256.0f + 0.5f) : 0u;
        if (above_row != top) {
            if (below_row == top) { above.swap(below); std::swap(above_row, below_row); }
            else { widen(top, above); above_row = top; }
        }
        if (below_row != bottom) { widen(bottom, below); below_row = bottom; }
        std::uint8_t *out = dest + static_cast<std::size_t>(y) * row_bytes;
        const std::uint8_t *a = above.data(), *b = below.data();
        const std::uint32_t wa = 256u - w;
        for (std::size_t i = 0u; i < row_bytes; ++i) out[i] = static_cast<std::uint8_t>((a[i] * wa + b[i] * w + 128u) >> 8u);
    }
}

// A cell's picture at another size. Smaller: every destination pixel is the
// average of the source area it covers. Larger: the four nearest source
// pixels, weighted by distance. Colour is weighted by alpha both ways, so the
// edge of a picture does not darken.
void fit_picture(const std::uint8_t *source, std::uint32_t sw, std::uint32_t sh, std::uint8_t *dest,
                 std::size_t dest_pitch, std::uint32_t dw, std::uint32_t dh) {
    if (dw >= sw && dh >= sh) {
        for (std::uint32_t y = 0u; y < dh; ++y) {
            const double fy = std::clamp((y + 0.5) * sh / dh - 0.5, 0.0, static_cast<double>(sh - 1u));
            const auto top = static_cast<std::uint32_t>(fy), bottom = std::min(top + 1u, sh - 1u);
            const double wy = fy - top;
            for (std::uint32_t x = 0u; x < dw; ++x) {
                const double fx = std::clamp((x + 0.5) * sw / dw - 0.5, 0.0, static_cast<double>(sw - 1u));
                const auto left = static_cast<std::uint32_t>(fx), right = std::min(left + 1u, sw - 1u);
                const double wx = fx - left;
                const std::pair<const std::uint8_t *, double> taps[4]{
                    {source + (static_cast<std::size_t>(top) * sw + left) * 4u, (1.0 - wx) * (1.0 - wy)},
                    {source + (static_cast<std::size_t>(top) * sw + right) * 4u, wx * (1.0 - wy)},
                    {source + (static_cast<std::size_t>(bottom) * sw + left) * 4u, (1.0 - wx) * wy},
                    {source + (static_cast<std::size_t>(bottom) * sw + right) * 4u, wx * wy}};
                double sum[3]{}, alpha = 0.0;
                for (const auto &[texel, share] : taps) {
                    alpha += share * texel[3];
                    for (int c = 0; c < 3; ++c) sum[c] += share * texel[3] * texel[c];
                }
                std::uint8_t *out = dest + static_cast<std::size_t>(y) * dest_pitch + static_cast<std::size_t>(x) * 4u;
                for (int c = 0; c < 3; ++c) out[c] = static_cast<std::uint8_t>(alpha > 0.0 ? sum[c] / alpha + 0.5 : 0.0);
                out[3] = static_cast<std::uint8_t>(alpha + 0.5);
            }
        }
        return;
    }
    const double step_x = static_cast<double>(sw) / dw, step_y = static_cast<double>(sh) / dh;
    for (std::uint32_t y = 0u; y < dh; ++y) {
        const double y0 = y * step_y, y1 = (y + 1u) * step_y;
        for (std::uint32_t x = 0u; x < dw; ++x) {
            const double x0 = x * step_x, x1 = (x + 1u) * step_x;
            double sum[3]{}, alpha = 0.0, area = 0.0;
            for (auto sy = static_cast<std::uint32_t>(y0); sy < sh && static_cast<double>(sy) < y1; ++sy) {
                const double hy = std::min(y1, static_cast<double>(sy) + 1.0) - std::max(y0, static_cast<double>(sy));
                for (auto sx = static_cast<std::uint32_t>(x0); sx < sw && static_cast<double>(sx) < x1; ++sx) {
                    const double wx = std::min(x1, static_cast<double>(sx) + 1.0) - std::max(x0, static_cast<double>(sx));
                    const std::uint8_t *texel = source + (static_cast<std::size_t>(sy) * sw + sx) * 4u;
                    const double share = wx * hy;
                    area += share;
                    alpha += share * texel[3];
                    for (int c = 0; c < 3; ++c) sum[c] += share * texel[3] * texel[c];
                }
            }
            std::uint8_t *out = dest + static_cast<std::size_t>(y) * dest_pitch + static_cast<std::size_t>(x) * 4u;
            for (int c = 0; c < 3; ++c) out[c] = static_cast<std::uint8_t>(alpha > 0.0 ? sum[c] / alpha + 0.5 : 0.0);
            out[3] = static_cast<std::uint8_t>(area > 0.0 ? alpha / area + 0.5 : 0.0);
        }
    }
}

} // namespace

void ge_texture_mods_watch_overlays(std::span<const GeTextureOverlayKey> keys, bool key_every_texture) {
    Mods &m = mods();
    m.overlay_watch.store(false, std::memory_order_release);
    const std::uint32_t count = static_cast<std::uint32_t>(std::min(keys.size(), m.overlay_keys.size()));
    for (std::uint32_t i = 0u; i < count; ++i) m.overlay_keys[i] = keys[i];
    m.overlay_key_count.store(count, std::memory_order_release);
    m.overlay_key_all.store(key_every_texture, std::memory_order_release);
    m.overlay_watch.store(count != 0u, std::memory_order_release);
}

void ge_texture_mods_set_overlays(std::span<const GeTextureOverlaySet> entries, bool at_once) {
    Mods &m = mods();
    const std::lock_guard<std::mutex> lock(m.mutex);
    for (const GeTextureOverlaySet &entry : entries) {
        if (at_once) {
            std::erase_if(m.overlay_pending, [&](const GeTextureOverlaySet &pending) { return pending.key == entry.key; });
            if (entry.overlay) m.overlays[entry.key] = entry.overlay;
            else m.overlays.erase(entry.key);
            continue;
        }
        // Nothing to do when the key's newest overlay is this one already.
        const std::shared_ptr<const GeTextureOverlay> *newest = nullptr;
        for (const GeTextureOverlaySet &pending : m.overlay_pending)
            if (pending.key == entry.key) newest = &pending.overlay;
        std::shared_ptr<const GeTextureOverlay> in_force;
        if (newest == nullptr) {
            if (const auto found = m.overlays.find(entry.key); found != m.overlays.end()) in_force = found->second;
            newest = &in_force;
        }
        if (*newest == entry.overlay) continue;
        m.overlay_pending.push_back(entry);
        m.overlay_changed.store(true, std::memory_order_release);
    }
}

void ge_texture_mods_watch_keys(std::span<const GeTextureOverlayKey> keys) {
    Mods &m = mods();
    m.page_key_count.store(0u, std::memory_order_release);
    const std::uint32_t count = static_cast<std::uint32_t>(std::min(keys.size(), m.page_keys.size()));
    for (std::uint32_t i = 0u; i < count; ++i) m.page_keys[i] = keys[i];
    m.page_key_count.store(count, std::memory_order_release);
}

bool ge_texture_mods_keying_for(std::uint32_t format, std::uint32_t width, std::uint32_t height) noexcept {
    const Mods &m = mods();
    if (m.keying.load(std::memory_order_relaxed)) return true;
    const std::uint32_t pages = m.page_key_count.load(std::memory_order_relaxed);
    for (std::uint32_t i = 0u; i < pages; ++i) {
        const GeTextureOverlayKey &watched = m.page_keys[i];
        if (watched.format == format && watched.width == width && watched.height == height) return true;
    }
    if (!m.overlay_watch.load(std::memory_order_relaxed)) return false;
    if (m.overlay_key_all.load(std::memory_order_relaxed)) return true;
    const std::uint32_t count = m.overlay_key_count.load(std::memory_order_relaxed);
    for (std::uint32_t i = 0u; i < count; ++i) {
        const GeTextureOverlayKey &watched = m.overlay_keys[i];
        if (watched.format == format && watched.width == width && watched.height == height) return true;
    }
    return false;
}

bool ge_texture_mods_take_overlay_changes(std::vector<std::uint64_t> &keys) {
    Mods &m = mods();
    if (!m.overlay_changed.load(std::memory_order_acquire)) return false;
    const std::lock_guard<std::mutex> lock(m.mutex);
    const std::size_t before = keys.size();
    for (GeTextureOverlaySet &pending : m.overlay_pending) {
        if (pending.overlay) m.overlays[pending.key] = std::move(pending.overlay);
        else m.overlays.erase(pending.key);
        keys.push_back(pending.key);
    }
    m.overlay_pending.clear();
    m.overlay_changed.store(false, std::memory_order_release);
    return keys.size() != before;
}

bool ge_texture_mods_has_overlay(std::uint64_t key) {
    Mods &m = mods();
    if (!m.overlay_watch.load(std::memory_order_relaxed)) return false;
    const std::lock_guard<std::mutex> lock(m.mutex);
    return m.overlays.find(key) != m.overlays.end();
}

namespace {

struct OverlayRect { std::uint32_t x0, y0, x1, y1; };

// What the composed picture is. `exact`: the picture in force is the game's own and the composed one is
// that size times a power of two. Its levels are then the game's picture enlarged to each size, down to
// the game's own size and no further, and the last level is the game's own texels. A draw that shows the
// texture at the size the game drew it for samples what the game sampled, and the sharper levels are only
// used where the screen has the pixels for them. Otherwise (a mod's picture underneath, or another
// factor) the smaller levels are filtered from level 0 down to 1x1, as for any replacement.
struct OverlayPlan {
    std::uint32_t width{}, height{}, levels{};
    bool exact{};
    std::size_t bytes{};
};

bool plan_overlay(const GeTextureOverlay &overlay, std::uint32_t width, std::uint32_t height, bool mod,
                  OverlayPlan &plan) noexcept {
    if (width == 0u || height == 0u || overlay.width == 0u || overlay.height == 0u) return false;
    const std::uint32_t scale = std::max(1u, overlay.scale);
    plan.width = std::max(width, overlay.width * scale);
    plan.height = std::max(height, overlay.height * scale);
    if (plan.width > kMaxReplacementSize || plan.height > kMaxReplacementSize) return false;
    plan.exact = false;
    if (!mod && width == overlay.width && height == overlay.height)
        for (std::uint32_t shift = 0u; shift < 8u && !plan.exact; ++shift)
            if (plan.width == width << shift && plan.height == height << shift) {
                plan.exact = true;
                plan.levels = shift + 1u;
            }
    plan.bytes = 0u;
    if (plan.exact) {
        for (std::uint32_t level = 0u; level < plan.levels; ++level)
            plan.bytes += static_cast<std::size_t>(plan.width >> level) * (plan.height >> level) * 4u;
    } else {
        plan.levels = 0u;
        for (std::uint32_t w = plan.width, h = plan.height;; w = std::max(1u, w >> 1u), h = std::max(1u, h >> 1u)) {
            plan.bytes += static_cast<std::size_t>(w) * h * 4u;
            ++plan.levels;
            if (w == 1u && h == 1u) break;
        }
    }
    return true;
}

// Texels x0..x1, y0..y1 of a level from the level above it: the box filter of
// ge_texture_build_mip_chain, colour weighted by alpha.
void mip_texels(const std::uint8_t *source, std::uint32_t sw, std::uint32_t sh, std::uint8_t *dest, std::uint32_t dw,
                std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1) noexcept {
    for (std::uint32_t y = y0; y < y1; ++y) {
        const std::uint32_t sy0 = std::min(sh - 1u, y * 2u), sy1 = std::min(sh - 1u, y * 2u + 1u);
        for (std::uint32_t x = x0; x < x1; ++x) {
            const std::uint32_t sx0 = std::min(sw - 1u, x * 2u), sx1 = std::min(sw - 1u, x * 2u + 1u);
            const std::uint8_t *texels[4]{
                source + (static_cast<std::size_t>(sy0) * sw + sx0) * 4u,
                source + (static_cast<std::size_t>(sy0) * sw + sx1) * 4u,
                source + (static_cast<std::size_t>(sy1) * sw + sx0) * 4u,
                source + (static_cast<std::size_t>(sy1) * sw + sx1) * 4u};
            std::uint32_t alpha = 0u, weighted[3]{}, plain[3]{};
            for (const std::uint8_t *texel : texels) {
                alpha += texel[3];
                for (int c = 0; c < 3; ++c) {
                    weighted[c] += static_cast<std::uint32_t>(texel[c]) * texel[3];
                    plain[c] += texel[c];
                }
            }
            std::uint8_t *out = dest + (static_cast<std::size_t>(y) * dw + x) * 4u;
            for (int c = 0; c < 3; ++c)
                out[c] = static_cast<std::uint8_t>(alpha != 0u ? (weighted[c] + alpha / 2u) / alpha
                                                               : (plain[c] + 2u) / 4u);
            out[3] = static_cast<std::uint8_t>((alpha + 2u) / 4u);
        }
    }
}

// The picture in force at the composed size with its levels, without the cells.
void make_overlay_base(const OverlayPlan &plan, const std::uint8_t *source, std::uint32_t width, std::uint32_t height,
                       std::vector<std::byte> &chain) {
    chain.assign(plan.bytes, std::byte{0});
    auto *out = reinterpret_cast<std::uint8_t *>(chain.data());
    if (plan.exact) {
        for (std::uint32_t level = 0u; level < plan.levels; ++level) {
            const std::uint32_t w = plan.width >> level, h = plan.height >> level;
            enlarge_linear(source, width, height, out, w, h);
            out += static_cast<std::size_t>(w) * h * 4u;
        }
        return;
    }
    enlarge_linear(source, width, height, out, plan.width, plan.height);
    (void)ge_texture_build_mip_chain(plan.width, plan.height, chain);
}

// One cell's picture into the rectangle x0..x1, y0..y1 of a level: the level of the picture that has
// exactly that size when there is one, else its level 0 fitted.
void paint_cell(const GeTextureOverlayCell &cell, std::uint8_t *level, std::uint32_t level_width, const OverlayRect &rect) {
    const std::size_t pitch = static_cast<std::size_t>(level_width) * 4u;
    std::uint8_t *to = level + static_cast<std::size_t>(rect.y0) * pitch + static_cast<std::size_t>(rect.x0) * 4u;
    const std::uint32_t w = rect.x1 - rect.x0, h = rect.y1 - rect.y0;
    const auto *in = reinterpret_cast<const std::uint8_t *>(cell.rgba8.data());
    std::size_t offset = 0u;
    std::uint32_t pw = cell.picture_width, ph = cell.picture_height;
    for (std::uint32_t k = 0u; k < std::max(1u, cell.picture_levels); ++k) {
        const std::size_t bytes = static_cast<std::size_t>(pw) * ph * 4u;
        if (offset + bytes > cell.rgba8.size()) break;
        if (pw == w && ph == h) {
            for (std::uint32_t y = 0u; y < h; ++y)
                std::memcpy(to + static_cast<std::size_t>(y) * pitch, in + offset + static_cast<std::size_t>(y) * pw * 4u,
                            static_cast<std::size_t>(w) * 4u);
            return;
        }
        offset += bytes;
        pw = std::max(1u, pw >> 1u);
        ph = std::max(1u, ph >> 1u);
    }
    fit_picture(in, cell.picture_width, cell.picture_height, to, pitch, w, h);
}

// Paints the cells into a chain made by make_overlay_base: what was in a cell is gone, in every level.
void paint_overlay(const OverlayPlan &plan, const GeTextureOverlay &overlay, std::vector<std::byte> &chain) {
    auto *base = reinterpret_cast<std::uint8_t *>(chain.data());
    // A cell in pixels of a level.
    const auto rect_of = [&](const GeTextureOverlayCell &cell, std::uint32_t w, std::uint32_t h) {
        const auto place = [](std::uint32_t at, std::uint32_t size, std::uint32_t of) {
            return static_cast<std::uint32_t>(static_cast<std::uint64_t>(at) * size / of);
        };
        return OverlayRect{std::min(w, place(cell.x, w, overlay.width)), std::min(h, place(cell.y, h, overlay.height)),
                           std::min(w, place(cell.x + cell.width, w, overlay.width)),
                           std::min(h, place(cell.y + cell.height, h, overlay.height))};
    };
    const auto usable = [](const GeTextureOverlayCell &cell) {
        return cell.picture_width != 0u && cell.picture_height != 0u &&
               cell.rgba8.size() >= static_cast<std::size_t>(cell.picture_width) * cell.picture_height * 4u;
    };
    if (plan.exact) {
        for (std::uint32_t level = 0u; level < plan.levels; ++level) {
            const std::uint32_t w = plan.width >> level, h = plan.height >> level;
            for (const GeTextureOverlayCell &cell : overlay.cells) {
                const OverlayRect rect = rect_of(cell, w, h);
                if (usable(cell) && rect.x0 < rect.x1 && rect.y0 < rect.y1) paint_cell(cell, base, w, rect);
            }
            base += static_cast<std::size_t>(w) * h * 4u;
        }
        return;
    }
    std::vector<OverlayRect> painted;
    for (const GeTextureOverlayCell &cell : overlay.cells) {
        const OverlayRect rect = rect_of(cell, plan.width, plan.height);
        if (!usable(cell) || rect.x0 >= rect.x1 || rect.y0 >= rect.y1) continue;
        paint_cell(cell, base, plan.width, rect);
        painted.push_back(rect);
    }
    // The smaller levels again, under the cells only.
    std::size_t source_offset = 0u;
    std::uint32_t sw = plan.width, sh = plan.height;
    for (std::uint32_t level = 1u; level < plan.levels; ++level) {
        const std::uint32_t dw = std::max(1u, sw >> 1u), dh = std::max(1u, sh >> 1u);
        const std::size_t dest_offset = source_offset + static_cast<std::size_t>(sw) * sh * 4u;
        for (OverlayRect &rect : painted) {
            rect = {rect.x0 >> 1u, rect.y0 >> 1u, std::min(dw, ((rect.x1 - 1u) >> 1u) + 1u), std::min(dh, ((rect.y1 - 1u) >> 1u) + 1u)};
            if (rect.x0 < rect.x1 && rect.y0 < rect.y1)
                mip_texels(base + source_offset, sw, sh, base + dest_offset, dw, rect.x0, rect.y0, rect.x1, rect.y1);
        }
        source_offset = dest_offset;
        sw = dw;
        sh = dh;
    }
}

} // namespace

GeTextureReplacementPtr ge_texture_compose_overlay(const GeTextureOverlay &overlay, std::uint32_t width,
                                                   std::uint32_t height, std::span<const std::byte> level0) {
    OverlayPlan plan;
    // A picture of the size the cells are measured on is the game's own; any other is a mod's.
    const bool mod = width != overlay.width || height != overlay.height;
    if (!plan_overlay(overlay, width, height, mod, plan) ||
        level0.size() < static_cast<std::size_t>(width) * height * 4u) return nullptr;
    auto picture = std::make_shared<GeTextureReplacement>();
    try {
        make_overlay_base(plan, reinterpret_cast<const std::uint8_t *>(level0.data()), width, height, picture->rgba8);
        paint_overlay(plan, overlay, picture->rgba8);
    } catch (...) { return nullptr; }
    picture->width = plan.width;
    picture->height = plan.height;
    picture->mip_levels = plan.levels;
    picture->exact_levels = plan.exact;
    picture->mod_picture = mod;
    return picture;
}

GeTextureReplacementPtr ge_texture_mods_compose_overlay(std::uint64_t key, const GeTextureReplacementPtr &base,
                                                        std::uint32_t width, std::uint32_t height,
                                                        std::span<const std::byte> rgba8) {
    Mods &m = mods();
    if (!m.overlay_watch.load(std::memory_order_relaxed)) return base;
    std::shared_ptr<const GeTextureOverlay> overlay;
    {
        const std::lock_guard<std::mutex> lock(m.mutex);
        if (const auto found = m.overlays.find(key); found != m.overlays.end()) overlay = found->second;
    }
    // The cells are measured on the game's own picture: another size is another picture.
    if (!overlay || overlay->width != width || overlay->height != height) return base;
    const std::uint64_t started = now_ns();
    const std::uint32_t source_w = base ? base->width : width, source_h = base ? base->height : height;
    const std::span<const std::byte> source = base ? std::span<const std::byte>(base->rgba8) : rgba8;
    OverlayPlan plan;
    if (!plan_overlay(*overlay, source_w, source_h, base != nullptr, plan) ||
        source.size() < static_cast<std::size_t>(source_w) * source_h * 4u) return base;
    auto made = std::make_shared<GeTextureReplacement>();
    try {
        // The picture without cells is kept from one change of the cells to the next.
        OverlayBase &kept = m.overlay_bases[key];
        if (kept.rgba8.size() != plan.bytes || kept.mod != (base != nullptr) || kept.source_width != source_w ||
            kept.source_height != source_h || kept.width != plan.width || kept.height != plan.height) {
            make_overlay_base(plan, reinterpret_cast<const std::uint8_t *>(source.data()), source_w, source_h, kept.rgba8);
            kept.mod = base != nullptr;
            kept.source_width = source_w;
            kept.source_height = source_h;
            kept.width = plan.width;
            kept.height = plan.height;
        }
        made->rgba8 = kept.rgba8;
        paint_overlay(plan, *overlay, made->rgba8);
    } catch (...) { return base; }
    made->width = plan.width;
    made->height = plan.height;
    made->mip_levels = plan.levels;
    made->exact_levels = plan.exact;
    made->mod_picture = base != nullptr;
    const GeTextureReplacementPtr picture = std::move(made);
    const std::uint64_t us = (now_ns() - started) / 1000u;
    const std::lock_guard<std::mutex> lock(m.mutex);
    ++m.report.overlay_composed;
    m.report.overlay_compose_us += us;
    m.report.overlay_compose_max_us = std::max(m.report.overlay_compose_max_us, us);
    m.report.overlay_bytes += picture->rgba8.size();
    return picture;
}

void ge_texture_mods_note_overlay_evictions(std::uint64_t textures) noexcept {
    Mods &m = mods();
    const std::lock_guard<std::mutex> lock(m.mutex);
    m.report.overlay_evicted += textures;
}

std::uint64_t ge_texture_mods_clock_ns() noexcept { return now_ns(); }

void ge_texture_mods_note_keyed(std::uint64_t started_ns) noexcept {
    Mods &m = mods();
    const std::uint64_t ns = now_ns() - started_ns;
    m.keyed_textures.fetch_add(1u, std::memory_order_relaxed);
    m.keying_ns.fetch_add(ns, std::memory_order_relaxed);
    if (ns > m.keying_max_ns.load(std::memory_order_relaxed)) m.keying_max_ns.store(ns, std::memory_order_relaxed);
}

std::uint32_t ge_texture_build_mip_chain(std::uint32_t width, std::uint32_t height,
                                         std::vector<std::byte> &rgba8) {
    std::size_t total = 0u;
    std::uint32_t levels = 0u;
    for (std::uint32_t w = width, h = height;; w = std::max(1u, w >> 1u), h = std::max(1u, h >> 1u)) {
        total += static_cast<std::size_t>(w) * h * 4u;
        ++levels;
        if (w == 1u && h == 1u) break;
    }
    rgba8.resize(total);
    std::size_t source_offset = 0u;
    std::uint32_t sw = width, sh = height;
    for (std::uint32_t level = 1u; level < levels; ++level) {
        const std::uint32_t dw = std::max(1u, sw >> 1u), dh = std::max(1u, sh >> 1u);
        const std::size_t dest_offset = source_offset + static_cast<std::size_t>(sw) * sh * 4u;
        mip_texels(reinterpret_cast<const std::uint8_t *>(rgba8.data()) + source_offset, sw, sh,
                   reinterpret_cast<std::uint8_t *>(rgba8.data()) + dest_offset, dw, 0u, 0u, dw, dh);
        source_offset = dest_offset;
        sw = dw;
        sh = dh;
    }
    return levels;
}

bool ge_texture_read_png(const std::filesystem::path &path, std::uint32_t &width, std::uint32_t &height,
                         std::vector<std::byte> &rgba8, std::string &error) {
    ComPtr<IWICImagingFactory> factory;
    return make_factory(factory, error) && read_png(factory.Get(), path, width, height, rgba8, error);
}

bool ge_texture_write_png(const std::filesystem::path &path, std::uint32_t width, std::uint32_t height,
                          std::span<const std::byte> rgba8, std::string &error) {
    ComPtr<IWICImagingFactory> factory;
    return make_factory(factory, error) && write_png(factory.Get(), path, width, height, rgba8, error);
}

} // namespace vcs
