#pragma once
#include <cstdint>
#include <ostream>

namespace chili {

// Passive metadata observer. Attach only for an explicit diagnostic opt-in.
// No input provider, guest memory access, clock, callback or scheduling API.
class ControllerSamplingProvenance {
public:
    enum class ReadKind { ReadBufferPositive };
    struct Sample {
        ReadKind kind{ReadKind::ReadBufferPositive};
        std::uint32_t buttons{}, returned_count{}, output{}, ra{};
        std::uint8_t x{128u}, y{128u};
        std::int32_t uid{};
        std::uint64_t guest_us{};
        bool analog{};
    };
    explicit ControllerSamplingProvenance(std::ostream &output) noexcept : output_(output) {}

    // Caller invokes this only after the existing successful guest write/result.
    void sampled(const Sample &sample) noexcept {
        ++successful_reads_;
        const bool changed = !have_last_ || sample.kind != last_.kind ||
            sample.buttons != last_.buttons || sample.x != last_.x ||
            sample.y != last_.y || sample.analog != last_.analog;
        last_ = sample; have_last_ = true;
        if (!changed) return;
        ++state_changes_;
        emit([&] {
            output_ << "[controller-sample] read_kind=read-buffer-positive nid=0x1f803938"
                    << " sample=" << successful_reads_ << " change=" << state_changes_
                    << " guest_us=" << sample.guest_us << " returned=" << sample.returned_count
                    << " buttons=0x" << std::hex << sample.buttons << std::dec
                    << " x=" << unsigned(sample.x) << " y=" << unsigned(sample.y)
                    << " mode=" << (sample.analog ? "analog" : "digital")
                    << " output=0x" << std::hex << sample.output
                    << " ra=0x" << sample.ra << std::dec << " uid=" << sample.uid << '\n';
        });
    }
    std::uint64_t successful_reads() const noexcept { return successful_reads_; }
    std::uint64_t state_changes() const noexcept { return state_changes_; }
    bool output_failed() const noexcept { return failed_; }

private:
    std::ostream &output_;
    Sample last_{};
    std::uint64_t successful_reads_{}, state_changes_{};
    bool have_last_{}, failed_{};
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
