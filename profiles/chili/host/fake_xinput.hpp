#pragma once
// Made-up Xbox-style pads for checks of the real program on a PC without a controller.
//
//   CHILI_FAKE_XINPUT=<file>
//
// The XInput slots answer from a text file instead of from Windows. One line per pad that is "connected":
//
//   <slot 0-3> <buttons> [<left trigger> <right trigger> [<lx> <ly> <rx> <ry>]]
//
// buttons: XInput's button bits in hexadecimal (0001 D-pad up, 0002 down, 0004 left, 0008 right, 0010 Start,
// 0020 Back, 0040 LS, 0080 RS, 0100 LB, 0200 RB, 1000 A, 2000 B, 4000 X, 8000 Y). Triggers 0 to 255, sticks
// -32768 to 32767 with up positive, as XInput gives them. A line that starts with ';' or '#' is a comment.
// A slot without a line has no pad; while the file does not exist there is no pad at all. The file is read
// again at most every few milliseconds, so a script changes the pad by replacing the file (in one step: a
// half-written file reads as fewer pads). Everything behind the slot's answer is the program's own code:
// the finder, the pad in use, the bindings, the device used last, the prompt pictures.
//
// The motor values the program sends to such a pad are logged ("[input] made-up pad ...").
// A PlayStation pad cannot be made up this way: those are read from Raw Input reports, not from XInput.
#include "pc_input.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
namespace chili {
class FakeXInput {
public:
    explicit FakeXInput(std::filesystem::path file) : file_(std::move(file)) {}
    // One pad line; false when it is not one.
    static bool parse_line(const std::string &line, unsigned &slot, XINPUT_GAMEPAD &pad) {
        std::istringstream in(line);
        std::string first, buttons;
        if (!(in >> first) || first[0] == ';' || first[0] == '#') return false;
        if (first.size() != 1u || first[0] < '0' || first[0] > '3' || !(in >> buttons)) return false;
        char *end = nullptr;
        const unsigned long bits = std::strtoul(buttons.c_str(), &end, 16);
        if (!end || *end || bits > 0xFFFFul) return false;
        slot = static_cast<unsigned>(first[0] - '0');
        pad = {};
        pad.wButtons = static_cast<WORD>(bits);
        long values[6]{};
        for (auto &value : values) if (!(in >> value)) break;
        const auto byte = [](long value) { return static_cast<BYTE>(value < 0 ? 0 : value > 255 ? 255 : value); };
        const auto axis = [](long value) { return static_cast<SHORT>(value < -32768 ? -32768 : value > 32767 ? 32767 : value); };
        pad.bLeftTrigger = byte(values[0]); pad.bRightTrigger = byte(values[1]);
        pad.sThumbLX = axis(values[2]); pad.sThumbLY = axis(values[3]); pad.sThumbRX = axis(values[4]); pad.sThumbRY = axis(values[5]);
        return true;
    }
    // XInputGetState's answer for a slot. Called by the pad finder's thread and by the game's.
    bool query(unsigned slot, XINPUT_STATE &state) {
        std::lock_guard<std::mutex> lock(mutex_);
        refresh();
        if (slot >= pads_.size() || !pads_[slot].present) return false;
        state = pads_[slot].state;
        return true;
    }
    // XInputSetState's: the values are logged when they change.
    bool vibrate(unsigned slot, std::uint16_t left, std::uint16_t right) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot >= pads_.size() || !pads_[slot].present) return false;
        if (motors_[slot][0] != left || motors_[slot][1] != right) {
            motors_[slot] = {left, right};
            std::printf("[input] made-up pad, XInput slot %u: motors %u %u of 65535\n", slot, unsigned(left), unsigned(right));
            std::fflush(stdout);
        }
        return true;
    }
private:
    struct Pad { bool present{}; XINPUT_STATE state{}; };
    void refresh() {
        const auto now = std::chrono::steady_clock::now();
        if (read_ && now - read_at_ < std::chrono::milliseconds(4)) return;
        read_ = true; read_at_ = now;
        std::ifstream in(file_);
        if (!in) {
            // No file: no pad. A file that is being replaced this instant: what it said last, for a tenth of a second.
            std::error_code error;
            if (std::filesystem::exists(file_, error)) { missing_ = false; return; }
            if (!missing_) { missing_ = true; missing_since_ = now; }
            if (now - missing_since_ >= std::chrono::milliseconds(100)) for (auto &pad : pads_) pad.present = false;
            return;
        }
        missing_ = false;
        std::array<Pad, 4> next{};
        std::string line;
        while (std::getline(in, line)) {
            unsigned slot{}; XINPUT_GAMEPAD pad{};
            if (!parse_line(line, slot, pad)) continue;
            next[slot].present = true; next[slot].state.Gamepad = pad;
        }
        for (std::size_t slot = 0; slot < next.size(); ++slot) {
            // The packet number goes up when the pad's state changes, as XInput's does.
            auto &was = pads_[slot];
            next[slot].state.dwPacketNumber = was.state.dwPacketNumber +
                (next[slot].present && (!was.present || std::memcmp(&was.state.Gamepad, &next[slot].state.Gamepad, sizeof(XINPUT_GAMEPAD)) != 0) ? 1u : 0u);
            if (!next[slot].present) motors_[slot] = {};
        }
        pads_ = next;
    }
    std::filesystem::path file_;
    std::mutex mutex_;
    std::array<Pad, 4> pads_{};
    std::array<std::array<std::uint16_t, 2>, 4> motors_{};
    std::chrono::steady_clock::time_point read_at_{}, missing_since_{};
    bool read_{}, missing_{};
};
// The input layer asks the file instead of Windows about the four XInput slots. Before the window is attached.
inline void install_fake_xinput(PcInput &input, const std::filesystem::path &file) {
    auto fake = std::make_shared<FakeXInput>(file);
    PcInputHost host;
    host.pad = [fake](unsigned slot, XINPUT_STATE &state) { return fake->query(slot, state); };
    host.vibrate = [fake](unsigned slot, std::uint16_t left, std::uint16_t right) { return fake->vibrate(slot, left, right); };
    input.set_host(std::move(host));
    std::cout << "[input] gamepads: the XInput slots are made up from " << file.string() << " (CHILI_FAKE_XINPUT)\n";
}
}
