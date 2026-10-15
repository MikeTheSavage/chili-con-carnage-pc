// PSPRecomp contributors, MIT license; see the repository LICENSE.
// Ring/context ABI and decoder setup are adapted from the MIT VCS host. Stream
// ingestion and AU assembly use actual guest PES bytes rather than source-file
// lookup or frame-counter estimates. PSP public ABI: PSPSDK src/mpeg/pspmpeg.h.
#include "mpeg_runtime.hpp"
#include "mpeg_provenance.hpp"
#include "av_runtime.hpp"
#include "thread_runtime.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <array>
#include <deque>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <span>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace chili {
namespace {
constexpr std::uint32_t bad_address = 0x80610103u;
constexpr std::uint32_t invalid_value = 0x806101FEu;
constexpr std::uint32_t no_data = 0x80618001u;
constexpr std::uint32_t decode_error = 0x80628002u;
constexpr std::uint32_t not_initialized = 0x80610100u;
constexpr std::uint32_t packet_size = 2048u;
constexpr std::uint32_t mpeg_work_size = 0x10000u;
constexpr std::size_t maximum_stream_buffer = 8u * 1024u * 1024u;
constexpr std::uint64_t unknown_timestamp = std::numeric_limits<std::uint64_t>::max();

std::uint32_t be32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint32_t value{};
    for (unsigned i = 0; i < 4; ++i) value = (value << 8u) | bytes[offset + i];
    return value;
}
std::uint64_t header_timestamp(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t value{};
    for (unsigned i = 0; i < 6; ++i) value = (value << 8u) | bytes[offset + i];
    return value;
}
bool pes_timestamp(std::span<const std::uint8_t> bytes, std::size_t offset, std::uint64_t &value) {
    if (offset + 5u > bytes.size() || !(bytes[offset] & 1u) ||
        !(bytes[offset + 2u] & 1u) || !(bytes[offset + 4u] & 1u)) return false;
    value = (static_cast<std::uint64_t>((bytes[offset] >> 1u) & 7u) << 30u) |
        (static_cast<std::uint64_t>(bytes[offset + 1u]) << 22u) |
        (static_cast<std::uint64_t>(bytes[offset + 2u] >> 1u) << 15u) |
        (static_cast<std::uint64_t>(bytes[offset + 3u]) << 7u) | (bytes[offset + 4u] >> 1u);
    return true;
}
void write_timestamp(psprecomp::GuestMemory &memory, std::uint32_t address, std::uint64_t value) {
    memory.store32(address, static_cast<std::uint32_t>(value >> 32u));
    memory.store32(address + 4u, static_cast<std::uint32_t>(value));
}
bool aligned_pointer(const psprecomp::GuestMemory &memory, std::uint32_t address, std::size_t size) {
    return address != 0u && (address & 3u) == 0u && memory.contains(address, size);
}

struct Header {
    std::uint32_t version{}, offset{}, size{}, width{}, height{};
    std::uint64_t first_pts{}, last_pts{};
};
bool parse_header(std::span<const std::uint8_t> bytes, Header &header) {
    if (bytes.size() < packet_size || !std::equal(bytes.begin(), bytes.begin() + 4u, "PSMF")) return false;
    header.version = static_cast<std::uint32_t>(bytes[4]) |
        (static_cast<std::uint32_t>(bytes[5]) << 8u) |
        (static_cast<std::uint32_t>(bytes[6]) << 16u) |
        (static_cast<std::uint32_t>(bytes[7]) << 24u);
    if (header.version != 0x32313030u && header.version != 0x33313030u &&
        header.version != 0x34313030u && header.version != 0x35313030u) return false;
    header.offset = be32(bytes, 8u);
    header.size = be32(bytes, 12u);
    header.first_pts = header_timestamp(bytes, 0x54u);
    header.last_pts = header_timestamp(bytes, 0x5Au);
    if (!header.offset || (header.offset & 2047u) || !header.size ||
        header.last_pts < header.first_pts) return false;
    const std::size_t streams = static_cast<std::size_t>(bytes[0x80u]) * 256u + bytes[0x81u];
    if (streams > (bytes.size() - 0x82u) / 16u) return false;
    for (std::size_t i = 0; i < streams; ++i) {
        const auto descriptor = 0x82u + i * 16u;
        if ((bytes[descriptor] & 0xF0u) == 0xE0u) {
            header.width = static_cast<std::uint32_t>(bytes[descriptor + 12u]) * 16u;
            header.height = static_cast<std::uint32_t>(bytes[descriptor + 13u]) * 16u;
            break;
        }
    }
    return header.width <= 512u && header.height <= 512u;
}

struct AccessUnit {
    std::vector<std::uint8_t> bytes;
    std::uint64_t pts{unknown_timestamp}, dts{unknown_timestamp};
    bool key{};
};
struct VideoFrame {
    std::vector<std::uint8_t> rgba;
    unsigned width{}, height{};
    std::uint64_t pts{unknown_timestamp};
};
struct Elementary {
    AVCodecParserContext *parser{};
    AVCodecContext *codec{};
    AVFrame *frame{};
    SwsContext *scaler{};
    SwrContext *resampler{};
    bool opened{}, flushed{};
    unsigned block_align{}, scaler_width{}, scaler_height{};
    int scaler_format{-1};
    std::uint64_t total_input{}, consumed_audio{}, audio_frames{}, anchor_frame{};
    std::uint64_t anchor_pts{unknown_timestamp};
    struct Marker { std::uint64_t offset, pts; };
    std::deque<Marker> audio_markers;
    std::vector<std::uint8_t> audio_bytes;
    std::deque<AccessUnit> units;
    std::deque<VideoFrame> decoded_video;
    std::size_t queued_bytes{};
    ~Elementary() {
        if (parser) av_parser_close(parser);
        if (frame) av_frame_free(&frame);
        if (codec) avcodec_free_context(&codec);
        if (scaler) sws_freeContext(scaler);
        if (resampler) swr_free(&resampler);
    }
    bool setup(bool video) {
        if (codec) return frame && (!video || parser);
        const auto *decoder = avcodec_find_decoder(video ? AV_CODEC_ID_H264 : AV_CODEC_ID_ATRAC3P);
        if (!decoder) return false;
        codec = avcodec_alloc_context3(decoder);
        frame = av_frame_alloc();
        if (video) parser = av_parser_init(AV_CODEC_ID_H264);
        return codec && frame && (!video || parser);
    }
    bool enqueue(AccessUnit unit) {
        if (queued_bytes + unit.bytes.size() > maximum_stream_buffer) return false;
        queued_bytes += unit.bytes.size();
        units.push_back(std::move(unit));
        return true;
    }
    bool append_video(std::span<const std::uint8_t> payload, std::uint64_t pts,
                      std::uint64_t dts, bool end = false) {
        if (!setup(true)) return false;
        std::vector<std::uint8_t> padded(payload.begin(), payload.end());
        padded.resize(payload.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        std::size_t offset{};
        unsigned attempts{};
        do {
            std::uint8_t *parsed{};
            int parsed_size{};
            const int used = av_parser_parse2(parser, codec, &parsed, &parsed_size,
                end ? nullptr : padded.data() + offset,
                static_cast<int>(payload.size() - offset),
                pts == unknown_timestamp ? AV_NOPTS_VALUE : static_cast<std::int64_t>(pts),
                dts == unknown_timestamp ? AV_NOPTS_VALUE : static_cast<std::int64_t>(dts),
                static_cast<std::int64_t>(total_input + offset));
            if (used < 0 || static_cast<std::size_t>(used) > payload.size() - offset ||
                (++attempts > payload.size() + 2u)) return false;
            if (parsed_size > 0) {
                AccessUnit unit;
                unit.bytes.assign(parsed, parsed + parsed_size);
                unit.pts = parser->pts == AV_NOPTS_VALUE ? unknown_timestamp : static_cast<std::uint64_t>(parser->pts);
                unit.dts = parser->dts == AV_NOPTS_VALUE ? unknown_timestamp : static_cast<std::uint64_t>(parser->dts);
                unit.key = parser->key_frame == 1;
                if (!enqueue(std::move(unit))) return false;
            }
            if (end || (used == 0 && parsed_size == 0)) break;
            offset += static_cast<std::size_t>(used);
            // A PES timestamp describes its first frame, not each subsequent
            // frame split out of the same payload by the parser.
            pts = dts = unknown_timestamp;
        } while (offset < payload.size());
        total_input += payload.size();
        return offset == payload.size() || end;
    }
    bool append_audio(std::span<const std::uint8_t> payload, std::uint64_t pts) {
        if (audio_bytes.size() + payload.size() > maximum_stream_buffer) return false;
        if (pts != unknown_timestamp) audio_markers.push_back({total_input, pts});
        total_input += payload.size();
        audio_bytes.insert(audio_bytes.end(), payload.begin(), payload.end());
        std::size_t cursor{};
        while (cursor + 8u <= audio_bytes.size()) {
            const auto *header = audio_bytes.data() + cursor;
            if (header[0] != 0x0Fu || header[1] != 0xD0u) return false;
            // The ten-bit PSMF field encodes eight-byte blocks minus one.
            const auto encoded_size = (static_cast<unsigned>(header[2] & 3u) * 256u + header[3] + 1u) * 8u;
            if (!encoded_size || encoded_size > 2112u) return false;
            if (cursor + 8u + encoded_size > audio_bytes.size()) break;
            while (!audio_markers.empty() && audio_markers.front().offset <= consumed_audio + cursor) {
                anchor_pts = audio_markers.front().pts;
                anchor_frame = audio_frames;
                audio_markers.pop_front();
            }
            AccessUnit unit;
            unit.bytes.assign(header + 8u, header + 8u + encoded_size);
            if (anchor_pts != unknown_timestamp)
                unit.pts = anchor_pts + (audio_frames - anchor_frame) * 2048ull * 90000ull / 44100ull;
            if (!enqueue(std::move(unit))) return false;
            ++audio_frames;
            cursor += 8u + encoded_size;
        }
        if (cursor) {
            audio_bytes.erase(audio_bytes.begin(), audio_bytes.begin() + static_cast<std::ptrdiff_t>(cursor));
            consumed_audio += cursor;
        }
        return true;
    }
    bool open_decoder(bool video, unsigned audio_block = 0u) {
        if (!setup(video)) return false;
        if (opened) return video || block_align == audio_block;
        codec->pkt_timebase = AVRational{1, 90000};
        if (!video) {
            codec->sample_rate = 44100;
            codec->block_align = static_cast<int>(audio_block);
            av_channel_layout_default(&codec->ch_layout, 2);
            block_align = audio_block;
        }
        if (avcodec_open2(codec, codec->codec, nullptr) < 0) return false;
        opened = true;
        return true;
    }
    bool send_unit(const AccessUnit &unit, bool video) {
        if (!open_decoder(video, static_cast<unsigned>(unit.bytes.size()))) return false;
        AVPacket *packet = av_packet_alloc();
        if (!packet) return false;
        const int allocated = av_new_packet(packet, static_cast<int>(unit.bytes.size()));
        if (allocated >= 0) {
            std::copy(unit.bytes.begin(), unit.bytes.end(), packet->data);
            packet->pts = unit.pts == unknown_timestamp ? AV_NOPTS_VALUE : static_cast<std::int64_t>(unit.pts);
            packet->dts = unit.dts == unknown_timestamp ? AV_NOPTS_VALUE : static_cast<std::int64_t>(unit.dts);
            packet->flags = unit.key ? AV_PKT_FLAG_KEY : 0;
        }
        const int sent = allocated < 0 ? allocated : avcodec_send_packet(codec, packet);
        av_packet_free(&packet);
        return sent >= 0;
    }
    bool receive_video() {
        for (;;) {
            const int received = avcodec_receive_frame(codec, frame);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return true;
            if (received < 0 || frame->width <= 0 || frame->height <= 0 ||
                frame->width > 512 || frame->height > 512 || decoded_video.size() >= 32u) return false;
            if (!scaler || scaler_width != static_cast<unsigned>(frame->width) ||
                scaler_height != static_cast<unsigned>(frame->height) || scaler_format != frame->format) {
                if (scaler) sws_freeContext(scaler);
                scaler = sws_getContext(frame->width, frame->height,
                    static_cast<AVPixelFormat>(frame->format), frame->width, frame->height,
                    AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                scaler_width = static_cast<unsigned>(frame->width);
                scaler_height = static_cast<unsigned>(frame->height); scaler_format = frame->format;
            }
            if (!scaler) return false;
            VideoFrame output;
            output.width = static_cast<unsigned>(frame->width);
            output.height = static_cast<unsigned>(frame->height);
            output.pts = frame->pts == AV_NOPTS_VALUE ? unknown_timestamp : static_cast<std::uint64_t>(frame->pts);
            output.rgba.resize(static_cast<std::size_t>(output.width) * output.height * 4u);
            auto *destination = output.rgba.data();
            const int stride = static_cast<int>(output.width * 4u);
            if (sws_scale(scaler, frame->data, frame->linesize, 0, frame->height,
                          &destination, &stride) != frame->height) return false;
            decoded_video.push_back(std::move(output));
            av_frame_unref(frame);
        }
    }
    bool decode_audio(const AccessUnit &unit, bool reset, std::array<std::uint8_t, 8192> &pcm) {
        if (!open_decoder(false, static_cast<unsigned>(unit.bytes.size()))) return false;
        if (reset) {
            avcodec_flush_buffers(codec);
            if (resampler) swr_free(&resampler);
        }
        if (!send_unit(unit, false) || avcodec_receive_frame(codec, frame) < 0) return false;
        if (frame->nb_samples != 2048 || frame->sample_rate != 44100 ||
            frame->ch_layout.nb_channels < 1 || frame->ch_layout.nb_channels > 2) return false;
        if (!resampler) {
            AVChannelLayout output_layout{};
            av_channel_layout_default(&output_layout, 2);
            const int result = swr_alloc_set_opts2(&resampler, &output_layout, AV_SAMPLE_FMT_S16, 44100,
                &frame->ch_layout, static_cast<AVSampleFormat>(frame->format), 44100, 0, nullptr);
            av_channel_layout_uninit(&output_layout);
            if (result < 0 || swr_init(resampler) < 0) return false;
        }
        auto *output = pcm.data();
        const int converted = swr_convert(resampler, &output, 2048,
            const_cast<const std::uint8_t **>(frame->extended_data), frame->nb_samples);
        av_frame_unref(frame);
        return converted == 2048;
    }
};
}

struct MpegRuntime::Impl {
    psprecomp::Runtime &runtime;
    AvRuntime &modules;
    ThreadRuntime *threads;
    bool initialized{};
    struct Ring {
        unsigned packets{}, data{}, callback{}, argument{}, gp{}, attached{},
                 written{}, read_cursor{}, write_cursor{}, used{};
        bool in_callback{};
    };
    struct Stream { unsigned type{}, number{}; };
    struct PendingAu { unsigned type{}, number{}, es_buffer{}; AccessUnit unit; };
    struct Context {
        unsigned handle{}, ring{}, frame_width{}, video_frames{}, last_width{}, last_height{}, last_status{}, last_result{};
        MpegProvenance::Context provenance;
        Header header;
        std::array<std::uint8_t, packet_size> header_bytes{};
        bool analyzed{}, end_flushed{};
        std::array<bool, 2> es_buffers{};
        std::map<unsigned, Stream> streams;
        std::map<std::pair<unsigned, unsigned>, std::unique_ptr<Elementary>> elementary;
        std::map<unsigned, PendingAu> pending;
        std::vector<std::uint8_t> program_bytes;
        std::uint64_t bytes_fed{};
    };
    struct Callback {
        psprecomp::AllegrexContext resume;
        unsigned ring{}, remaining{}, requested{}, accepted{};
    };
    std::map<unsigned, Ring> rings;
    std::map<unsigned, Context> contexts;
    std::map<int, std::vector<Callback>> callbacks;
    std::map<int, std::vector<psprecomp::AllegrexContext>> delays;
    unsigned next_stream{0x70000u};
    bool trace_enabled{std::getenv("PSPRECOMP_MPEG_TRACE") != nullptr};
    unsigned trace_count{};
    unsigned parse_failure_count{};
    bool failure_dumped{};
    std::unique_ptr<MpegProvenance> provenance;
    MpegRuntime::FrameObserver frame_observer;
    static std::map<psprecomp::Runtime *, Impl *> instances;

    Impl(psprecomp::Runtime &rt, AvRuntime &av, ThreadRuntime *scheduler)
        : runtime(rt), modules(av), threads(scheduler) {
        if (const char *setting = std::getenv("PSPRECOMP_MPEG_PROVENANCE")) {
            if (*setting) {
                if (std::string_view(setting) != "1")
                    throw psprecomp::Error("PSPRECOMP_MPEG_PROVENANCE must be 1 or unset");
                provenance = std::make_unique<MpegProvenance>(std::cout);
            }
        }
        if (instances.contains(&rt)) throw psprecomp::Error("Only one MPEG service may bind a runtime");
        instances[&rt] = this;
        modules.set_unload_guard(3u, [this] {
            return !initialized && rings.empty() && contexts.empty() && callbacks.empty() && delays.empty();
        });
        rt.register_function(MpegRuntime::callback_return_trampoline, &callback_returned, "chili_mpeg_callback_return");
        rt.register_function(MpegRuntime::decode_resume_trampoline, &decode_resumed, "chili_mpeg_decode_resume");
        auto bind = [&](unsigned nid, auto method) {
            rt.register_hle("sceMpeg", nid, [this, method, nid](auto &, auto &c) {
                // Opt-in bounded ABI diagnostics. Callbacks and delayed decodes
                // can change the active guest context, so record both identities.
                const bool trace = trace_enabled && trace_count < 4096u;
                const auto caller_uid = trace ? psprecomp::runtime_thread_uid() : -1;
                if (trace) {
                    ++trace_count;
                    std::cout << "[mpeg-trace] call=" << trace_count << " nid=" << psprecomp::hex32(nid)
                              << " uid=" << caller_uid << " ra=" << psprecomp::hex32(c.gpr[31]);
                    for (unsigned arg=4; arg<8; ++arg)
                        std::cout << " r" << arg << '=' << psprecomp::hex32(c.gpr[arg]);
                    std::cout << '\n';
                }
                if (!modules.loaded(3u)) c.set_gpr(2u, not_initialized);
                else (this->*method)(c);
                if (trace)
                    std::cout << "[mpeg-trace] return nid=" << psprecomp::hex32(nid)
                              << " caller_uid=" << caller_uid << " current_uid=" << psprecomp::runtime_thread_uid()
                              << " pc=" << psprecomp::hex32(c.pc) << " v0=" << psprecomp::hex32(c.gpr[2])
                              << " stopped=" << runtime.stopped() << '\n';
                if (trace_enabled && nid == 0xFE246728u && c.gpr[2] == decode_error &&
                    psprecomp::runtime_thread_uid() == caller_uid &&
                    std::getenv("PSPRECOMP_MPEG_STOP_ON_VIDEO_ERROR") != nullptr)
                    runtime.stop("MPEG diagnostic stop on video-AU error");
            });
        };
        bind(0x682A619Bu, &Impl::init); bind(0x874624D6u, &Impl::finish);
        bind(0xD7A29F46u, &Impl::query_ring_size); bind(0xC132E22Fu, &Impl::query_work_size);
        bind(0x37295ED8u, &Impl::construct_ring); bind(0x13407F13u, &Impl::destruct_ring);
        bind(0xB240A59Eu, &Impl::put_ring); bind(0xB5F6DC87u, &Impl::available_ring);
        bind(0xD8C5F121u, &Impl::create); bind(0x606A4649u, &Impl::remove);
        bind(0x21FF80E4u, &Impl::query_offset); bind(0x611E9E11u, &Impl::query_stream_size);
        bind(0x42560F23u, &Impl::register_stream); bind(0x591A4AA2u, &Impl::unregister_stream);
        bind(0xA780CF7Eu, &Impl::allocate_es); bind(0xCEB870B1u, &Impl::free_es);
        bind(0x167AFD9Eu, &Impl::init_au); bind(0xF8DCB679u, &Impl::query_atrac_size);
        bind(0xFE246728u, &Impl::get_video_au); bind(0xE1CE83A7u, &Impl::get_audio_au);
        bind(0x0E3C2E9Du, &Impl::decode_video); bind(0x800C44DFu, &Impl::decode_audio);
        bind(0x740FCCD1u, &Impl::stop_video); bind(0x4571CC64u, &Impl::flush_video);
        bind(0x0F6C18D7u, &Impl::detail_video);
    }
    ~Impl() { modules.set_unload_guard(3u, {}); instances.erase(&runtime); }
    int uid() const { return threads ? threads->current_uid() : psprecomp::runtime_thread_uid(); }
    MpegProvenance::Summary video_summary(const Context &state) const {
        MpegProvenance::Summary summary;
        for (const auto &[key, stream] : state.elementary)
            if (key.first == 0u) summary.queued_video += stream->decoded_video.size();
        for (const auto &[address, pending] : state.pending) {
            (void)address;
            if (pending.type == 0u) ++summary.pending_video;
        }
        summary.bytes_fed = state.bytes_fed;
        summary.guest_video_frames = state.video_frames; summary.last_status = state.last_status;
        return summary;
    }
    bool receive_video(Context &state, Elementary &stream) {
        if (!provenance) return stream.receive_video();
        const auto before = stream.decoded_video.size();
        const bool received = stream.receive_video();
        provenance->decoded(state.provenance, stream.decoded_video.size() - before);
        return received;
    }
    void result(psprecomp::AllegrexContext &c, unsigned value = 0u) { c.set_gpr(2u, value); }
    Context *context(psprecomp::AllegrexContext &c) {
        auto found = contexts.find(c.gpr[4]);
        if (found == contexts.end()) { result(c, invalid_value); return nullptr; }
        if (!aligned_pointer(runtime.memory(), c.gpr[4], 4u) ||
            runtime.memory().load32(c.gpr[4]) != found->second.handle) {
            result(c, invalid_value); return nullptr;
        }
        return &found->second;
    }
    void publish(unsigned address, const Ring &ring) {
        runtime.memory().store32(address + 4u, ring.written);
        runtime.memory().store32(address + 8u, ring.write_cursor);
        runtime.memory().store32(address + 12u, ring.used);
    }
    void init(psprecomp::AllegrexContext &c) {
        if (!avcodec_find_decoder(AV_CODEC_ID_H264) || !avcodec_find_decoder(AV_CODEC_ID_ATRAC3P)) {
            result(c, decode_error); return;
        }
        initialized = true; result(c);
    }
    void finish(psprecomp::AllegrexContext &c) {
        if (!contexts.empty() || !callbacks.empty() || !delays.empty()) { result(c, invalid_value); return; }
        initialized = false; result(c);
        if (provenance) provenance->finished(rings.size());
    }
    void query_ring_size(psprecomp::AllegrexContext &c) {
        const auto count = static_cast<std::int32_t>(c.gpr[4]);
        const auto bytes = static_cast<std::uint64_t>(c.gpr[4]) * (packet_size + 104u);
        result(c, count < 0 || bytes > 0x7FFFFFFFu ? invalid_value : static_cast<unsigned>(bytes));
    }
    void query_work_size(psprecomp::AllegrexContext &c) { result(c, c.gpr[4] ? invalid_value : mpeg_work_size); }
    void construct_ring(psprecomp::AllegrexContext &c) {
        const auto address = c.gpr[4], count = c.gpr[5], data = c.gpr[6], size = c.gpr[7];
        const auto bytes = static_cast<std::uint64_t>(count) * (packet_size + 104u);
        if (!count || static_cast<std::int32_t>(count) < 0 || bytes > size) { result(c, invalid_value); return; }
        if (!aligned_pointer(runtime.memory(), address, 48u) ||
            !aligned_pointer(runtime.memory(), data, static_cast<std::size_t>(bytes)) ||
            !c.gpr[8] || (c.gpr[8] & 3u)) { result(c, bad_address); return; }
        if (rings.contains(address)) { result(c, invalid_value); return; }
        Ring ring;
        ring.packets = count; ring.data = data; ring.callback = c.gpr[8];
        ring.argument = c.gpr[9]; ring.gp = c.gpr[28];
        rings.emplace(address, ring);
        runtime.memory().zero(address, 48u);
        runtime.memory().store32(address, count);
        runtime.memory().store32(address + 16u, packet_size);
        runtime.memory().store32(address + 20u, data);
        runtime.memory().store32(address + 24u, ring.callback);
        runtime.memory().store32(address + 28u, ring.argument);
        runtime.memory().store32(address + 32u, data + count * packet_size);
        runtime.memory().store32(address + 44u, ring.gp);
        result(c);
    }
    void destruct_ring(psprecomp::AllegrexContext &c) {
        auto found = rings.find(c.gpr[4]);
        if (found == rings.end() || found->second.attached || found->second.in_callback) { result(c, invalid_value); return; }
        rings.erase(found); result(c);
    }
    void available_ring(psprecomp::AllegrexContext &c) {
        auto found = rings.find(c.gpr[4]);
        if (found == rings.end()) { result(c, invalid_value); return; }
        result(c, found->second.packets - found->second.used);
    }
    void invoke_callback(psprecomp::AllegrexContext &c, Callback &frame, Ring &ring) {
        const unsigned offset = ring.write_cursor % ring.packets;
        frame.requested = std::min({frame.remaining, ring.packets - offset, ring.packets - ring.used});
        c = frame.resume;
        c.gpr[4] = ring.data + offset * packet_size;
        c.gpr[5] = frame.requested; c.gpr[6] = ring.argument; c.gpr[28] = ring.gp;
        c.gpr[31] = MpegRuntime::callback_return_trampoline; c.pc = ring.callback;
    }
    void put_ring(psprecomp::AllegrexContext &c) {
        auto found = rings.find(c.gpr[4]);
        if (found == rings.end() || found->second.in_callback || !found->second.attached) { result(c, invalid_value); return; }
        if (static_cast<std::int32_t>(c.gpr[5]) < 0 || static_cast<std::int32_t>(c.gpr[6]) < 0) {
            result(c, invalid_value); return;
        }
        if (threads && !threads->can_wait()) { result(c, 0x800201A7u); return; }
        auto &ring = found->second;
        Callback frame;
        frame.ring = c.gpr[4]; frame.resume = c; frame.resume.pc = c.gpr[31];
        frame.remaining = std::min({c.gpr[5], c.gpr[6], ring.packets - ring.used});
        if (!frame.remaining) { result(c); return; }
        auto &stack = callbacks[uid()];
        if (stack.size() >= 8u) { result(c, invalid_value); return; }
        stack.push_back(std::move(frame)); ring.in_callback = true;
        invoke_callback(c, stack.back(), ring);
    }
    static void callback_returned(psprecomp::Runtime &rt, psprecomp::AllegrexContext &c) {
        auto *self = instances.at(&rt);
        auto found = self->callbacks.find(self->uid());
        if (found == self->callbacks.end() || found->second.empty()) {
            rt.stop("MPEG callback return without a saved thread context"); return;
        }
        auto &frame = found->second.back();
        auto ring_found = self->rings.find(frame.ring);
        if (ring_found == self->rings.end()) { rt.stop("MPEG callback returned to a destroyed ring"); return; }
        auto &ring = ring_found->second;
        const auto returned = static_cast<std::int32_t>(c.gpr[2]);
        const unsigned accepted = returned > 0 ? std::min(static_cast<unsigned>(returned), frame.requested) : 0u;
        ring.used += accepted; ring.written += accepted; ring.write_cursor += accepted;
        frame.accepted += accepted; frame.remaining -= accepted;
        self->publish(frame.ring, ring);
        if (accepted == frame.requested && frame.remaining && ring.used < ring.packets) {
            self->invoke_callback(c, frame, ring); return;
        }
        c = frame.resume;
        c.set_gpr(2u, returned < 0 && !frame.accepted ? static_cast<unsigned>(returned) : frame.accepted);
        ring.in_callback = false;
        found->second.pop_back(); if (found->second.empty()) self->callbacks.erase(found);
    }
    void create(psprecomp::AllegrexContext &c) {
        const auto output = c.gpr[4], data = c.gpr[5], size = c.gpr[6], ring_address = c.gpr[7];
        if (!initialized) { result(c, not_initialized); return; }
        if (!aligned_pointer(runtime.memory(), output, 4u) || size < mpeg_work_size ||
            !aligned_pointer(runtime.memory(), data, size)) { result(c, bad_address); return; }
        auto ring = rings.find(ring_address);
        if (ring == rings.end() || ring->second.attached || contexts.contains(output) ||
            c.gpr[9] || c.gpr[10] || c.gpr[8] > 512u) { result(c, invalid_value); return; }
        Context state;
        state.handle = data + 0x30u; state.ring = ring_address; state.frame_width = c.gpr[8];
        runtime.memory().store32(output, state.handle);
        const std::array<std::uint8_t, 12> magic{'L','I','B','M','P','E','G',0,'0','0','1',0};
        runtime.memory().copy_in(state.handle, magic);
        runtime.memory().store32(state.handle + 12u, 0xFFFFFFFFu);
        runtime.memory().store32(state.handle + 16u, ring_address);
        runtime.memory().store32(state.handle + 20u, runtime.memory().load32(ring_address + 32u));
        runtime.memory().store32(ring_address + 40u, output);
        ring->second.attached = output;
        contexts.emplace(output, std::move(state)); result(c);
        if (provenance) provenance->created(contexts.at(output).provenance, output, contexts.at(output).handle);
    }
    void remove(psprecomp::AllegrexContext &c) {
        auto *state = context(c); if (!state) return;
        auto &ring = rings.at(state->ring);
        const bool awaiting_decode_return = std::any_of(delays.begin(), delays.end(), [&](const auto &entry) {
            return std::any_of(entry.second.begin(), entry.second.end(), [&](const auto &saved) {
                return saved.gpr[4] == c.gpr[4];
            });
        });
        if (ring.in_callback || awaiting_decode_return) { result(c, invalid_value); return; }
        ring.attached = 0u; runtime.memory().store32(state->ring + 40u, 0u);
        if (provenance) provenance->summary(state->provenance, "delete", video_summary(*state));
        contexts.erase(c.gpr[4]); result(c);
    }
    void query_offset(psprecomp::AllegrexContext &c) {
        auto *state = context(c); if (!state) return;
        if (!aligned_pointer(runtime.memory(), c.gpr[5], packet_size) ||
            !aligned_pointer(runtime.memory(), c.gpr[6], 4u)) { result(c, bad_address); return; }
        std::array<std::uint8_t, packet_size> bytes{};
        runtime.memory().copy_out(c.gpr[5], bytes);
        Header header;
        if (!parse_header(bytes, header)) { runtime.memory().store32(c.gpr[6], 0u); result(c, invalid_value); return; }
        if (!state->analyzed || state->header_bytes != bytes) {
            if (provenance && state->analyzed) {
                provenance->summary(state->provenance, "header-replaced", video_summary(*state));
                provenance->next_segment(state->provenance);
            }
            state->elementary.clear(); state->pending.clear(); state->program_bytes.clear();
            state->bytes_fed = 0u; state->end_flushed = false; state->video_frames = 0u;
            state->last_status = state->last_result = state->last_width = state->last_height = 0u;
        }
        state->header = header; state->header_bytes = bytes; state->analyzed = true;
        runtime.memory().store32(c.gpr[6], header.offset); result(c);
    }
    void query_stream_size(psprecomp::AllegrexContext &c) {
        if (!aligned_pointer(runtime.memory(), c.gpr[4], packet_size) ||
            !aligned_pointer(runtime.memory(), c.gpr[5], 4u)) { result(c, bad_address); return; }
        std::array<std::uint8_t, packet_size> bytes{}; runtime.memory().copy_out(c.gpr[4], bytes);
        Header header;
        if (!parse_header(bytes, header)) { runtime.memory().store32(c.gpr[5], 0u); result(c, invalid_value); return; }
        runtime.memory().store32(c.gpr[5], header.size); result(c);
    }
    void register_stream(psprecomp::AllegrexContext &c) {
        auto *state = context(c); if (!state) return;
        const unsigned type = c.gpr[5] == 15u ? 1u : c.gpr[5];
        if (type > 1u || c.gpr[6] > 15u) { result(c, invalid_value); return; }
        const unsigned id = next_stream++;
        state->streams.emplace(id, Stream{type, c.gpr[6]}); result(c, id);
    }
    void unregister_stream(psprecomp::AllegrexContext &c) {
        auto *state = context(c); if (!state) return;
        auto found = state->streams.find(c.gpr[5]);
        if (found == state->streams.end()) { result(c, invalid_value); return; }
        const auto stream = found->second; state->streams.erase(found);
        std::erase_if(state->pending, [stream](const auto &entry) {
            return entry.second.type == stream.type && entry.second.number == stream.number;
        });
        result(c);
    }
    void allocate_es(psprecomp::AllegrexContext &c) {
        auto *state = context(c); if (!state) return;
        for (unsigned i = 0; i < state->es_buffers.size(); ++i) if (!state->es_buffers[i]) {
            state->es_buffers[i] = true; result(c, i + 1u); return;
        }
        result(c, 0u);
    }
    void free_es(psprecomp::AllegrexContext &c) {
        auto *state = context(c); if (!state) return;
        const auto id = c.gpr[5];
        if (id < 1u || id > 2u || !state->es_buffers[id - 1u]) { result(c, invalid_value); return; }
        state->es_buffers[id - 1u] = false;
        std::erase_if(state->pending, [id](const auto &entry) { return entry.second.type == 0u && entry.second.es_buffer == id; });
        result(c);
    }
    void init_au(psprecomp::AllegrexContext &c) {
        auto *state = context(c); if (!state) return;
        const auto es = c.gpr[5], address = c.gpr[6];
        if (!aligned_pointer(runtime.memory(), address, 24u)) { result(c, bad_address); return; }
        const bool video = es >= 1u && es <= 2u && state->es_buffers[es - 1u];
        if (!video && !aligned_pointer(runtime.memory(), es, 2112u)) { result(c, bad_address); return; }
        runtime.memory().zero(address, 24u);
        if (!video) write_timestamp(runtime.memory(), address + 8u, unknown_timestamp);
        runtime.memory().store32(address + 16u, es);
        runtime.memory().store32(address + 20u, video ? 2048u : 2112u);
        state->pending.erase(address); result(c);
    }
    void query_atrac_size(psprecomp::AllegrexContext &c) {
        if (!context(c)) return;
        if (!aligned_pointer(runtime.memory(), c.gpr[5], 4u) ||
            !aligned_pointer(runtime.memory(), c.gpr[6], 4u)) { result(c, bad_address); return; }
        runtime.memory().store32(c.gpr[5], 2112u); runtime.memory().store32(c.gpr[6], 8192u); result(c);
    }
    Elementary &elementary(Context &state, unsigned type, unsigned number) {
        auto &entry = state.elementary[{type, number}];
        if (!entry) entry = std::make_unique<Elementary>();
        return *entry;
    }
    bool parse_program(Context &state) {
        auto &bytes = state.program_bytes;
        std::size_t cursor{};
        const auto failed = [&](unsigned line, std::size_t at) {
            if (trace_enabled && parse_failure_count++ < 32u) {
                std::cout << "[mpeg-parse-failure] line=" << line << " cursor=" << at
                          << " buffered=" << bytes.size() << " fed=" << state.bytes_fed
                          << " stream_size=" << state.header.size << " next=";
                for (std::size_t i=at; i<std::min(bytes.size(),at+16u); ++i)
                    std::cout << psprecomp::hex32(bytes[i]) << ' ';
                std::cout << '\n';
            }
            if (!failure_dumped) {
                if (const char *prefix=std::getenv("PSPRECOMP_MPEG_FAILURE_DUMP")) { if (*prefix) {
                    failure_dumped=true;
                    const auto save=[&](const char *suffix, std::span<const std::uint8_t> data) {
                        std::ofstream output(std::string(prefix)+suffix,std::ios::binary|std::ios::trunc);
                        output.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));
                        output.close();
                        std::cout << "[mpeg-failure-dump] " << suffix << " bytes=" << data.size()
                                  << " saved=" << !output.fail() << '\n';
                    };
                    save("_program.bin",std::span<const std::uint8_t>(bytes).first(std::min(bytes.size(),maximum_stream_buffer)));
                    save("_header.bin",state.header_bytes);
                    const auto &ring=rings.at(state.ring);
                    const auto ring_size=static_cast<std::uint64_t>(ring.packets)*packet_size;
                    if (ring_size && ring_size<=maximum_stream_buffer && runtime.memory().contains(ring.data,static_cast<std::size_t>(ring_size))) {
                        std::vector<std::uint8_t> snapshot(static_cast<std::size_t>(ring_size));
                        runtime.memory().copy_out(ring.data,snapshot); save("_ring.bin",snapshot);
                    }
                }}
            }
            return false;
        };
        while (cursor + 4u <= bytes.size()) {
            if (bytes[cursor] != 0u || bytes[cursor + 1u] != 0u || bytes[cursor + 2u] != 1u) {
                // MPEG pack padding may straddle a ring packet boundary.
                if (bytes[cursor] != 0u && bytes[cursor] != 0xFFu) return failed(__LINE__, cursor);
                ++cursor; continue;
            }
            const unsigned id = bytes[cursor + 3u];
            if (id == 0xBAu) {
                if (cursor + 14u > bytes.size()) break;
                if ((bytes[cursor + 4u] & 0xC0u) != 0x40u) return failed(__LINE__, cursor);
                const auto size = 14u + (bytes[cursor + 13u] & 7u);
                if (cursor + size > bytes.size()) break;
                cursor += size; continue;
            }
            if (id == 0xB9u) { cursor += 4u; continue; }
            if (cursor + 6u > bytes.size()) break;
            unsigned size = static_cast<unsigned>(bytes[cursor + 4u]) * 256u + bytes[cursor + 5u] + 6u;
            if (size == 6u) {
                // Unbounded video PES packets end at the next container start
                // code, not at a H264 NAL start code inside their payload.
                if ((id & 0xF0u) != 0xE0u) return failed(__LINE__, cursor);
                std::size_t next = cursor + 6u;
                while (next + 4u <= bytes.size() && !(bytes[next] == 0u && bytes[next + 1u] == 0u &&
                    bytes[next + 2u] == 1u && bytes[next + 3u] >= 0xB9u)) ++next;
                if (next + 4u > bytes.size()) {
                    if (state.bytes_fed < state.header.size) break;
                    next = bytes.size();
                }
                size = static_cast<unsigned>(next - cursor);
            }
            if (cursor + size > bytes.size()) break;
            const bool video = (id & 0xF0u) == 0xE0u;
            if (video || id == 0xBDu) {
                if (size < 9u || (bytes[cursor + 6u] & 0xC0u) != 0x80u) return failed(__LINE__, cursor);
                const auto header_size = bytes[cursor + 8u];
                if (9u + header_size > size) return failed(__LINE__, cursor);
                auto packet = std::span<const std::uint8_t>(bytes).subspan(cursor, size);
                std::uint64_t pts = unknown_timestamp, dts = unknown_timestamp;
                const unsigned flags = packet[7u] & 0xC0u;
                if (flags == 0x40u || (flags && (header_size < 5u || !pes_timestamp(packet, 9u, pts)))) return failed(__LINE__, cursor);
                if (flags == 0xC0u && (header_size < 10u || !pes_timestamp(packet, 14u, dts))) return failed(__LINE__, cursor);
                if ((flags == 0x80u && (packet[9u] >> 4u) != 2u) ||
                    (flags == 0xC0u && ((packet[9u] >> 4u) != 3u || (packet[14u] >> 4u) != 1u))) return failed(__LINE__, cursor);
                const auto payload = packet.subspan(9u + header_size);
                if (video) {
                    if (!elementary(state, 0u, id & 15u).append_video(payload, pts, dts)) return failed(__LINE__, cursor);
                } else {
                    if (payload.size() < 4u) return failed(__LINE__, cursor);
                    if (payload[0] >= 0x10u) return failed(__LINE__, cursor); // PCM/private codecs are not imported by Chili.
                    if (!elementary(state, 1u, payload[0] & 15u).append_audio(payload.subspan(4u), pts)) return failed(__LINE__, cursor);
                }
            } else if (id != 0xBBu && id != 0xBCu && id != 0xBEu && id != 0xBFu) return failed(__LINE__, cursor);
            cursor += size;
        }
        if (cursor) bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cursor));
        if (state.bytes_fed >= state.header.size && !state.end_flushed) {
            if (!bytes.empty() && !std::all_of(bytes.begin(), bytes.end(), [](auto value) { return value == 0u || value == 0xFFu; })) return failed(__LINE__, cursor);
            for (auto &[key, stream] : state.elementary) {
                if (key.first == 0u && !stream->append_video({}, unknown_timestamp, unknown_timestamp, true)) return failed(__LINE__, cursor);
                if (key.first == 1u && !stream->audio_bytes.empty()) return failed(__LINE__, cursor);
            }
            state.end_flushed = true;
        }
        return bytes.size() <= maximum_stream_buffer;
    }
    unsigned pump(Context &state, Elementary &desired) {
        auto &ring = rings.at(state.ring);
        while (desired.units.empty() && ring.used && state.bytes_fed < state.header.size) {
            const unsigned count = static_cast<unsigned>(std::min<std::uint64_t>(packet_size, state.header.size - state.bytes_fed));
            const unsigned address = ring.data + (ring.read_cursor % ring.packets) * packet_size;
            if (!runtime.memory().contains(address, count)) return bad_address;
            const auto old_size = state.program_bytes.size(); state.program_bytes.resize(old_size + count);
            runtime.memory().copy_out(address, std::span<std::uint8_t>(state.program_bytes).subspan(old_size, count));
            ++ring.read_cursor; --ring.used; state.bytes_fed += count; publish(state.ring, ring);
            if (!parse_program(state)) return decode_error;
        }
        return desired.units.empty() ? no_data : 0u;
    }
    void get_au(psprecomp::AllegrexContext &c, unsigned type) {
        auto *state = context(c); if (!state) return;
        auto registered = state->streams.find(c.gpr[5]);
        if (!state->analyzed || registered == state->streams.end() || registered->second.type != type) { result(c, invalid_value); return; }
        const auto au_address = c.gpr[6], attribute = c.gpr[7];
        if (!aligned_pointer(runtime.memory(), au_address, 24u) ||
            (attribute && !aligned_pointer(runtime.memory(), attribute, 4u))) { result(c, bad_address); return; }
        const unsigned es = runtime.memory().load32(au_address + 16u);
        if (type == 0u && (es < 1u || es > 2u || !state->es_buffers[es - 1u])) { result(c, invalid_value); return; }
        if (type == 1u && !aligned_pointer(runtime.memory(), es, 2112u)) { result(c, bad_address); return; }
        if (state->pending.contains(au_address)) { result(c, invalid_value); return; }
        auto &stream = elementary(*state, type, registered->second.number);
        const auto error = pump(*state, stream);
        if (error) { result(c, error); return; }
        PendingAu pending;
        pending.type = type; pending.number = registered->second.number; pending.es_buffer = es;
        pending.unit = std::move(stream.units.front()); stream.units.pop_front();
        stream.queued_bytes -= pending.unit.bytes.size();
        if (type == 1u) runtime.memory().copy_in(es, pending.unit.bytes);
        write_timestamp(runtime.memory(), au_address, pending.unit.pts);
        write_timestamp(runtime.memory(), au_address + 8u, pending.unit.dts);
        runtime.memory().store32(au_address + 20u, static_cast<unsigned>(pending.unit.bytes.size()));
        if (attribute) runtime.memory().store32(attribute, 0u);
        state->pending.emplace(au_address, std::move(pending)); result(c);
    }
    void get_video_au(psprecomp::AllegrexContext &c) { get_au(c, 0u); }
    void get_audio_au(psprecomp::AllegrexContext &c) { get_au(c, 1u); }
    bool output_video(Context &state, Elementary &stream, unsigned width, unsigned destination_pointer,
                      unsigned status_pointer, unsigned au_address = 0u, unsigned caller_ra = 0u) {
        if (stream.decoded_video.empty()) { runtime.memory().store32(status_pointer, 0u); state.last_status = 0u; return true; }
        const auto &frame = stream.decoded_video.front();
        if (!width) width = state.frame_width ? state.frame_width : frame.width;
        const unsigned destination = runtime.memory().load32(destination_pointer);
        if (width < frame.width || width > 512u || !aligned_pointer(runtime.memory(), destination,
            static_cast<std::size_t>(width) * frame.height * 4u)) return false;
        for (unsigned y = 0; y < frame.height; ++y) {
            runtime.memory().copy_in(destination + y * width * 4u,
                std::span<const std::uint8_t>(frame.rgba).subspan(static_cast<std::size_t>(y) * frame.width * 4u, frame.width * 4u));
        }
        if (frame_observer) frame_observer(destination, width, frame.width, frame.height, frame.rgba);
        if (au_address) write_timestamp(runtime.memory(), au_address, frame.pts);
        state.last_width = frame.width; state.last_height = frame.height;
        state.last_status = state.last_result = 1u; ++state.video_frames;
        if (provenance) provenance->written(state.provenance,
            {destination, width, frame.width, frame.height, caller_ra, uid(),
             threads ? threads->now_us() : 0u, frame.pts, au_address == 0u});
        stream.decoded_video.pop_front(); runtime.memory().store32(status_pointer, 1u); return true;
    }
    void delay_result(psprecomp::AllegrexContext &c, unsigned microseconds) {
        result(c);
        if (!threads) return;
        auto resume = c; resume.pc = c.gpr[31]; delays[uid()].push_back(resume);
        c.gpr[4] = microseconds; c.gpr[31] = MpegRuntime::decode_resume_trampoline;
        runtime.invoke_import("ThreadManForUser", 0xCEADEB47u, c);
    }
    static void decode_resumed(psprecomp::Runtime &rt, psprecomp::AllegrexContext &c) {
        auto *self = instances.at(&rt);
        auto found = self->delays.find(self->uid());
        if (found == self->delays.end() || found->second.empty()) { rt.stop("MPEG decode resume without saved context"); return; }
        c = found->second.back(); found->second.pop_back(); if (found->second.empty()) self->delays.erase(found);
    }
    void decode_video(psprecomp::AllegrexContext &c) {
        auto *state = context(c); if (!state) return;
        const auto au = c.gpr[5], width = c.gpr[6], output = c.gpr[7], status = c.gpr[8];
        if (!aligned_pointer(runtime.memory(), au, 24u) || !aligned_pointer(runtime.memory(), output, 4u) ||
            !aligned_pointer(runtime.memory(), status, 4u)) { result(c, bad_address); return; }
        if (threads && !threads->can_wait()) { result(c, 0x800201A7u); return; }
        auto pending = state->pending.find(au);
        if (pending == state->pending.end() || pending->second.type != 0u) { result(c, no_data); return; }
        auto &stream = elementary(*state, 0u, pending->second.number);
        if (width > 512u || (state->header.width && width && width < state->header.width)) { result(c, invalid_value); return; }
        const auto destination = runtime.memory().load32(output);
        const auto stride = width ? width : (state->frame_width ? state->frame_width : state->header.width);
        if (!stride || !state->header.height || !aligned_pointer(runtime.memory(), destination,
            static_cast<std::size_t>(stride) * state->header.height * 4u)) { result(c, bad_address); return; }
        const bool sent = stream.send_unit(pending->second.unit, true);
        state->pending.erase(pending);
        if (!sent || !receive_video(*state, stream)) {
            state->last_result = decode_error; state->last_status = 0u;
            runtime.memory().store32(status, 0u); result(c, decode_error); return;
        }
        if (!output_video(*state, stream, width, output, status, au, c.gpr[31])) { result(c, bad_address); return; }
        delay_result(c, state->video_frames <= 1u ? 3600u : 5400u);
    }
    void decode_audio(psprecomp::AllegrexContext &c) {
        auto *state = context(c); if (!state) return;
        const auto au = c.gpr[5], output = c.gpr[6], reset = c.gpr[7];
        if (!aligned_pointer(runtime.memory(), au, 24u) || !aligned_pointer(runtime.memory(), output, 8192u)) { result(c, bad_address); return; }
        if (reset > 1u) { result(c, invalid_value); return; }
        if (threads && !threads->can_wait()) { result(c, 0x800201A7u); return; }
        auto pending = state->pending.find(au);
        if (pending == state->pending.end() || pending->second.type != 1u) { result(c, no_data); return; }
        auto &stream = elementary(*state, 1u, pending->second.number);
        const unsigned es = runtime.memory().load32(au + 16u), size = runtime.memory().load32(au + 20u);
        if (es != pending->second.es_buffer || size != pending->second.unit.bytes.size() ||
            !runtime.memory().contains(es, size)) { result(c, bad_address); return; }
        // Encoded ATRAC bytes are guest-visible. Decode the current ES buffer,
        // so guest modifications between GetAu and Decode are observable.
        runtime.memory().copy_out(es, pending->second.unit.bytes);
        std::array<std::uint8_t, 8192> pcm{};
        const bool decoded = stream.decode_audio(pending->second.unit, reset != 0u, pcm);
        state->pending.erase(pending);
        if (!decoded) { result(c, decode_error); return; }
        runtime.memory().copy_in(output, pcm); delay_result(c, 3000u);
    }
    void stop_video(psprecomp::AllegrexContext &c) {
        auto *state = context(c); if (!state) return;
        if (!aligned_pointer(runtime.memory(), c.gpr[6], 4u) || !aligned_pointer(runtime.memory(), c.gpr[7], 4u)) { result(c, bad_address); return; }
        if (threads && !threads->can_wait()) { result(c, 0x800201A7u); return; }
        for (auto &[key, stream] : state->elementary) if (key.first == 0u && stream->opened) {
            if (!stream->flushed) {
                if (avcodec_send_packet(stream->codec, nullptr) < 0 || !receive_video(*state, *stream)) { result(c, decode_error); return; }
                stream->flushed = true;
            }
            if (!stream->decoded_video.empty()) {
                if (!output_video(*state, *stream, c.gpr[5], c.gpr[6], c.gpr[7], 0u, c.gpr[31])) { result(c, bad_address); return; }
                delay_result(c, 5400u); return;
            }
        }
        runtime.memory().store32(c.gpr[7], 0u); state->last_status = 0u; result(c);
        if (provenance) provenance->drained(state->provenance, video_summary(*state));
    }
    void flush_video(psprecomp::AllegrexContext &c) {
        auto *state = context(c); if (!state) return;
        if (provenance) {
            provenance->summary(state->provenance, "flush", video_summary(*state));
            provenance->next_segment(state->provenance);
        }
        std::erase_if(state->pending, [](const auto &entry) { return entry.second.type == 0u; });
        std::erase_if(state->elementary, [](const auto &entry) { return entry.first.first == 0u; });
        state->last_status = state->last_result = 0u; state->video_frames = 0u; result(c);
    }
    void detail_video(psprecomp::AllegrexContext &c) {
        auto *state = context(c); if (!state) return;
        if (!aligned_pointer(runtime.memory(), c.gpr[5], 36u)) { result(c, bad_address); return; }
        const std::array<unsigned, 9> detail{state->last_result, state->video_frames, state->last_width,
            state->last_height, 0u, 0u, 0u, 0u, state->last_status};
        for (unsigned i = 0; i < detail.size(); ++i) runtime.memory().store32(c.gpr[5] + i * 4u, detail[i]);
        result(c);
    }
};

std::map<psprecomp::Runtime *, MpegRuntime::Impl *> MpegRuntime::Impl::instances;
MpegRuntime::MpegRuntime(psprecomp::Runtime &rt, AvRuntime &modules, ThreadRuntime *scheduler)
    : impl_(std::make_unique<Impl>(rt, modules, scheduler)) {}
MpegRuntime::~MpegRuntime() = default;
void MpegRuntime::set_frame_observer(FrameObserver observer) { impl_->frame_observer = std::move(observer); }
}
