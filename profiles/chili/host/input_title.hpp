#pragma once
#include "input_pipeline.hpp"
#include "pc_input.hpp"
#include <cstdio>
#include <string>
namespace chili {
// What the title bar says about the input, after the window's own title.
//
// Always: whether a click will capture the mouse, that it is captured and how
// to free it, and, when the player presses or moves something while a replay or
// a script owns the input, that this is why nothing happens.
// With the input test on ([Input] ShowInput, or its toggle key): the scheme,
// the kind of the last raw mouse report, the last movement that reached the
// game with where it was measured, the buttons and sticks of the last
// sample, the pad in use and the device used last. This is how a player on a
// streamed or remote PC can tell what the pointer really delivers, and which
// pad the game took.
inline std::string input_title_text(const PcInput &input, InputSource source, const ControllerInput &sent) {
    const auto &config = input.config();
    const auto &report = input.report();
    const bool ignored = input.seconds_since_ignored() < 5.0;
    std::string text;
    if (source == InputSource::Replay) { if (ignored) text = "a replay is driving the input: keyboard and mouse are ignored until it ends"; }
    else if (source == InputSource::Live) { if (ignored) text = "a script is driving the input: keyboard and mouse are ignored"; }
    else if (config.mouse_enabled && config.mouse_capture_on_click) {
        if (input.capture_forbidden()) { if (ignored) text = "scripted session: the mouse is not captured"; }
        else if (input.mouse_captured()) text = "mouse captured, F1 frees it";
        else if (input.capture_pending()) text = "the mouse is captured when play starts, F1 cancels";
        else text = "click to capture the mouse";
    }
    if (!input.show_input()) return text;
    char line[384];
    const char *owner = source == InputSource::Replay ? "replay" : source == InputSource::Live ? "script" : "player";
    std::snprintf(line, sizeof(line), "%s%s | input from %s | mouse %s, last report %s, moved %+.0f %+.0f by %s%s | sent buttons %04X move %u,%u look %u,%u | pad: %s | last device: %s",
        text.empty() ? "" : " | ", scheme_name(config.scheme), owner,
        input.mouse_captured() ? "captured" : input.capture_pending() ? "waiting for play" : "free", mouse_source_name(report.last_report),
        report.moved_x, report.moved_y, mouse_source_name(report.source),
        config.scheme != InputScheme::DualAnalog ? "" : config.mouse_exact ? ", applied as that angle" : ", applied as stick travel",
        static_cast<unsigned>(sent.buttons & 0xFFFFu), unsigned(sent.x), unsigned(sent.y), unsigned(sent.rx), unsigned(sent.ry),
        input.pad_text().c_str(), input.device() == InputDevice::Gamepad ? "pad" : "keyboard");
    return text + line;
}
// The same for the log when the game ends.
inline std::string input_summary(const PcInput &input) {
    const auto &report = input.report();
    char line[1536];
    std::snprintf(line, sizeof(line),
        "[input] mouse: raw input %s, captures=%llu, raw relative reports=%llu, raw absolute reports=%llu, samples measured by the cursor=%llu, "
        "travel=%.0f,%.0f counts, last movement by %s, button presses=%llu, wheel notches=%llu\n"
        "[input] keyboard: key presses=%llu; presses and movements ignored while a replay or script had the input=%llu; host samples=%llu\n"
        "[input] gamepad: pad in use %s; pads that arrived: xbox-style=%llu, playstation=%llu%s; the pad in use changed %llu times; "
        "a PlayStation pad that fell silent was put to rest %llu times\n"
        "[input] device used last: %s; host samples with the pad=%llu, with keyboard and mouse=%llu, changes=%llu\n"
        "[input] vibration: calls by the game=%llu, settings sent to a pad=%llu",
        input.raw_registered() ? "registered" : "NOT registered", static_cast<unsigned long long>(report.captures),
        static_cast<unsigned long long>(report.raw_relative_reports), static_cast<unsigned long long>(report.raw_absolute_reports),
        static_cast<unsigned long long>(report.cursor_samples), report.travel_x, report.travel_y, mouse_source_name(report.source),
        static_cast<unsigned long long>(report.mouse_presses), static_cast<unsigned long long>(report.wheel_notches),
        static_cast<unsigned long long>(report.key_presses), static_cast<unsigned long long>(report.ignored),
        static_cast<unsigned long long>(report.samples),
        input.pad_text().c_str(), static_cast<unsigned long long>(report.pads_xbox), static_cast<unsigned long long>(report.pads_playstation),
        input.hid_registered() ? "" : " (PlayStation pads are not read)", static_cast<unsigned long long>(report.pad_changes),
        static_cast<unsigned long long>(report.pad_silences),
        input.device() == InputDevice::Gamepad ? "pad" : "keyboard", static_cast<unsigned long long>(report.samples_gamepad),
        static_cast<unsigned long long>(report.samples_keyboard_mouse), static_cast<unsigned long long>(report.device_changes),
        static_cast<unsigned long long>(report.game_vibration_calls), static_cast<unsigned long long>(report.vibration_sent));
    std::string text = line;
    // A PlayStation pad that is still connected: how its reports were read.
    for (const auto &pad : input.hid_pad_lines()) text += "\n[input] " + pad;
    return text;
}
// Keeps the window's title up to date, at most five times a second and only
// when the text changes.
class InputTitle {
public:
    void update(HWND window, const PcInput &input, InputSource source, const ControllerInput &sent, double now) {
        if (!window || (now - updated_ < 0.2 && now >= updated_)) return;
        updated_ = now;
        if (window != window_) {
            wchar_t title[128]{};
            GetWindowTextW(window, title, 128);
            base_ = title; window_ = window; shown_.clear();
        }
        auto text = input_title_text(input, source, sent);
        if (text == shown_) return;
        std::wstring title = base_;
        if (!text.empty()) { title += L" - "; title.append(text.begin(), text.end()); }
        SetWindowTextW(window, title.c_str());
        shown_ = std::move(text);
    }

private:
    HWND window_{};
    std::wstring base_;
    std::string shown_;
    double updated_{-1.0};
};
}
