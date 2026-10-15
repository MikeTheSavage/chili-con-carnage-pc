#pragma once
// The pause menu's legend for the settings page: "<Triangle> Settings", centred right under the game's
// pause box, drawn by the host through the overlay pass (page_draw.hpp) while a pause window that has no
// `Settings` line of its own has the focus (settings_entry.hpp says when). The button is the font's own
// Triangle picture, which the button prompts repaint for the device in use (prompt_style.hpp), so the legend
// shows the key or pad button that is bound; the 32 x 32 Triangle sprite the menu scene has is never drawn
// in a mission and so is not in the cache. The word is a legend's own style: scale 0.7, the menu's orange
// with the dark red outline.
//
// Why under the box and not in its legend row: the row is full. Every pause window of the disc has the Circle
// sprite at (117, 197), 32 x 32, with `Back` at +32, +7, and the Cross sprite at (344, 197) with `Select`
// ending at it; in a picture of the game `Back` ends at x 190 and `Select` begins at 276 (English), which
// leaves 86 pixels for a legend that needs 89, and less in French and Spanish. The box is 272 x 218 at
// (111, 16.5): it ends at y 234.5, and the 37 rows below it show the dimmed scene in every pause window.
#include "page_draw.hpp"
#include "page_font_guest.hpp"
#include "settings_entry.hpp"
namespace chili {
// The middle of the pause box, the row of the word (a text box's y, as the scene data has it), and the
// picture's rows (a font picture drawn 24 high stands about as tall as the disc inside a 32 x 32 legend sprite).
inline constexpr float pause_legend_centre = 247.0f, pause_legend_word_y = 244.0f, pause_legend_picture_top = 241.0f,
    pause_legend_picture_height = 24.0f, pause_legend_gap = 3.0f;

// Adds the legend to a list. Returns what it covers; an empty box when the font has no such picture.
inline PageBox build_pause_legend(PageDraw &draw, const PageFont &font, std::string_view word = "Settings") {
    const PageGlyph *picture = font.find(page_glyph::triangle);
    if (!picture) return {};
    const PageTextStyle style = PageTextStyle::legend();
    const float picture_width = picture->width() * pause_legend_picture_height / (font.cell_height() - 1.0f);
    const float word_width = page_text_width(font, word, style);
    const float left = std::floor(pause_legend_centre - (picture_width + pause_legend_gap + word_width) * 0.5f);
    (void)draw.glyph(font, page_glyph::triangle, {left, pause_legend_picture_top, left + picture_width, pause_legend_picture_top + pause_legend_picture_height});
    (void)draw.text(font, word, left + picture_width + pause_legend_gap, pause_legend_word_y, style);
    return {left, pause_legend_picture_top, left + picture_width + pause_legend_gap + word_width, pause_legend_picture_top + pause_legend_picture_height};
}

// Puts the legend into the picture and takes it out again. One overlay exists (gpu/ge_gpu_backend.hpp) and
// the settings page draws with it too: the legend is only ever set while the page is closed, and it is taken
// away only when it was this class that set it. The page clears the overlay once when it closes, which can
// come after the legend was set again: while the legend is due it is looked at with every pad sample and
// put back when the overlay is empty.
class PauseLegend {
public:
    // After the texture mods have started and before the game's first draw: the font sheets get their
    // content keys from then on. `gpu`: Direct3D 12 draws the game (the software renderer has no overlay).
    void start(bool gpu) {
        active_ = gpu;
        if (gpu) vcs::ge_gpu_backend_watch_overlay_textures(page_textures);
    }
    // What SettingsEntry says, on the game's thread.
    void set(SettingsEntry::Legend state, const psprecomp::GuestMemory &memory) {
        if (!active_) return;
        if (state == SettingsEntry::Legend::PageOwns) { mine_ = false; return; }
        if (state == SettingsEntry::Legend::Hidden) {
            if (mine_) vcs::ge_gpu_backend_set_overlay(nullptr);
            mine_ = false;
            return;
        }
        // Still there: nothing to do (one atomic load per pad sample while the legend is up).
        if (mine_ && vcs::ge_gpu_backend_overlay_set()) return;
        // The game's glyph table: there once the game has drawn text, long before any pause menu.
        if (!font_.loaded() && !load_page_font(memory, font_)) return;
        PageDraw draw;
        box_ = build_pause_legend(draw, font_);
        if (box_.width() <= 0.0f) return;
        vcs::ge_gpu_backend_set_overlay(draw.finish());
        mine_ = true;
        ++shown_;
    }
    // How often the legend was put into the picture.
    [[nodiscard]] unsigned shown() const noexcept { return shown_; }
    // What the legend drawn last covers, in PSP pixels.
    [[nodiscard]] const PageBox &box() const noexcept { return box_; }
private:
    PageFont font_;
    PageBox box_{};
    bool active_{}, mine_{};
    unsigned shown_{};
};
}
