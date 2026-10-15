// Checks of ChiliSoundExport (sound_export.cpp) that need no game data: the stream header, the stored layout
// of long streams, the loudness measure against the EBU's reference tones, the level change, the files it
// writes, and the tool's answers to discs that are not the game. With the developer's own disc image as the
// third argument it also exports both tracks from it and checks what comes out.
//
//   chili_sound_export_test <scratch folder> <ChiliSoundExport.exe> [<the game's .iso>]
#include "sound_export.hpp"
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace {
using namespace chili::sound_export;
int failures = 0, checks = 0;
void check(bool ok, const std::string &what) {
    ++checks;
    if (!ok) { ++failures; std::cout << "FAIL " << what << '\n'; }
}
void close_to(double value, double expected, double tolerance, const std::string &what) {
    check(std::abs(value - expected) <= tolerance, what + " = " + fixed(value, 3) + ", expected " + fixed(expected, 3));
}
template <class F> bool throws(F &&f) { try { f(); } catch (const std::exception &) { return true; } return false; }

void put16(std::vector<std::uint8_t> &b, std::uint32_t v) { b.push_back(static_cast<std::uint8_t>(v)); b.push_back(static_cast<std::uint8_t>(v >> 8u)); }
void put32(std::vector<std::uint8_t> &b, std::uint32_t v) { put16(b, v & 0xFFFFu); put16(b, v >> 16u); }
void text(std::vector<std::uint8_t> &b, const char *t) { b.insert(b.end(), t, t + 4); }

// A made-up ATRAC3plus stream: real header layout, frames of a counting pattern (not decodable audio).
std::vector<std::uint8_t> stream(unsigned frames, unsigned block, unsigned samples, unsigned skip, int loop_begin, int loop_end, std::uint8_t seed) {
    std::vector<std::uint8_t> chunks;
    text(chunks, "fmt "); put32(chunks, 52u);
    put16(chunks, 0xFFFEu); put16(chunks, 2u); put32(chunks, 44100u); put32(chunks, block * 44100u / 2048u); put16(chunks, block); put16(chunks, 0u);
    put16(chunks, 34u); put16(chunks, 0u); put32(chunks, 3u);
    for (const int g : {0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44, 0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62}) chunks.push_back(static_cast<std::uint8_t>(g));
    for (int i = 0; i < 12; ++i) chunks.push_back(0u);
    text(chunks, "fact"); put32(chunks, 8u); put32(chunks, samples); put32(chunks, skip);
    if (loop_begin >= 0) {
        text(chunks, "smpl"); put32(chunks, 60u);
        for (int i = 0; i < 7; ++i) put32(chunks, 0u);
        put32(chunks, 1u); put32(chunks, 24u); put32(chunks, 0u); put32(chunks, 0u);
        put32(chunks, static_cast<std::uint32_t>(loop_begin)); put32(chunks, static_cast<std::uint32_t>(loop_end)); put32(chunks, 0u); put32(chunks, 0u);
    }
    text(chunks, "data"); put32(chunks, frames * block);
    for (unsigned i = 0; i < frames * block; ++i) chunks.push_back(static_cast<std::uint8_t>(i * 7u + seed));
    std::vector<std::uint8_t> out;
    text(out, "RIFF"); put32(out, static_cast<std::uint32_t>(4u + chunks.size())); text(out, "WAVE");
    out.insert(out.end(), chunks.begin(), chunks.end());
    return out;
}
// The way assets00.naz stores a long stream: per layer a first block of the header and `head` frames, then
// chunks of `per_chunk` frames of every layer.
std::vector<std::uint8_t> stored(const std::vector<std::vector<std::uint8_t>> &layers, std::size_t data_offset, unsigned block, unsigned head, unsigned per_chunk) {
    std::vector<std::uint8_t> out;
    const std::size_t first = data_offset + std::size_t{head} * block;
    for (const auto &layer : layers) out.insert(out.end(), layer.begin(), layer.begin() + static_cast<std::ptrdiff_t>(first));
    for (std::size_t at = first; at < layers[0].size();) {
        const auto size = static_cast<std::uint32_t>(std::min<std::size_t>(std::size_t{per_chunk} * block, layers[0].size() - at));
        put32(out, static_cast<std::uint32_t>(4u + 4u * layers.size() + size * layers.size()));
        for (std::size_t k = 0; k < layers.size(); ++k) put32(out, size);
        for (const auto &layer : layers) out.insert(out.end(), layer.begin() + static_cast<std::ptrdiff_t>(at), layer.begin() + static_cast<std::ptrdiff_t>(at + size));
        at += size;
    }
    out.insert(out.end(), 12u, 0u);   // what follows a file in the archive
    return out;
}

std::vector<std::int16_t> tone(double dbfs, double seconds, unsigned rate, unsigned channels) {
    const double amplitude = std::pow(10.0, dbfs / 20.0) * 32767.0;
    std::vector<std::int16_t> out;
    for (std::size_t i = 0; i < static_cast<std::size_t>(seconds * rate); ++i)
        for (unsigned c = 0; c < channels; ++c) out.push_back(static_cast<std::int16_t>(std::lround(amplitude * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * i / rate))));
    return out;
}

void test_parts() {
    const std::string digits = "123456789";
    check(crc32(std::span(reinterpret_cast<const std::uint8_t *>(digits.data()), digits.size())) == 0xCBF43926u, "crc32 check value");

    const auto plain = stream(300u, 560u, 300u * 2048u - 2048u - 368u - 5u, 2048u, -1, -1, 3u);
    const auto a = parse_atrac(plain);
    check(a.channels == 2u && a.rate == 44100u && a.block == 560u && a.skip == 2048u && a.lead_in() == 2416u, "parse_atrac format and fact");
    check(a.data_offset == 96u && a.data_size == 300u * 560u && !a.looped, "parse_atrac data chunk");
    check(a.key == crc32(std::span(plain).subspan(96u, 256u)), "parse_atrac key = CRC-32 of the first 256 data bytes");
    const auto looped = stream(100u, 376u, 100u * 2048u - 3506u - 368u, 3506u, 3506, 3506 + 100 * 2048 - 3506 - 368 - 1, 9u);
    const auto b = parse_atrac(looped);
    check(b.looped && b.loop_begin == 3506u && b.loop_end == 3506u + b.samples - 1u && b.data_offset == 164u, "parse_atrac smpl loop");
    auto broken = plain; broken[12u + 8u + 24u] ^= 1u;
    check(throws([&] { parse_atrac(broken); }), "parse_atrac refuses another codec");
    check(throws([&] { parse_atrac(stream(10u, 560u, 10u * 2048u, 2048u, -1, -1, 0u)); }), "parse_atrac refuses more samples than the frames hold");
    check(throws([&] { parse_atrac(std::span(plain).first(plain.size() - 1u)); }), "parse_atrac refuses a cut stream");

    // Stored layouts: one layer, two layers, a plain file, a cut file.
    const auto one = stored({plain}, 96u, 560u, 57u, 57u);
    check(one.size() > plain.size() && plain_stream(one) == plain, "plain_stream: one layer stored in chunks");
    const auto other = stream(300u, 560u, 300u * 2048u - 2048u - 368u - 5u, 2048u, -1, -1, 77u);
    const auto two = stored({plain, other}, 96u, 560u, 57u, 57u);
    check(plain_stream(two, 0u) == plain && plain_stream(two, 1u) == other, "plain_stream: two layers stored in chunks");
    check(throws([&] { plain_stream(two, 2u); }), "plain_stream: no third layer");
    check(plain_stream(plain) == plain, "plain_stream: a plain stream comes back as it is");
    check(throws([&] { plain_stream(std::span(one).first(one.size() - 5000u)); }), "plain_stream refuses a cut file");

    // Loudness against the EBU's reference signals (Tech 3341): 1 kHz sine, both channels.
    close_to(loudness(tone(-23.0, 20.0, 48000u, 2u), 2u, 48000u), -23.0, 0.1, "1 kHz -23 dBFS stereo, 48 kHz");
    close_to(loudness(tone(-23.0, 20.0, 44100u, 2u), 2u, 44100u), -23.0, 0.1, "1 kHz -23 dBFS stereo, 44.1 kHz");
    close_to(loudness(tone(-33.0, 20.0, 44100u, 2u), 2u, 44100u), -33.0, 0.1, "1 kHz -33 dBFS stereo, 44.1 kHz");
    close_to(loudness(tone(-23.0, 20.0, 44100u, 1u), 1u, 44100u), -26.01, 0.1, "1 kHz -23 dBFS one channel");
    {
        auto gated = tone(-36.0, 10.0, 48000u, 2u);
        const auto loud = tone(-23.0, 60.0, 48000u, 2u), quiet = tone(-36.0, 10.0, 48000u, 2u);
        gated.insert(gated.end(), loud.begin(), loud.end()); gated.insert(gated.end(), quiet.begin(), quiet.end());
        close_to(loudness(gated, 2u, 48000u), -23.0, 0.1, "Tech 3341 case 3 (-36/-23/-36 dBFS, relative gate)");
        auto absolute = tone(-72.0, 10.0, 48000u, 2u);
        const auto mid = tone(-36.0, 60.0, 48000u, 2u), low = tone(-72.0, 10.0, 48000u, 2u);
        absolute.insert(absolute.end(), mid.begin(), mid.end()); absolute.insert(absolute.end(), low.begin(), low.end());
        close_to(loudness(absolute, 2u, 48000u), -36.0, 0.1, "Tech 3341 case 4 (-72/-36/-72 dBFS, gates)");
    }
    check(std::isinf(loudness(std::vector<std::int16_t>(44100u * 4u, 0), 2u, 44100u)), "silence has no loudness");
    check(std::isinf(loudness(tone(-20.0, 0.3, 44100u, 2u), 2u, 44100u)), "less than one block has no loudness");

    // Level: to -16 LUFS, never above -1 dBFS.
    close_to(level_gain(-12.34, -0.35), -3.66, 1e-9, "gain for a loud track");
    close_to(level_gain(-20.0, -2.0), 1.0, 1e-9, "gain held by the peak");
    close_to(level_gain(-20.0, -10.0), 4.0, 1e-9, "gain for a quiet track");
    std::vector<std::int16_t> values{1000, -1000, 30000, -32768, 0};
    apply_gain(values, 20.0 * std::log10(2.0));
    check(values == std::vector<std::int16_t>{2000, -2000, 32767, -32768, 0}, "apply_gain doubles and saturates");
    auto sine = tone(-6.0, 20.0, 44100u, 2u);
    const double before = loudness(sine, 2u, 44100u);
    apply_gain(sine, level_gain(before, peak_dbfs(sine)));
    close_to(loudness(sine, 2u, 44100u), -16.0, 0.05, "a -6 dBFS tone brought to -16 LUFS");

    // Files.
    const std::vector<std::int16_t> pcm{1, -2, 3, -4};
    const auto wav = wav_bytes(pcm, 2u, 44100u);
    check(wav.size() == 52u && std::memcmp(wav.data(), "RIFF", 4) == 0 && le32(wav, 4) == 44u && std::memcmp(wav.data() + 8, "WAVEfmt ", 8) == 0 &&
          le16(wav, 20) == 1u && le16(wav, 22) == 2u && le32(wav, 24) == 44100u && le32(wav, 28) == 176400u && le16(wav, 32) == 4u &&
          le16(wav, 34) == 16u && std::memcmp(wav.data() + 36, "data", 4) == 0 && le32(wav, 40) == 8u && le16(wav, 44) == 1u && le16(wav, 46) == 0xFFFEu,
          "wav_bytes header and samples");
    Result r{"menu", 2u, 44100u, 10101105u, 0u, 10101105u, -16.0, -3.66, -4.01};
    check(loop_text(r) == "track=menu\nchannels=2\nrate=44100\nframes=10101105\nloop_start=0\nloop_end=10101105\nlufs=-16.00\ngain_db=-3.66\npeak_dbfs=-4.01\n", "loop_text");
    check(loop_file(L"C:\\t\\music.wav") == std::filesystem::path(L"C:\\t\\music.loop.txt") && loop_file(L"C:\\t\\music") == std::filesystem::path(L"C:\\t\\music.loop.txt"), "loop_file name");
}

struct Run { int code{-1}; std::string out; };
// Runs the tool hidden with stdout captured (stderr goes to this test's own stderr).
Run run(const std::filesystem::path &exe, const std::vector<std::wstring> &args) {
    std::wstring line = L"\"" + exe.wstring() + L"\"";
    for (const auto &arg : args) line += L" \"" + arg + L"\"";
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE read_end{}, write_end{};
    Run result;
    if (!CreatePipe(&read_end, &write_end, &inherit, 0)) return result;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW start{sizeof start};
    start.dwFlags = STARTF_USESTDHANDLES;
    start.hStdOutput = write_end; start.hStdError = GetStdHandle(STD_ERROR_HANDLE); start.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> command(line.begin(), line.end()); command.push_back(0);
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &start, &process)) {
        CloseHandle(write_end); write_end = nullptr;
        char buffer[4096]; DWORD got = 0;
        while (ReadFile(read_end, buffer, sizeof buffer, &got, nullptr) && got) result.out.append(buffer, got);
        WaitForSingleObject(process.hProcess, 120000);
        DWORD code = 0; GetExitCodeProcess(process.hProcess, &code); result.code = static_cast<int>(code);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
    }
    if (write_end) CloseHandle(write_end);
    CloseHandle(read_end);
    return result;
}
std::map<std::string, std::string> fields(const std::string &line) {
    std::map<std::string, std::string> out;
    std::istringstream words(line);
    for (std::string word; words >> word;)
        if (const auto at = word.find('='); at != std::string::npos) out[word.substr(0, at)] = word.substr(at + 1);
    return out;
}
std::string read_text(const std::filesystem::path &file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void write_file(const std::filesystem::path &file, const std::string &bytes) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << bytes;
}

void test_tool(const std::filesystem::path &scratch, const std::filesystem::path &exe) {
    const auto out = scratch / L"out.wav";
    const auto nothing_left = [&] {
        return !std::filesystem::exists(out) && !std::filesystem::exists(loop_file(out)) &&
               !std::filesystem::exists(scratch / L"out.wav.part") && !std::filesystem::exists(scratch / L"out.loop.txt.part");
    };
    check(run(exe, {}).code == exit_usage, "no arguments: usage");
    check(run(exe, {(scratch / L"x").wstring(), out.wstring(), L"--track", L"radio"}).code == exit_usage, "unknown track: usage");
    check(run(exe, {(scratch / L"no such disc.iso").wstring(), out.wstring()}).code == exit_disc_unreadable && nothing_left(), "missing disc");
    write_file(scratch / L"not a disc.iso", std::string(64u * 1024u, 'x'));
    check(run(exe, {(scratch / L"not a disc.iso").wstring(), out.wstring()}).code == exit_disc_unreadable && nothing_left(), "a file that is no disc image");
    write_file(scratch / L"CISO.iso", "CISO" + std::string(64u * 1024u, '\0'));
    check(run(exe, {(scratch / L"CISO.iso").wstring(), out.wstring()}).code == exit_disc_unreadable && nothing_left(), "a compressed image");
    write_file(scratch / L"other" / L"UMD_DATA.BIN", "ULES-00000|0000000000000000|0001|G");
    check(run(exe, {(scratch / L"other").wstring(), out.wstring()}).code == exit_wrong_disc && nothing_left(), "another game's disc folder");
    std::filesystem::create_directories(scratch / L"empty");
    check(run(exe, {(scratch / L"empty").wstring(), out.wstring()}).code == exit_wrong_disc && nothing_left(), "a folder that is no disc");
    // The right id, but no archive and a wrong title clip: made up, no game data.
    write_file(scratch / L"made up" / L"UMD_DATA.BIN", "ULUS-10216|0000000000000000|0001|G");
    check(run(exe, {(scratch / L"made up").wstring(), out.wstring()}).code == exit_track_missing && nothing_left(), "no assets00.naz");
    write_file(scratch / L"made up" / L"PSP_GAME" / L"USRDIR" / L"assets00.naz", std::string(4096u, '\0'));
    check(run(exe, {(scratch / L"made up").wstring(), out.wstring()}).code == exit_disc_unreadable && nothing_left(), "a damaged assets00.naz");
    const auto clip = stream(100u, 376u, 100u * 2048u - 3506u - 368u, 3506u, -1, -1, 1u);
    write_file(scratch / L"made up" / L"PSP_GAME" / L"SND0.AT3", std::string(clip.begin(), clip.end()));
    check(run(exe, {(scratch / L"made up").wstring(), out.wstring(), L"--track", L"title"}).code == exit_track_missing && nothing_left(), "a title clip that is not the game's");
}

// The developer's own disc: both tracks exported, their numbers and the loop, the level change.
void test_disc(const std::filesystem::path &scratch, const std::filesystem::path &exe, const std::filesystem::path &iso) {
    struct Expect { const wchar_t *track; std::size_t frames; };
    for (const auto &expect : {Expect{L"menu", 10101105u}, Expect{L"title", 1366238u}}) {
        const auto name = std::wstring(expect.track);
        const auto out = scratch / (L"disc_" + name + L".wav"), raw = scratch / (L"disc_" + name + L"_raw.wav");
        const auto ran = run(exe, {iso.wstring(), out.wstring(), L"--track", name});
        auto f = fields(ran.out);
        const std::string label = std::filesystem::path(name).string();
        check(ran.code == exit_ok && ran.out.rfind("ok ", 0) == 0, label + ": exit 0, ok line");
        check(f["channels"] == "2" && f["rate"] == "44100" && f["samples"] == std::to_string(expect.frames), label + ": channels, rate, samples");
        check(f["loop"] == "0," + std::to_string(expect.frames), label + ": the loop is the whole track");
        close_to(std::atof(f["lufs"].c_str()), -16.0, 0.05, label + ": loudness");
        check(std::atof(f["peak_dbfs"].c_str()) <= -1.0, label + ": peak at most -1 dBFS");
        std::error_code size_error;
        check(std::filesystem::file_size(out, size_error) == 44u + expect.frames * 4u, label + ": WAV size");
        check(read_text(loop_file(out)).find("loop_start=0\nloop_end=" + std::to_string(expect.frames) + "\n") != std::string::npos, label + ": loop file");
        const auto plain = run(exe, {iso.wstring(), raw.wstring(), L"--track", name, L"--raw"});
        check(plain.code == exit_ok && fields(plain.out)["gain_db"] == "0.00", label + ": --raw keeps the decoder's samples");
        // The levelled file is the raw one times the gain (printed to 0.01 dB), to the nearest sample.
        const auto x = read_text(out), y = read_text(raw);
        const double g = std::pow(10.0, std::atof(f["gain_db"].c_str()) / 20.0);
        double worst = 0.0;
        for (std::size_t i = 44u; i + 1u < std::min(x.size(), y.size()); i += 2u) {
            const auto level = static_cast<std::int16_t>(static_cast<std::uint8_t>(x[i]) | (static_cast<std::uint8_t>(x[i + 1u]) << 8));
            const auto source = static_cast<std::int16_t>(static_cast<std::uint8_t>(y[i]) | (static_cast<std::uint8_t>(y[i + 1u]) << 8));
            worst = std::max(worst, std::abs(level - source * g) - 0.001 * std::abs(source * g));
        }
        check(x.size() == y.size() && worst <= 1.0, label + ": levelled = raw x gain");
        std::error_code ignore;
        for (const auto &file : {out, raw, loop_file(out), loop_file(raw)}) std::filesystem::remove(file, ignore);
    }
}
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 3) { std::cerr << "usage: chili_sound_export_test <scratch folder> <ChiliSoundExport.exe> [<the game's .iso>]\n"; return 2; }
    const std::filesystem::path scratch = argv[1], exe = argv[2];
    std::error_code ignore;
    std::filesystem::remove_all(scratch, ignore);
    std::filesystem::create_directories(scratch);
    test_parts();
    test_tool(scratch, exe);
    if (argc > 3) test_disc(scratch, exe, argv[3]);
    std::filesystem::remove_all(scratch, ignore);
    std::cout << (failures ? "FAILED " : "passed ") << (checks - failures) << " of " << checks << " checks" << (argc > 3 ? " (with the disc)" : " (no disc given)") << '\n';
    return failures ? 1 : 0;
}
