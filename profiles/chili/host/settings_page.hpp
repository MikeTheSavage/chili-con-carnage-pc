#pragma once
// The game's own settings page (CONTROLS.md, "The settings page inside the game"): what it holds, how it is
// worked and how it is drawn. Nothing here touches the game, the window or Direct3D: the page reads and
// changes the settings through settings::Live (settings_apply.hpp), takes pad samples and pointer places
// in PSP pixels, and builds a draw list (page_draw.hpp). host/settings_page.cpp puts it into the game;
// tools/settings_page_test.cpp works it without one.
//
//   pages     the top page (five sections) and one page per section, their rows from settings_table.hpp
//   frames    over the menu scene (the game's Settings screen: its background, logo, headline at the top
//             right, the tilted line under the logo, legends at the bottom), or inside the pause menu's box
//   rows      a name in the menu's orange, its value white at the right; the row in focus pulses red and
//             stands 10 pixels to the right; a value that waits for a restart is hot orange with the chili;
//             a row that cannot be changed here is at half brightness with the closed lock, and says why
//             for three seconds when it is tried
//   buttons   D-pad or stick up and down move the focus (held: after 12 samples every 3rd), left and right
//             change a value (a choice wraps, a slider repeats while held), Cross chooses or steps to the
//             right, Circle goes back a page (from the top page it closes), Start closes
//   pointer   the row under it takes the focus when it moves, a click chooses, a click held on a slider
//             and moved changes it by the distance, the wheel moves the focus, the legends and the two
//             list pointers are buttons; a right click is Circle (the caller says so with right_click())
//
// What differs from the design's numbers, and why (measured with the game's glyph widths): the rows begin
// under the tilted line and the logo (y 93; the design's y 72 runs into both), so the seven visible rows
// stand 18 apart, not 21 (the game's own pause menu sets its lines at scale 0.8 17 apart); in the pause box
// the rows are at scale 0.65 between x 120 and 374 (at 0.7 between 128 and 366 "Button pictures" and
// "PlayStation" overlap), 17.5 apart, and the reasons a row can have there are worded to fit one line of
// the box (settings_table.hpp, lock_text). A reason that still needs two lines (an error's text) stands one
// line higher, so its last line is where one line would be.
#include "page_draw.hpp"
#include "settings_apply.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
namespace chili::settings {

// The PSP's buttons as a pad sample carries them.
namespace pad {
inline constexpr std::uint32_t select = 0x0001u, start = 0x0008u, up = 0x0010u, right = 0x0020u, down = 0x0040u, left = 0x0080u,
    triangle = 0x1000u, circle = 0x2000u, cross = 0x4000u, square = 0x8000u;
}

// Where things stand in a frame, in PSP pixels.
struct PageLayout {
    // The pause menu: an opaque box under everything (the game's own box is translucent and its lines are under it).
    bool boxed{};
    // What the page covers: the whole picture, or the pause menu's box.
    PageBox area{};
    float headline_y{};
    // Rows: where a name begins and a value ends, the first row's box position, the distance between rows.
    float name_x{}, value_right{}, row_y{}, row_pitch{};
    std::size_t visible{};
    float scale{}, lift{};
    // The reason of a locked row: where it begins, how wide a line may be, and the lines' distance.
    float reason_x{}, reason_y{}, reason_width{}, reason_pitch{}, reason_scale{}, reason_lift{};
    // Legends: the 32 x 32 button at the left with its word beside it, the one at the right with its word in
    // front of it (ending at right_word_end); the left/right D-pad's own place.
    PageBox left_button{}, right_button{}, right_dpad{};
    float left_word_x{}, right_word_end{}, word_y{};
};
// The menu scene: the places of the game's own Settings screen (headline box at (120, 20), the tilted line
// at (20, 50), names at x 74.5, values ending at 391, legends at (8, 232) and (440, 232)).
// Seven rows from y 93, 18 apart (the last at 201), the reason's line at 222 under them.
inline constexpr PageLayout main_layout{false, {0.0f, 0.0f, 480.0f, 272.0f}, 20.0f, 74.5f, 391.0f, 93.0f, 18.0f, 7u, 0.8f, 8.0f,
    74.5f, 222.0f, 400.0f, 16.0f, 0.7f, 7.0f,
    {8.0f, 232.0f, 40.0f, 264.0f}, {440.0f, 232.0f, 472.0f, 264.0f}, {442.0f, 234.0f, 474.0f, 266.0f}, 44.0f, 436.0f, 239.0f};
// The pause menu: its box is 272 x 218 at (111, 16.5), its headline at y 28, its legends at (117, 197) and (344, 197).
// Seven rows from y 53, 17.5 apart (the last at 158), the reason's line at 179 between them and the legends.
inline constexpr PageLayout pause_layout{true, {111.0f, 16.5f, 383.0f, 234.5f}, 28.0f, 120.0f, 374.0f, 53.0f, 17.5f, 7u, 0.65f, 6.5f,
    120.0f, 179.0f, 254.0f, 14.5f, 0.65f, 6.5f,
    {117.0f, 197.0f, 149.0f, 229.0f}, {344.0f, 197.0f, 376.0f, 229.0f}, {344.0f, 197.0f, 376.0f, 229.0f}, 153.0f, 340.0f, 204.0f};
constexpr const PageLayout &layout_of(PageFrame frame) noexcept { return frame == PageFrame::Pause ? pause_layout : main_layout; }

// The page's own words. The legends' are the game's (strings 4, 2, 125, 1 and 3 of its menu table).
namespace page_word {
inline constexpr std::string_view back = "Back", select = "Select", change = "Change", ok = "Ok", cancel = "Cancel";
inline constexpr std::string_view reset_question = "Reset to the defaults?", not_saved = "Could not save.";
// Said for three seconds when a change is taken and waits for the next start of the game: the chili after the
// value is the lasting mark, and this is what it means, once.
inline constexpr std::string_view after_restart = "After a restart.";
}
// The pause menu's box as the page fills it. The game's own box is translucent black over the dimmed scene;
// the page's has to be opaque (the game's lines are under it), so it takes the tone the game's box has over
// a mission's picture (the middle of the box in Story 1-1 at internal scale 4 and 8: 85, 80, 64 of 255).
inline constexpr PageColour pause_box_colour{0.333f, 0.314f, 0.251f};
// How long a locked row's reason stays, the selected row's pulse (rounds per second) and how long the row
// takes to move its 10 pixels: the game's menu values.
inline constexpr double reason_seconds = 3.0, pulse_per_second = 0.5, nudge_seconds = 0.1;
inline constexpr float nudge_pixels = 10.0f;
// A held direction acts again after this many samples and then at every `repeat_every`-th: 0.4 seconds and
// ten times a second at the game's 30 frames per second. The game reads the pad once per frame, so at 60
// frames per second both are doubled (SettingsPage::open_page asks the settings for the rate).
inline constexpr unsigned repeat_after = 12u, repeat_every = 3u;
// A slider held with the mouse: this many pixels of the window to a step.
inline constexpr float drag_pixels_per_step = 4.0f;

class SettingsPage {
public:
    // What a row does when it is chosen or changed.
    enum class Act : std::uint8_t { Link, Switch, Choice, Slider, Defaults };
    struct RowView {
        const Row *row{};
        Act act{};
        std::string name, value;
        bool pending{};
        Lock lock{Lock::None};
    };
    // What the two legends say; a word that is empty is not shown.
    struct Legends {
        std::string_view left, right;
        bool right_is_dpad{};
    };
    using Ready = std::function<bool(std::uint64_t)>;

    explicit SettingsPage(Live *live = nullptr) : live_(live) {}
    void attach(Live *live) noexcept { live_ = live; }
    // What the font's D-pad-left and D-pad-right pictures are now, for the Change legend when the game's own
    // left/right sprite is not in the cache. Keys: two key caps, side by side. Pad: one D-pad with one arm
    // lit each, as the button prompts paint a controller's (20 of the cell's rows high): the legend is one
    // D-pad with both arms lit, as the game's own sprite shows it. Psp: the same with the PSP's own pictures,
    // which fill their cells.
    enum class DpadPictures : std::uint8_t { Keys, Pad, Psp };
    void set_dpad_pictures(DpadPictures pictures) noexcept { dpad_pictures_ = pictures; }
    [[nodiscard]] bool open() const noexcept { return open_; }
    [[nodiscard]] PageFrame frame() const noexcept { return frame_; }
    [[nodiscard]] Page page() const noexcept { return page_; }
    [[nodiscard]] const std::vector<RowView> &rows() const noexcept { return rows_; }
    [[nodiscard]] std::size_t focus() const noexcept { return focus_; }
    [[nodiscard]] std::size_t first_visible() const noexcept { return first_; }
    [[nodiscard]] bool asking() const noexcept { return question_; }
    [[nodiscard]] bool dragging() const noexcept { return drag_.active; }
    [[nodiscard]] std::string_view reason() const noexcept { return now_ < reason_until_ ? std::string_view(reason_) : std::string_view(); }
    [[nodiscard]] const PageLayout &layout() const noexcept { return layout_of(frame_); }
    // Lines for the log since the last call: what was opened, changed, written.
    [[nodiscard]] std::vector<std::string> take_events() { return std::exchange(events_, {}); }

    // Opens the page in a frame, on `page` (a section's page goes back to the top page). The settings file
    // is read again first: what the launcher wrote meanwhile is in force and shown. Buttons held now do
    // nothing until they are let go. False without settings.
    bool open_page(PageFrame frame, Page page = Page::Top) {
        if (!live_) return false;
        for (auto &line : live_->reload()) events_.push_back(std::move(line));
        open_ = true; frame_ = frame;
        blocked_ = ~0u; held_ = 0u; repeat_.fill(0u);
        // [Game] FrameLock=1 is 60 game frames, and pad samples, per second.
        samples_per_30th_ = equivalent(live_->running(get("Game", "FrameLock")), "1") ? 2u : 1u;
        drag_ = {};
        show(page == Page::None ? Page::Top : page, 0u);
        events_.push_back("page open: " + std::string(page_name(page_)) + (frame == PageFrame::Pause ? ", in the pause menu's box" : ", over the menu"));
        return true;
    }
    // Writes what is not written yet and closes.
    void close() {
        if (!open_) return;
        flush_slider();
        if (live_ && live_->unsaved() && live_->host().writable) {
            const auto out = live_->flush();
            if (!out.written) events_.push_back("NOT written at close (" + out.error + ")");
        }
        open_ = false; question_ = false; drag_ = {};
        events_.push_back("page closed");
    }

    // The clock the pulse, the row's move and a reason's three seconds go by: seconds, from anywhere.
    void tick(double seconds) {
        now_ = seconds;
        // A row's lock can change under the page (the device used last, a file the launcher wrote).
        if (open_ && page_ != Page::TexturesMods && !drag_.active && now_ - refreshed_ >= 0.5) refresh();
    }

    // ---- The pad: one sample ---------------------------------------------------------------------------

    void pad(std::uint32_t buttons, std::uint8_t stick_x = 128u, std::uint8_t stick_y = 128u) {
        if (!open_) return;
        std::uint32_t now = buttons & (pad::up | pad::down | pad::left | pad::right | pad::cross | pad::circle | pad::start);
        // The left stick past half is the D-pad (W and S are the stick, with the default keys).
        if (stick_y < 64u) now |= pad::up; else if (stick_y > 192u) now |= pad::down;
        if (stick_x < 64u) now |= pad::left; else if (stick_x > 192u) now |= pad::right;
        // What was held when the page opened acts only after it was let go.
        blocked_ &= now;
        now &= ~blocked_;
        const std::uint32_t pressed = now & ~held_, released = held_ & ~now;
        held_ = now;
        if (pressed & pad::start) { close(); return; }
        if (pressed & pad::circle) { back(); if (!open_) return; }
        else if (pressed & pad::cross) ok();
        static constexpr std::uint32_t directions[]{pad::up, pad::down, pad::left, pad::right};
        for (std::size_t i = 0; i < std::size(directions); ++i) {
            if (!(now & directions[i])) { repeat_[i] = 0u; continue; }
            const unsigned count = repeat_[i]++;
            const bool first = (pressed & directions[i]) != 0u;
            const unsigned after = repeat_after * samples_per_30th_, every = repeat_every * samples_per_30th_;
            const bool again = !first && count >= after && (count - after) % every == 0u;
            if (!first && !again) continue;
            if (directions[i] == pad::up) move(-1);
            else if (directions[i] == pad::down) move(+1);
            // A held Left or Right goes on with a slider only: a choice would run round.
            else if (first || focused_is(Act::Slider)) change(directions[i] == pad::right ? +1 : -1);
        }
        // A slider is written when its key is let go.
        if ((released & (pad::left | pad::right | pad::cross)) && !(now & (pad::left | pad::right | pad::cross))) flush_slider();
    }

    // ---- What the buttons do (also the steps of a scripted run) ------------------------------------------

    // The focus one row up (-1) or down (+1); the list wraps.
    void move(int direction) {
        if (!open_ || question_ || rows_.empty()) return;
        const std::size_t count = rows_.size();
        focus_on((focus_ + count + (direction > 0 ? 1u : count - 1u)) % count);
        ensure_visible();
    }
    // The focused row's value one step to the left (-1) or right (+1).
    void change(int direction) {
        if (!open_ || question_ || rows_.empty()) return;
        const RowView view = rows_[focus_];
        if (view.act == Act::Link || view.act == Act::Defaults) return;
        if (view.lock != Lock::None) { say(lock_text(view.lock)); return; }
        Live::Outcome out;
        switch (view.row->kind) {
        case RowKind::Setting: out = live_->step(*view.row->setting, direction, frame_, view.act != Act::Slider); break;
        case RowKind::Quality: out = live_->step_quality(direction, frame_); break;
        case RowKind::Textures: out = live_->step_textures(direction, frame_); break;
        case RowKind::HdPack: out = live_->step_pack(direction, frame_); break;
        default: return;
        }
        if (view.act == Act::Slider && out.ok) slider_dirty_ = true;
        after(out, view.act != Act::Slider, view.value);
    }
    // Cross: opens a section, steps a value to the right, asks before Defaults, answers a question with Ok.
    void ok() {
        if (!open_ || rows_.empty()) return;
        if (question_) { answer(true); return; }
        const RowView &view = rows_[focus_];
        if (view.act == Act::Link) show(view.row->target, 0u);
        else if (view.act == Act::Defaults) {
            if (view.lock != Lock::None) say(lock_text(view.lock));
            else { flush_slider(); question_ = true; reason_until_ = 0.0; }
        } else change(+1);
    }
    // Circle: Cancel in a question, else the page above; from the top page the settings page closes.
    void back() {
        if (!open_) return;
        if (question_) { answer(false); return; }
        const Page from = page_, parent = page_parent(page_);
        if (parent == Page::None) { close(); return; }
        show(parent, 0u);
        for (std::size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].act == Act::Link && rows_[i].row->target == from) { focus_ = i; break; }
        ensure_visible();
    }

    // ---- The pointer, in PSP pixels ----------------------------------------------------------------------

    // Pixels of the window to a PSP pixel (a slider's drag is measured in the window's pixels).
    void set_pointer_scale(float window_pixels_per_pixel) noexcept { if (window_pixels_per_pixel > 0.0f) pointer_scale_ = window_pixels_per_pixel; }
    // The pointer moved to (x, y): the row under it takes the focus; a held slider follows it.
    void pointer(float x, float y) {
        if (!open_) return;
        if (drag_.active) { drag_to(x); return; }
        if (question_) return;
        if (const auto row = row_at(x, y); row && *row != focus_) focus_on(*row);
    }
    // The left button went down at (x, y).
    void press(float x, float y) {
        if (!open_) return;
        const Legends words = legends();
        const bool on_left = !words.left.empty() && left_legend_box().contains(x, y), on_right = !words.right.empty() && right_legend_box().contains(x, y);
        if (question_) {
            if (on_left) answer(false); else if (on_right) answer(true);
            return;
        }
        if (on_left) { back(); return; }
        if (on_right) { ok(); return; }
        const std::size_t visible = layout().visible;
        // A list pointer is small: a click a few pixels beside it is still meant for it.
        const auto on_pointer = [&](bool down) { const PageBox box = pointer_box(down); return PageBox{box.x0 - 3.0f, box.y0 - 2.0f, box.x1 + 3.0f, box.y1 + 2.0f}.contains(x, y); };
        if (first_ > 0u && on_pointer(false)) { scroll_to(first_ > visible ? first_ - visible : 0u); return; }
        if (first_ + visible < rows_.size() && on_pointer(true)) { scroll_to(std::min(first_ + visible, rows_.size() - visible)); return; }
        const auto row = row_at(x, y);
        if (!row) return;
        if (*row != focus_) focus_on(*row);
        const RowView &view = rows_[focus_];
        // A slider is held: moved, it changes by the distance; let go where it was pressed, it steps to the right.
        if (view.act == Act::Slider && view.lock == Lock::None && view.row->kind == RowKind::Setting) { drag_ = {true, false, x, slider_at(*view.row->setting)}; return; }
        ok();
    }
    // The left button came up.
    void release(float x, float) {
        if (!open_ || !drag_.active) return;
        drag_to(x);
        const bool moved = drag_.moved;
        drag_ = {};
        if (!moved) change(+1);
        flush_slider();
    }
    // A held slider is let go without the button (the window lost the mouse).
    void cancel_drag() { if (drag_.active) { drag_ = {}; flush_slider(); } }
    // The right button: Circle.
    void right_click() { cancel_drag(); back(); }
    // The wheel: the focus one row per notch, away from the player is up.
    void wheel(int notches) {
        for (int i = 0; i < std::abs(notches) && i < 32; ++i) move(notches > 0 ? -1 : +1);
    }

    // ---- Where things are (also the pointer's targets) ---------------------------------------------------

    [[nodiscard]] float row_y(std::size_t visible_index) const noexcept { return layout().row_y + layout().row_pitch * static_cast<float>(visible_index); }
    // The band of the visible row `visible_index` that the pointer chooses it by.
    [[nodiscard]] PageBox row_box(std::size_t visible_index) const noexcept {
        const PageLayout &l = layout();
        const float y = row_y(visible_index);
        return {l.name_x - 6.0f, y + 1.0f, l.value_right, y + 1.0f + l.row_pitch};
    }
    [[nodiscard]] PageBox left_legend_box() const noexcept {
        const PageLayout &l = layout();
        return {l.left_button.x0, l.left_button.y0, l.left_word_x + 68.0f, l.left_button.y1};
    }
    [[nodiscard]] PageBox right_legend_box() const noexcept {
        const PageLayout &l = layout();
        return {l.right_word_end - 96.0f, l.right_button.y0, std::max(l.right_button.x1, l.right_dpad.x1), std::max(l.right_button.y1, l.right_dpad.y1)};
    }
    // The small pointer that says the list goes on: above the first visible row at the right edge of the rows
    // (upwards), or below the last one (downwards). Never on a value's own line, where it would read as that
    // value's arrow.
    [[nodiscard]] PageBox pointer_box(bool down) const noexcept {
        const PageLayout &l = layout();
        const float right = std::min(l.value_right + 12.0f, l.area.x1 - 1.0f), left = right - 12.0f;
        const float edge = down ? row_y(l.visible - 1u) + 1.0f + l.row_pitch + list_pointer_gap : row_y(0u) + 1.0f - list_pointer_gap - list_pointer_height;
        return {left, edge, right, edge + list_pointer_height};
    }
    [[nodiscard]] Legends legends() const {
        if (question_) return {page_word::cancel, page_word::ok, false};
        if (rows_.empty()) return {page_word::back, {}, false};
        const RowView &view = rows_[focus_];
        // A prompt is there only for what the button does on this row.
        if (view.lock != Lock::None) return {page_word::back, {}, false};
        if (view.act == Act::Link || view.act == Act::Defaults) return {page_word::back, page_word::select, false};
        return {page_word::back, page_word::change, true};
    }
    // The row's shift to the right: the focused one is on its way to 10 pixels, the one before it on its way back.
    [[nodiscard]] float nudge(std::size_t index) const noexcept {
        const float t = static_cast<float>(std::clamp((now_ - focus_since_) / nudge_seconds, 0.0, 1.0));
        return index == focus_ ? nudge_pixels * t : index == previous_ ? nudge_pixels * (1.0f - t) : 0.0f;
    }
    // The focused row's colour: between dark and bright red, one round in two seconds.
    [[nodiscard]] PageColour pulse() const noexcept {
        const float t = 0.5f - 0.5f * static_cast<float>(std::cos(now_ * pulse_per_second * 6.283185307179586));
        return {page_colour::selected_dark.r + (page_colour::selected_bright.r - page_colour::selected_dark.r) * t, 0.0f, 0.0f, 1.0f};
    }

    // The logo's beat, as the game's menus have it (the two animations of its fragment, measured on the main
    // menu at 30 frames per second: 16 frames a round): a tenth larger within 0.3 seconds, quickly at first,
    // back within 0.18, then still for the rest of the round.
    [[nodiscard]] float logo_scale() const noexcept {
        const double t = std::fmod(now_ < 0.0 ? 0.0 : now_, logo_round_seconds);
        if (t < 0.3) { const double left = 1.0 - t / 0.3; return static_cast<float>(1.1 - 0.1 * left * left); }
        if (t < 0.48) return static_cast<float>(1.1 - 0.1 * (t - 0.3) / 0.18);
        return 1.0f;
    }
    static constexpr double logo_round_seconds = 16.0 / 30.0;

    // ---- Drawing ----------------------------------------------------------------------------------------

    [[nodiscard]] PageTextStyle name_style() const noexcept {
        PageTextStyle style = PageTextStyle::line();
        style.scale = layout().scale; style.lift = layout().lift;
        return style;
    }
    [[nodiscard]] PageTextStyle value_style() const noexcept {
        PageTextStyle style = PageTextStyle::value();
        style.scale = layout().scale; style.lift = layout().lift;
        style.align = PageAlign::Right; style.width = value_box_width;
        return style;
    }
    [[nodiscard]] PageTextStyle reason_style() const noexcept {
        PageTextStyle style = PageTextStyle::legend();
        style.scale = layout().reason_scale; style.lift = layout().reason_lift;
        style.fill = page_colour::headline; style.outline = page_colour::headline_outline;
        return style;
    }
    // The headline of the frame: the menu scene's hot orange at the top right, or the pause menu's pale yellow in the middle.
    [[nodiscard]] PageTextStyle headline_style() const noexcept {
        if (frame_ != PageFrame::Pause) return PageTextStyle::headline();
        PageTextStyle style;
        // The pause menu's own `Headline` box: 272 wide at (110, 28), scale 0.9, its text 15 above the box's line.
        style.scale = 0.9f; style.lift = 15.0f; style.fill = page_colour::pause_headline; style.outlined = false;
        style.shadow_x = style.shadow_y = 3.0f; style.align = PageAlign::Centre; style.width = 272.0f;
        return style;
    }
    // A value as it is set: with the chili while it waits for a restart, cut short with two dots where the
    // name leaves it too little room.
    [[nodiscard]] std::string value_text(const PageFont &font, const RowView &view, float room) const {
        const PageTextStyle style = value_style();
        std::string mark;
        if (view.pending) { mark.push_back(' '); page_append(mark, page_glyph::chili); }
        std::string text = view.value;
        if (page_text_width(font, text + mark, style) <= room) return text + mark;
        while (text.size() > 1u) {
            // Whole code points only: the bytes that continue one, then the byte that begins it.
            while (text.size() > 1u && (static_cast<unsigned char>(text.back()) & 0xC0u) == 0x80u) text.pop_back();
            text.pop_back();
            while (text.size() > 1u && text.back() == ' ') text.pop_back();
            if (page_text_width(font, text + ".." + mark, style) <= room) break;
        }
        return text + ".." + mark;
    }
    // A reason in at most two lines of the frame's width.
    [[nodiscard]] std::vector<std::string> reason_lines(const PageFont &font) const {
        const PageTextStyle style = reason_style();
        const std::string text(reason());
        std::vector<std::string> lines;
        if (text.empty()) return lines;
        if (page_text_width(font, text, style) <= layout().reason_width) { lines.push_back(text); return lines; }
        std::size_t cut = std::string::npos;
        for (std::size_t at = text.find(' '); at != std::string::npos; at = text.find(' ', at + 1u)) {
            if (page_text_width(font, std::string_view(text).substr(0u, at), style) > layout().reason_width) break;
            cut = at;
        }
        if (cut == std::string::npos) { lines.push_back(text); return lines; }
        lines.push_back(text.substr(0u, cut));
        lines.push_back(text.substr(cut + 1u));
        return lines;
    }

    // The page as a draw list. `ready` says whether a texture is in the renderer's cache (the background, the
    // logo and the button sprites are, once the game has drawn with them); a legend whose sprite is not is
    // drawn from the font's picture of the button. Nothing when the page is closed.
    void build(PageDraw &draw, const PageFont &font, const Ready &ready) const {
        if (!open_) return;
        const PageLayout &l = layout();
        const auto has = [&](std::uint64_t key) { return ready && ready(key); };
        if (l.boxed) draw.rect(l.area, pause_box_colour);
        else {
            if (has(page_texture::background_page)) draw.sprite(page_texture::background_page, l.area);
            else if (has(page_texture::background_main)) draw.sprite(page_texture::background_main, l.area);
            else draw.rect(l.area, {0.16f, 0.05f, 0.02f});
            // Where the game's draw puts the logo: a pixel wider on each side than its node's 180 x 90 at (-23, 0),
            // and beating about its middle as it does on every menu screen.
            if (has(page_texture::logo)) {
                const float beat = logo_scale();
                draw.sprite(page_texture::logo, {67.0f - 91.0f * beat, 45.0f - 45.0f * beat, 67.0f + 91.0f * beat, 45.0f + 45.0f * beat});
            }
        }
        if (l.boxed) draw.text(font, page_name(page_), 110.0f, l.headline_y, headline_style());
        else {
            draw.text(font, page_name(page_), 120.0f, l.headline_y, headline_style());
            // The tilted line under the logo says where this page hangs; the top page has none.
            if (const Page parent = page_parent(page_); parent != Page::None) draw.text(font, page_name(parent), 20.0f, 50.0f, PageTextStyle::category());
        }
        const std::size_t end = std::min(rows_.size(), first_ + l.visible);
        for (std::size_t i = first_; i < end; ++i) {
            const RowView &view = rows_[i];
            // A question stands where the third and fourth visible rows are.
            if (question_ && (i - first_ == 2u || i - first_ == 3u)) continue;
            const float y = row_y(i - first_);
            const bool locked = view.lock != Lock::None;
            PageTextStyle name = name_style();
            if (i == focus_) name.fill = pulse();
            std::string text = view.name;
            if (locked) {
                // The game's look of a line that cannot be chosen: every colour halved, the pulse of the row
                // in focus too (it pulses between the halves of the two reds).
                name.fill = name.fill.dimmed(0.5f);
                name.outline = name.outline.dimmed(0.5f); name.picture_tint = 0.5f;
                text.push_back(' '); page_append(text, page_glyph::lock_closed);
            }
            // The top page in the pause box: its five lines stand in the middle, as the pause menu's own do.
            if (l.boxed && view.value.empty() && page_ == Page::Top) {
                name.align = PageAlign::Centre; name.width = l.area.width();
                draw.text(font, text, l.area.x0 + nudge(i), y, name);
                continue;
            }
            draw.text(font, text, l.name_x + nudge(i), y, name);
            if (view.value.empty()) continue;
            PageTextStyle value = value_style();
            if (view.pending) value.fill = page_colour::headline;
            if (locked) { value.fill = value.fill.dimmed(0.5f); value.picture_tint = 0.5f; }
            const float room = l.value_right - (l.name_x + nudge_pixels + page_text_width(font, text, name) + value_gap);
            draw.text(font, value_text(font, view, room), l.value_right - value.width, y, value);
        }
        // The list goes on: a small gold pointer beside the first or the last visible row.
        if (first_ > 0u) list_pointer(draw, false);
        if (first_ + l.visible < rows_.size()) list_pointer(draw, true);
        // A reason's last line stands on the reason's line; a second one stands above it.
        const auto lines = reason_lines(font);
        for (std::size_t i = 0; i < lines.size(); ++i)
            draw.text(font, lines[i], l.reason_x, l.reason_y - l.reason_pitch * static_cast<float>(lines.size() - 1u - i), reason_style());
        if (question_) {
            // The page darkened, and a band of its own for the line where the third and fourth rows stand.
            const float top = row_y(2u) + 1.0f, bottom = row_y(4u) + 1.0f;
            draw.rect(l.area, page_colour::black.faded(0.5f));
            draw.rect({l.area.x0, top, l.area.x1, bottom}, page_colour::black.faded(0.85f));
            PageTextStyle question = name_style();
            question.align = PageAlign::Centre; question.width = l.area.width();
            draw.text(font, page_word::reset_question, l.area.x0, (top + bottom) * 0.5f - 15.0f * question.scale, question);
        }
        const Legends words = legends();
        if (!words.left.empty()) {
            button(draw, font, has, page_texture::button_circle, page_glyph::circle, l.left_button);
            draw.text(font, words.left, l.left_word_x, l.word_y, PageTextStyle::legend());
        }
        if (!words.right.empty()) {
            float word_end = l.right_word_end;
            if (!words.right_is_dpad) button(draw, font, has, page_texture::button_cross, page_glyph::cross, l.right_button);
            else if (has(page_texture::button_dpad_lr)) draw.sprite(page_texture::button_dpad_lr, l.right_dpad);
            else if (dpad_pictures_ != DpadPictures::Keys) {
                // The game's left/right sprite is in the cache only once its own Settings screen was shown.
                // Without it: one D-pad with both arms lit from the font's two pictures, the left half of the
                // one with the left arm lit and the right half of the other, as large as the sprite's pad.
                const float grow = dpad_pictures_ == DpadPictures::Psp ? dpad_glyph_grow_psp : button_glyph_grow;
                const PageBox box{l.right_dpad.x0, l.right_dpad.y0 - grow, l.right_dpad.x1, l.right_dpad.y1 + grow};
                draw.glyph(font, page_glyph::dpad_left, box, {}, 0.0f, 0.5f);
                draw.glyph(font, page_glyph::dpad_right, box, {}, 0.5f, 1.0f);
            } else {
                // The font's two D-pad pictures side by side at the font's own size, ending where the sprite
                // would; the word makes room for them.
                const float right = l.right_dpad.x1, top = (l.right_dpad.y0 + l.right_dpad.y1) * 0.5f - 14.5f;
                draw.glyph(font, page_glyph::dpad_left, {right - 2.0f * dpad_glyph_width, top, right - dpad_glyph_width, top + 29.0f});
                draw.glyph(font, page_glyph::dpad_right, {right - dpad_glyph_width, top, right, top + 29.0f});
                word_end = right - 2.0f * dpad_glyph_width - 4.0f;
            }
            PageTextStyle right = PageTextStyle::legend();
            right.align = PageAlign::Right; right.width = 96.0f;
            draw.text(font, words.right, word_end - right.width, l.word_y, right);
        }
    }

    // The width of the box a value is set in (it ends at value_right), and the least distance between a name and its value.
    static constexpr float value_box_width = 240.0f, value_gap = 8.0f;
    // A legend drawn from the font: the picture of a button stands in a cell 29 rows high and is about 17
    // pixels across where the 32 x 32 sprite's is about 27, so the cell is drawn this much higher than the
    // sprite's box on each side; the two D-pad pictures stay at the font's size, each this wide.
    static constexpr float button_glyph_grow = 5.5f, dpad_glyph_width = 27.0f;
    // The PSP's own D-pad picture fills its 27 x 29 cell: drawn this much higher than the sprite's 32 x 32 box
    // on each side it is as wide as the box.
    static constexpr float dpad_glyph_grow_psp = 1.2f;
    // The list pointers: how high their box is and how far it stands from the rows.
    static constexpr float list_pointer_height = 8.0f, list_pointer_gap = 1.0f;

private:
    // A held slider: where the pointer was when the button went down, and the slider's step then.
    struct Drag { bool active{}, moved{}; float from{}; long start{}; };

    [[nodiscard]] bool focused_is(Act act) const noexcept { return !rows_.empty() && rows_[focus_].act == act; }
    // The visible row under a place, as an index into rows().
    [[nodiscard]] std::optional<std::size_t> row_at(float x, float y) const noexcept {
        const std::size_t end = std::min(rows_.size(), first_ + layout().visible);
        for (std::size_t i = first_; i < end; ++i) if (row_box(i - first_).contains(x, y)) return i;
        return std::nullopt;
    }
    void focus_on(std::size_t index) {
        flush_slider();
        previous_ = focus_; focus_ = index; focus_since_ = now_;
        // A reason goes with the focus.
        reason_until_ = 0.0;
    }
    // The list moves by whole rows so that the focus is the first or the last visible row.
    void ensure_visible() noexcept {
        const std::size_t visible = layout().visible;
        if (focus_ < first_) first_ = focus_;
        else if (focus_ >= first_ + visible) first_ = focus_ + 1u - visible;
        if (rows_.size() <= visible) first_ = 0u;
        else first_ = std::min(first_, rows_.size() - visible);
    }
    // The list a page further (a click on a list pointer); the focus stays among the visible rows.
    void scroll_to(std::size_t first) {
        first_ = first;
        const std::size_t last = std::min(rows_.size(), first_ + layout().visible) - 1u;
        if (focus_ < first_) focus_on(first_); else if (focus_ > last) focus_on(last);
    }
    void show(Page page, std::size_t focus) {
        flush_slider();
        page_ = page; question_ = false; reason_until_ = 0.0; drag_ = {};
        refresh();
        focus_ = rows_.empty() ? 0u : std::min(focus, rows_.size() - 1u);
        previous_ = none; first_ = 0u; focus_since_ = now_;
        ensure_visible();
    }
    void say(std::string_view why) { reason_.assign(why); reason_until_ = now_ + reason_seconds; }
    void answer(bool yes) {
        question_ = false;
        if (!yes) return;
        after(live_->reset(page_, frame_), true, {});
    }
    // What became of a change: a line for the log, a reason when it was refused or could not be saved, and,
    // for three seconds, what the chili means when the value now waits for the next start.
    void after(const Live::Outcome &out, bool wrote, std::string_view before) {
        refresh();
        const bool same = !before.empty() && focus_ < rows_.size() && rows_[focus_].value == before && out.ok;
        if (!out.line.empty() && !same) events_.push_back(out.line);
        if (!out.ok) say(out.lock != Lock::None ? lock_text(out.lock) : std::string_view(out.error));
        else if (wrote && !out.written && live_->host().writable) say(page_word::not_saved);
        else if (out.pending && !same) say(page_word::after_restart);
        else reason_until_ = 0.0;
    }
    void flush_slider() {
        if (!slider_dirty_) return;
        slider_dirty_ = false;
        if (!live_ || !live_->unsaved()) return;
        const auto out = live_->flush();
        if (out.written) events_.push_back("written");
        else if (live_->host().writable) { events_.push_back("NOT written (" + out.error + ")"); say(page_word::not_saved); }
    }
    // A held slider follows the pointer: one step for every `drag_pixels_per_step` pixels of the window.
    void drag_to(float x) {
        if (focus_ >= rows_.size() || rows_[focus_].act != Act::Slider || rows_[focus_].row->kind != RowKind::Setting) { drag_ = {}; return; }
        const long steps = static_cast<long>((x - drag_.from) * pointer_scale_ / drag_pixels_per_step);
        if (steps) drag_.moved = true;
        const Setting &setting = *rows_[focus_].row->setting;
        // From where the slider stood when it was taken: moved back, it is back.
        const long at = slider_at(setting), next = std::clamp(drag_.start + steps, 0l, slider_steps(setting));
        if (next == at) return;
        const std::string before = rows_[focus_].value;
        const auto out = live_->set(setting, slider_value(setting, next), frame_, false);
        if (out.ok) slider_dirty_ = true;
        after(out, false, before);
    }
    // The step a slider stands on, counted from its lowest value.
    [[nodiscard]] long slider_at(const Setting &setting) const {
        const auto held = number(live_->value(setting));
        return std::clamp(std::lround(((held ? *held : setting.min) - setting.min) / setting.step), 0l, slider_steps(setting));
    }
    // Whether Defaults can change anything here: locked when every row it would reset is.
    [[nodiscard]] Lock defaults_lock(Page page) const {
        Lock first = Lock::None;
        bool any = false;
        const auto look = [&](Page which) {
            for (const auto &row : rows_of(which)) {
                Lock lock = Lock::None;
                if (row.kind == RowKind::Setting) { if (same_name(row.setting->key, "Scheme")) continue; lock = live_->locked(*row.setting, frame_); }
                else if (row.kind == RowKind::Quality) lock = live_->quality_lock(frame_);
                else if (row.kind == RowKind::Textures) { lock = live_->textures_lock(frame_); if (lock == Lock::NoHdPack) lock = Lock::None; }
                else continue;
                if (lock == Lock::None) any = true; else if (first == Lock::None) first = lock;
            }
        };
        look(page);
        if (page == Page::Controls) { look(Page::KeyboardMouse); look(Page::Gamepad); }
        return any ? Lock::None : first;
    }
    // The rows of the page as they stand now.
    void refresh() {
        refreshed_ = now_;
        rows_.clear();
        if (!live_) return;
        for (const auto &row : rows_of(page_)) {
            RowView view;
            view.row = &row; view.name = std::string(row.name);
            switch (row.kind) {
            case RowKind::Setting:
                view.act = row.setting->kind == Kind::Slider ? Act::Slider : row.setting->kind == Kind::Switch ? Act::Switch : Act::Choice;
                view.value = live_->shown(*row.setting); view.pending = live_->pending(*row.setting); view.lock = live_->locked(*row.setting, frame_);
                break;
            case RowKind::Link: view.act = Act::Link; break;
            case RowKind::Quality:
                view.act = Act::Choice; view.value = live_->quality_shown(); view.pending = live_->quality_pending(); view.lock = live_->quality_lock(frame_);
                break;
            case RowKind::Textures:
                view.act = Act::Choice; view.value = live_->textures_shown(); view.pending = live_->textures_pending(); view.lock = live_->textures_lock(frame_);
                break;
            case RowKind::HdPack:
                // Only with more than one pack to choose from.
                if (!live_->pack_row()) continue;
                view.act = Act::Choice; view.value = live_->pack(); view.pending = live_->textures_pending(); view.lock = live_->pack_lock(frame_);
                break;
            case RowKind::Defaults: view.act = Act::Defaults; view.lock = defaults_lock(page_); break;
            }
            rows_.push_back(std::move(view));
        }
        if (focus_ >= rows_.size()) focus_ = rows_.empty() ? 0u : rows_.size() - 1u;
    }
    template <typename Has>
    static void button(PageDraw &draw, const PageFont &font, const Has &has, std::uint64_t sprite, char32_t glyph, const PageBox &box) {
        if (has(sprite)) draw.sprite(sprite, box);
        else draw.glyph(font, glyph, {box.x0, box.y0 - button_glyph_grow, box.x1, box.y1 + button_glyph_grow});
    }
    // A gold triangle with an ink edge, 8 pixels wide, in the middle of its box.
    void list_pointer(PageDraw &draw, bool down) const {
        const PageBox box = pointer_box(down);
        const float x = (box.x0 + box.x1) * 0.5f, y = (box.y0 + box.y1) * 0.5f, tip = down ? 1.0f : -1.0f;
        static constexpr PageColour gold{1.0f, 0.82f, 0.2f};
        draw.triangle({x - 5.0f, y - 3.5f * tip}, {x + 5.0f, y - 3.5f * tip}, {x, y + 4.5f * tip}, page_colour::black);
        draw.triangle({x - 3.6f, y - 2.6f * tip}, {x + 3.6f, y - 2.6f * tip}, {x, y + 3.0f * tip}, gold);
    }

    static constexpr std::size_t none = static_cast<std::size_t>(-1);
    Live *live_{};
    bool open_{}, question_{}, slider_dirty_{};
    DpadPictures dpad_pictures_{DpadPictures::Keys};
    PageFrame frame_{PageFrame::MainMenu};
    Page page_{Page::Top};
    std::vector<RowView> rows_;
    std::size_t focus_{}, previous_{none}, first_{};
    double now_{}, focus_since_{-1.0}, reason_until_{}, refreshed_{};
    std::string reason_;
    std::uint32_t held_{}, blocked_{};
    std::array<unsigned, 4> repeat_{};
    unsigned samples_per_30th_{1u};
    Drag drag_{};
    float pointer_scale_{3.0f};
    std::vector<std::string> events_;
};
}
