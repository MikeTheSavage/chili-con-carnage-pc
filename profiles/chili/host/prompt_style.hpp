#pragma once
// On-screen button prompts that show the input in use.
//
// The game names PSP buttons, in three ways and all of them data: pictures in its text font (tutorial
// lines), five 32x32 sprites beside the menu's legend words, and the Cross sprite again as the "use"
// marker. The host binds every key, mouse button and pad button to a PSP button. So a prompt shows the
// picture of whatever is bound to that PSP button on the device the player used last, and seven
// textures carry all of it: the two font sheets and the five sprites. Nothing in guest memory changes.
//
//   style = f([Input] Prompts, [Input.Gamepad] Glyphs, device used last, kind of pad)   (input_device.hpp)
//   glyph = the binding of a PSP button in that style, by the priority rule below        (PcInputConfig)
//   picture = prompt_glyphs.hpp; into the textures = gpu/ge_texture_mods.hpp overlays
//
// PromptController::frame() runs once per game frame on the game thread, and sampled() with each pad sample
// the player owns, for the screens that end no frame. Direct3D 12 only: the software renderer draws from
// guest memory and shows the game's own PSP pictures, as does Prompts=psp.
//
// Scripted runs (a replay, the script channel, deterministic mode) compare frame pictures, so they show
// the PSP pictures whatever the ini says. Test variables, for scripted checks:
//   CHILI_PROMPTS=keyboard|xbox|playstation|psp   this style, whatever the settings and the devices say
//   CHILI_PROMPTS=auto                             a scripted run follows the settings like a player's (a
//                                                  deterministic one by its frame count, 30 to the second;
//                                                  one in real time by the real clock)
//   CHILI_PROMPTS_AT=<frame>:<style>[,...]         the forced style changes at these game frames
//   CHILI_PROMPT_DEVICE_AT=<frame>:<keyboard|xbox|playstation>[,...]
//                                                  at these frames the device used last is set, as the
//                                                  input code sets it when that device is touched
//   CHILI_PROMPT_KEY_ALL=1                         key every texture, not only the seven shapes (to measure)
//   CHILI_PROMPT_PAINTER=off                       as if Direct2D were missing: the PSP pictures and one log line
#include "gpu/ge_texture_mods.hpp"
#include "input_device.hpp"
#include "pc_input.hpp"
#include "prompt_glyphs.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace chili {
enum class PromptStyle : std::uint8_t { Psp = 0, Keyboard = 1, Xbox = 2, PlayStation = 3 };
inline const char *prompt_style_name(PromptStyle style) noexcept {
    switch (style) {
    case PromptStyle::Keyboard: return "keyboard and mouse";
    case PromptStyle::Xbox: return "Xbox pad";
    case PromptStyle::PlayStation: return "PlayStation pad";
    default: return "PSP";
    }
}
inline std::optional<PromptStyle> prompt_style_from_name(std::string_view name) noexcept {
    if (PcInputConfig::same_text(name, "keyboard")) return PromptStyle::Keyboard;
    if (PcInputConfig::same_text(name, "xbox")) return PromptStyle::Xbox;
    if (PcInputConfig::same_text(name, "playstation")) return PromptStyle::PlayStation;
    if (PcInputConfig::same_text(name, "psp")) return PromptStyle::Psp;
    return {};
}
// The style the settings and the input ask for. Prompts=auto follows the device used last; a gamepad
// looks as Glyphs says, and with Glyphs=auto like the pad used last (Xbox while none has been seen).
constexpr PromptStyle resolve_prompt_style(PromptsSetting prompts, GlyphsSetting glyphs, InputDevice device, PadKind kind) noexcept {
    const PromptStyle pad = glyphs == GlyphsSetting::Xbox ? PromptStyle::Xbox
        : glyphs == GlyphsSetting::PlayStation ? PromptStyle::PlayStation
        : kind == PadKind::PlayStation ? PromptStyle::PlayStation : PromptStyle::Xbox;
    switch (prompts) {
    case PromptsSetting::Psp: return PromptStyle::Psp;
    case PromptsSetting::Keyboard: return PromptStyle::Keyboard;
    case PromptsSetting::Gamepad: return pad;
    default: return device == InputDevice::Gamepad ? pad : PromptStyle::Keyboard;
    }
}
inline PromptStyle resolve_prompt_style(const InputDeviceState &state) noexcept {
    return resolve_prompt_style(state.prompts.load(std::memory_order_relaxed), state.glyphs.load(std::memory_order_relaxed),
        state.device.load(std::memory_order_relaxed), state.pad_kind.load(std::memory_order_relaxed));
}
// A style is shown once it has been asked for without a break for 250 ms, and not sooner than half a
// second after the switch before it: a mouse nudged while a pad is in the hands does not make the
// pictures flicker.
class PromptDebounce {
public:
    static constexpr std::uint64_t hold_ms = 250u, gap_ms = 500u;
    explicit PromptDebounce(PromptStyle start = PromptStyle::Keyboard) noexcept : shown_(start), wanted_(start) {}
    PromptStyle shown() const noexcept { return shown_; }
    PromptStyle update(PromptStyle wanted, std::uint64_t now_ms) noexcept {
        if (wanted != wanted_) { wanted_ = wanted; since_ = now_ms; }
        if (wanted_ != shown_ && now_ms - since_ >= hold_ms && (!switched_ || now_ms - switched_at_ >= gap_ms)) {
            shown_ = wanted_; switched_ = true; switched_at_ = now_ms;
        }
        return shown_;
    }
private:
    PromptStyle shown_, wanted_;
    std::uint64_t since_{}, switched_at_{};
    bool switched_{};
};

// ---- Which picture stands for a PSP button --------------------------------------------------------

// The cap of a key, named as the ini names it and shortened to fit: a letter is the one printed on the
// key in the keyboard layout in use (the ini binds by position); words that do not fit get a sign.
inline Glyph key_glyph(int vk) {
    Glyph glyph; glyph.kind = GlyphKind::Key;
    auto &cap = glyph.keys[0];
    const auto text = [&](std::string_view label) { cap = key_cap(label); };
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { const char c = static_cast<char>(vk); text(std::string_view(&c, 1u)); return glyph; }
    if (vk >= VK_F1 && vk <= VK_F12) { char name[4]; std::snprintf(name, sizeof(name), "F%d", vk - VK_F1 + 1); text(name); return glyph; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { const char name[3]{'N', static_cast<char>('0' + vk - VK_NUMPAD0), 0}; text(name); return glyph; }
    switch (vk) {
    case VK_SPACE: cap = key_cap(KeySign::Space); return glyph;
    case VK_RETURN: cap = key_cap(KeySign::Enter); return glyph;
    case VK_BACK: cap = key_cap(KeySign::Backspace); return glyph;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: cap = key_cap(KeySign::Shift); return glyph;
    case VK_UP: cap = key_cap(KeySign::Up); return glyph;
    case VK_DOWN: cap = key_cap(KeySign::Down); return glyph;
    case VK_LEFT: cap = key_cap(KeySign::Left); return glyph;
    case VK_RIGHT: cap = key_cap(KeySign::Right); return glyph;
    case VK_ESCAPE: text("Esc"); return glyph;
    case VK_TAB: text("Tab"); return glyph;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: text("Ctrl"); return glyph;
    case VK_MENU: case VK_LMENU: case VK_RMENU: text("Alt"); return glyph;
    case VK_INSERT: text("Ins"); return glyph;
    case VK_DELETE: text("Del"); return glyph;
    case VK_HOME: text("Home"); return glyph;
    case VK_END: text("End"); return glyph;
    case VK_PRIOR: text("PgUp"); return glyph;
    case VK_NEXT: text("PgDn"); return glyph;
    case VK_CAPITAL: text("Caps"); return glyph;
    default: break;
    }
    // The punctuation keys: what the layout in use prints on them, else the US character of the ini's name.
    static constexpr std::pair<int, char> punctuation[]{{VK_OEM_COMMA, ','}, {VK_OEM_PERIOD, '.'}, {VK_OEM_2, '/'}, {VK_OEM_1, ';'},
        {VK_OEM_7, '\''}, {VK_OEM_4, '['}, {VK_OEM_6, ']'}, {VK_OEM_5, '\\'}, {VK_OEM_MINUS, '-'}, {VK_OEM_PLUS, '='}, {VK_OEM_3, '`'}};
    for (const auto &[key, us] : punctuation) if (key == vk) {
        const UINT mapped = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_CHAR) & 0x7FFFu;
        char c = mapped > 0x20u && mapped < 0x100u && mapped != 0x7Fu ? static_cast<char>(static_cast<unsigned char>(mapped)) : us;
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        text(std::string_view(&c, 1u));
        return glyph;
    }
    text("?");
    return glyph;
}
inline bool prompt_dpad_target(PadTarget target) noexcept {
    return target == PadTarget::DpadUp || target == PadTarget::DpadDown || target == PadTarget::DpadLeft || target == PadTarget::DpadRight;
}
// Keyboard and mouse: the left and right mouse buttons before any key, a key before the other mouse
// buttons and the wheel; among the keys of a D-pad target an arrow key first, otherwise the first key the
// ini names.
inline Glyph keyboard_prompt_glyph(const PcInputConfig &config, PadTarget target) {
    Glyph glyph;
    if (target == PadTarget::None) return glyph;
    const auto mouse = [&](MousePart part) { glyph.kind = GlyphKind::Mouse; glyph.index = static_cast<std::uint8_t>(part); return glyph; };
    if (config.mouse_enabled) {
        if (config.mouse_buttons[0] == target) return mouse(MousePart::Left);
        if (config.mouse_buttons[1] == target) return mouse(MousePart::Right);
    }
    int first = 0, arrow = 0;
    for (const auto &binding : config.keys) {
        if (binding.target != target) continue;
        if (!first) first = binding.vk;
        if (!arrow && (binding.vk == VK_UP || binding.vk == VK_DOWN || binding.vk == VK_LEFT || binding.vk == VK_RIGHT)) arrow = binding.vk;
    }
    if (prompt_dpad_target(target) && arrow) return key_glyph(arrow);
    if (first) return key_glyph(first);
    if (config.mouse_enabled) {
        if (config.mouse_buttons[2] == target) return mouse(MousePart::Middle);
        if (config.mouse_buttons[3] == target) return mouse(MousePart::Side4);
        if (config.mouse_buttons[4] == target) return mouse(MousePart::Side5);
        if (config.wheel_up == target) return mouse(MousePart::WheelUp);
        if (config.wheel_down == target) return mouse(MousePart::WheelDown);
    }
    return glyph;
}
// Place of a named pad button in PcInputConfig::pad_buttons (the two triggers follow the 14 names).
constexpr std::size_t prompt_pad_index(std::string_view name) noexcept {
    for (std::size_t i = 0; i < PcInputConfig::pad_button_names.size(); ++i)
        if (name == PcInputConfig::pad_button_names[i].first) return i;
    return name == "LT" ? PcInputConfig::pad_left_trigger : name == "RT" ? PcInputConfig::pad_right_trigger : 16u;
}
// The picture of pad button `index` of PcInputConfig::pad_buttons. A PlayStation pad uses the table by
// position: A is Cross, B Circle, X Square, Y Triangle, LB L1, LT L2, LS L3, Back Share, Start Options.
inline Glyph pad_button_glyph(std::size_t index, PadLook look) {
    Glyph glyph; glyph.look = look;
    const auto is = [&](std::string_view name) { return index == prompt_pad_index(name); };
    const auto face = [&](std::uint8_t which) { glyph.kind = look == PadLook::PlayStation ? GlyphKind::PlayStationFace : GlyphKind::XboxFace; glyph.index = which; };
    const auto dpad = [&](std::uint8_t arm) { glyph.kind = GlyphKind::Dpad; glyph.directions = arm; };
    const auto side = [&](GlyphKind kind, std::uint8_t right) { glyph.kind = kind; glyph.index = right; };
    if (is("A")) face(xbox_a); else if (is("B")) face(xbox_b); else if (is("X")) face(xbox_x); else if (is("Y")) face(xbox_y);
    else if (is("DpadUp")) dpad(glyph_up); else if (is("DpadDown")) dpad(glyph_down);
    else if (is("DpadLeft")) dpad(glyph_left); else if (is("DpadRight")) dpad(glyph_right);
    else if (is("LB")) side(GlyphKind::Shoulder, 0u); else if (is("RB")) side(GlyphKind::Shoulder, 1u);
    else if (is("LT")) side(GlyphKind::Trigger, 0u); else if (is("RT")) side(GlyphKind::Trigger, 1u);
    else if (is("LS")) side(GlyphKind::Stick, 0u); else if (is("RS")) side(GlyphKind::Stick, 1u);
    else if (is("Start")) glyph.kind = GlyphKind::Menu; else if (is("Back")) glyph.kind = GlyphKind::View;
    return glyph;
}
// A pad: the control of the same name when it is bound to the target (the D-pad to the D-pad, Start to
// Start, Back to Select), otherwise the first button in the table's order.
inline Glyph pad_prompt_glyph(const std::array<PadTarget, 16> &buttons, PadTarget target, PadLook look) {
    Glyph glyph; glyph.look = look;
    if (target == PadTarget::None) return glyph;
    static constexpr std::pair<PadTarget, const char *> same_name[]{{PadTarget::DpadUp, "DpadUp"}, {PadTarget::DpadDown, "DpadDown"},
        {PadTarget::DpadLeft, "DpadLeft"}, {PadTarget::DpadRight, "DpadRight"}, {PadTarget::Start, "Start"}, {PadTarget::Select, "Back"}};
    for (const auto &[named, button] : same_name)
        if (named == target) { const auto index = prompt_pad_index(button); if (index < buttons.size() && buttons[index] == target) return pad_button_glyph(index, look); }
    for (std::size_t index = 0; index < buttons.size(); ++index) if (buttons[index] == target) return pad_button_glyph(index, look);
    return glyph;
}
// The pad's table as a pad of that look reads it. The gamepad code's PcInputConfig::pad_target(button, kind)
// knows a separate PlayStation layout ([Input.Gamepad.PlayStation]); a configuration without that accessor
// has the one table for both.
template <typename Config>
std::array<PadTarget, 16> prompt_pad_table(const Config &config, PadLook look) {
    std::array<PadTarget, 16> table = config.pad_buttons;
    if constexpr (requires { { config.pad_target(std::size_t{0}, PadKind::PlayStation) } -> std::convertible_to<PadTarget>; }) {
        const PadKind kind = look == PadLook::PlayStation ? PadKind::PlayStation : PadKind::Xbox;
        for (std::size_t button = 0; button < table.size(); ++button) table[button] = config.pad_target(button, kind);
    }
    return table;
}
inline Glyph prompt_glyph(const PcInputConfig &config, PromptStyle style, PadTarget target) {
    if (style == PromptStyle::Keyboard) return keyboard_prompt_glyph(config, target);
    const PadLook look = style == PromptStyle::PlayStation ? PadLook::PlayStation : PadLook::Xbox;
    return pad_prompt_glyph(prompt_pad_table(config, look), target, look);
}
// The PSP's analog stick, pushed in `directions`: the move keys, or the left stick (the sticks of a pad
// are not bound, the left one always moves).
inline Glyph prompt_move_glyph(const PcInputConfig &config, PromptStyle style, std::uint8_t directions) {
    Glyph glyph;
    if (style != PromptStyle::Keyboard) {
        glyph.kind = GlyphKind::Stick; glyph.look = style == PromptStyle::PlayStation ? PadLook::PlayStation : PadLook::Xbox;
        glyph.directions = directions;
        return glyph;
    }
    const auto first_key = [&](PadTarget target) { for (const auto &binding : config.keys) if (binding.target == target) return binding.vk; return 0; };
    const auto cap = [&](PadTarget target) { const int vk = first_key(target); return vk ? key_glyph(vk).keys[0] : key_cap("-"); };
    static constexpr std::pair<std::uint8_t, PadTarget> single[]{{glyph_up, PadTarget::MoveForward}, {glyph_left, PadTarget::MoveLeft},
        {glyph_down, PadTarget::MoveBack}, {glyph_right, PadTarget::MoveRight}};
    for (const auto &[direction, target] : single) if (directions == direction) { const int vk = first_key(target); return vk ? key_glyph(vk) : glyph; }
    glyph.kind = GlyphKind::KeyCluster;
    glyph.keys = {cap(PadTarget::MoveForward), cap(PadTarget::MoveLeft), cap(PadTarget::MoveBack), cap(PadTarget::MoveRight)};
    return glyph;
}

// ---- The seven textures -----------------------------------------------------------------------------

// Content keys of mods/TEXTURES.md and the GE format of each texture.
inline constexpr std::uint64_t prompt_font_fill_key = 0x5d0c6d1a663a8515ull;      // /data/Fonts/ODMidText_24.font, 256x512 CLUT8
inline constexpr std::uint64_t prompt_font_outline_key = 0x002d10b31e312e9dull;   // ODMidText_24_Stroke3.font, 256x512 CLUT4
struct PromptSprite { std::uint64_t key; std::uint32_t format; PadTarget target, second; };
inline constexpr std::array<PromptSprite, 5> prompt_sprites{{
    {0xdc8cc6b510da1d80ull, 5u, PadTarget::Cross, PadTarget::None},       // /data/Textures/Menu/ButtonCross.bmp; also the "use" marker
    {0xebff042e1d0cf176ull, 5u, PadTarget::Circle, PadTarget::None},      // ButtonCircle.bmp
    {0xe6fd3c31244ec517ull, 5u, PadTarget::Square, PadTarget::None},      // ButtonSquare.bmp
    {0xeabe4d818979a897ull, 5u, PadTarget::Triangle, PadTarget::None},    // ButtonTriangle.bmp
    {0xe8cc8b252a16c095ull, 9u, PadTarget::DpadLeft, PadTarget::DpadRight}}};  // ButtonDpadLR.bmp, DXT3
// A picture of the text font: its place on the 256x512 sheet (30 rows high) and what it stands for. The
// same table in both sheets. Left as the PSP draws them: HOME, the volume, display and sound buttons and
// the D-pad with nothing lit.
struct PromptFontCell { std::uint16_t code, x, y, width; PadTarget target; std::uint8_t move; };
inline constexpr std::uint32_t prompt_font_cell_height = 30u;
inline constexpr std::array<PromptFontCell, 17> prompt_font_cells{{
    {0x00A2u, 0u, 330u, 20u, PadTarget::Square, 0u}, {0x00A3u, 21u, 330u, 20u, PadTarget::Triangle, 0u},
    {0x00A4u, 42u, 330u, 20u, PadTarget::Circle, 0u}, {0x00A5u, 63u, 330u, 20u, PadTarget::Cross, 0u},
    {0x00A7u, 111u, 330u, 27u, PadTarget::Start, 0u}, {0x00A8u, 139u, 330u, 27u, PadTarget::Select, 0u},
    {0x00AFu, 0u, 360u, 28u, PadTarget::L, 0u}, {0x00B1u, 29u, 360u, 28u, PadTarget::R, 0u},
    {0x00B3u, 86u, 360u, 27u, PadTarget::DpadUp, 0u}, {0x00BCu, 114u, 360u, 27u, PadTarget::DpadRight, 0u},
    {0x00B5u, 142u, 360u, 27u, PadTarget::DpadDown, 0u}, {0x00B6u, 170u, 360u, 27u, PadTarget::DpadLeft, 0u},
    {0x00B7u, 198u, 360u, 22u, PadTarget::None, glyph_all_directions}, {0x00B8u, 221u, 360u, 20u, PadTarget::None, glyph_up},
    {0x00B9u, 0u, 390u, 21u, PadTarget::None, glyph_right}, {0x00BAu, 22u, 390u, 20u, PadTarget::None, glyph_down},
    {0x00BBu, 43u, 390u, 22u, PadTarget::None, glyph_left}}};
inline Glyph prompt_cell_glyph(const PcInputConfig &config, PromptStyle style, const PromptFontCell &cell) {
    return cell.move ? prompt_move_glyph(config, style, cell.move) : prompt_glyph(config, style, cell.target);
}
// Where a glyph sits in a font cell, in PSP pixels from the cell's corner: a pixel in from the sides (the
// outline twin's edge fills it), and the rows the font's capitals and the PSP's own discs use.
inline constexpr float prompt_cell_inset = 1.0f, prompt_cell_top = 4.5f, prompt_cell_rows = 20.0f;
// Font sheets are composed at one, two or four times their size: a tutorial line draws a cell at about
// half a screen pixel per PSP pixel and internal scale, so four is one texel per pixel at scale 8, and a
// sheet with its smaller levels (11 MB) still fits a frame's upload arena. A power of two, so that the
// composed sheet's last level is the game's own sheet (GeTextureReplacement::exact_levels) and the game's
// text looks as it did. Sprites follow the internal scale, one to eight.
inline constexpr unsigned prompt_sheet_scale(unsigned internal_scale) noexcept { return internal_scale >= 4u ? 4u : internal_scale >= 2u ? 2u : 1u; }
// The scales a picture is painted at for a texture `scale` times the game's size: one per level of the
// composed texture when the scale is a power of two (4, 2, 1), otherwise that scale alone.
inline std::vector<unsigned> prompt_level_scales(unsigned scale) {
    std::vector<unsigned> scales{scale};
    if ((scale & (scale - 1u)) == 0u) for (unsigned next = scale / 2u; next >= 1u; next /= 2u) scales.push_back(next);
    return scales;
}

// The overlays of one style for the seven textures, in the order fill sheet, outline sheet, five sprites.
using PromptOverlays = std::array<std::shared_ptr<const vcs::GeTextureOverlay>, 7>;
inline constexpr std::array<vcs::GeTextureOverlayKey, 7> prompt_overlay_keys{{
    {prompt_font_fill_key, 5u, 256u, 512u}, {prompt_font_outline_key, 4u, 256u, 512u},
    {prompt_sprites[0].key, prompt_sprites[0].format, 32u, 32u}, {prompt_sprites[1].key, prompt_sprites[1].format, 32u, 32u},
    {prompt_sprites[2].key, prompt_sprites[2].format, 32u, 32u}, {prompt_sprites[3].key, prompt_sprites[3].format, 32u, 32u},
    {prompt_sprites[4].key, prompt_sprites[4].format, 32u, 32u}}};
// A cell of `width` x `height` PSP pixels at (x, y) whose picture is the same rectangle of each canvas,
// one canvas per level (the first at `scales[0]` pixels per PSP pixel, then the smaller ones).
inline vcs::GeTextureOverlayCell prompt_overlay_cell(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height,
                                                     const std::vector<GlyphPicture> &canvases, const std::vector<unsigned> &scales,
                                                     std::uint32_t canvas_x, std::uint32_t canvas_y) {
    vcs::GeTextureOverlayCell cell;
    cell.x = x; cell.y = y; cell.width = width; cell.height = height;
    cell.picture_width = width * scales[0]; cell.picture_height = height * scales[0];
    cell.picture_levels = static_cast<std::uint32_t>(canvases.size());
    for (std::size_t level = 0; level < canvases.size(); ++level) {
        const unsigned scale = scales[level];
        const GlyphPicture cut = glyph_cut(canvases[level], canvas_x * scale, canvas_y * scale, width * scale, height * scale);
        const auto *bytes = reinterpret_cast<const std::byte *>(cut.rgba.data());
        cell.rgba8.insert(cell.rgba8.end(), bytes, bytes + cut.rgba.size());
    }
    return cell;
}
// The two sprites' worth of glyphs for the menu's left/right sprite: the D-pad with both arms lit when a
// pad's own D-pad does both, otherwise the two bindings side by side.
inline bool prompt_dpad_pair(const Glyph &left, const Glyph &right) noexcept {
    return left.kind == GlyphKind::Dpad && right.kind == GlyphKind::Dpad;
}

// ---- The font's marks on a larger sheet ---------------------------------------------------------------

// The padlocks and the chili of the text font ('{' open, '}' closed, '~'): pictures of 26 and 23 by 30
// texels. A mod that replaces the font sheets with larger ones (an HD lettering pack) can only enlarge
// them, and beside its sharp letters they are the one thing that is not sharp: the settings page shows the
// closed lock after every locked row and the chili after every value that waits for a restart, the game its
// locks in the mission lists. So where a mod replaces the fill sheet these three cells are painted new
// (prompt_glyphs.hpp, GlyphKind::Lock and Chili) at eight times the game's size, in every style, the PSP
// one included. Without such a mod nothing is painted: the game's own pictures go with its own letters.
struct PromptMarkCell { std::uint16_t code, x, y, width; GlyphKind kind; std::uint8_t index; };
inline constexpr std::array<PromptMarkCell, 3> prompt_mark_cells{{
    {u'{', 27u, 420u, 26u, GlyphKind::Lock, 1u}, {u'}', 54u, 420u, 26u, GlyphKind::Lock, 0u}, {u'~', 212u, 420u, 23u, GlyphKind::Chili, 0u}}};
inline constexpr unsigned prompt_mark_scale = 8u;
// Where a mark sits in its cell, in PSP pixels: a pixel and a half in from the sides and the top (the outline
// twin grows by one), over 26 of the 29 rows the game draws of a cell.
inline constexpr float prompt_mark_inset = 1.5f, prompt_mark_rows = 26.0f;
struct PromptMarks {
    std::vector<vcs::GeTextureOverlayCell> fill, outline;
    [[nodiscard]] bool empty() const noexcept { return fill.empty(); }
};
inline bool paint_prompt_marks(GlyphPainter &painter, unsigned scale, PromptMarks &out, std::string &error) {
    out = {};
    const auto scales = prompt_level_scales(scale);
    const std::uint32_t top = prompt_mark_cells[0].y;
    std::vector<GlyphPicture> fills, outlines;
    for (const unsigned level : scales) {
        const float unit = static_cast<float>(level);
        if (!painter.begin(256u * level, prompt_font_cell_height * level, error)) return false;
        for (const auto &cell : prompt_mark_cells) {
            Glyph glyph; glyph.kind = cell.kind; glyph.index = cell.index;
            painter.draw(glyph, GlyphBox{(static_cast<float>(cell.x) + prompt_mark_inset) * unit, (static_cast<float>(cell.y - top) + prompt_mark_inset) * unit,
                (static_cast<float>(cell.width) - 2.0f * prompt_mark_inset) * unit, prompt_mark_rows * unit});
        }
        GlyphPicture fill;
        if (!painter.end(fill, error)) return false;
        outlines.push_back(glyph_outline(fill, level));
        fills.push_back(std::move(fill));
    }
    for (const auto &cell : prompt_mark_cells) {
        out.fill.push_back(prompt_overlay_cell(cell.x, cell.y, cell.width, prompt_font_cell_height, fills, scales, cell.x, cell.y - top));
        out.outline.push_back(prompt_overlay_cell(cell.x, cell.y, cell.width, prompt_font_cell_height, outlines, scales, cell.x, cell.y - top));
    }
    return true;
}
// Paints one style's pictures: the font cells on one canvas (their outline twins are grown from it), the
// sprites on another, and both again at each smaller scale a level of the composed texture has, so a small
// picture is drawn for its size and not shrunk from a large one. `sheet_scale` and `sprite_scale` are
// pixels per PSP pixel.
inline bool paint_prompt_overlays(GlyphPainter &painter, const PcInputConfig &config, PromptStyle style, unsigned sheet_scale,
                                  unsigned sprite_scale, PromptOverlays &out, std::string &error, const PromptMarks *marks = nullptr) {
    // `marks`: the font's lock and chili drawn new for a mod's larger sheet; they go into both sheets in
    // every style, and are all the PSP style has.
    const auto with_marks = [&](vcs::GeTextureOverlay &fill_sheet, vcs::GeTextureOverlay &outline_sheet) {
        if (!marks) return;
        fill_sheet.cells.insert(fill_sheet.cells.end(), marks->fill.begin(), marks->fill.end());
        outline_sheet.cells.insert(outline_sheet.cells.end(), marks->outline.begin(), marks->outline.end());
    };
    if (style == PromptStyle::Psp) {
        out = {};
        if (marks && !marks->empty()) {
            auto fill_sheet = std::make_shared<vcs::GeTextureOverlay>(), outline_sheet = std::make_shared<vcs::GeTextureOverlay>();
            for (const auto &sheet : {fill_sheet, outline_sheet}) { sheet->width = 256u; sheet->height = 512u; sheet->scale = sheet_scale; }
            with_marks(*fill_sheet, *outline_sheet);
            out[0] = std::move(fill_sheet);
            out[1] = std::move(outline_sheet);
        }
        return true;
    }
    // The rows of the sheet that hold the cells.
    std::uint32_t top = 512u, bottom = 0u;
    for (const auto &cell : prompt_font_cells) { top = std::min<std::uint32_t>(top, cell.y); bottom = std::max<std::uint32_t>(bottom, cell.y + prompt_font_cell_height); }
    const auto sheet_scales = prompt_level_scales(sheet_scale);
    std::vector<GlyphPicture> fills, outlines;
    for (const unsigned scale : sheet_scales) {
        const float unit = static_cast<float>(scale);
        if (!painter.begin(256u * scale, (bottom - top) * scale, error)) return false;
        for (const auto &cell : prompt_font_cells)
            painter.draw(prompt_cell_glyph(config, style, cell), GlyphBox{(static_cast<float>(cell.x) + prompt_cell_inset) * unit,
                (static_cast<float>(cell.y - top) + prompt_cell_top) * unit, (static_cast<float>(cell.width) - 2.0f * prompt_cell_inset) * unit, prompt_cell_rows * unit});
        GlyphPicture fill;
        if (!painter.end(fill, error)) return false;
        outlines.push_back(glyph_outline(fill, scale));
        fills.push_back(std::move(fill));
    }
    auto fill_sheet = std::make_shared<vcs::GeTextureOverlay>(), outline_sheet = std::make_shared<vcs::GeTextureOverlay>();
    for (const auto &sheet : {fill_sheet, outline_sheet}) { sheet->width = 256u; sheet->height = 512u; sheet->scale = sheet_scale; }
    for (const auto &cell : prompt_font_cells) {
        fill_sheet->cells.push_back(prompt_overlay_cell(cell.x, cell.y, cell.width, prompt_font_cell_height, fills, sheet_scales, cell.x, cell.y - top));
        outline_sheet->cells.push_back(prompt_overlay_cell(cell.x, cell.y, cell.width, prompt_font_cell_height, outlines, sheet_scales, cell.x, cell.y - top));
    }
    with_marks(*fill_sheet, *outline_sheet);
    out[0] = std::move(fill_sheet);
    out[1] = std::move(outline_sheet);
    // The sprites: a disc or cap of 29 PSP pixels in the 32x32 picture, as large as the game's own.
    const auto sprite_scales = prompt_level_scales(sprite_scale);
    std::vector<GlyphPicture> sprites;
    for (const unsigned scale : sprite_scales) {
        const float sprite = static_cast<float>(scale);
        if (!painter.begin(32u * scale * static_cast<std::uint32_t>(prompt_sprites.size()), 32u * scale, error)) return false;
        for (std::size_t i = 0; i < prompt_sprites.size(); ++i) {
            const float left = static_cast<float>(i) * 32.0f * sprite;
            const Glyph first = prompt_glyph(config, style, prompt_sprites[i].target);
            if (prompt_sprites[i].second == PadTarget::None) { painter.draw(first, GlyphBox{left + 1.5f * sprite, 1.5f * sprite, 29.0f * sprite, 29.0f * sprite}); continue; }
            Glyph second = prompt_glyph(config, style, prompt_sprites[i].second);
            if (prompt_dpad_pair(first, second)) {
                second.directions = static_cast<std::uint8_t>(second.directions | first.directions);
                painter.draw(second, GlyphBox{left + 1.5f * sprite, 1.5f * sprite, 29.0f * sprite, 29.0f * sprite});
            } else {
                painter.draw(first, GlyphBox{left + 0.5f * sprite, 6.0f * sprite, 15.0f * sprite, 20.0f * sprite});
                painter.draw(second, GlyphBox{left + 16.5f * sprite, 6.0f * sprite, 15.0f * sprite, 20.0f * sprite});
            }
        }
        GlyphPicture canvas;
        if (!painter.end(canvas, error)) return false;
        sprites.push_back(std::move(canvas));
    }
    for (std::size_t i = 0; i < prompt_sprites.size(); ++i) {
        auto overlay = std::make_shared<vcs::GeTextureOverlay>();
        overlay->width = 32u; overlay->height = 32u; overlay->scale = sprite_scale;
        overlay->cells.push_back(prompt_overlay_cell(0u, 0u, 32u, 32u, sprites, sprite_scales, 32u * static_cast<std::uint32_t>(i), 0u));
        out[2u + i] = std::move(overlay);
    }
    return true;
}

// ---- The controller -----------------------------------------------------------------------------------

class PromptController {
public:
    PromptController() = default;
    PromptController(const PromptController &) = delete;
    PromptController &operator=(const PromptController &) = delete;
    ~PromptController() { for (const auto &line : report()) std::printf("[prompts] %s\n", line.c_str()); std::fflush(stdout); }
    // Call once, after the renderer and the texture mods have started and before the first draw.
    // `internal_scale` is the hardware renderer's, 0 when the software renderer draws. `scripted`: a replay,
    // the script channel or deterministic mode: the PSP pictures unless a test variable asks for others.
    // `frame_clock`: the run's time is its frame count (a deterministic run goes as fast as the host lets
    // it), so the debounce counts frames of a thirtieth of a second and the pad-sample path does not look.
    // A scripted run in real time that is made to follow the input (CHILI_PROMPTS=auto) goes by the real
    // clock, like a player's. `settings_can_change`: the run has the game's own settings page, where Prompts
    // can leave `psp` while the game runs: the pictures are painted and their textures watched also when
    // the setting says psp at the start (the PSP pictures are then shown until it changes). Returns the lines
    // for the log.
    std::vector<std::string> start(const PcInputConfig &config, unsigned internal_scale, bool scripted) { return start(config, internal_scale, scripted, scripted); }
    std::vector<std::string> start(const PcInputConfig &config, unsigned internal_scale, bool scripted, bool frame_clock, bool settings_can_change = false) {
        config_ = config; frame_clock_ = frame_clock;
        std::vector<std::string> lines;
        const auto variable = [](const char *name) { const char *value = std::getenv(name); return std::string(value ? value : ""); };
        const std::string asked = variable("CHILI_PROMPTS");
        bool follow = !scripted;
        if (PcInputConfig::same_text(asked, "auto")) follow = true;
        else if (!asked.empty()) {
            if (const auto style = prompt_style_from_name(asked)) forced_ = *style;
            else lines.push_back("CHILI_PROMPTS must be keyboard, xbox, playstation, psp or auto: '" + asked + "' ignored");
        }
        parse_steps(variable("CHILI_PROMPTS_AT"), style_steps_, lines, "CHILI_PROMPTS_AT", true);
        parse_steps(variable("CHILI_PROMPT_DEVICE_AT"), device_steps_, lines, "CHILI_PROMPT_DEVICE_AT", false);
        if (!device_steps_.empty()) follow = true;
        auto &state = input_device_state();
        revision_ = state.bindings_revision.load(std::memory_order_relaxed);
        const bool psp_setting = state.prompts.load(std::memory_order_relaxed) == PromptsSetting::Psp;
        // A mod has a font sheet of its own: the font's padlocks and chili are drawn new for it (PromptMarks),
        // whatever the style, so the painter starts also with Prompts=psp.
        const bool marks_wanted = internal_scale != 0u && vcs::ge_texture_mods_has(prompt_font_fill_key);
        if (!forced_ && style_steps_.empty()) {
            if (!follow) { lines.push_back("the game's own PSP pictures (a scripted run; CHILI_PROMPTS=keyboard|xbox|playstation|auto shows others)"); return lines; }
            if (psp_setting && !settings_can_change && !marks_wanted) { lines.push_back("the game's own PSP pictures ([Input] Prompts=psp)"); return lines; }
        }
        if (internal_scale == 0u) { lines.push_back("the game's own PSP pictures (the software renderer draws them from the game's memory)"); return lines; }
        std::string error;
        if (PcInputConfig::same_text(variable("CHILI_PROMPT_PAINTER"), "off")) error = "the painter is switched off (CHILI_PROMPT_PAINTER)";
        if (!error.empty() || !painter_.start(error)) { lines.push_back("the game's own PSP pictures: " + error); return lines; }
        sheet_scale_ = prompt_sheet_scale(internal_scale);
        sprite_scale_ = std::min(internal_scale, 8u);
        if (marks_wanted) {
            std::string why;
            if (paint_prompt_marks(painter_, prompt_mark_scale, marks_, why)) lines.push_back("a mod has its own lettering: the font's padlocks and its chili are drawn new for it, at 8x");
            else { marks_ = {}; lines.push_back("the font's padlocks and chili stay the mod's own: " + why); }
        }
        if (!paint(error)) { lines.push_back("the game's own PSP pictures: " + error); return lines; }
        active_ = true;
        vcs::ge_texture_mods_watch_overlays(prompt_overlay_keys, !variable("CHILI_PROMPT_KEY_ALL").empty());
        const PromptStyle first = forced_ ? *forced_ : follow ? resolve_prompt_style(state) : PromptStyle::Psp;
        follow_ = follow;
        debounce_ = PromptDebounce(first);
        show(first, true);
        char text[320];
        std::snprintf(text, sizeof(text), "pictures of %s%s; painted in %.1f ms (%s, font sheets at %ux, sprites at %ux)",
            first == PromptStyle::Psp && !forced_ && follow && psp_setting ? "the game's own PSP buttons ([Input] Prompts=psp), the others ready for a change of the setting" : prompt_style_name(first),
            forced_ ? " (CHILI_PROMPTS)" : follow ? ", following the input in use ([Input] Prompts=auto|keyboard|gamepad|psp)" : "",
            paint_ms_, painter_.font_name().c_str(), sheet_scale_, sprite_scale_);
        lines.emplace_back(text);
        return lines;
    }
    bool active() const noexcept { return active_; }
    PromptStyle shown() const noexcept { return shown_; }
    std::uint64_t switches() const noexcept { return switches_; }
    // Once per game frame, before the frame is drawn. `frame` is the number of the frame that just ended.
    void frame(std::uint64_t frame) { tick(frame, frame_clock_ ? frame * 100u / 3u : real_ms()); }
    // From the pad-sample path, for the player's own samples. Some screens read the pad without ending a
    // game frame (the briefing's Continue): no frame() comes there, and a change of device would not show
    // until the next screen. So the style is looked at here too, but only when no look has been taken for
    // sample_tick_ms of the real clock: while frames end, frame() decides and this does nothing. Never in a
    // run whose clock is its frame count (a deterministic run). True when the style was looked at.
    static constexpr std::uint64_t sample_tick_ms = 50u;
    bool sampled() { return sample_tick(frame_, real_ms()); }
    bool sample_tick(std::uint64_t frame, std::uint64_t now_ms) {
        if (!active_ || frame_clock_ || now_ms - ticked_ms_ < sample_tick_ms) return false;
        tick(frame, now_ms);
        ++sample_ticks_;
        return true;
    }
    // Looks the sample path has taken (0 in a run where every look was a frame's).
    std::uint64_t sample_ticks() const noexcept { return sample_ticks_; }
    void tick(std::uint64_t frame, std::uint64_t now_ms) {
        if (!active_) return;
        frame_ = frame; ticked_ms_ = now_ms;
        auto &state = input_device_state();
        while (!device_steps_.empty() && frame >= device_steps_.front().first) {
            // What the input code does when that device is touched.
            const PromptStyle device = device_steps_.front().second;
            device_steps_.erase(device_steps_.begin());
            state.device.store(device == PromptStyle::Keyboard ? InputDevice::KeyboardMouse : InputDevice::Gamepad, std::memory_order_relaxed);
            if (device != PromptStyle::Keyboard)
                state.pad_kind.store(device == PromptStyle::PlayStation ? PadKind::PlayStation : PadKind::Xbox, std::memory_order_relaxed);
        }
        while (!style_steps_.empty() && frame >= style_steps_.front().first) { forced_ = style_steps_.front().second; style_steps_.erase(style_steps_.begin()); }
        report_switch(frame, now_ms);
        // New bindings: the pictures are painted again and shown at once.
        if (const auto revision = state.bindings_revision.load(std::memory_order_relaxed); revision != revision_) {
            revision_ = revision;
            config_ = PcInput::instance().config();
            std::string error;
            if (paint(error)) show(shown_, false);
        }
        const PromptStyle wanted = forced_ ? *forced_ : follow_ ? debounce_.update(resolve_prompt_style(state), now_ms) : PromptStyle::Psp;
        if (wanted == shown_) return;
        report_switch(~std::uint64_t{0}, now_ms);
        const PromptStyle before = shown_;
        show(wanted, false);
        ++switches_;
        pending_ = {true, frame, now_ms, before, vcs::ge_texture_mods_report()};
    }
    // The lines for the end of the log.
    std::vector<std::string> report() {
        std::vector<std::string> lines;
        if (!active_ || reported_) return lines;
        reported_ = true;
        report_switch(~std::uint64_t{0}, 0u);
        const auto r = vcs::ge_texture_mods_report();
        char text[400];
        std::snprintf(text, sizeof(text), "%llu style change%s; pictures put into %llu textures in %.2f ms (largest %.2f ms), %llu taken out of the cache; "
            "textures keyed for it: %llu in %.3f ms (largest %.3f ms); looks at the style from the pad-sample path, where no frame ended: %llu",
            static_cast<unsigned long long>(switches_), switches_ == 1u ? "" : "s",
            static_cast<unsigned long long>(r.overlay_composed), static_cast<double>(r.overlay_compose_us) / 1000.0, static_cast<double>(r.overlay_compose_max_us) / 1000.0,
            static_cast<unsigned long long>(r.overlay_evicted), static_cast<unsigned long long>(r.keyed_textures),
            static_cast<double>(r.keying_ns) / 1.0e6, static_cast<double>(r.keying_max_ns) / 1.0e6, static_cast<unsigned long long>(sample_ticks_));
        lines.emplace_back(text);
        return lines;
    }
private:
    using Steps = std::vector<std::pair<std::uint64_t, PromptStyle>>;
    struct Pending { bool waiting{}; std::uint64_t frame{}, ms{}; PromptStyle before{}; vcs::GeTextureModsReport at{}; };
    static std::uint64_t real_ms() {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    static void parse_steps(std::string_view text, Steps &steps, std::vector<std::string> &lines, const char *name, bool psp_allowed) {
        while (!text.empty()) {
            const auto comma = std::min(text.find(','), text.size());
            const auto item = PcInputConfig::trim(text.substr(0u, comma));
            text.remove_prefix(std::min(comma + 1u, text.size()));
            if (item.empty()) continue;
            const auto colon = item.find(':');
            std::uint64_t frame = 0u; bool digits = colon != std::string_view::npos && colon != 0u;
            for (std::size_t i = 0; digits && i < colon; ++i) { digits = item[i] >= '0' && item[i] <= '9'; frame = frame * 10u + static_cast<std::uint64_t>(item[i] - '0'); }
            const auto style = digits ? prompt_style_from_name(item.substr(colon + 1u)) : std::nullopt;
            if (!style || (!psp_allowed && *style == PromptStyle::Psp)) { lines.push_back(std::string(name) + ": '" + std::string(item) + "' is not <frame>:<style>, ignored"); continue; }
            steps.emplace_back(frame, *style);
        }
        std::stable_sort(steps.begin(), steps.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    }
    // All three styles, so a change of style paints nothing; the PSP style has the font's marks when a mod's
    // lettering asks for them, and nothing otherwise.
    bool paint(std::string &error) {
        const auto started = std::chrono::steady_clock::now();
        const PromptMarks *marks = marks_.empty() ? nullptr : &marks_;
        for (const PromptStyle style : {PromptStyle::Keyboard, PromptStyle::Xbox, PromptStyle::PlayStation, PromptStyle::Psp})
            if (!paint_prompt_overlays(painter_, config_, style, sheet_scale_, sprite_scale_, overlays_[static_cast<std::size_t>(style)], error, marks)) return false;
        paint_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        return true;
    }
    void show(PromptStyle style, bool at_once) {
        std::array<vcs::GeTextureOverlaySet, 7> entries;
        const auto &overlays = overlays_[static_cast<std::size_t>(style)];
        for (std::size_t i = 0; i < entries.size(); ++i) entries[i] = {prompt_overlay_keys[i].key, overlays[i]};
        vcs::ge_texture_mods_set_overlays(entries, at_once);
        shown_ = style;
    }
    // What the change of style cost the renderer, once its textures have been drawn again: three frames
    // after the change, or a quarter of a second after it on a screen that ends no frame.
    static constexpr std::uint64_t report_after_ms = 250u;
    void report_switch(std::uint64_t frame, std::uint64_t now_ms) {
        if (!pending_.waiting || (frame != ~std::uint64_t{0} && frame < pending_.frame + 3u && now_ms - pending_.ms < report_after_ms)) return;
        pending_.waiting = false;
        const auto now = vcs::ge_texture_mods_report();
        char text[320];
        std::snprintf(text, sizeof(text), "frame %llu: %s -> %s; %llu textures left the cache, %llu drawn again with new pictures in %.2f ms (%.1f MB)",
            static_cast<unsigned long long>(pending_.frame), prompt_style_name(pending_.before), prompt_style_name(shown_),
            static_cast<unsigned long long>(now.overlay_evicted - pending_.at.overlay_evicted),
            static_cast<unsigned long long>(now.overlay_composed - pending_.at.overlay_composed),
            static_cast<double>(now.overlay_compose_us - pending_.at.overlay_compose_us) / 1000.0,
            static_cast<double>(now.overlay_bytes - pending_.at.overlay_bytes) / (1024.0 * 1024.0));
        std::printf("[prompts] %s\n", text);
        std::fflush(stdout);
    }
    PcInputConfig config_{};
    GlyphPainter painter_;
    PromptMarks marks_;
    std::array<PromptOverlays, 4> overlays_{};
    PromptDebounce debounce_{};
    std::optional<PromptStyle> forced_;
    Steps style_steps_, device_steps_;
    Pending pending_{};
    PromptStyle shown_{PromptStyle::Psp};
    // The frame and the time (of the clock tick() was given) of the last look at the style.
    std::uint64_t switches_{}, frame_{}, ticked_ms_{}, sample_ticks_{};
    std::uint32_t revision_{};
    unsigned sheet_scale_{1u}, sprite_scale_{1u};
    double paint_ms_{};
    bool active_{}, frame_clock_{}, follow_{}, reported_{};
};
}
