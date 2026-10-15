#pragma once
#include "controller_runtime.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace chili::live_control {
namespace fs = std::filesystem;
inline constexpr std::size_t command_bytes = 256;
inline constexpr std::uint32_t supported_buttons = 0xF3F9u;
inline constexpr std::uint32_t maximum_pulse_ms = 5000;
inline constexpr std::uint32_t maximum_tap_samples = 3600;
inline constexpr std::uint64_t poll_interval_ms = 50;
struct Error : std::runtime_error { using std::runtime_error::runtime_error; };
// One command per file, "<sequence> <operation> ...":
//   press <buttons> <duration_ms> <x> <y> [<rx> <ry>]   pad state for a time on the pulse clock
//   tap <buttons> <samples> <x> <y> [<rx> <ry>]         pad state for that many controller samples
//   hold <buttons> <x> <y> [<rx> <ry>]                  pad state until the next press/tap/hold/release
// <rx> <ry> are the second stick of the dual-analog scheme, 128 128 when left out.
//   release                                 hand input back to the underlying source
//   capture <label>                         save the displayed frame
//   quit                                    stop the run cleanly
enum class Kind { Press, Capture, Hold, Release, Tap, Quit };
struct Command {
    std::uint64_t sequence{};
    Kind kind{};
    ControllerInput input;
    std::uint32_t duration_ms{};
    std::string label;
    std::uint32_t samples{};
};
inline bool safe_name(std::string_view text, std::size_t limit) {
    if (text.empty() || text.size() > limit) return false;
    for (const unsigned char c : text)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}
inline std::uint64_t number(std::string_view text, bool hexadecimal = false) {
    unsigned base = 10;
    if (hexadecimal && text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16; text.remove_prefix(2);
    }
    if (text.empty()) throw Error("empty unsigned control number");
    std::uint64_t result{};
    for (const unsigned char c : text) {
        const unsigned digit = c >= '0' && c <= '9' ? c - '0' :
            base == 16 && c >= 'a' && c <= 'f' ? c - 'a' + 10 :
            base == 16 && c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
        if (digit >= base || result > (std::numeric_limits<std::uint64_t>::max() - digit) / base)
            throw Error("invalid or overflowing unsigned control number");
        result = result * base + digit;
    }
    return result;
}
inline Command parse(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > command_bytes) throw Error("control command size is invalid");
    for (const unsigned char c : bytes)
        if (c > 126 || (c < 32 && c != '\n' && c != '\r' && c != '\t'))
            throw Error("control command must be bounded ASCII text");
    std::istringstream input{std::string(bytes)};
    std::array<std::string, 9> tokens;
    std::size_t count{};
    while (count < tokens.size() && input >> tokens[count]) ++count;
    if (count < 2 || count == tokens.size()) throw Error("control command token count is invalid");
    Command result; result.sequence = number(tokens[0]);
    if (!result.sequence) throw Error("control sequence must be positive");
    const auto& operation = tokens[1];
    if (operation == "capture" && count == 3) {
        if (!safe_name(tokens[2], 48)) throw Error("control capture label is invalid");
        result.kind = Kind::Capture; result.label = std::move(tokens[2]); return result;
    }
    if ((operation == "release" || operation == "quit") && count == 2) {
        result.kind = operation == "quit" ? Kind::Quit : Kind::Release; return result;
    }
    // `first` is the index of <x>; a right stick, when given, follows <y>.
    const auto pad = [&](const std::string& buttons_text, std::size_t first) {
        const bool right = count == first + 4;
        const auto buttons = number(buttons_text, true), x = number(tokens[first]), y = number(tokens[first + 1]);
        const auto rx = right ? number(tokens[first + 2]) : 128u, ry = right ? number(tokens[first + 3]) : 128u;
        if ((buttons & ~std::uint64_t(supported_buttons)) || x > 255 || y > 255 || rx > 255 || ry > 255)
            throw Error("control buttons, duration or analog values are invalid");
        return ControllerInput{static_cast<std::uint32_t>(buttons), static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y),
                               static_cast<std::uint8_t>(rx), static_cast<std::uint8_t>(ry)};
    };
    if (operation == "hold" && (count == 5 || count == 7)) {
        result.kind = Kind::Hold; result.input = pad(tokens[2], 3); return result;
    }
    if ((operation != "press" && operation != "tap") || (count != 6 && count != 8)) throw Error("control operation or fields are invalid");
    const bool timed = operation == "press";
    const auto amount = number(tokens[3]);
    result.input = pad(tokens[2], 4);
    if (!amount || amount > (timed ? maximum_pulse_ms : maximum_tap_samples))
        throw Error("control buttons, duration or analog values are invalid");
    if (timed) { result.kind = Kind::Press; result.duration_ms = static_cast<std::uint32_t>(amount); }
    else { result.kind = Kind::Tap; result.samples = static_cast<std::uint32_t>(amount); }
    return result;
}
struct Config {
    fs::path control_file, frame_prefix;
    // Optional. One status line per handled command, replaced atomically:
    // "<sequence> ok|failed|stale|rejected <detail>". Senders wait on it
    // instead of on the log, which may sit in a stdio buffer.
    fs::path ack_file;
    std::wstring frame_extension{L".bmp"};
    bool enabled() const { return !control_file.empty(); }
};
inline void local_path(const fs::path& path) {
    if (!path.is_absolute() || !path.has_root_name() || path.native().starts_with(L"\\\\"))
        throw Error("live control paths must be absolute local paths");
    for (const auto& part : path.relative_path()) if (part == L"." || part == L"..")
        throw Error("live control paths must not contain traversal");
}
inline DWORD attributes(const fs::path& path) {
    const auto value = GetFileAttributesW(path.c_str());
    if (value == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return value;
        throw Error("cannot inspect live control path");
    }
    if (value & FILE_ATTRIBUTE_REPARSE_POINT) throw Error("live control reparse paths are unsupported");
    return value;
}
inline void directory(const fs::path& path) {
    local_path(path);
    auto current = path.root_path();
    auto flags = attributes(current);
    if (flags == INVALID_FILE_ATTRIBUTES || !(flags & FILE_ATTRIBUTE_DIRECTORY)) throw Error("live control root is absent");
    for (const auto& part : path.relative_path()) {
        current /= part; flags = attributes(current);
        if (flags == INVALID_FILE_ATTRIBUTES || !(flags & FILE_ATTRIBUTE_DIRECTORY)) throw Error("live control parent is absent or not a directory");
    }
}
inline fs::path environment_path(const wchar_t* name) {
    SetLastError(ERROR_SUCCESS);
    const auto needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (!needed) {
        const auto error = GetLastError();
        if (error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND) return {};
        throw Error("cannot read live control environment");
    }
    std::array<wchar_t, 32768> text{};
    if (needed > text.size()) throw Error("live control environment path is too long");
    const auto count = GetEnvironmentVariableW(name, text.data(), static_cast<DWORD>(text.size()));
    if (!count || count >= text.size()) throw Error("live control environment changed during read");
    return fs::path(std::wstring(text.data(), count));
}
inline Config from_environment() {
    const auto control = environment_path(L"PSPRECOMP_LIVE_CONTROL");
    if (control.empty()) return {};
    const auto prefix = environment_path(L"PSPRECOMP_LIVE_FRAME_PREFIX");
    if (prefix.empty()) throw Error("PSPRECOMP_LIVE_CONTROL requires PSPRECOMP_LIVE_FRAME_PREFIX");
    Config config{control, prefix};
    config.ack_file = fs::path(control.native() + L".ack");
    // Captures are PNG unless PSPRECOMP_LIVE_FRAME_FORMAT=bmp.
    const auto format = environment_path(L"PSPRECOMP_LIVE_FRAME_FORMAT");
    if (format.empty() || format == L"png") config.frame_extension = L".png";
    else if (format != L"bmp") throw Error("PSPRECOMP_LIVE_FRAME_FORMAT must be png or bmp");
    return config;
}
inline std::optional<std::string> read_latest(const fs::path& path) {
    directory(path.parent_path());
    const auto flags = attributes(path);
    if (flags == INVALID_FILE_ATTRIBUTES) return {};
    if (flags & FILE_ATTRIBUTE_DIRECTORY) throw Error("live control input is not a plain file");
    struct Handle {
        HANDLE value;
        ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } handle{CreateFileW(path.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (handle.value == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return {};
        throw Error("cannot read live control command");
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    LARGE_INTEGER before{}, after{};
    if (!GetFileInformationByHandleEx(handle.value, FileAttributeTagInfo, &tag, sizeof(tag)) || !GetFileSizeEx(handle.value, &before))
        throw Error("cannot verify live control read handle");
    if ((tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || before.QuadPart <= 0 || before.QuadPart > static_cast<LONGLONG>(command_bytes))
        throw Error("live control file type or size is invalid");
    std::array<char, command_bytes> bytes{}; DWORD count{};
    if (!ReadFile(handle.value, bytes.data(), static_cast<DWORD>(before.QuadPart), &count, nullptr) ||
        !GetFileSizeEx(handle.value, &after) || count != static_cast<DWORD>(before.QuadPart) || after.QuadPart != before.QuadPart)
        throw Error("live control command changed or was truncated during read");
    // Writers must atomically replace a complete file. The handle keeps the
    // coherent old generation during an atomic rename; no partial append is used.
    return std::string(bytes.data(), count);
}
struct CaptureResult {
    std::uint64_t sequence{};
    std::string label;
    fs::path path;
    bool attempted{}, saved{};
};
class DiagnosticLiveChannel {
public:
    using Clock = std::function<std::uint64_t()>;
    using Log = std::function<void(const std::string&)>;
    using Context = std::function<std::string()>;
    using Quit = std::function<void()>;
    DiagnosticLiveChannel() : DiagnosticLiveChannel(from_environment()) {}
    // `clock` (milliseconds) throttles file polling. `pulse_clock` times press
    // durations and defaults to `clock`; a deterministic run passes guest time
    // so a press covers the same guest interval at any host speed.
    explicit DiagnosticLiveChannel(Config config, Clock clock = {}, Log log = {}, Clock pulse_clock = {})
        : config_(std::move(config)), clock_(std::move(clock)), pulse_clock_(std::move(pulse_clock)), log_(std::move(log)) {
        if (!clock_) clock_ = [] { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); };
        if (!pulse_clock_) pulse_clock_ = clock_;
        if (!log_) log_ = [](const auto& line) { std::cout << line << std::endl; };
        if (!enabled()) return;
        local_path(config_.control_file); local_path(config_.frame_prefix);
        if (!safe_name(config_.frame_prefix.filename().string(), 64)) throw Error("live frame prefix basename is invalid");
        if (config_.control_file == config_.frame_prefix) throw Error("live frame prefix must differ from the control file");
        directory(config_.control_file.parent_path()); directory(config_.frame_prefix.parent_path());
        if (!config_.ack_file.empty()) {
            local_path(config_.ack_file);
            if (config_.ack_file == config_.control_file) throw Error("live acknowledgement file must differ from the control file");
            directory(config_.ack_file.parent_path());
        }
        report("[live-control] enabled; latest commands polled at 50 ms; last presented guest frames only");
    }
    bool enabled() const { return config_.enabled(); }
    std::uint64_t last_sequence() const { return last_sequence_; }
    std::optional<CaptureResult> last_capture() const { return last_capture_; }
    // Appended to applied-command log and acknowledgement lines, e.g. " sample=120 frame=64".
    void set_context(Context context) { context_ = std::move(context); }
    void set_quit_handler(Quit quit) { quit_ = std::move(quit); }
    std::optional<ControllerInput> active_sample() const {
        if (!enabled() || !pulse_) return {};
        if (pulse_->counted) return pulse_->samples ? std::optional<ControllerInput>(pulse_->input) : std::nullopt;
        const auto now = pulse_clock_();
        if (now < pulse_->start || now >= pulse_->end) return {};
        return pulse_->input;
    }
    // The input pipeline calls this once per guest controller sample that took
    // its state from active_sample(); it is what makes a tap last N samples.
    void sample_consumed() { if (pulse_ && pulse_->counted && pulse_->samples) --pulse_->samples; }
    template<class Source> ControllerInput sample(Source&& source) const {
        if (const auto active = active_sample()) return *active;
        return std::invoke(std::forward<Source>(source));
    }
    // Call on the runtime/UI thread from the existing after-dispatch host pump.
    // Providers never read files, consume edges, advance commands, or capture.
    template<class Capture> void host_pump(Capture&& capture) {
        if (!enabled()) return;
        const auto now = clock_();
        if (last_poll_ && (now < *last_poll_ || now - *last_poll_ < poll_interval_ms)) return;
        last_poll_ = now;
        Command command; std::uint64_t pulse_now{};
        bool fresh{};
        try {
            const auto text = read_latest(config_.control_file);
            if (!text || *text == last_text_) return;
            last_text_ = *text; fresh = true;
            command = parse(*text);
            if (command.sequence <= last_sequence_) {
                acknowledge(std::to_string(last_sequence_) + " stale " + std::to_string(command.sequence));
                return;
            }
            pulse_now = pulse_clock_();
            if (command.kind == Kind::Press && pulse_now > std::numeric_limits<std::uint64_t>::max() - command.duration_ms)
                throw Error("live control pulse clock would overflow");
        } catch (const Error& error) { rejected(error.what(), fresh); return; }
        last_sequence_ = command.sequence;
        last_error_.clear();
        std::string context;
        try { if (context_) context = context_(); } catch (...) {}
        if (command.kind != Kind::Capture) {
            constexpr auto forever = std::numeric_limits<std::uint64_t>::max();
            const char* name = "quit";
            if (command.kind == Kind::Press) { name = "press"; pulse_ = Pulse{command.input, pulse_now, pulse_now + command.duration_ms}; }
            else if (command.kind == Kind::Hold) { name = "hold"; pulse_ = Pulse{command.input, 0, forever}; }
            else if (command.kind == Kind::Tap) { name = "tap"; pulse_ = Pulse{command.input, 0, forever, command.samples, true}; }
            else if (command.kind == Kind::Release) { name = "release"; pulse_.reset(); }
            std::ostringstream line;
            line << "[live-control] applied sequence=" << command.sequence << ' ' << name;
            if (command.kind == Kind::Press || command.kind == Kind::Hold || command.kind == Kind::Tap) {
                line << " buttons=" << command.input.buttons;
                if (command.kind == Kind::Press) line << " duration_ms=" << command.duration_ms;
                if (command.kind == Kind::Tap) line << " samples=" << command.samples;
                line << " analog=" << unsigned(command.input.x) << ',' << unsigned(command.input.y);
                if (command.input.rx != 128u || command.input.ry != 128u)
                    line << " right=" << unsigned(command.input.rx) << ',' << unsigned(command.input.ry);
            }
            line << context;
            report(line.str());
            acknowledge(std::to_string(command.sequence) + " ok " + name + context);
            if (command.kind == Kind::Quit && quit_) { try { quit_(); } catch (...) {} }
            return;
        }
        // Include sequence in every output name and refuse existing destinations.
        // A capture does not cancel a timed input pulse or take a native UI image.
        CaptureResult result{command.sequence, command.label,
            fs::path(config_.frame_prefix.native() + L"_" + std::to_wstring(command.sequence) + L"_" +
                     fs::path(command.label).native() + config_.frame_extension), false, false};
        try {
            directory(result.path.parent_path());
            if (attributes(result.path) != INVALID_FILE_ATTRIBUTES) throw Error("live frame destination already exists");
            result.attempted = true;
            result.saved = bool(std::invoke(std::forward<Capture>(capture), result.path));
        } catch (...) { result.saved = false; }
        last_capture_ = std::move(result);
        report("[live-control] capture sequence=" + std::to_string(command.sequence) + " label=" + command.label +
               " attempted=" + std::to_string(last_capture_->attempted) + " saved=" + std::to_string(last_capture_->saved) + context);
        acknowledge(std::to_string(command.sequence) + (last_capture_->saved ? " ok capture " : " failed capture ") +
                    last_capture_->path.filename().string() + context);
    }
private:
    // `counted` pulses last for `samples` controller samples instead of a time span.
    struct Pulse { ControllerInput input; std::uint64_t start, end; std::uint32_t samples{}; bool counted{}; };
    Config config_; Clock clock_, pulse_clock_; Log log_;
    Context context_; Quit quit_;
    std::optional<Pulse> pulse_;
    std::optional<std::uint64_t> last_poll_;
    std::uint64_t last_sequence_{};
    std::optional<CaptureResult> last_capture_;
    std::string last_error_, last_text_;
    unsigned rejection_reports_{};
    void report(const std::string& line) noexcept { try { log_(line); } catch (...) {} }
    // Best effort: a sender that sees no acknowledgement times out and says so.
    void acknowledge(const std::string& line) noexcept {
        if (config_.ack_file.empty()) return;
        try {
            const fs::path staged(config_.ack_file.native() + L".tmp");
            const auto handle = CreateFileW(staged.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) return;
            const auto text = line + "\n"; DWORD written{};
            const bool ok = WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
            CloseHandle(handle);
            if (!ok) return;
            // A reader may hold the old file open for an instant.
            for (unsigned attempt = 0; attempt < 20; ++attempt) {
                if (MoveFileExW(staged.c_str(), config_.ack_file.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
                Sleep(1);
            }
        } catch (...) {}
    }
    // `fresh`: new file content failed to parse, so a sender is waiting for an
    // answer even when the error text repeats. A file that could not be read
    // (for instance mid-replacement) is simply polled again.
    void rejected(const std::string& error, bool fresh) {
        if (fresh) acknowledge(std::to_string(last_sequence_) + " rejected " + error);
        if (error == last_error_) return;
        last_error_ = error;
        if (rejection_reports_ < 16) {
            ++rejection_reports_; report("[live-control] rejected: " + error);
            if (rejection_reports_ == 16) report("[live-control] further malformed-input diagnostics suppressed");
        }
    }
};
} // namespace chili::live_control
