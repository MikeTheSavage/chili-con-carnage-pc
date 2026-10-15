// ChiliSoundExport: one track of the game's music, read from the player's own disc, as a WAV for the
// installer to play while it works. Nothing of the game's audio ships with the installer; this reads it
// from the disc the player chose, the way the game does.
//
//   ChiliSoundExport <disc.iso or disc folder> <out.wav> [--track menu|title] [--raw]
//
//   menu   (default) the title screen and main menu music, data_stream_psp/sounds/music_no_combo/
//          mainmenu_music_loop inside PSP_GAME/USRDIR/assets00.naz: 229.05 s. It starts with the title
//          screen and runs on through the main menu; at its end the game rewinds it to sample 0, so the
//          loop is the whole track.
//   title  PSP_GAME/SND0.AT3, the 31 s clip the PSP's own menu plays for the disc; its loop is its smpl chunk.
//   --raw  the decoder's samples as they are; without it the track is brought to -16 LUFS (EBU R 128
//          integrated loudness), never above -1 dBFS.
//
// The disc must be Chili Con Carnage, USA (ULUS-10216), and the track must be the bytes the game ships. The
// stream is decoded the way the game's own decoder does it (host/atrac_stream.cpp: FFmpeg's ATRAC3plus
// decoder, one packet per frame, the lead-in of encoder skip + 368 samples cut, as many samples as the fact
// chunk says). Output: <out.wav>, 16-bit PCM with the stream's channels and rate, and beside it
// <out>.loop.txt (track, channels, rate, frames, loop_start, loop_end, lufs, gain_db, peak_dbfs; one
// key=value per line; the loop in frames, end exclusive). stdout gets one line:
//
//   ok channels=2 rate=44100 samples=10101105 loop=0,10101105 track=menu lufs=-16.00 gain_db=-3.66 peak_dbfs=-4.01 ms=650
//
// A failure prints one line on stderr, leaves no output file and exits with a code of sound_export.hpp:
// 1 usage, 2 the disc cannot be read, 3 another disc, 4 the track is missing or differs, 5 decoding failed,
// 6 the output cannot be written.
#include "sound_export.hpp"
#include "launcher.hpp"
#include "naz_archive.hpp"
#include <chrono>
#include <iostream>
#include <optional>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}
namespace {
using namespace chili::sound_export;

struct Track {
    const char *name;
    const char *path;          // in the archive (archive spelling) or on the disc
    bool in_archive;
    std::uint64_t stored_size;
    std::uint32_t stored_crc;  // CRC-32 of the file as stored: a read error or a changed file shows here
    std::uint32_t key;         // the stream's key, as the game's sound log prints it
};
constexpr Track tracks[] = {
    {"menu", "data_stream_psp/sounds/music_no_combo/mainmenu_music_loop.mediastream_data_psp", true, 2763824u, 0xA6AE156Cu, 0xA2A17AE6u},
    {"title", "PSP_GAME/SND0.AT3", false, 251708u, 0x06E2962Du, 0xB87992A2u},
};

std::string utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

// A file's bytes inside a host file (an .iso, or a file of a disc folder). Reads that come up short throw:
// a damaged image or a disc that is gone must not turn into silence or noise.
struct Place {
    std::filesystem::path file;
    std::uint64_t offset{}, size{};
};
class ExactSource final : public chili::vfs::Source {
public:
    explicit ExactSource(Place place) : place_(std::move(place)), in_(place_.file, std::ios::binary) {
        if (!in_) throw Failure(exit_disc_unreadable, "cannot open " + utf8(place_.file));
    }
    [[nodiscard]] std::uint64_t size() const override { return place_.size; }
    std::size_t read(std::uint64_t offset, std::span<std::uint8_t> out) override {
        if (offset >= place_.size) return 0u;
        const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), place_.size - offset));
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(place_.offset + offset));
        in_.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(wanted));
        if (static_cast<std::size_t>(in_.gcount()) != wanted) throw Failure(exit_disc_unreadable, "read error in " + utf8(place_.file));
        return wanted;
    }
    std::vector<std::uint8_t> all(std::uint64_t offset, std::uint64_t size) {
        std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
        if (read(offset, out) != out.size()) throw Failure(exit_disc_unreadable, "read error in " + utf8(place_.file));
        return out;
    }
private:
    Place place_;
    std::ifstream in_;
};

// The player's disc: an .iso read in place (host/disc_image.hpp) or the folder of an unpacked disc.
class Disc {
public:
    explicit Disc(const std::filesystem::path &path) {
        std::error_code status;
        if (std::filesystem::is_directory(path, status)) folder_ = path;
        else if (std::filesystem::is_regular_file(path, status)) {
            try { iso_ = std::make_shared<chili::IsoImage>(path); }
            catch (const std::exception &error) { throw Failure(exit_disc_unreadable, error.what()); }
        } else throw Failure(exit_disc_unreadable, utf8(path) + ": not found");
    }
    [[nodiscard]] std::optional<Place> find(const char *inside) const {
        if (iso_) {
            const auto *entry = iso_->file(inside);
            if (!entry) return std::nullopt;
            return Place{iso_->path(), entry->offset, entry->size};
        }
        const auto file = folder_ / std::filesystem::path(inside).make_preferred();
        std::error_code status;
        const auto size = std::filesystem::file_size(file, status);
        if (status) return std::nullopt;
        return Place{file, 0u, size};
    }
private:
    std::shared_ptr<chili::IsoImage> iso_;
    std::filesystem::path folder_;
};

void check_disc(const Disc &disc) {
    const auto id_file = disc.find("UMD_DATA.BIN");
    if (!id_file || id_file->size > 4096u) throw Failure(exit_wrong_disc, "not a PSP game disc (no UMD_DATA.BIN)");
    const auto bytes = ExactSource(*id_file).all(0u, id_file->size);
    const std::string id(bytes.begin(), std::find(bytes.begin(), bytes.end(), static_cast<std::uint8_t>('|')));
    if (id != chili::launcher::disc_id) throw Failure(exit_wrong_disc, "not Chili Con Carnage, USA (" + std::string(chili::launcher::disc_id) + ")");
}

// The track's file as the disc stores it.
std::vector<std::uint8_t> stored_track(const Disc &disc, const Track &track) {
    std::vector<std::uint8_t> bytes;
    if (track.in_archive) {
        const auto archive = disc.find("PSP_GAME/USRDIR/assets00.naz");
        if (!archive) throw Failure(exit_track_missing, "PSP_GAME/USRDIR/assets00.naz is missing");
        auto source = std::make_shared<ExactSource>(*archive);
        chili::naz::Directory directory;
        try { directory = chili::naz::read_directory(*source); }
        catch (const Failure &) { throw; }
        catch (const std::exception &error) { throw Failure(exit_disc_unreadable, error.what()); }
        const auto found = std::find_if(directory.entries.begin(), directory.entries.end(),
            [&](const chili::naz::Entry &entry) { return chili::naz::normal_name(entry.name) == track.path; });
        if (found == directory.entries.end()) throw Failure(exit_track_missing, std::string("the archive has no ") + track.path);
        if (found->size != track.stored_size) throw Failure(exit_track_missing, std::string(track.path) + " differs from the game's");
        bytes = source->all(found->data_offset(), found->size);
    } else {
        const auto file = disc.find(track.path);
        if (!file) throw Failure(exit_track_missing, std::string(track.path) + " is missing");
        if (file->size != track.stored_size) throw Failure(exit_track_missing, std::string(track.path) + " differs from the game's");
        bytes = ExactSource(*file).all(0u, file->size);
    }
    if (crc32(bytes) != track.stored_crc) throw Failure(exit_track_missing, std::string(track.path) + " differs from the game's (or a read error)");
    return bytes;
}

// FFmpeg's ATRAC3plus decoder fed one frame per packet, as host/atrac_stream.cpp does for the game: the
// same samples the game is handed (checked against a run's CHILI_ATRAC_TAP).
std::vector<std::int16_t> decode(std::span<const std::uint8_t> file, const Atrac &a) {
    struct Codec {
        AVCodecContext *context{}; AVFrame *frame{}; AVPacket *packet{}; SwrContext *convert{};
        ~Codec() { if (convert) swr_free(&convert); if (packet) av_packet_free(&packet); if (frame) av_frame_free(&frame); if (context) avcodec_free_context(&context); }
    } codec;
    const auto fail = [](const std::string &what) -> std::vector<std::int16_t> { throw Failure(exit_decode_failed, what); };
    const auto *decoder = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
    if (!decoder) return fail("FFmpeg has no ATRAC3plus decoder");
    codec.context = avcodec_alloc_context3(decoder); codec.frame = av_frame_alloc(); codec.packet = av_packet_alloc();
    if (!codec.context || !codec.frame || !codec.packet) return fail("out of memory");
    codec.context->sample_rate = static_cast<int>(a.rate);
    codec.context->block_align = static_cast<int>(a.block);
    av_channel_layout_default(&codec.context->ch_layout, static_cast<int>(a.channels));
    if (avcodec_open2(codec.context, decoder, nullptr) < 0) return fail("the decoder does not open");
    std::vector<std::int16_t> out, block(2048u * a.channels);
    out.reserve(std::size_t{a.samples} * a.channels);
    unsigned skip = a.lead_in();
    for (std::size_t at = a.data_offset; at + a.block <= a.data_offset + a.data_size && out.size() < std::size_t{a.samples} * a.channels; at += a.block) {
        if (av_new_packet(codec.packet, static_cast<int>(a.block)) < 0) return fail("out of memory");
        std::memcpy(codec.packet->data, file.data() + at, a.block);
        const int sent = avcodec_send_packet(codec.context, codec.packet);
        av_packet_unref(codec.packet);
        if (sent < 0 || avcodec_receive_frame(codec.context, codec.frame) < 0) return fail("frame " + std::to_string((at - a.data_offset) / a.block) + " does not decode");
        if (codec.frame->nb_samples != 2048 || codec.frame->ch_layout.nb_channels != static_cast<int>(a.channels)) return fail("the decoder gives an unexpected frame");
        if (!codec.convert) {
            AVChannelLayout layout{};
            av_channel_layout_default(&layout, static_cast<int>(a.channels));
            int status = swr_alloc_set_opts2(&codec.convert, &layout, AV_SAMPLE_FMT_S16, static_cast<int>(a.rate), &codec.frame->ch_layout,
                                             static_cast<AVSampleFormat>(codec.frame->format), static_cast<int>(a.rate), 0, nullptr);
            av_channel_layout_uninit(&layout);
            if (status >= 0) status = swr_init(codec.convert);
            if (status < 0) return fail("the sample converter does not open");
        }
        auto *target = reinterpret_cast<std::uint8_t *>(block.data());
        const int converted = swr_convert(codec.convert, &target, 2048, const_cast<const std::uint8_t **>(codec.frame->extended_data), 2048);
        av_frame_unref(codec.frame);
        if (converted != 2048) return fail("the sample converter fails");
        const unsigned cut = std::min(skip, 2048u);
        skip -= cut;
        const auto keep = std::min<std::size_t>(2048u - cut, std::size_t{a.samples} - out.size() / a.channels);
        out.insert(out.end(), block.begin() + cut * a.channels, block.begin() + (cut + keep) * a.channels);
    }
    if (out.size() != std::size_t{a.samples} * a.channels) return fail("the stream ends early");
    return out;
}

constexpr char usage[] = "usage: ChiliSoundExport <disc.iso or disc folder> <out.wav> [--track menu|title] [--raw]";
}

int wmain(int argc, wchar_t **argv) {
    const auto started = std::chrono::steady_clock::now();
    std::filesystem::path wav_path, loop_path;
    bool wrote_wav = false;
    try {
        std::vector<std::filesystem::path> positional;
        std::wstring track_name = L"menu";
        bool raw = false;
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--track" && i + 1 < argc) track_name = argv[++i];
            else if (arg == L"--raw") raw = true;
            else if (arg.size() > 1 && arg[0] == L'-' && arg[1] == L'-') throw Failure(exit_usage, usage);
            else positional.emplace_back(arg);
        }
        if (positional.size() != 2u) throw Failure(exit_usage, usage);
        const Track *track = nullptr;
        for (const auto &candidate : tracks)
            if (track_name == std::filesystem::path(candidate.name).wstring()) track = &candidate;
        if (!track) throw Failure(exit_usage, usage);
        wav_path = positional[1];
        loop_path = loop_file(wav_path);

        const Disc disc(positional[0]);
        check_disc(disc);
        const auto stored = stored_track(disc, *track);
        std::vector<std::uint8_t> stream;
        Atrac atrac;
        try {
            stream = track->in_archive ? plain_stream(stored) : stored;
            atrac = parse_atrac(stream);
        } catch (const std::exception &error) { throw Failure(exit_track_missing, std::string(track->path) + ": " + error.what()); }
        if (atrac.key != track->key) throw Failure(exit_track_missing, std::string(track->path) + " differs from the game's");
        auto pcm = decode(stream, atrac);

        Result result;
        result.track = track->name;
        result.channels = atrac.channels;
        result.rate = atrac.rate;
        result.frames = atrac.samples;
        // The loop: the stream's smpl chunk in output frames, else the whole track (what the game does with
        // the menu music: at its end it rewinds it to sample 0).
        result.loop_start = atrac.looped ? atrac.loop_begin - atrac.skip : 0u;
        result.loop_end = atrac.looped ? std::min<std::size_t>(std::size_t{atrac.loop_end} - atrac.skip + 1u, atrac.samples) : atrac.samples;
        const double measured = loudness(pcm, atrac.channels, atrac.rate);
        result.gain_db = raw ? 0.0 : level_gain(measured, peak_dbfs(pcm));
        if (result.gain_db != 0.0) apply_gain(pcm, result.gain_db);
        result.lufs = result.gain_db != 0.0 ? loudness(pcm, atrac.channels, atrac.rate) : measured;
        result.peak = peak_dbfs(pcm);

        const auto wav = wav_bytes(pcm, atrac.channels, atrac.rate);
        write_whole(wav_path, wav);
        wrote_wav = true;
        const auto text = loop_text(result);
        write_whole(loop_path, std::span(reinterpret_cast<const std::uint8_t *>(text.data()), text.size()));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        std::cout << "ok channels=" << result.channels << " rate=" << result.rate << " samples=" << result.frames
                  << " loop=" << result.loop_start << ',' << result.loop_end << " track=" << result.track
                  << " lufs=" << fixed(result.lufs, 2) << " gain_db=" << fixed(result.gain_db, 2)
                  << " peak_dbfs=" << fixed(result.peak, 2) << " ms=" << ms << std::endl;
        return exit_ok;
    } catch (const std::exception &error) {
        const auto *failure = dynamic_cast<const Failure *>(&error);
        std::error_code ignore;
        if (wrote_wav) std::filesystem::remove(wav_path, ignore);
        std::cerr << "ChiliSoundExport: " << error.what() << std::endl;
        return failure ? failure->code : exit_decode_failed;
    }
}
