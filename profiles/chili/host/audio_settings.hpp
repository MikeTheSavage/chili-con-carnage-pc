#pragma once
#include "audio_runtime.hpp"
#include "mod_files.hpp"
#include <algorithm>
namespace chili {
// Sound output: the [Audio] section of ChiliNative.ini.
//
//   BufferMs=70    milliseconds of sound kept ahead of the audio device, 20 to 500. It is what plays
//                  while the game works on a frame, so it has to cover the longest frame; it is also
//                  how late every sound is heard. Raise it when the sound crackles.
//
// The environment wins over the file, for scripts:
//
//   CHILI_AUDIO_BUFFER_MS=<n>
//   CHILI_AUDIO_BAND_MS=<n>     how far above BufferMs the game's own clock times its sound queue before
//                               the device holds it back (default 23; 0 = the device alone times it)
//   CHILI_AUDIO_CATCH_UP=0      never let the game run ahead of its clock to refill a low buffer
inline const char *default_audio_ini() {
    return
        "\n"
        "[Audio]\n"
        "; Milliseconds of sound kept ahead of the audio device (20 to 500). Raise it when the sound crackles;\n"
        "; every sound is heard this much later.\n"
        "; BufferMs=70\n";
}

struct AudioSettings {
    unsigned buffer_ms{70u}, band_ms{23u};
    bool catch_up{true};
    std::vector<std::string> problems;

    static bool parse_number(std::string_view text, unsigned &out) {
        if (text.empty() || text.size() > 4u) return false;
        unsigned value = 0u;
        for (const char c : text) { if (c < '0' || c > '9') return false; value = value * 10u + static_cast<unsigned>(c - '0'); }
        out = value;
        return true;
    }
    void set(std::string_view name, std::string_view value, const std::string &where) {
        unsigned number = 0u;
        if (ModSetup::same_text(name, "BufferMs")) {
            if (parse_number(value, number) && number >= 20u && number <= 500u) buffer_ms = number;
            else problems.push_back(where + "BufferMs must be 20 to 500");
        } else if (ModSetup::same_text(name, "BandMs")) {
            if (parse_number(value, number) && number <= 500u) band_ms = number;
            else problems.push_back(where + "the band must be 0 to 500");
        } else if (ModSetup::same_text(name, "CatchUp")) {
            if (value == "0" || value == "1") catch_up = value == "1";
            else problems.push_back(where + "catch up must be 0 or 1");
        } else problems.push_back(where + "unknown setting '" + std::string(name) + "'");
    }
    void apply(std::string_view text) {
        std::string section;
        unsigned line_number = 0u;
        while (!text.empty()) {
            const auto end = std::min(text.find('\n'), text.size());
            auto line = text.substr(0u, end);
            text.remove_prefix(std::min(end + 1u, text.size()));
            ++line_number;
            if (const auto comment = line.find_first_of(";#"); comment != std::string_view::npos) line = line.substr(0u, comment);
            line = ModSetup::trim(line);
            if (line.empty()) continue;
            if (line.front() == '[') {
                section = line.back() == ']' ? std::string(ModSetup::trim(line.substr(1u, line.size() - 2u))) : std::string();
                continue;
            }
            if (!ModSetup::same_text(section, "Audio")) continue;
            const auto where = "line " + std::to_string(line_number) + ": ";
            const auto equals = line.find('=');
            if (equals == std::string_view::npos || ModSetup::trim(line.substr(0u, equals)).empty()) { problems.push_back(where + "expected name=value"); continue; }
            const auto name = ModSetup::trim(line.substr(0u, equals));
            // The file offers BufferMs only; the other two are test settings of the environment.
            if (ModSetup::same_text(name, "BufferMs")) set(name, ModSetup::trim(line.substr(equals + 1u)), where);
            else problems.push_back(where + "unknown setting '" + std::string(name) + "'");
        }
    }
    // Settings from `ini` (it may be missing), then the environment.
    static AudioSettings load(const std::filesystem::path &ini) {
        AudioSettings settings;
        std::error_code status;
        if (std::filesystem::is_regular_file(ini, status)) {
            std::ifstream stream(ini, std::ios::binary);
            std::ostringstream text;
            text << stream.rdbuf();
            settings.apply(text.str());
        }
        const auto environment = [&](const wchar_t *variable, const char *name) {
            const auto wide = ModSetup::environment(variable);
            if (wide.empty()) return;
            std::string value;
            for (const auto c : wide) value.push_back(c > 126 || c < 32 ? '?' : static_cast<char>(c));
            std::string label;
            for (const wchar_t *c = variable; *c; ++c) label.push_back(static_cast<char>(*c));
            settings.set(name, value, label + ": ");
        };
        environment(L"CHILI_AUDIO_BUFFER_MS", "BufferMs");
        environment(L"CHILI_AUDIO_BAND_MS", "BandMs");
        environment(L"CHILI_AUDIO_CATCH_UP", "CatchUp");
        return settings;
    }
    // The device side of the audio options.
    void apply_to(AudioOptions &options) const {
        options.lead_frames = buffer_ms * 441u / 10u;
        options.band_frames = band_ms * 441u / 10u;
        options.catch_up = catch_up;
    }
};

// Adds the commented [Audio] template to an ini that has no such section.
inline void add_default_audio_settings(const std::filesystem::path &ini) {
    std::error_code status;
    if (!std::filesystem::is_regular_file(ini, status)) return;
    std::string text;
    {
        std::ifstream stream(ini, std::ios::binary);
        std::ostringstream content;
        content << stream.rdbuf();
        text = ModSetup::lower(content.str());
    }
    if (text.find("[audio]") != std::string::npos) return;
    std::ofstream stream(ini, std::ios::binary | std::ios::app);
    stream << default_audio_ini();
}
}
