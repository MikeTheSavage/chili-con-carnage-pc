#pragma once
#include <atomic>
#include <cstdint>
namespace chili {
// What the player touched last, and how the on-screen button prompts are set to look.
// Written by the input code (host/pc_input.hpp: the device used last, the kind of pad,
// [Input] Prompts and [Input.Gamepad] Glyphs from the ini), read by the prompt pictures
// (host/prompt_style.hpp). One state for the process, lock-free; replay, live-injected
// and deterministic samples do not touch it.
enum class InputDevice : std::uint8_t { KeyboardMouse = 0, Gamepad = 1 };
enum class PadKind : std::uint8_t { None = 0, Xbox = 1, PlayStation = 2 };
enum class PromptsSetting : std::uint8_t { Auto = 0, Keyboard = 1, Gamepad = 2, Psp = 3 };  // [Input] Prompts
enum class GlyphsSetting : std::uint8_t { Auto = 0, Xbox = 1, PlayStation = 2 };            // [Input.Gamepad] Glyphs

struct InputDeviceState {
    std::atomic<InputDevice> device{InputDevice::KeyboardMouse};
    std::atomic<PadKind> pad_kind{PadKind::None};       // the pad used last; None until one was seen
    std::atomic<PromptsSetting> prompts{PromptsSetting::Auto};
    std::atomic<GlyphsSetting> glyphs{GlyphsSetting::Auto};
    std::atomic<std::uint32_t> bindings_revision{0};     // goes up when the binding tables change
};
inline InputDeviceState &input_device_state() noexcept {
    static InputDeviceState state;
    return state;
}
}
