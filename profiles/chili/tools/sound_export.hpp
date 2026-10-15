#pragma once
// The parts of ChiliSoundExport that need no disc and no decoder, kept apart so sound_export_test.cpp can
// check them on made-up bytes: the stream header, the stored layout of a long stream in assets00.naz, the
// loudness measure and the files the tool writes.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
namespace chili::sound_export {

// Exit codes. Every failure also prints one line on stderr and leaves no output file behind.
enum Exit : int {
    exit_ok = 0,
    exit_usage = 1,
    exit_disc_unreadable = 2,   // missing, not a plain ISO 9660 image or disc folder, compressed (CSO), read error
    exit_wrong_disc = 3,        // UMD_DATA.BIN missing or not ULUS-10216
    exit_track_missing = 4,     // the track is not on the disc, or not the bytes the game ships
    exit_decode_failed = 5,
    exit_write_failed = 6,
};
struct Failure : std::runtime_error {
    int code;
    Failure(int exit_code, const std::string &what) : std::runtime_error(what), code(exit_code) {}
};

inline std::uint32_t le16(std::span<const std::uint8_t> b, std::size_t p) { return b[p] | (std::uint32_t{b[p + 1]} << 8u); }
inline std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t p) { return le16(b, p) | (le16(b, p + 2) << 16u); }
inline bool tag(std::span<const std::uint8_t> b, std::size_t p, const char *name) { return std::memcmp(b.data() + p, name, 4) == 0; }

// CRC-32 (the zlib one). The key of a stream is the CRC-32 of the first 256 bytes of its data chunk: the
// key=... a run with PSPRECOMP_LOG=sound prints for every stream the game starts (tools/sound_assets.py).
inline std::uint32_t crc32(std::span<const std::uint8_t> bytes, std::uint32_t crc = 0u) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256u; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1u) : c >> 1u;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (const auto byte : bytes) crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8u);
    return ~crc;
}

// What an ATRAC3plus RIFF/WAVE stream says about itself, checked the way the game's decoder checks it
// (host/atrac_stream.cpp attach()).
struct Atrac {
    unsigned channels{}, rate{}, block{}, samples{}, skip{};
    std::size_t data_offset{}, data_size{};
    bool looped{};
    unsigned loop_begin{}, loop_end{};   // the smpl chunk's loop in file samples, end inclusive
    std::uint32_t key{};
    // The decoder's 368 samples of delay plus the encoder's skip: cut off the front, as a PSP does.
    [[nodiscard]] unsigned lead_in() const { return skip + 368u; }
};
inline Atrac parse_atrac(std::span<const std::uint8_t> file) {
    const auto bad = [](const std::string &what) -> Atrac { throw std::runtime_error("the stream " + what); };
    if (file.size() < 12u || !tag(file, 0, "RIFF") || !tag(file, 8, "WAVE")) return bad("is not RIFF/WAVE");
    const std::uint64_t length = std::uint64_t{le32(file, 4)} + 8u;
    if (length > file.size()) return bad("is cut short");
    Atrac a;
    bool format{}, fact{};
    for (std::size_t p = 12u; p + 8u <= length;) {
        const std::uint32_t size = le32(file, p + 4);
        if (p + 8u + std::uint64_t{size} > length) return bad("has a chunk that runs past its end");
        const auto payload = file.subspan(p + 8u, size);
        if (tag(file, p, "data")) { a.data_offset = p + 8u; a.data_size = size; break; }
        if (tag(file, p, "fmt ")) {
            static constexpr std::array<std::uint8_t, 16> atrac3plus{0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44,
                                                                     0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62};
            if (size < 40u || le16(payload, 0) != 0xFFFEu || !std::equal(atrac3plus.begin(), atrac3plus.end(), payload.begin() + 24))
                return bad("is not ATRAC3plus");
            a.channels = le16(payload, 2); a.rate = le32(payload, 4); a.block = le16(payload, 12);
            if (a.channels < 1u || a.channels > 2u || a.rate != 44100u || !a.block || a.block > 2112u) return bad("has an unusable format");
            format = true;
        } else if (tag(file, p, "fact")) {
            if (size != 4u && size != 8u && size != 12u) return bad("has a damaged fact chunk");
            a.samples = le32(payload, 0);
            a.skip = size >= 8u ? le32(payload, size == 12u ? 8 : 4) : 0u;
            fact = true;
        } else if (tag(file, p, "smpl") && size >= 36u) {
            const auto loops = le32(payload, 28);
            if (loops && size >= 60u) { a.looped = true; a.loop_begin = le32(payload, 44); a.loop_end = le32(payload, 48); }
        }
        p += 8u + ((std::size_t{size} + 1u) & ~std::size_t{1});
    }
    if (!format || !fact || !a.data_offset) return bad("lacks its fmt, fact or data chunk");
    if (!a.data_size || a.data_size % a.block || a.data_offset + a.data_size > length) return bad("has a damaged data chunk");
    const std::uint64_t raw = std::uint64_t{a.data_size / a.block} * 2048u;
    if (!a.samples || std::uint64_t{a.samples} + a.lead_in() > raw) return bad("holds fewer samples than it says");
    if (a.looped && (a.loop_begin < a.skip || a.loop_end < a.loop_begin || a.loop_end - a.skip >= a.samples)) a.looped = false;
    a.key = crc32(file.subspan(a.data_offset, std::min<std::size_t>(256u, a.data_size)));
    return a;
}

// A standalone stream of assets00.naz (data_stream_psp/.../<name>.mediastream_data_psp) as it is stored: one
// block per layer, each a RIFF header and a whole number of frames, then chunks of <u32 chunk bytes> <u32
// bytes of layer 0> .. <u32 bytes of layer n-1> followed by that many bytes of each layer in turn. A long
// stream with one layer is stored the same way (tools/sound_assets.py, split_layers). Returns one layer as a
// plain RIFF stream; bytes that are a plain stream already come back as they are.
inline std::vector<std::uint8_t> plain_stream(std::span<const std::uint8_t> data, unsigned layer = 0u) {
    if (data.size() < 12u || !tag(data, 0, "RIFF") || !tag(data, 8, "WAVE")) throw std::runtime_error("the stream is not RIFF/WAVE");
    const std::size_t riff = std::size_t{le32(data, 4)} + 8u;
    std::vector<std::size_t> heads{0u};
    std::size_t block = 0u;
    {
        const auto found = std::search(data.begin() + 12, data.end(), data.begin(), data.begin() + 12);
        if (found != data.end()) {
            const auto step = static_cast<std::size_t>(found - data.begin());
            for (std::size_t at = step; at + 12u <= data.size() && std::equal(data.begin(), data.begin() + 12, data.begin() + static_cast<std::ptrdiff_t>(at)); at += step)
                heads.push_back(at);
            block = step;
        }
    }
    if (heads.size() == 1u) {
        // One layer: its first block ends where the first chunk header stands, after whole frames.
        const auto first = parse_atrac(data.size() >= riff ? data.first(riff) : data);
        block = 0u;
        if (data.size() > riff) {
            for (std::size_t at = first.data_offset + first.block; at + 8u <= std::min<std::size_t>(data.size(), 1u << 20u); at += first.block) {
                const auto total = le32(data, at), size = le32(data, at + 4);
                if (size && total == size + 8u && size % first.block == 0u) { block = at; break; }
            }
        }
        if (!block) return {data.begin(), data.begin() + static_cast<std::ptrdiff_t>(std::min(data.size(), riff))};
    }
    const std::size_t count = heads.size();
    if (layer >= count) throw std::runtime_error("the stream has no layer " + std::to_string(layer));
    std::vector<std::uint8_t> out(data.begin() + static_cast<std::ptrdiff_t>(heads[layer]),
                                  data.begin() + static_cast<std::ptrdiff_t>(heads[layer] + block));
    for (std::size_t at = block * count; at + 4u + 4u * count <= data.size();) {
        const std::uint32_t total = le32(data, at);
        std::uint64_t sum = 4u + 4u * count;
        for (std::size_t k = 0; k < count; ++k) sum += le32(data, at + 4u + 4u * k);
        if (total != sum || at + total > data.size()) break;
        std::size_t cursor = at + 4u + 4u * count;
        for (std::size_t k = 0; k < count; ++k) {
            const auto size = le32(data, at + 4u + 4u * k);
            if (k == layer) out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(cursor), data.begin() + static_cast<std::ptrdiff_t>(cursor + size));
            cursor += size;
        }
        at += total;
    }
    if (out.size() < riff) throw std::runtime_error("the stream is cut short (" + std::to_string(out.size()) + " of " + std::to_string(riff) + " bytes)");
    out.resize(riff);
    return out;
}

// Integrated loudness in LUFS (ITU-R BS.1770-4, EBU R 128): K-weighting (the two filters for any rate, from
// their analog prototypes), 400 ms blocks every 100 ms, the absolute gate at -70 LUFS and the relative gate
// 10 LU below. Left and right weigh 1. Minus infinity for silence or less than one block.
inline double loudness(std::span<const std::int16_t> pcm, unsigned channels, unsigned rate) {
    struct Biquad {
        double b0, b1, b2, a1, a2, z1{}, z2{};
        double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    };
    const double pi = 3.14159265358979323846;
    double K = std::tan(pi * 1681.974450955533 / rate), Q = 0.7071752369554196;
    const double Vh = std::pow(10.0, 3.999843853973347 / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
    double a0 = 1.0 + K / Q + K * K;
    const Biquad shelf{(Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                       2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
    K = std::tan(pi * 38.13547087602444 / rate); Q = 0.5003270373238773; a0 = 1.0 + K / Q + K * K;
    const Biquad high{1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
    const std::size_t frames = pcm.size() / channels, step = rate / 10u, block = 4u * step;
    if (!channels || frames < block) return -std::numeric_limits<double>::infinity();
    // Sum of squares of every 100 ms step, all channels together (equal weights).
    std::vector<double> steps(frames / step, 0.0);
    for (unsigned c = 0; c < channels; ++c) {
        Biquad f1 = shelf, f2 = high;
        for (std::size_t i = 0; i < steps.size() * step; ++i) {
            const double y = f2.run(f1.run(pcm[i * channels + c] / 32768.0));
            steps[i / step] += y * y;
        }
    }
    std::vector<double> z;   // mean square of each block
    for (std::size_t s = 0; s + 4u <= steps.size(); ++s) z.push_back((steps[s] + steps[s + 1] + steps[s + 2] + steps[s + 3]) / block);
    const auto lufs = [](double ms) { return -0.691 + 10.0 * std::log10(ms); };
    double sum = 0.0; std::size_t n = 0u;
    for (const auto v : z) if (v > 0.0 && lufs(v) > -70.0) { sum += v; ++n; }
    if (!n) return -std::numeric_limits<double>::infinity();
    const double relative = lufs(sum / n) - 10.0;
    sum = 0.0; n = 0u;
    for (const auto v : z) if (v > 0.0 && lufs(v) > -70.0 && lufs(v) > relative) { sum += v; ++n; }
    return lufs(sum / n);
}
inline double peak_dbfs(std::span<const std::int16_t> pcm) {
    int peak = 0;
    for (const auto v : pcm) peak = std::max(peak, std::abs(int{v}));
    return peak ? 20.0 * std::log10(peak / 32768.0) : -std::numeric_limits<double>::infinity();
}

// The gain (dB) that brings the track to `target` LUFS, lowered where needed so the loudest sample stays at or
// below `ceiling` dBFS.
inline double level_gain(double measured, double peak, double target = -16.0, double ceiling = -1.0) {
    if (!std::isfinite(measured) || !std::isfinite(peak)) return 0.0;
    return std::min(target - measured, ceiling - peak);
}
inline void apply_gain(std::span<std::int16_t> pcm, double gain_db) {
    const double g = std::pow(10.0, gain_db / 20.0);
    for (auto &v : pcm) v = static_cast<std::int16_t>(std::clamp(std::lround(v * g), -32768l, 32767l));
}

// 16-bit PCM WAV with the plain 44-byte header.
inline std::vector<std::uint8_t> wav_bytes(std::span<const std::int16_t> pcm, unsigned channels, unsigned rate) {
    std::vector<std::uint8_t> out;
    const auto put16 = [&](std::uint32_t v) { out.push_back(static_cast<std::uint8_t>(v)); out.push_back(static_cast<std::uint8_t>(v >> 8u)); };
    const auto put32 = [&](std::uint32_t v) { put16(v & 0xFFFFu); put16(v >> 16u); };
    const auto text = [&](const char *t) { out.insert(out.end(), t, t + 4); };
    const auto bytes = static_cast<std::uint32_t>(pcm.size() * 2u);
    out.reserve(44u + bytes);
    text("RIFF"); put32(36u + bytes); text("WAVE"); text("fmt "); put32(16u); put16(1u); put16(channels);
    put32(rate); put32(rate * channels * 2u); put16(channels * 2u); put16(16u); text("data"); put32(bytes);
    for (const auto v : pcm) put16(static_cast<std::uint16_t>(v));
    return out;
}

// What the tool prints and writes beside the WAV: one key=value per line.
struct Result {
    std::string track;
    unsigned channels{}, rate{};
    std::size_t frames{}, loop_start{}, loop_end{};   // the loop in output frames, end exclusive
    double lufs{}, gain_db{}, peak{};
};
inline std::string fixed(double v, int places) {
    if (!std::isfinite(v)) return v < 0 ? "-inf" : "inf";
    char text[64];
    std::snprintf(text, sizeof text, "%.*f", places, v);
    return text;
}
inline std::string loop_text(const Result &r) {
    return "track=" + r.track + "\nchannels=" + std::to_string(r.channels) + "\nrate=" + std::to_string(r.rate) +
           "\nframes=" + std::to_string(r.frames) + "\nloop_start=" + std::to_string(r.loop_start) + "\nloop_end=" +
           std::to_string(r.loop_end) + "\nlufs=" + fixed(r.lufs, 2) + "\ngain_db=" + fixed(r.gain_db, 2) +
           "\npeak_dbfs=" + fixed(r.peak, 2) + "\n";
}
// <out>.wav -> <out>.loop.txt (any other name gets .loop.txt added).
inline std::filesystem::path loop_file(const std::filesystem::path &wav) {
    auto name = wav;
    if (name.extension() == L".wav" || name.extension() == L".WAV") name.replace_extension();
    name += L".loop.txt";
    return name;
}
// Writes through <file>.part and renames, so a reader never sees half a file.
inline void write_whole(const std::filesystem::path &file, std::span<const std::uint8_t> bytes) {
    auto part = file;
    part += L".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) throw Failure(exit_write_failed, "cannot create " + part.string());
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) {
            std::error_code ignore;
            std::filesystem::remove(part, ignore);
            throw Failure(exit_write_failed, "cannot write " + part.string());
        }
    }
    std::error_code status;
    std::filesystem::rename(part, file, status);
    if (status) {
        std::filesystem::remove(part, status);
        throw Failure(exit_write_failed, "cannot write " + file.string());
    }
}
}
