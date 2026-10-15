#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>
#include <string_view>

namespace chili {

// Metadata-only observer. It does not access guest memory, decode media, or
// schedule work. Sparse output continues beyond the startup ABI trace budget.
class MpegProvenance {
public:
    static constexpr std::uint64_t unknown_pts = std::numeric_limits<std::uint64_t>::max();
    static constexpr std::uint64_t sample_interval = 120u;
    struct Write {
        unsigned destination{}, stride{}, width{}, height{}, ra{};
        int uid{};
        std::uint64_t guest_us{}, pts{unknown_pts};
        bool drain{};
    };
    struct Context {
        unsigned address{}, handle{};
        std::uint64_t lifetime{}, segment{1u}, decoded{}, written{}, segment_decoded{}, segment_written{};
        Write last{};
        bool have_last{}, drain_reported{};
    };
    struct Summary {
        std::uint64_t queued_video{}, pending_video{}, bytes_fed{};
        unsigned guest_video_frames{}, last_status{};
    };

    explicit MpegProvenance(std::ostream &output) noexcept : output_(output) {}
    void created(Context &context, unsigned address, unsigned handle) noexcept {
        context.address = address; context.handle = handle; context.lifetime = ++created_;
        summary(context, "create", {});
    }
    void decoded(Context &context, std::uint64_t count) noexcept {
        context.decoded += count; context.segment_decoded += count; decoded_ += count;
    }
    void written(Context &context, Write write) noexcept {
        ++context.written; ++context.segment_written; ++written_;
        context.last = write; context.have_last = true; context.drain_reported = false;
        if (context.segment_written <= 3u || context.segment_written % sample_interval == 0u)
            emit([&] { prefix(context, "write"); last_write(context); totals(context); output_ << '\n'; });
    }
    void summary(const Context &context, std::string_view event, Summary state) noexcept {
        emit([&] {
            prefix(context, event); totals(context);
            output_ << " queued_video=" << state.queued_video << " pending_video=" << state.pending_video
                    << " bytes_fed=" << state.bytes_fed << " guest_video_frames=" << state.guest_video_frames
                    << " last_status=" << state.last_status;
            if (context.have_last) last_write(context);
            output_ << '\n';
        });
    }
    void drained(Context &context, Summary state) noexcept {
        if (context.drain_reported) return;
        context.drain_reported = true;
        summary(context, "drain-empty", state);
    }
    // Called only after a genuine successful header replacement or flush.
    // Lifetime/global counters retain all prior segments, including discarded
    // decoded frames. Guest sceMpegAvcDecodeDetail counters remain independent.
    void next_segment(Context &context) noexcept {
        ++context.segment; context.segment_decoded = context.segment_written = 0u;
        context.have_last = context.drain_reported = false;
    }
    void finished(std::size_t retained_rings) noexcept {
        emit([&] {
            output_ << "[mpeg-video] event=finish contexts_created=" << created_
                    << " global_decoded=" << decoded_ << " global_written=" << written_
                    << " retained_rings=" << retained_rings << '\n';
        });
    }
    bool output_failed() const noexcept { return failed_; }
private:
    std::ostream &output_;
    std::uint64_t created_{}, decoded_{}, written_{};
    bool failed_{};
    void prefix(const Context &context, std::string_view event) {
        output_ << "[mpeg-video] event=" << event << " context=" << context.address
                << " handle=" << context.handle << " lifetime=" << context.lifetime
                << " segment=" << context.segment;
    }
    void totals(const Context &context) {
        output_ << " decoded=" << context.decoded << " written=" << context.written
                << " segment_decoded=" << context.segment_decoded << " segment_written=" << context.segment_written
                << " global_decoded=" << decoded_ << " global_written=" << written_;
    }
    void last_write(const Context &context) {
        const auto &write = context.last;
        output_ << " destination=" << write.destination << " stride_pixels=" << write.stride
                << " stride_bytes=" << std::uint64_t(write.stride) * 4u
                << " format=RGBA8888 width=" << write.width << " height=" << write.height
                << " written_bytes=" << std::uint64_t(write.width) * write.height * 4u
                << " uid=" << write.uid << " ra=" << write.ra << " guest_us=" << write.guest_us
                << " drain=" << unsigned(write.drain) << " decoded_frame_pts90k=";
        if (write.pts == unknown_pts) output_ << "unknown";
        else output_ << write.pts;
    }
    template<class Emit> void emit(Emit &&body) noexcept {
        if (failed_) return;
        const auto flags = output_.flags(); const auto width = output_.width();
        try {
            output_.width(0); output_ << std::dec; body();
            output_.flags(flags); output_.width(width);
            if (!output_) failed_ = true;
        } catch (...) {
            failed_ = true;
            try { output_.flags(flags); output_.width(width); } catch (...) {}
        }
    }
};
}
