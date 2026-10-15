#pragma once
#include "psprecomp/guest_memory.hpp"
#include <cstdint>
#include <memory>

namespace chili {
// Guest-buffer-backed RIFF ATRAC stream. ID allocation and PSP thread yielding
// remain in AtracRuntime; this helper never opens or identifies host media files.
class AtracStream {
public:
    AtracStream(psprecomp::GuestMemory &, unsigned codec_type);
    ~AtracStream();
    AtracStream(const AtracStream &)=delete;
    AtracStream &operator=(const AtracStream &)=delete;
    std::uint32_t attach(unsigned buffer,unsigned read_bytes,unsigned buffer_bytes);
    std::uint32_t stream_info(unsigned write_pointer,unsigned available_bytes,unsigned file_offset);
    std::uint32_t add_data(unsigned bytes);
    std::uint32_t decode(unsigned pcm,unsigned samples,unsigned ended,unsigned remaining_frames);
    std::uint32_t remaining(unsigned output);
    std::uint32_t position(unsigned output);
    std::uint32_t reset_info(unsigned sample,unsigned output);
    std::uint32_t reset(unsigned sample,unsigned first_buffer_bytes,unsigned second_buffer_bytes);
    std::uint32_t error_info(unsigned output);
    bool attached() const noexcept;
    unsigned channels() const noexcept;
    unsigned total_samples() const noexcept;
    unsigned loop_start() const noexcept;
    unsigned loop_end() const noexcept;
    // What attach() read from the stream's header, for the sound trace.
    struct Info {unsigned buffer{},capacity{},file_size{},data_offset{},data_size{},block{},channels{},samples{};bool resident{};};
    Info info() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
