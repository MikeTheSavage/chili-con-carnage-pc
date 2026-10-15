#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
namespace chili {
// PSPRECOMP_LOG=sound (or all): one "[sound]" line per sound start, for telling which firmware path
// carries a sound: a mixer voice keyed on (sceSasCore, ADPCM data in guest memory) or a stream handed
// to the ATRAC3plus decoder (sceAtrac). Read once per process; off, the trace costs one test per start.
inline bool sound_log_enabled() noexcept {
    static const bool enabled = [] {
        const char *value = std::getenv("PSPRECOMP_LOG");
        return value && *value && (std::strstr(value, "sound") || std::strstr(value, "all"));
    }();
    return enabled;
}
// Frames of sound the game has handed to the output so far. A trace line carries it as seconds
// ("out="): the place in a capture of a deterministic run (CHILI_AUDIO_WAV, blocks back to back)
// near which the sound starts. The mixer runs up to two blocks ahead of it.
inline std::uint64_t &sound_output_frames() noexcept {
    static std::uint64_t frames{};
    return frames;
}
inline double sound_output_seconds() noexcept { return static_cast<double>(sound_output_frames()) / 44100.0; }
// CHILI_SOUND_SOLO, a test switch for measuring one kind of sound in a capture of the output:
//   voices        the streams are silenced, only the mixer voices are heard
//   streams       the mixer voices are silenced, only the streams are heard
//   mono_streams  as streams, and the two-channel streams (music, ambience, cutscene sound) are silenced
//                 too: what is left is the one-channel streams, which is the player's speech
// The silenced sounds still run their full course, so the game behaves as it does without the switch.
enum class SoundSolo : unsigned { All, Voices, Streams, MonoStreams };
inline SoundSolo sound_solo() noexcept {
    static const SoundSolo solo = [] {
        const char *value = std::getenv("CHILI_SOUND_SOLO");
        if (!value || !*value) return SoundSolo::All;
        if (!std::strcmp(value, "voices")) return SoundSolo::Voices;
        if (!std::strcmp(value, "streams")) return SoundSolo::Streams;
        if (!std::strcmp(value, "mono_streams")) return SoundSolo::MonoStreams;
        return SoundSolo::All;
    }();
    return solo;
}
// CRC-32 of the first bytes of a sound's data: with the size it names the asset (tools/sound_assets.py).
inline std::uint32_t sound_key(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0u; bit < 8u; ++bit) crc = (crc >> 1u) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
}
