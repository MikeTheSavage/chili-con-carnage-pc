// Adapted from the MIT-licensed PSPRecomp VCS profile's generic SAS mixer.
// State and HLE callbacks are isolated per runtime. Reverb/noise fidelity and
// envelope timing require further validation against PSP hardware.
#include "sas_runtime.hpp"
#include "sound_trace.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <utility>
namespace chili {
struct SasRuntime::Impl {
bool loaded{};
// PSPRECOMP_LOG=sound: a line per voice keyed on. CHILI_SOUND_SOLO: the voices are mixed as always
// and left out of the output when a run is to carry the streams alone.
const bool trace{sound_log_enabled()};
const bool voices_silenced{sound_solo() == SoundSolo::Streams || sound_solo() == SoundSolo::MonoStreams};
void bind(psprecomp::Runtime &runtime, const char *library, std::uint32_t nid,
          psprecomp::Runtime::HleFunction handler) {
    runtime.register_hle(library, nid,
        [this, handler = std::move(handler)](auto &rt, auto &ctx) {
            if (!loaded) { rt.stop("SAS call requires loaded native mixer module"); return; }
            handler(rt, ctx);
        });
}

static void set_success(psprecomp::AllegrexContext &c) { c.set_gpr(2, 0u); }
enum class SasVoiceType : std::uint8_t {
    Off,
    Vag,
    Noise,
};

enum class SasEnvelopePhase : std::uint8_t {
    Attack,
    Decay,
    Sustain,
    Release,
    Off,
};

struct SasVoiceState {
    SasVoiceType type{SasVoiceType::Off};
    std::uint32_t data_address{};
    std::int32_t data_size{};
    bool loop{};
    std::int32_t noise_frequency{};
    std::int32_t pitch{0x1000};
    std::int32_t left_volume{0x1000};
    std::int32_t right_volume{0x1000};
    std::int32_t effect_left_volume{0x1000};
    std::int32_t effect_right_volume{0x1000};
    std::array<std::int32_t, 4> adsr_rates{};
    // Attack rises, everything else falls -- the same parity rule that
    // __sceSasSetADSRmode enforces on the game (even for attack, odd for decay
    // and release).  Defaulting all four to zero broke that rule: zero is
    // "linear increase", so a voice configured through __sceSasSetADSR alone,
    // which sets rates and never touches the modes, walked its release phase
    // *upward*.  The envelope pinned at maximum, `height <= 0` never happened,
    // and a keyed-off looping voice -- the vehicle engine -- kept sounding at
    // full volume under the pause menu.
    std::array<std::int32_t, 4> adsr_modes{0, 1, 1, 1};
    std::int32_t sustain_level{};
    std::uint32_t simple_adsr1{};
    std::uint32_t simple_adsr2{};
    SasEnvelopePhase envelope_phase{SasEnvelopePhase::Off};
    std::uint32_t key_on_delay_samples{};
    bool on{};
    bool playing{};
    bool paused{};
    std::uint32_t envelope_height{};
    std::uint64_t total_samples{};
    std::uint64_t remaining_samples{};

    // Stateful VAG decoder.  A voice is frequently re-used by the game, so a
    // KeyOn must rewind all of these fields rather than resuming at the end of
    // the previous sound.
    std::uint32_t decode_offset{};
    std::int32_t history1{};
    std::int32_t history2{};
    std::array<std::int16_t, 28> block_samples{};
    std::uint32_t block_position{28u};
    std::uint32_t loop_start_offset{};
    std::int32_t loop_start_history1{};
    std::int32_t loop_start_history2{};
    bool loop_start_valid{};
    bool finished{};

    // Pitch interpolation keeps a source sample pair alive across grain
    // boundaries.  The previous nearest-neighbour stepping clicked badly on
    // pitched engine/weapon/ambient effects.
    std::int16_t current_sample{};
    std::int16_t next_sample{};
    bool current_sample_valid{};
    bool next_sample_valid{};
    std::uint32_t pitch_accumulator{}; // 12-bit fraction, 0x1000 == one sample

    // Deterministic noise generator for the SAS noise-voice path.
    std::uint32_t noise_lfsr{0x13579BDFu};
    std::uint32_t noise_phase{};
    std::int16_t noise_sample{};
};
struct SasReverbState {
    std::int32_t type{-1};
    std::int32_t delay{};
    std::int32_t feedback{};
    std::uint32_t left_volume{};
    std::uint32_t right_volume{};
    // PSP SAS starts with the dry bus enabled.  Wet processing is opt-in.
    bool dry{true};
    bool wet{};
    // Persistent effect history.  This is intentionally owned by the SAS core
    // rather than rebuilt per grain so effect-only voices do not disappear at
    // grain boundaries and delay tails remain continuous.
    std::vector<std::int32_t> history_left;
    std::vector<std::int32_t> history_right;
    std::size_t history_cursor{};
};

struct SasState {
    bool initialized{};
    std::uint32_t core_address{};
    std::uint32_t grain_size{};
    std::uint32_t max_voices{32u};
    std::uint32_t output_mode{};
    std::uint32_t sample_rate{44100u};
    std::array<SasVoiceState, 32> voices{};
    SasReverbState reverb{};
};

SasState sas_state{};
std::uint64_t sas_core_mix_calls{};
std::uint64_t sas_core_with_mix_calls{};

bool sas_audio_diagnostics_enabled() {
    static const bool enabled = std::getenv("PSPRECOMP_AUDIO_DIAG") != nullptr ||
        std::getenv("PSPRECOMP_SAS_DIAG") != nullptr;
    return enabled;
}

std::size_t sas_playing_voice_count() {
    return static_cast<std::size_t>(std::count_if(
        sas_state.voices.begin(), sas_state.voices.end(),
        [](const SasVoiceState &voice) { return voice.playing && !voice.paused; }));
}

void sas_log_mix_checkpoint(const char *kind, std::uint64_t count) {
    if (!sas_audio_diagnostics_enabled()) return;
    if (count <= 8u || (count % 256u) == 0u) {
        std::cerr << "[sas] " << kind << " call=" << count
                  << " voices=" << sas_playing_voice_count()
                  << " dry=" << sas_state.reverb.dry
                  << " wet=" << sas_state.reverb.wet
                  << " effect_type=" << sas_state.reverb.type
                  << " grain=" << sas_state.grain_size << "\n";
    }
}

static constexpr std::uint32_t kSasErrorInvalidGrain = 0x80420001u;
static constexpr std::uint32_t kSasErrorInvalidMaxVoices = 0x80420002u;
static constexpr std::uint32_t kSasErrorInvalidOutputMode = 0x80420003u;
static constexpr std::uint32_t kSasErrorInvalidSampleRate = 0x80420004u;
static constexpr std::uint32_t kSasErrorBadAddress = 0x80420005u;
static constexpr std::uint32_t kSasErrorInvalidVoice = 0x80420010u;
static constexpr std::uint32_t kSasErrorInvalidNoiseFrequency = 0x80420011u;
static constexpr std::uint32_t kSasErrorInvalidPitch = 0x80420012u;
static constexpr std::uint32_t kSasErrorInvalidAdsrMode = 0x80420013u;
static constexpr std::uint32_t kSasErrorInvalidParameter = 0x80420014u;
static constexpr std::uint32_t kSasErrorInvalidLoop = 0x80420015u;
static constexpr std::uint32_t kSasErrorVoicePaused = 0x80420016u;
static constexpr std::uint32_t kSasErrorInvalidVolume = 0x80420018u;
static constexpr std::uint32_t kSasErrorInvalidAdsrRate = 0x80420019u;
static constexpr std::uint32_t kSasErrorReverbType = 0x80420020u;
static constexpr std::uint32_t kSasErrorReverbFeedback = 0x80420021u;
static constexpr std::uint32_t kSasErrorReverbDelay = 0x80420022u;
static constexpr std::uint32_t kSasErrorReverbVolume = 0x80420023u;
static constexpr std::uint32_t kSasErrorNotInitialized = 0x80420100u;
static constexpr std::uint32_t kSasEnvelopeMaximum = 0x40000000u;

bool sas_valid_core(std::uint32_t core) noexcept {
    return sas_state.initialized && core == sas_state.core_address;
}

SasVoiceState *sas_voice(std::uint32_t core, std::int32_t voice, psprecomp::AllegrexContext &ctx) {
    if (!sas_valid_core(core)) {
        ctx.set_gpr(2, kSasErrorNotInitialized);
        return nullptr;
    }
    if (voice < 0 || voice >= 32) {
        ctx.set_gpr(2, kSasErrorInvalidVoice);
        return nullptr;
    }
    return &sas_state.voices[static_cast<std::size_t>(voice)];
}

void sas_reset_voice_duration(SasVoiceState &voice) noexcept {
    if (voice.type == SasVoiceType::Vag && voice.data_size > 0) {
        // PSP VAG/ADPCM uses 16-byte blocks producing 28 PCM samples.  This is
        // an upper bound for streams with an earlier end marker, but it remains
        // useful for diagnostics and never drives end-of-voice by itself.
        voice.total_samples = static_cast<std::uint64_t>(voice.data_size / 16) * 28u;
        voice.remaining_samples = voice.total_samples;
    } else {
        voice.total_samples = 0u;
        voice.remaining_samples = 0u;
    }
}

void sas_reset_decoder(SasVoiceState &voice) noexcept {
    voice.decode_offset = 0u;
    voice.history1 = 0;
    voice.history2 = 0;
    voice.block_position = 28u;
    voice.loop_start_offset = 0u;
    voice.loop_start_history1 = 0;
    voice.loop_start_history2 = 0;
    voice.loop_start_valid = false;
    voice.finished = false;
    voice.current_sample = 0;
    voice.next_sample = 0;
    voice.current_sample_valid = false;
    voice.next_sample_valid = false;
    voice.pitch_accumulator = 0u;
    voice.noise_lfsr = 0x13579BDFu;
    voice.noise_phase = 0u;
    voice.noise_sample = 0;
    sas_reset_voice_duration(voice);
}

// Envelope rates retain their zero initialization until the guest sets them.
std::int64_t sas_walk_envelope_curve(std::int64_t height, std::int32_t mode,
                                     std::int32_t rate) noexcept {
    const std::int64_t r = std::max<std::int64_t>(0, rate);
    switch (mode) {
    case 0: // linear increase
        return height + r;
    case 1: // linear decrease
        return height - r;
    case 2: // linear bent increase
        return height + (height <= static_cast<std::int64_t>(kSasEnvelopeMaximum) * 3 / 4
            ? r : r / 4);
    case 3: { // exponential decrease
        std::int64_t delta = height - static_cast<std::int64_t>(kSasEnvelopeMaximum);
        delta += ((-delta) * r) >> 32;
        return delta + kSasEnvelopeMaximum - (r + 3) / 4;
    }
    case 4: { // exponential increase
        std::int64_t delta = height - static_cast<std::int64_t>(kSasEnvelopeMaximum);
        delta += ((-delta) * r) >> 32;
        return delta + kSasEnvelopeMaximum + 0x4000;
    }
    case 5: // direct
        return r;
    default:
        return height;
    }
}

std::uint32_t sas_step_envelope(SasVoiceState &voice) noexcept {
    if (!voice.playing) return 0u;

    // Real PSP SAS holds the envelope at zero briefly after KeyOn.  Keeping the
    // delay in samples (rather than grains) avoids a sharp transient and makes
    // the same voice deterministic at every grain size.
    if (voice.key_on_delay_samples != 0u) {
        --voice.key_on_delay_samples;
        voice.envelope_height = 0u;
        return 0u;
    }

    if (!voice.on && voice.envelope_phase != SasEnvelopePhase::Off)
        voice.envelope_phase = SasEnvelopePhase::Release;

    std::int64_t height = voice.envelope_height;
    switch (voice.envelope_phase) {
    case SasEnvelopePhase::Attack:
        height = sas_walk_envelope_curve(height, voice.adsr_modes[0], voice.adsr_rates[0]);
        if (height >= static_cast<std::int64_t>(kSasEnvelopeMaximum) || height < 0) {
            height = kSasEnvelopeMaximum;
            voice.envelope_phase = SasEnvelopePhase::Decay;
        }
        break;
    case SasEnvelopePhase::Decay:
        height = sas_walk_envelope_curve(height, voice.adsr_modes[1], voice.adsr_rates[1]);
        if (height <= voice.sustain_level) {
            height = std::max<std::int64_t>(0, voice.sustain_level);
            voice.envelope_phase = SasEnvelopePhase::Sustain;
        }
        break;
    case SasEnvelopePhase::Sustain:
        height = sas_walk_envelope_curve(height, voice.adsr_modes[2], voice.adsr_rates[2]);
        if (height <= 0) {
            height = 0;
            voice.envelope_phase = SasEnvelopePhase::Release;
        } else if (height > static_cast<std::int64_t>(kSasEnvelopeMaximum)) {
            height = kSasEnvelopeMaximum;
        }
        break;
    case SasEnvelopePhase::Release:
        height = sas_walk_envelope_curve(height, voice.adsr_modes[3], voice.adsr_rates[3]);
        if (height <= 0) {
            height = 0;
            voice.envelope_phase = SasEnvelopePhase::Off;
            voice.playing = false;
        }
        break;
    case SasEnvelopePhase::Off:
        height = 0;
        voice.playing = false;
        break;
    }

    height = std::clamp<std::int64_t>(height, 0, kSasEnvelopeMaximum);
    voice.envelope_height = static_cast<std::uint32_t>(height);
    return voice.envelope_height;
}

std::int32_t sas_simple_rate(std::uint32_t value) noexcept {
    value &= 0x7Fu;
    if (value == 0x7Fu) return 0;
    const std::uint64_t base = static_cast<std::uint64_t>(7u - (value & 3u)) << 26u;
    const std::uint64_t rate = base >> (value >> 2u);
    return static_cast<std::int32_t>(std::max<std::uint64_t>(1u, rate));
}

std::int32_t sas_exponent_rate(std::uint32_t value) noexcept {
    value &= 0x7Fu;
    if (value == 0x7Fu) return 0;
    const std::uint64_t base = static_cast<std::uint64_t>(7u - (value & 3u)) << 24u;
    const std::uint64_t rate = base >> (value >> 2u);
    return static_cast<std::int32_t>(std::max<std::uint64_t>(1u, rate));
}

void sas_decode_simple_adsr(SasVoiceState &voice) noexcept {
    const std::uint32_t a1 = voice.simple_adsr1;
    const std::uint32_t a2 = voice.simple_adsr2;
    voice.adsr_rates[0] = sas_simple_rate(a1 >> 8u);
    voice.adsr_modes[0] = (a1 & 0x8000u) == 0u ? 0 : 2;

    const std::uint32_t decay = (a1 >> 4u) & 0x0Fu;
    voice.adsr_rates[1] = decay == 0u ? 0x7FFFFFFF :
        static_cast<std::int32_t>(0x80000000u >> decay);
    voice.adsr_modes[1] = 3;

    voice.adsr_modes[2] = static_cast<std::int32_t>((a2 >> 14u) & 3u);
    voice.adsr_rates[2] = voice.adsr_modes[2] == 3
        ? sas_exponent_rate(a2 >> 6u) : sas_simple_rate(a2 >> 6u);

    const std::uint32_t release = a2 & 0x1Fu;
    voice.adsr_modes[3] = (a2 & 0x20u) == 0u ? 1 : 3;
    if (release == 31u) {
        voice.adsr_rates[3] = 0;
    } else if (voice.adsr_modes[3] == 1) {
        if (release == 30u) voice.adsr_rates[3] = 0x40000000;
        else if (release == 29u) voice.adsr_rates[3] = 1;
        else voice.adsr_rates[3] = static_cast<std::int32_t>(0x10000000u >> release);
    } else {
        voice.adsr_rates[3] = release == 0u ? 0x7FFFFFFF :
            static_cast<std::int32_t>(0x80000000u >> release);
    }

    voice.sustain_level = static_cast<std::int32_t>(((a1 & 0x0Fu) + 1u) << 26u);
}

// PSP VAG is the PS1 ADPCM format: 16-byte blocks holding 28 four-bit samples
// plus a shift/filter byte and a flags byte. The predictor coefficients are the
// standard table, scaled by 64.
static constexpr std::int32_t kVagFilter0[16] = {
    0, 60, 115, 98, 122, 0, 0, 52, 55, 60, 0, 0, 0, 2, 125, 0
};
static constexpr std::int32_t kVagFilter1[16] = {
    0, 0, -52, -55, -60, 0, 0, 0, -2, -125, 0, -91, 0, -216, -6, -151
};

// Decodes the next 16-byte block into voice.block_samples.  Loop markers follow
// the PSP SAS convention used by VCS: 6 marks the loop start, 3 the loop end,
// and 7 the terminal block.  Marker 1 is also accepted as a terminal block for
// ordinary PSX-style VAG assets.
bool sas_decode_next_block(const psprecomp::GuestMemory &memory, SasVoiceState &voice) {
    if (voice.data_address == 0u || voice.data_size <= 0 || voice.finished) return false;

    const auto rewind_loop = [&]() {
        // PSP SAS keeps the ADPCM predictor history across a loop jump.
        // Resetting it at the marker makes otherwise seamless ambient loops
        // click every time they wrap.
        voice.decode_offset = voice.loop_start_valid ? voice.loop_start_offset : 0u;
        voice.remaining_samples = voice.total_samples;
    };

    if (voice.decode_offset + 16u > static_cast<std::uint32_t>(voice.data_size)) {
        if (!voice.loop) return false;
        rewind_loop();
    }
    const std::uint32_t relative_offset = voice.decode_offset;
    const std::uint32_t base = voice.data_address + relative_offset;
    if (!memory.contains(base, 16u)) return false;

    const std::uint32_t header = memory.aot_load8(base);
    const std::uint32_t flags = memory.aot_load8(base + 1u);
    const std::int32_t history_before_1 = voice.history1;
    const std::int32_t history_before_2 = voice.history2;
    std::int32_t shift = static_cast<std::int32_t>(header & 0x0Fu);
    std::int32_t filter = static_cast<std::int32_t>((header >> 4u) & 0x0Fu);
    // All four predictor bits and all four shift bits are meaningful on the
    // PSP SAS decoder.
    filter &= 0x0F;
    shift &= 0x0F;

    // Flag 7 is a terminal marker block; it does not contribute 28 samples.
    if (flags == 7u) {
        voice.finished = true;
        return false;
    }

    for (std::uint32_t index = 0u; index < 28u; ++index) {
        const std::uint32_t byte = memory.aot_load8(base + 2u + index / 2u);
        const std::uint32_t nibble = (index & 1u) != 0u ? (byte >> 4u) : (byte & 0x0Fu);
        std::int32_t sample = static_cast<std::int32_t>(nibble << 12u);
        if (sample & 0x8000) sample = static_cast<std::int32_t>(sample | 0xFFFF0000u);
        sample >>= shift;
        sample += (voice.history1 * kVagFilter0[filter] +
                   voice.history2 * kVagFilter1[filter] + 32) / 64;
        sample = std::clamp(sample, -32768, 32767);
        voice.block_samples[index] = static_cast<std::int16_t>(sample);
        voice.history2 = voice.history1;
        voice.history1 = sample;
    }

    if (flags == 6u) {
        // Re-entering the loop must restore the predictor state from immediately
        // before the loop-start block, otherwise each pass drifts/clicks.
        voice.loop_start_offset = relative_offset;
        voice.loop_start_history1 = history_before_1;
        voice.loop_start_history2 = history_before_2;
        voice.loop_start_valid = true;
    }

    voice.decode_offset += 16u;
    voice.block_position = 0u;

    if (flags == 3u) {
        if (voice.loop) rewind_loop();
        else voice.finished = true;
    } else if (flags == 1u) {
        voice.finished = true;
    }
    return true;
}

bool sas_fetch_vag_sample(const psprecomp::GuestMemory &memory, SasVoiceState &voice,
                          std::int16_t &sample) {
    if (voice.block_position >= 28u) {
        if (voice.finished || !sas_decode_next_block(memory, voice)) return false;
    }
    sample = voice.block_samples[voice.block_position++];
    if (voice.remaining_samples != 0u) --voice.remaining_samples;
    return true;
}

bool sas_prepare_sample_pair(const psprecomp::GuestMemory &memory, SasVoiceState &voice) {
    if (!voice.current_sample_valid) {
        if (!sas_fetch_vag_sample(memory, voice, voice.current_sample)) return false;
        voice.current_sample_valid = true;
    }
    if (!voice.next_sample_valid) {
        std::int16_t next{};
        if (sas_fetch_vag_sample(memory, voice, next)) {
            voice.next_sample = next;
            voice.next_sample_valid = true;
        }
    }
    return true;
}

std::int32_t sas_render_vag_sample(const psprecomp::GuestMemory &memory, SasVoiceState &voice) {
    if (!sas_prepare_sample_pair(memory, voice)) {
        voice.playing = false;
        voice.on = false;
        voice.envelope_height = 0u;
        return 0;
    }

    const std::int32_t current = voice.current_sample;
    const std::int32_t next = voice.next_sample_valid ? voice.next_sample : current;
    const std::int32_t sample = current +
        ((next - current) * static_cast<std::int32_t>(voice.pitch_accumulator)) / 0x1000;

    const std::uint32_t pitch = voice.pitch < 0 ? 0u : static_cast<std::uint32_t>(voice.pitch);
    voice.pitch_accumulator += pitch;
    while (voice.pitch_accumulator >= 0x1000u && voice.playing) {
        voice.pitch_accumulator -= 0x1000u;
        if (!voice.next_sample_valid) {
            // The current sample was the last one.  It has just been rendered;
            // retire the voice without creating a discontinuous extra zero.
            voice.playing = false;
            voice.on = false;
            voice.envelope_height = 0u;
            break;
        }
        voice.current_sample = voice.next_sample;
        voice.current_sample_valid = true;
        std::int16_t following{};
        if (sas_fetch_vag_sample(memory, voice, following)) {
            voice.next_sample = following;
            voice.next_sample_valid = true;
        } else {
            voice.next_sample_valid = false;
        }
    }
    return sample;
}

std::int32_t sas_render_noise_sample(SasVoiceState &voice) noexcept {
    // Map 0..63 to progressively faster LFSR updates.  Exact spectral shaping is
    // hardware-specific, but implementing the path is crucial: VCS uses SAS
    // noise voices for effects that were previously completely silent.
    voice.noise_phase += static_cast<std::uint32_t>(voice.noise_frequency + 1);
    while (voice.noise_phase >= 64u) {
        voice.noise_phase -= 64u;
        const std::uint32_t feedback =
            ((voice.noise_lfsr >> 0u) ^ (voice.noise_lfsr >> 1u) ^
             (voice.noise_lfsr >> 21u) ^ (voice.noise_lfsr >> 31u)) & 1u;
        voice.noise_lfsr = (voice.noise_lfsr >> 1u) | (feedback << 31u);
        voice.noise_sample = (voice.noise_lfsr & 1u) != 0u ? 12288 : -12288;
    }
    return voice.noise_sample;
}

// Renders one voice into separate dry and effect-send accumulators.  PSP SAS
// has two independent volume pairs per voice.  The old HLE threw the effect
// pair away entirely, so any sound routed only to the wet bus was silent.
void sas_render_voice(const psprecomp::GuestMemory &memory, SasVoiceState &voice,
                      std::vector<std::int32_t> &dry_mix,
                      std::vector<std::int32_t> &effect_send,
                      std::uint32_t frames) {
    if (!voice.playing || voice.paused || voice.type == SasVoiceType::Off) return;

    for (std::uint32_t frame = 0u; frame < frames && voice.playing; ++frame) {
        const bool key_on_delay = voice.key_on_delay_samples != 0u;
        const std::uint32_t envelope = sas_step_envelope(voice);
        if (!voice.playing || key_on_delay) continue;
        // Zero gain silences output but must not freeze the source decoder.
        // A muted finite sound still reaches its end marker; paused voices
        // remain excluded above and retain their decoder position.

        std::int32_t sample = 0;
        if (voice.type == SasVoiceType::Vag)
            sample = sas_render_vag_sample(memory, voice);
        else if (voice.type == SasVoiceType::Noise)
            sample = sas_render_noise_sample(voice);

        const auto accumulate = [&](std::vector<std::int32_t> &target,
                                    std::int32_t left_volume,
                                    std::int32_t right_volume) {
            const std::int64_t left_gain =
                (static_cast<std::int64_t>(left_volume) * envelope) >> 30;
            const std::int64_t right_gain =
                (static_cast<std::int64_t>(right_volume) * envelope) >> 30;
            target[frame * 2u] += static_cast<std::int32_t>((sample * left_gain) >> 12);
            target[frame * 2u + 1u] += static_cast<std::int32_t>((sample * right_gain) >> 12);
        };
        accumulate(dry_mix, voice.left_volume, voice.right_volume);
        accumulate(effect_send, voice.effect_left_volume, voice.effect_right_volume);
    }
}

void sas_render_buses(const psprecomp::GuestMemory &memory, std::uint32_t frames,
                      std::vector<std::int32_t> &dry_mix,
                      std::vector<std::int32_t> &effect_send) {
    dry_mix.assign(static_cast<std::size_t>(frames) * 2u, 0);
    effect_send.assign(static_cast<std::size_t>(frames) * 2u, 0);
    for (auto &voice : sas_state.voices)
        sas_render_voice(memory, voice, dry_mix, effect_send, frames);
}

// A conservative host-side recreation of the SAS effect-send path.  The exact
// PSP presets are hardware-specific, but the important observable semantics are
// preserved here: effectLeft/effectRight feed a persistent wet bus, RevVON can
// select dry/wet independently, RevEVOL scales wet output, and delay/feedback
// persist between grains.  Type OFF is a transparent wet send rather than a
// destructive silent sink; this avoids dropping effect-routed SFX while still
// keeping the default (dry on, wet off) bit-for-bit simple.
void sas_process_effect_send(const std::vector<std::int32_t> &effect_send,
                             std::vector<std::int32_t> &wet_mix,
                             std::uint32_t frames) {
    wet_mix.assign(static_cast<std::size_t>(frames) * 2u, 0);
    if (!sas_state.reverb.wet) return;

    const std::int64_t global_left = sas_state.reverb.left_volume;
    const std::int64_t global_right = sas_state.reverb.right_volume;
    if (sas_state.reverb.type < 0) {
        for (std::uint32_t frame = 0u; frame < frames; ++frame) {
            wet_mix[frame * 2u] = static_cast<std::int32_t>(
                (static_cast<std::int64_t>(effect_send[frame * 2u]) * global_left) >> 12);
            wet_mix[frame * 2u + 1u] = static_cast<std::int32_t>(
                (static_cast<std::int64_t>(effect_send[frame * 2u + 1u]) * global_right) >> 12);
        }
        return;
    }

    // Long enough for every legal delay parameter while keeping the history
    // tiny compared with the rest of the guest runtime.  Preset type changes
    // the base spacing; delay and feedback remain the caller-controlled knobs.
    constexpr std::size_t kEffectHistoryFrames = 16384u;
    auto &reverb = sas_state.reverb;
    if (reverb.history_left.size() != kEffectHistoryFrames) {
        reverb.history_left.assign(kEffectHistoryFrames, 0);
        reverb.history_right.assign(kEffectHistoryFrames, 0);
        reverb.history_cursor = 0u;
    }
    const std::size_t type_offset = static_cast<std::size_t>(std::clamp(reverb.type, 0, 8)) * 73u;
    const std::size_t delay_frames = std::clamp<std::size_t>(
        64u + type_offset + static_cast<std::size_t>(reverb.delay) * 24u,
        1u, kEffectHistoryFrames - 1u);
    const std::int64_t feedback = std::clamp<std::int32_t>(reverb.feedback, 0, 127);

    for (std::uint32_t frame = 0u; frame < frames; ++frame) {
        const std::size_t read_index =
            (reverb.history_cursor + kEffectHistoryFrames - delay_frames) % kEffectHistoryFrames;
        const std::int64_t delayed_left = reverb.history_left[read_index];
        const std::int64_t delayed_right = reverb.history_right[read_index];
        const std::int64_t input_left = effect_send[frame * 2u];
        const std::int64_t input_right = effect_send[frame * 2u + 1u];

        // Wet output includes the current send plus one delayed component.  The
        // feedback line itself is bounded to signed 24-bit-ish headroom so a
        // pathological guest setting cannot accumulate indefinitely.
        const std::int64_t effect_left = input_left + delayed_left;
        const std::int64_t effect_right = input_right + delayed_right;
        const std::int64_t next_left = input_left + (delayed_left * feedback) / 128;
        const std::int64_t next_right = input_right + (delayed_right * feedback) / 128;
        reverb.history_left[reverb.history_cursor] = static_cast<std::int32_t>(
            std::clamp<std::int64_t>(next_left, -0x7FFFFF, 0x7FFFFF));
        reverb.history_right[reverb.history_cursor] = static_cast<std::int32_t>(
            std::clamp<std::int64_t>(next_right, -0x7FFFFF, 0x7FFFFF));
        reverb.history_cursor = (reverb.history_cursor + 1u) % kEffectHistoryFrames;

        wet_mix[frame * 2u] = static_cast<std::int32_t>((effect_left * global_left) >> 12);
        wet_mix[frame * 2u + 1u] = static_cast<std::int32_t>((effect_right * global_right) >> 12);
    }
}

// Mixed output mode: CoreWithMix starts from caller PCM, then the selected SAS
// dry/wet buses are added.  Plain Core uses the same path without caller input.
void sas_mix_into(psprecomp::Runtime &rt, std::uint32_t output, std::uint32_t frames,
                  bool include_input = false,
                  std::uint32_t input_left = 0x1000u,
                  std::uint32_t input_right = 0x1000u) {
    static thread_local std::vector<std::int32_t> dry_mix;
    static thread_local std::vector<std::int32_t> effect_send;
    static thread_local std::vector<std::int32_t> wet_mix;
    sas_render_buses(rt.memory(), frames, dry_mix, effect_send);
    sas_process_effect_send(effect_send, wet_mix, frames);

    for (std::uint32_t frame = 0u; frame < frames; ++frame) {
        std::int64_t l = 0;
        std::int64_t r = 0;
        if (include_input) {
            const auto input_l = static_cast<std::int16_t>(
                rt.memory().aot_load16(output + frame * 4u));
            const auto input_r = static_cast<std::int16_t>(
                rt.memory().aot_load16(output + frame * 4u + 2u));
            l += (static_cast<std::int64_t>(input_l) * input_left) >> 12;
            r += (static_cast<std::int64_t>(input_r) * input_right) >> 12;
        }
        if (sas_state.reverb.dry && !voices_silenced) {
            l += dry_mix[frame * 2u];
            r += dry_mix[frame * 2u + 1u];
        }
        if (sas_state.reverb.wet && !voices_silenced) {
            l += wet_mix[frame * 2u];
            r += wet_mix[frame * 2u + 1u];
        }
        rt.memory().store16(output + frame * 4u, static_cast<std::uint16_t>(
            static_cast<std::int16_t>(std::clamp<std::int64_t>(l, -32768, 32767))));
        rt.memory().store16(output + frame * 4u + 2u, static_cast<std::uint16_t>(
            static_cast<std::int16_t>(std::clamp<std::int64_t>(r, -32768, 32767))));
    }
}

// Raw output mode exposes four non-interleaved planes: dry L, dry R, effect L,
// effect R.  The previous HLE incorrectly treated mode 1 as mono and allocated
// only two bytes per frame, which could both lose send-routed sounds and write
// the wrong guest buffer layout.
void sas_mix_raw(psprecomp::Runtime &rt, std::uint32_t output, std::uint32_t frames) {
    static thread_local std::vector<std::int32_t> dry_mix;
    static thread_local std::vector<std::int32_t> effect_send;
    sas_render_buses(rt.memory(), frames, dry_mix, effect_send);
    const std::uint32_t left_base = output;
    const std::uint32_t right_base = output + frames * 2u;
    const std::uint32_t send_left_base = output + frames * 4u;
    const std::uint32_t send_right_base = output + frames * 6u;
    for (std::uint32_t frame = 0u; frame < frames; ++frame) {
        const auto store = [&](std::uint32_t base, std::int32_t value) {
            rt.memory().store16(base + frame * 2u, static_cast<std::uint16_t>(
                static_cast<std::int16_t>(std::clamp(value, -32768, 32767))));
        };
        store(left_base, dry_mix[frame * 2u]);
        store(right_base, dry_mix[frame * 2u + 1u]);
        store(send_left_base, effect_send[frame * 2u]);
        store(send_right_base, effect_send[frame * 2u + 1u]);
    }
}


explicit Impl(psprecomp::Runtime &runtime) {
    bind(runtime, "sceSasCore", 0x42778A9Fu,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t core = ctx.gpr[4];
            const std::uint32_t grain = ctx.gpr[5];
            const std::uint32_t max_voices = ctx.gpr[6];
            const std::uint32_t output_mode = ctx.gpr[7];
            const std::uint32_t sample_rate = ctx.gpr[8];
            if ((core & 0x3Fu) != 0u || !rt.memory().contains(core, 64u)) {
                ctx.set_gpr(2, kSasErrorBadAddress); return;
            }
            if (max_voices == 0u || max_voices > 32u) {
                ctx.set_gpr(2, kSasErrorInvalidMaxVoices); return;
            }
            if (grain < 0x40u || grain > 0x800u || (grain & 0x1Fu) != 0u) {
                ctx.set_gpr(2, kSasErrorInvalidGrain); return;
            }
            if (output_mode > 1u) {
                ctx.set_gpr(2, kSasErrorInvalidOutputMode); return;
            }
            if (sample_rate != 44100u) {
                ctx.set_gpr(2, kSasErrorInvalidSampleRate); return;
            }
            sas_state = SasState{};
            sas_core_mix_calls = 0u;
            sas_core_with_mix_calls = 0u;
            sas_state.initialized = true;
            sas_state.core_address = core;
            sas_state.grain_size = grain;
            // Hardware exposes all 32 voices even when maxVoices is smaller.
            sas_state.max_voices = 32u;
            sas_state.output_mode = output_mode;
            sas_state.sample_rate = sample_rate;
            for (auto &voice : sas_state.voices) voice.pitch = 0x1000;
            rt.memory().zero(core, 64u);
            set_success(ctx);
        });

    bind(runtime, "sceSasCore", 0x99944089u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            const std::uint32_t address = ctx.gpr[6];
            std::int32_t size = static_cast<std::int32_t>(ctx.gpr[7]);
            const std::int32_t loop = static_cast<std::int32_t>(ctx.gpr[8]);
            if (size == 0 || (static_cast<std::uint32_t>(size) & 0xFu) != 0u) {
                ctx.set_gpr(2, kSasErrorInvalidParameter); return;
            }
            if (loop != 0 && loop != 1) {
                ctx.set_gpr(2, kSasErrorInvalidLoop); return;
            }
            if (size < 0) size = 0;
            if (size > 0 && !rt.memory().contains(address, static_cast<std::size_t>(size))) {
                // PSP ignores an invalid VAG pointer, leaving the voice configured.
                set_success(ctx); return;
            }
            voice->type = SasVoiceType::Vag;
            voice->data_address = address;
            voice->data_size = size;
            voice->loop = loop != 0;
            sas_reset_decoder(*voice);
            if (voice->on) voice->playing = true;
            set_success(ctx);
        });

    bind(runtime, "sceSasCore", 0xB7660A23u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            const std::int32_t frequency = static_cast<std::int32_t>(ctx.gpr[6]);
            if (frequency < 0 || frequency >= 64) {
                ctx.set_gpr(2, kSasErrorInvalidNoiseFrequency); return;
            }
            voice->type = SasVoiceType::Noise;
            voice->noise_frequency = frequency;
            sas_reset_decoder(*voice);
            if (voice->on) voice->playing = true;
            set_success(ctx);
        });

    bind(runtime, "sceSasCore", 0xAD84D37Fu,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            const std::int32_t pitch = static_cast<std::int32_t>(ctx.gpr[6]);
            if (pitch < 0 || pitch > 0x4000) {
                ctx.set_gpr(2, kSasErrorInvalidPitch); return;
            }
            voice->pitch = pitch;
            set_success(ctx);
        });

    bind(runtime, "sceSasCore", 0x440CA7D8u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            const std::array<std::int32_t, 4> volumes{
                static_cast<std::int32_t>(ctx.gpr[6]), static_cast<std::int32_t>(ctx.gpr[7]),
                static_cast<std::int32_t>(ctx.gpr[8]), static_cast<std::int32_t>(ctx.gpr[9])};
            for (const auto volume : volumes) {
                if (static_cast<std::int64_t>(volume) < -0x1000ll || static_cast<std::int64_t>(volume) > 0x1000ll) {
                    ctx.set_gpr(2, kSasErrorInvalidVolume); return;
                }
            }
            voice->left_volume = volumes[0]; voice->right_volume = volumes[1];
            voice->effect_left_volume = volumes[2]; voice->effect_right_volume = volumes[3];
            if (sas_audio_diagnostics_enabled() &&
                (volumes[2] != 0 || volumes[3] != 0)) {
                std::cerr << "[sas] volume voice=" << static_cast<std::int32_t>(ctx.gpr[5])
                          << " dry=" << volumes[0] << "," << volumes[1]
                          << " effect=" << volumes[2] << "," << volumes[3] << "\n";
            }
            set_success(ctx);
        });

    bind(runtime, "sceSasCore", 0x019B25EBu,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            const std::uint32_t flags = ctx.gpr[6] & 0xFu;
            const std::array<std::int32_t, 4> rates{
                static_cast<std::int32_t>(ctx.gpr[7]), static_cast<std::int32_t>(ctx.gpr[8]),
                static_cast<std::int32_t>(ctx.gpr[9]), static_cast<std::int32_t>(ctx.gpr[10])};
            for (std::size_t i = 0; i < rates.size(); ++i) {
                if ((flags & (1u << i)) != 0u && rates[i] < 0) {
                    ctx.set_gpr(2, kSasErrorInvalidAdsrRate); return;
                }
            }
            for (std::size_t i = 0; i < rates.size(); ++i)
                if ((flags & (1u << i)) != 0u) voice->adsr_rates[i] = rates[i];
            set_success(ctx);
        });

    bind(runtime, "sceSasCore", 0x9EC3676Au,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            const std::uint32_t flags = ctx.gpr[6] & 0xFu;
            std::array<std::int32_t, 4> modes{
                static_cast<std::int32_t>(ctx.gpr[7] & 0x7FFFFFFFu), static_cast<std::int32_t>(ctx.gpr[8] & 0x7FFFFFFFu),
                static_cast<std::int32_t>(ctx.gpr[9] & 0x7FFFFFFFu), static_cast<std::int32_t>(ctx.gpr[10] & 0x7FFFFFFFu)};
            const bool invalid_attack = modes[0] > 5 || (modes[0] & 1) != 0;
            const bool invalid_decay = modes[1] > 5 || (modes[1] & 1) != 1;
            const bool invalid_sustain = modes[2] > 5;
            const bool invalid_release = modes[3] > 5 || (modes[3] & 1) != 1;
            const std::array<bool, 4> invalid{invalid_attack, invalid_decay, invalid_sustain, invalid_release};
            for (std::size_t i = 0; i < invalid.size(); ++i) {
                if ((flags & (1u << i)) != 0u && invalid[i]) {
                    ctx.set_gpr(2, kSasErrorInvalidAdsrMode); return;
                }
            }
            for (std::size_t i = 0; i < modes.size(); ++i)
                if ((flags & (1u << i)) != 0u) voice->adsr_modes[i] = modes[i];
            set_success(ctx);
        });

    bind(runtime, "sceSasCore", 0x5F9529F6u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            voice->sustain_level = static_cast<std::int32_t>(ctx.gpr[6]);
            set_success(ctx);
        });

    bind(runtime, "sceSasCore", 0xCBCD4F79u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            if (((ctx.gpr[7] >> 13u) & 1u) != 0u) {
                ctx.set_gpr(2, kSasErrorInvalidAdsrMode); return;
            }
            voice->simple_adsr1 = ctx.gpr[6] & 0xFFFFu;
            voice->simple_adsr2 = ctx.gpr[7] & 0xFFFFu;
            sas_decode_simple_adsr(*voice);
            set_success(ctx);
        });

    bind(runtime, "sceSasCore", 0x76F01ACAu,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            if (voice->paused || voice->on) {
                ctx.set_gpr(2, kSasErrorVoicePaused); return;
            }
            if (trace) {
                std::array<std::uint8_t, 64> head{};
                const auto bytes = voice->type == SasVoiceType::Vag
                    ? std::min<std::size_t>(head.size(), static_cast<std::size_t>(std::max(voice->data_size, 0))) : 0u;
                if (bytes != 0u && rt.memory().contains(voice->data_address, bytes))
                    rt.memory().copy_out(voice->data_address, std::span<std::uint8_t>(head.data(), bytes));
                char text[256];
                std::snprintf(text, sizeof(text),
                    "[sound] voice %d keyon out=%.3f type=%s data=0x%08X size=%d loop=%d pitch=0x%04X (%.0f Hz) volume=%d,%d key=%08x\n",
                    static_cast<int>(ctx.gpr[5]), sound_output_seconds(),
                    voice->type == SasVoiceType::Vag ? "adpcm" : voice->type == SasVoiceType::Noise ? "noise" : "off",
                    voice->data_address, voice->data_size, voice->loop ? 1 : 0, static_cast<unsigned>(voice->pitch),
                    voice->pitch * 44100.0 / 4096.0, voice->left_volume, voice->right_volume,
                    sound_key(std::span<const std::uint8_t>(head.data(), bytes)));
                std::cout << text;
            }
            sas_reset_decoder(*voice);
            voice->on = true;
            voice->playing = voice->type != SasVoiceType::Off;
            voice->envelope_height = 0u;
            voice->envelope_phase = SasEnvelopePhase::Attack;
            voice->key_on_delay_samples = voice->type == SasVoiceType::Vag ? 33u : 32u;
            if (sas_audio_diagnostics_enabled())
                std::cerr << "[sas] keyon voice=" << static_cast<std::int32_t>(ctx.gpr[5])
                          << " type=" << static_cast<int>(voice->type)
                          << " pitch=" << voice->pitch
                          << " loop=" << voice->loop << "\n";
            set_success(ctx);
        });

    bind(runtime, "sceSasCore", 0xA0CF2FA4u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            if (voice->paused || !voice->on) {
                ctx.set_gpr(2, kSasErrorVoicePaused); return;
            }
            voice->on = false;
            voice->envelope_phase = SasEnvelopePhase::Release;
            // A release rate of zero never walks the envelope down, so a looping
            // voice keyed off here would sound forever.  Log it: this is the
            // remaining suspect for the vehicle engine that keeps running under
            // the pause menu.
            if (sas_audio_diagnostics_enabled())
                std::cerr << "[sas] keyoff voice=" << static_cast<std::int32_t>(ctx.gpr[5])
                          << " loop=" << voice->loop
                          << " release_mode=" << voice->adsr_modes[3]
                          << " release_rate=" << voice->adsr_rates[3]
                          << " height=" << voice->envelope_height << "\n";
            set_success(ctx);
        });

    bind(runtime, "sceSasCore", 0x787D04D5u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            std::uint32_t mask = ctx.gpr[5];
            const bool pause = ctx.gpr[6] != 0u;
            for (std::size_t i = 0; i < sas_state.voices.size(); ++i)
                if ((mask & (1u << i)) != 0u) sas_state.voices[i].paused = pause;
            set_success(ctx);
        });

    bind(runtime, "sceSasCore", 0x2C8E6AB3u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            std::uint32_t flags = 0u;
            for (std::size_t i = 0; i < sas_state.voices.size(); ++i)
                if (sas_state.voices[i].paused) flags |= 1u << i;
            ctx.set_gpr(2, flags);
        });

    bind(runtime, "sceSasCore", 0x68A46B95u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            std::uint32_t flags = 0u;
            for (std::size_t i = 0; i < sas_state.voices.size(); ++i)
                if (!sas_state.voices[i].playing) flags |= 1u << i;
            ctx.set_gpr(2, flags);
        });

    bind(runtime, "sceSasCore", 0x74AE582Au,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            ctx.set_gpr(2, voice->envelope_height);
        });

    bind(runtime, "sceSasCore", 0x33D4AB37u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            const std::int32_t type = static_cast<std::int32_t>(ctx.gpr[5]);
            if (type < -1 || type > 8) { ctx.set_gpr(2, kSasErrorReverbType); return; }
            if (sas_state.reverb.type != type) {
                sas_state.reverb.type = type;
                sas_state.reverb.history_left.clear();
                sas_state.reverb.history_right.clear();
                sas_state.reverb.history_cursor = 0u;
            }
            set_success(ctx);
        });

    bind(runtime, "sceSasCore", 0x267A6DD2u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            const std::int32_t delay = static_cast<std::int32_t>(ctx.gpr[5]);
            const std::int32_t feedback = static_cast<std::int32_t>(ctx.gpr[6]);
            if (delay < 0 || delay >= 128) { ctx.set_gpr(2, kSasErrorReverbDelay); return; }
            if (feedback < 0 || feedback >= 128) { ctx.set_gpr(2, kSasErrorReverbFeedback); return; }
            sas_state.reverb.delay = delay; sas_state.reverb.feedback = feedback; set_success(ctx);
        });

    bind(runtime, "sceSasCore", 0xD5A229C9u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            if (ctx.gpr[5] > 0x1000u || ctx.gpr[6] > 0x1000u) {
                ctx.set_gpr(2, kSasErrorReverbVolume); return;
            }
            sas_state.reverb.left_volume = ctx.gpr[5]; sas_state.reverb.right_volume = ctx.gpr[6]; set_success(ctx);
        });

    bind(runtime, "sceSasCore", 0xF983B186u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            sas_state.reverb.dry = ctx.gpr[5] != 0u; sas_state.reverb.wet = ctx.gpr[6] != 0u; set_success(ctx);
        });

    bind(runtime, "sceSasCore", 0xA3589D81u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            const std::uint32_t output = ctx.gpr[5];
            const std::size_t bytes = static_cast<std::size_t>(sas_state.grain_size) *
                (sas_state.output_mode == 0u ? 4u : 8u);
            if (!rt.memory().contains(output, bytes)) { ctx.set_gpr(2, kSasErrorInvalidParameter); return; }
            ++sas_core_mix_calls;
            sas_log_mix_checkpoint("core", sas_core_mix_calls);
            if (sas_state.output_mode == 0u)
                sas_mix_into(rt, output, sas_state.grain_size);
            else
                sas_mix_raw(rt, output, sas_state.grain_size);
            set_success(ctx);
        });

    bind(runtime, "sceSasCore", 0x50A14DFCu,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            if (sas_state.output_mode == 1u) { ctx.set_gpr(2, 0x800001FFu); return; }
            const std::uint32_t inout = ctx.gpr[5];
            const std::size_t bytes = static_cast<std::size_t>(sas_state.grain_size) * 4u;
            if (!rt.memory().contains(inout, bytes)) { ctx.set_gpr(2, kSasErrorInvalidParameter); return; }
            const std::uint32_t input_left = ctx.gpr[6];
            const std::uint32_t input_right = ctx.gpr[7];
            if (input_left > 0x1000u || input_right > 0x1000u) {
                ctx.set_gpr(2, kSasErrorInvalidVolume); return;
            }
            ++sas_core_with_mix_calls;
            sas_log_mix_checkpoint("core-with-mix", sas_core_with_mix_calls);
            sas_mix_into(rt, inout, sas_state.grain_size, true, input_left, input_right);
            set_success(ctx);
        });


}
};
SasRuntime::SasRuntime(psprecomp::Runtime &rt):impl_(std::make_unique<Impl>(rt)) {}
SasRuntime::~SasRuntime() = default;
bool SasRuntime::load() {
    if (impl_->loaded) return false;
    impl_->sas_state = Impl::SasState{};
    impl_->sas_core_mix_calls = impl_->sas_core_with_mix_calls = 0;
    impl_->loaded = true;
    return true;
}
bool SasRuntime::unload() {
    if (!impl_->loaded) return false;
    impl_->sas_state = Impl::SasState{};
    impl_->sas_core_mix_calls = impl_->sas_core_with_mix_calls = 0;
    impl_->loaded = false;
    return true;
}
bool SasRuntime::loaded() const noexcept { return impl_->loaded; }
}
