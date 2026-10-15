#pragma once
#include "controller_replay.hpp"
#include "diagnostic_live_channel.hpp"
#include <cstdint>
#include <optional>
namespace chili {
// Chooses the pad state for each guest controller sample and feeds the
// recorder. Order: an active live-channel command, then the replay while it
// lasts, then the host keyboard/gamepad (never in deterministic mode).
// Whatever was chosen is recorded, so a session driven through the live channel
// on top of a replay records a longer replay.
enum class InputSource : std::uint8_t {Host,Replay,Live};
class InputPipeline {
public:
    InputPipeline(live_control::DiagnosticLiveChannel &live,InputReplay &replay,InputRecorder &recorder,bool host_input)
        :live_(live),replay_(replay),recorder_(recorder),host_input_(host_input){}
    // Call exactly once per sample handed to the guest.
    ControllerInput sample(){
        const auto index=samples_++;
        ControllerInput value;
        if(const auto live=live_.active_sample()){value=*live;live_.sample_consumed();source_=InputSource::Live;}
        else if(replay_.active(index)){value=replay_.at(index);source_=InputSource::Replay;}
        else{if(host_input_)value=poll_host_controller();source_=InputSource::Host;}
        recorder_.observe(index,value);
        return value;
    }
    // Who owns the next sample, and who owned the last one handed out.
    InputSource next_source() const {
        return live_.active_sample()?InputSource::Live:replay_.active(samples_)?InputSource::Replay:InputSource::Host;
    }
    InputSource source() const noexcept {return source_;}
    std::uint64_t samples() const noexcept {return samples_;}
    // Buttons for the native utility dialogs, which read outside the guest
    // sample stream. Empty means: use the host device.
    std::optional<std::uint32_t> scripted_buttons(){
        if(const auto live=live_.active_sample())return live->buttons;
        if(replay_.active(samples_))return replay_.at(samples_).buttons;
        return {};
    }
private:
    live_control::DiagnosticLiveChannel &live_;InputReplay &replay_;InputRecorder &recorder_;
    bool host_input_;std::uint64_t samples_{};InputSource source_{InputSource::Host};
};
}
