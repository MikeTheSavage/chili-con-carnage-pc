#pragma once
// The port's own pictures for the on-screen button prompts: key caps, a mouse, Xbox-style and
// PlayStation-style gamepad controls. They are painted with Direct2D and DirectWrite into a Windows
// Imaging Component bitmap at whatever size the caller asks for (a cell of the game's text font is 20 to
// 28 by 30 PSP pixels, a menu sprite 32 by 32, both times the renderer's internal scale), in the game's
// own look for its menu buttons: a dark disc or cap with a light rim and a light legend.
// No game art and no third-party art is used. host/prompt_style.hpp decides which glyph stands for which
// PSP button; host/gpu/ge_texture_mods.hpp puts the pictures into the game's textures.
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace chili {

enum class GlyphKind : std::uint8_t {
    Unbound,          // nothing is bound: a grey cap with a dash
    Key,              // one key cap
    KeyCluster,       // four key caps laid out like the arrow keys: up, left, down, right
    Mouse,            // a mouse with one part lit (MousePart)
    XboxFace,         // A B X Y
    PlayStationFace,  // Cross Circle Square Triangle
    Shoulder,         // LB RB, L1 R1
    Trigger,          // LT RT, L2 R2
    Stick,            // a thumb stick: pressed in, or pushed in one or in all directions
    Dpad,             // the D-pad with the arms of `directions` lit
    Menu,             // Start, Options
    View,             // Back, Share
    // Two marks of the game's text font that are pictures, drawn new for a font sheet a mod has replaced
    // with a larger one (prompt_style.hpp, PromptMarks): beside sharp letters the game's own 26 x 30 pictures,
    // enlarged, are the one thing that is not sharp.
    Lock,             // the padlock: index 0 closed, 1 open
    Chili             // the chili pepper
};
// What a key cap shows: a short text, or a sign drawn for a key whose name does not fit a cap.
enum class KeySign : std::uint8_t { Text, Space, Enter, Shift, Tab, Backspace, Up, Down, Left, Right };
enum class MousePart : std::uint8_t { Left, Right, Middle, WheelUp, WheelDown, Side4, Side5, Move };
enum class PadLook : std::uint8_t { Xbox, PlayStation };
// Bits of Glyph::directions.
inline constexpr std::uint8_t glyph_up = 1u, glyph_right = 2u, glyph_down = 4u, glyph_left = 8u,
                              glyph_all_directions = 15u;
// Glyph::index of a face button.
inline constexpr std::uint8_t xbox_a = 0u, xbox_b = 1u, xbox_x = 2u, xbox_y = 3u;
inline constexpr std::uint8_t ps_cross = 0u, ps_circle = 1u, ps_square = 2u, ps_triangle = 3u;

struct KeyCap {
    KeySign sign{KeySign::Text};
    std::array<char, 6> text{};   // ASCII, up to five characters, for KeySign::Text
    friend bool operator==(const KeyCap &, const KeyCap &) = default;
};
[[nodiscard]] inline KeyCap key_cap(std::string_view text) noexcept {
    KeyCap cap;
    for (std::size_t i = 0u; i < text.size() && i + 1u < cap.text.size(); ++i) cap.text[i] = text[i];
    return cap;
}
[[nodiscard]] inline KeyCap key_cap(KeySign sign) noexcept { KeyCap cap; cap.sign = sign; return cap; }

struct Glyph {
    GlyphKind kind{GlyphKind::Unbound};
    PadLook look{PadLook::Xbox};   // Shoulder, Trigger, Stick, Menu, View: whose names and shapes
    std::uint8_t index{};          // Mouse: MousePart. Faces: which. Shoulder, Trigger, Stick: 0 left, 1 right
    std::uint8_t directions{};     // Stick: pushed directions, 0 = pressed in. Dpad: lit arms
    std::array<KeyCap, 4> keys{};  // Key: keys[0]. KeyCluster: up, left, down, right
    friend bool operator==(const Glyph &, const Glyph &) = default;
};

// Straight alpha, 8 bits a channel, red first, rows from the top.
struct GlyphPicture {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> rgba;
};
// Pixels of the canvas a glyph may fill; it is centred there.
struct GlyphBox { float x{}, y{}, width{}, height{}; };

class GlyphPainter {
public:
    GlyphPainter();
    ~GlyphPainter();
    GlyphPainter(const GlyphPainter &) = delete;
    GlyphPainter &operator=(const GlyphPainter &) = delete;
    // Direct2D, DirectWrite and Windows Imaging Component on the calling thread, which then does all the
    // painting. False when one of them is missing; `error` names it and the prompts stay the game's own.
    [[nodiscard]] bool start(std::string &error);
    // A transparent canvas of this many pixels.
    [[nodiscard]] bool begin(std::uint32_t width, std::uint32_t height, std::string &error);
    void draw(const Glyph &glyph, const GlyphBox &box);
    [[nodiscard]] bool end(GlyphPicture &picture, std::string &error);
    // The font the legends are set in, for the log.
    [[nodiscard]] const std::string &font_name() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

// The picture with a dark edge of `radius` pixels around everything it shows. The game draws its outline
// font underneath its text font as edge and drop shadow, so a text picture needs this twin.
[[nodiscard]] GlyphPicture glyph_outline(const GlyphPicture &fill, std::uint32_t radius);
// A rectangle of a picture as a picture of its own.
[[nodiscard]] GlyphPicture glyph_cut(const GlyphPicture &picture, std::uint32_t x, std::uint32_t y,
                                     std::uint32_t width, std::uint32_t height);

} // namespace chili
