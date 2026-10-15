#pragma once
#include "mod_files.hpp"
#include <algorithm>
namespace chili {
// How the picture is drawn: the [Rendering] section of ChiliNative.ini.
//
//   Preset=low | medium | high | custom
//                              how much the picture may cost. For now it decides the resolution that
//                              InternalScale=auto picks: low at most 2 (960x544, for integrated graphics),
//                              medium at most 4 (1920x1088), high and custom at most 8 (3840x2176). A key
//                              given in the file wins over the preset; custom is the keys alone
//   Backend=d3d12 | software   d3d12 draws on the graphics card; software is the slow CPU reference renderer
//   InternalScale=auto | 1..8  rendering resolution in multiples of the PSP's 480x272. auto fits the monitor
//                              when someone plays (4 at 1080p, 6 at 1440p, 8 at 2160p; less on a graphics
//                              card with little memory), and is 1 in an unattended run, so that run's
//                              frame images match the software renderer's
//   AntiAliasing=fxaa | off | msaa2 | msaa4 | msaa8
//                              fxaa smooths the edges of the 3D scene after it is drawn, before the HUD and
//                              the text go on top; msaaN draws every surface with N samples per pixel.
//                              MSAA=1 | 2 | 4 | 8 is the older spelling of the msaa choices
//   Anisotropy=16 | 8 | 4 | 2 | 1
//                              anisotropic filtering of the textures of 3D draws (1 = the game's own filter)
//   TextureFilter=game | linear | nearest
//                              textures of 3D draws: as the game asks, always smoothed, or always blocky.
//                              The HUD, the text, the screen copy and the bloom keep the game's filter
//   SSAO=off | low | high      ambient occlusion: corners and creases of the 3D scene are darkened
//   Glow=smooth | game | off   d3d12: the game's full-screen glow. smooth draws its passes at the PSP's own
//                              size and enlarges the result smoothly; game draws them at the rendering
//                              resolution, as the game's draws say (a lattice of squares at high resolutions);
//                              off leaves the glow out. At InternalScale=1 smooth and game are the same
//   Fullscreen=true | false    start borderless over the whole monitor (F11 or Alt+Enter switches while playing)
//   EarlyPresent=true | false  d3d12: show a scene when it has been drawn instead of one game frame later, when
//                              the game copies it to its screen buffer. Frame images and screenshots show the
//                              same picture the window does. Scripted runs (scripts/unattended.ini) switch it
//                              off, so their frame images line up with the software renderer's
//   GeThread=true | false      d3d12: the game's display lists are drawn on a thread of their own while the game
//                              computes its next frame, as on the PSP. false draws each list where the game
//                              hands it over. Deterministic runs always do that
//
// The environment wins over the file, for scripts:
//
//   CHILI_RENDER_PRESET=low|medium|high|custom
//   CHILI_RENDER_BACKEND=d3d12|software
//   CHILI_RENDER_SCALE=<n>, CHILI_RENDER_MSAA=<n>, CHILI_RENDER_EARLY_PRESENT=true|false,
//   CHILI_RENDER_GE_THREAD=true|false|force (force: also in a deterministic run, to test the thread)
//   CHILI_RENDER_AA=<AntiAliasing>, CHILI_RENDER_ANISOTROPY=<n>, CHILI_RENDER_FILTER=<TextureFilter>,
//   CHILI_RENDER_SSAO=<SSAO>, CHILI_RENDER_GLOW=<Glow>
//   CHILI_RENDER_STRICT=1      stop when Direct3D 12 cannot start, or when the renderer meets a draw it
//                              cannot decode, instead of falling back to software or skipping the draw
inline const char *default_rendering_ini() {
    return
        "\n"
        "[Rendering]\n"
        "; How much the picture may cost: low (integrated graphics), medium, high. custom: only the keys below.\n"
        "; Preset=high\n"
        "; d3d12 draws on the graphics card. software is the slow reference renderer.\n"
        "; Backend=d3d12\n"
        "; Rendering resolution in multiples of the PSP's 480x272: auto fits the monitor (4 at 1080p, 8 at 2160p),\n"
        "; or 1 to 8.\n"
        "; InternalScale=auto\n"
        "; Edge smoothing: off, fxaa (the 3D scene is smoothed after it is drawn; the HUD and text stay sharp),\n"
        "; or msaa2, msaa4, msaa8 (multisampling, heavier).\n"
        "; AntiAliasing=fxaa\n"
        "; Anisotropic filtering of textures in the 3D scene: 1 (the game's own filter), 2, 4, 8 or 16.\n"
        "; Anisotropy=16\n"
        "; Textures in the 3D scene: game (as the game asks), linear (always smoothed) or nearest (always blocky).\n"
        "; TextureFilter=game\n"
        "; Ambient occlusion, a soft darkening of corners and creases: off, low or high.\n"
        "; SSAO=off\n"
        "; The game's full-screen glow: smooth (drawn at the PSP's size and enlarged smoothly), game (as the\n"
        "; game draws it: blocky at high resolutions) or off.\n"
        "; Glow=smooth\n"
        "; Start in borderless full screen. F11 or Alt+Enter switches while playing.\n"
        "; Fullscreen=false\n"
        "; Show each scene as soon as it is drawn, one game frame sooner than the PSP did (33 ms at 30 frames\n"
        "; per second). false: at the game's own copy to the screen.\n"
        "; EarlyPresent=true\n"
        "; Draw on a second processor core while the game computes its next frame. false: one core does both.\n"
        "; GeThread=true\n";
}

struct RenderSettings {
    enum class Backend { Software, D3D12 };
    enum class Preset { Low, Medium, High, Custom };
    Backend backend{Backend::D3D12};
    Preset preset{Preset::High};
    // internal_scale 0 is "auto": resolved_scale() picks the number.
    unsigned internal_scale{0u}, msaa{1u};
    enum class TextureFilter { Game, Linear, Nearest };
    enum class AmbientOcclusion { Off, Low, High };
    enum class Glow { Smooth, Game, Off };
    // A player's defaults: anisotropic filtering and FXAA on. scripts/unattended.ini switches both off,
    // so a scripted run's frame images are the game's own picture.
    unsigned anisotropy{16u};
    TextureFilter texture_filter{TextureFilter::Game};
    // Post-process edge smoothing; multisampling is `msaa`.
    bool fxaa{true};
    AmbientOcclusion ssao{AmbientOcclusion::Off};
    // scripts/unattended.ini sets game: a scripted run's frame images are what the game's draws say.
    Glow glow{Glow::Smooth};
    bool strict{}, fullscreen{}, early_present{true}, ge_thread{true};
    // CHILI_RENDER_GE_THREAD=force: the list thread also in a deterministic run (a test of the thread itself).
    bool ge_thread_forced{};
    std::vector<std::string> problems;

    static bool parse_backend(std::string_view text, Backend &out) {
        const auto name = ModSetup::lower(text);
        if (name == "d3d12" || name == "dx12" || name == "directx12") { out = Backend::D3D12; return true; }
        if (name == "software" || name == "cpu") { out = Backend::Software; return true; }
        return false;
    }
    static bool parse_number(std::string_view text, unsigned &out) {
        if (text.empty() || text.size() > 4u) return false;
        unsigned value = 0u;
        for (const char c : text) { if (c < '0' || c > '9') return false; value = value * 10u + static_cast<unsigned>(c - '0'); }
        out = value;
        return true;
    }
    // `where` names the source of a bad value in the problem text.
    void set(std::string_view name, std::string_view value, const std::string &where) {
        const auto problem = [&](const std::string &what) { problems.push_back(where + what); };
        unsigned number = 0u;
        if (ModSetup::same_text(name, "Preset")) {
            if (ModSetup::same_text(value, "low")) preset = Preset::Low;
            else if (ModSetup::same_text(value, "medium")) preset = Preset::Medium;
            else if (ModSetup::same_text(value, "high")) preset = Preset::High;
            else if (ModSetup::same_text(value, "custom")) preset = Preset::Custom;
            else problem("Preset must be low, medium, high or custom");
        } else if (ModSetup::same_text(name, "Backend")) {
            if (!parse_backend(value, backend)) problem("Backend must be d3d12 or software");
        } else if (ModSetup::same_text(name, "InternalScale")) {
            if (ModSetup::same_text(value, "auto")) internal_scale = 0u;
            else if (parse_number(value, number) && number >= 1u && number <= 8u) internal_scale = number;
            else problem("InternalScale must be auto or 1 to 8");
        } else if (ModSetup::same_text(name, "MSAA")) {
            if (parse_number(value, number) && (number == 1u || number == 2u || number == 4u || number == 8u)) { msaa = number; if (number > 1u) fxaa = false; }
            else problem("MSAA must be 1, 2, 4 or 8");
        } else if (ModSetup::same_text(name, "AntiAliasing")) {
            const auto choice = ModSetup::lower(value);
            if (choice == "off" || choice == "none") { msaa = 1u; fxaa = false; }
            else if (choice == "fxaa") { msaa = 1u; fxaa = true; }
            else if (choice == "msaa2") { msaa = 2u; fxaa = false; }
            else if (choice == "msaa4") { msaa = 4u; fxaa = false; }
            else if (choice == "msaa8") { msaa = 8u; fxaa = false; }
            else problem("AntiAliasing must be off, fxaa, msaa2, msaa4 or msaa8");
        } else if (ModSetup::same_text(name, "Anisotropy")) {
            if (parse_number(value, number) && (number == 1u || number == 2u || number == 4u || number == 8u || number == 16u)) anisotropy = number;
            else problem("Anisotropy must be 1, 2, 4, 8 or 16");
        } else if (ModSetup::same_text(name, "TextureFilter")) {
            const auto choice = ModSetup::lower(value);
            if (choice == "game") texture_filter = TextureFilter::Game;
            else if (choice == "linear") texture_filter = TextureFilter::Linear;
            else if (choice == "nearest") texture_filter = TextureFilter::Nearest;
            else problem("TextureFilter must be game, linear or nearest");
        } else if (ModSetup::same_text(name, "SSAO")) {
            const auto choice = ModSetup::lower(value);
            if (choice == "off" || choice == "false" || choice == "0") ssao = AmbientOcclusion::Off;
            else if (choice == "low") ssao = AmbientOcclusion::Low;
            else if (choice == "high") ssao = AmbientOcclusion::High;
            else problem("SSAO must be off, low or high");
        } else if (ModSetup::same_text(name, "Glow")) {
            const auto choice = ModSetup::lower(value);
            if (choice == "smooth") glow = Glow::Smooth;
            else if (choice == "game") glow = Glow::Game;
            else if (choice == "off") glow = Glow::Off;
            else problem("Glow must be smooth, game or off");
        } else if (ModSetup::same_text(name, "Fullscreen")) {
            if (ModSetup::same_text(value, "true") || value == "1") fullscreen = true;
            else if (ModSetup::same_text(value, "false") || value == "0") fullscreen = false;
            else problem("Fullscreen must be true or false");
        } else if (ModSetup::same_text(name, "EarlyPresent")) {
            if (ModSetup::same_text(value, "true") || value == "1") early_present = true;
            else if (ModSetup::same_text(value, "false") || value == "0") early_present = false;
            else problem("EarlyPresent must be true or false");
        } else if (ModSetup::same_text(name, "GeThread")) {
            if (ModSetup::same_text(value, "true") || value == "1") ge_thread = true;
            else if (ModSetup::same_text(value, "false") || value == "0") ge_thread = false;
            else problem("GeThread must be true or false");
        } else problem("unknown setting '" + std::string(name) + "'");
    }
    const char *preset_name() const noexcept {
        switch (preset) {
        case Preset::Low: return "low";
        case Preset::Medium: return "medium";
        case Preset::High: return "high";
        default: return "custom";
        }
    }
    // What the preset allows InternalScale=auto. The picture's other costs (multisampling, filtering,
    // post-processing) belong here too once they exist: one place per preset.
    unsigned auto_scale_limit() const noexcept { return preset == Preset::Low ? 2u : preset == Preset::Medium ? 4u : 8u; }
    // The scale to render at. auto: enough for a monitor `screen_height` pixels
    // high, at most what the preset allows (the renderer lowers it further on a
    // graphics card with little memory); an unattended run stays at the PSP's
    // own resolution.
    unsigned resolved_scale(unsigned screen_height, bool unattended) const {
        if (internal_scale) return internal_scale;
        if (unattended) return 1u;
        return std::clamp((screen_height + 271u) / 272u, 1u, auto_scale_limit());
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
            if (!ModSetup::same_text(section, "Rendering")) continue;
            const auto where = "line " + std::to_string(line_number) + ": ";
            const auto equals = line.find('=');
            if (equals == std::string_view::npos || ModSetup::trim(line.substr(0u, equals)).empty()) { problems.push_back(where + "expected name=value"); continue; }
            set(ModSetup::trim(line.substr(0u, equals)), ModSetup::trim(line.substr(equals + 1u)), where);
        }
    }
    // Settings from `ini` (it may be missing), then the environment.
    static RenderSettings load(const std::filesystem::path &ini) {
        RenderSettings settings;
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
        environment(L"CHILI_RENDER_PRESET", "Preset");
        environment(L"CHILI_RENDER_BACKEND", "Backend");
        environment(L"CHILI_RENDER_SCALE", "InternalScale");
        environment(L"CHILI_RENDER_MSAA", "MSAA");
        environment(L"CHILI_RENDER_AA", "AntiAliasing");
        environment(L"CHILI_RENDER_ANISOTROPY", "Anisotropy");
        environment(L"CHILI_RENDER_FILTER", "TextureFilter");
        environment(L"CHILI_RENDER_SSAO", "SSAO");
        environment(L"CHILI_RENDER_GLOW", "Glow");
        environment(L"CHILI_RENDER_EARLY_PRESENT", "EarlyPresent");
        if (ModSetup::environment(L"CHILI_RENDER_GE_THREAD") == L"force") settings.ge_thread = settings.ge_thread_forced = true;
        else environment(L"CHILI_RENDER_GE_THREAD", "GeThread");
        settings.strict = ModSetup::environment(L"CHILI_RENDER_STRICT") == L"1";
        return settings;
    }
};

// Adds the commented [Rendering] template to an ini that has no such section.
inline void add_default_render_settings(const std::filesystem::path &ini) {
    std::error_code status;
    if (!std::filesystem::is_regular_file(ini, status)) return;
    std::string text;
    {
        std::ifstream stream(ini, std::ios::binary);
        std::ostringstream content;
        content << stream.rdbuf();
        text = ModSetup::lower(content.str());
    }
    if (text.find("[rendering]") != std::string::npos) return;
    std::ofstream stream(ini, std::ios::binary | std::ios::app);
    stream << default_rendering_ini();
}
}
