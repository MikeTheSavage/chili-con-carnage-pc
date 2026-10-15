// The settings page in the game: settings_page.hpp put between the game, the renderer and the window.
// What main.cpp calls is settings_page_host.hpp; what the game's menus call to open the page is
// settings_page_api.hpp (page_request_open, page_is_open).
//
// The page is drawn by the renderer's overlay pass from a list handed over at every frame start while it is
// open; it takes every pad sample while it is open, and the game gets a neutral one, so the game's menu
// under it stays on its line; it takes the window's mouse messages. It never writes guest memory and puts no
// function over guest code. It reads guest memory for two things: the game's glyph table (page_font_guest.hpp)
// and the game's window stack, to say in the log which menu line the page was opened over and to close
// when that menu goes away under it.
#include "settings_page_host.hpp"
#include "menu_scene.hpp"
#include "page_font_guest.hpp"
#include "png_writer.hpp"
#include "settings_page.hpp"
#include "window_picture.hpp"
#include <chrono>
#include <cstdio>
#include <iostream>
#include <optional>
namespace chili {
namespace {
using settings::Page;
using settings::SettingsPage;

// ---- The game's menu under the page ------------------------------------------------------------------

// The top of the game's window stack: the
// scripts' globals at [0x08DFBC04], master_control in their member block at +0x278, its window list at
// +0x12C; a window's name at +0x48, its selected row and column at +0x60 and +0x64 of its member block, its
// lines at +0x50. Logic nodes are on the stack too, so a window's members are read only for the windows
// named here. Reads only.
struct MenuTop {
    std::uint32_t window{};
    std::string name, selected;
    int row{-1};
    unsigned depth{};
    bool known{};
};
std::uint32_t word(const psprecomp::GuestMemory &memory, std::uint32_t address) {
    return address >= 0x08800000u && memory.contains(address, 4u) ? memory.load32(address) : 0u;
}
std::string text_at(const psprecomp::GuestMemory &memory, std::uint32_t address) {
    std::string text;
    for (std::uint32_t i = 0; address >= 0x08800000u && i < 48u && memory.contains(address + i, 1u); ++i) {
        const auto c = memory.load8(address + i);
        if (c < 32u || c > 126u) break;
        text.push_back(static_cast<char>(c));
    }
    return text;
}
MenuTop menu_top(const psprecomp::GuestMemory &memory) {
    MenuTop top;
    const auto control = word(memory, word(memory, word(memory, 0x08DFBC04u) + 0x14u) + 0x278u);
    const auto list = word(memory, word(memory, control + 0x14u) + 0x12Cu);
    const auto count = word(memory, list + 8u), entries = word(memory, word(memory, list + 4u) + 4u);
    if (!count || count > 32u || !entries) return top;
    top.depth = count;
    top.window = word(memory, entries + (count - 1u) * 4u);
    top.name = text_at(memory, word(memory, top.window + 0x48u));
    for (const std::string_view name : {"Menu_Main", "SubMenu_Profile", "SubMenu_Settings", "SubMenu_IngameMain"}) top.known = top.known || top.name == name;
    if (!top.known) return top;
    const auto members = word(memory, top.window + 0x14u);
    const auto columns = std::max(word(memory, members + 0x28u), 1u);
    const auto row = static_cast<std::int32_t>(word(memory, members + 0x60u)), column = static_cast<std::int32_t>(word(memory, members + 0x64u));
    const auto lines = word(memory, members + 0x50u);
    const auto line_count = word(memory, lines + 8u), line_entries = word(memory, word(memory, lines + 4u) + 4u);
    top.row = row;
    if (row >= 0 && column >= 0 && columns <= 8u) {
        const auto index = static_cast<std::uint32_t>(row) * columns + static_cast<std::uint32_t>(column);
        if (index < line_count && line_count <= 64u) top.selected = text_at(memory, word(memory, word(memory, line_entries + index * 4u) + 0x48u));
    }
    return top;
}
std::string menu_text(const MenuTop &top) {
    if (!top.depth) return "the game's menu: no window is up";
    std::string text = "the game's menu: " + (top.name.empty() ? std::string("?") : top.name);
    if (top.known) text += " row " + std::to_string(top.row) + " '" + top.selected + "'";
    return text + " (" + std::to_string(top.depth) + " on the stack)";
}

std::wstring wide_variable(const wchar_t *name) {
    std::wstring value(512u, L'\0');
    for (;;) {
        const auto count = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
        if (!count) return {};
        if (count < value.size()) { value.resize(count); return value; }
        value.resize(count + 1u);
    }
}
std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    for (;;) {
        const auto at = text.find(separator);
        parts.emplace_back(text.substr(0u, at));
        if (at == std::string_view::npos) return parts;
        text.remove_prefix(at + 1u);
    }
}
bool number_of(const std::string &text, float &value) {
    char *end = nullptr;
    value = std::strtof(text.c_str(), &end);
    return end != text.c_str() && *end == '\0';
}

// ---- The page in the game -----------------------------------------------------------------------------

class Host {
public:
    std::vector<std::string> start(PageStart start) {
        std::vector<std::string> lines;
        start_ = std::move(start);
        if (!start_.live) return lines;
        enabled_ = true;
        page_.attach(start_.live);
        began_ = std::chrono::steady_clock::now();
        // The page's textures get content keys from the first draw on (a texture uploaded before has none).
        if (start_.gpu) vcs::ge_gpu_backend_watch_overlay_textures(page_textures);
        const std::string asked = settings::environment_value("CHILI_PAGE_AT");
        for (std::string_view rest = asked; !rest.empty();) {
            // Steps are parted by ';' (a ',' in front of the next "<frame>:" does too; a value may hold commas).
            std::size_t end = rest.size();
            for (std::size_t at = 0; at < rest.size(); ++at) {
                if (rest[at] == ';') { end = at; break; }
                if (rest[at] != ',') continue;
                std::size_t digits = at + 1u;
                while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9') ++digits;
                if (digits > at + 1u && digits < rest.size() && rest[digits] == ':') { end = at; break; }
            }
            const auto item = settings::trimmed(rest.substr(0u, end));
            rest.remove_prefix(std::min(end + 1u, rest.size()));
            if (item.empty()) continue;
            const auto colon = item.find(':');
            std::uint64_t frame = 0u;
            bool digits = colon != std::string_view::npos && colon != 0u && colon <= 9u;
            for (std::size_t i = 0; digits && i < colon; ++i) { digits = item[i] >= '0' && item[i] <= '9'; frame = frame * 10u + static_cast<std::uint64_t>(item[i] - '0'); }
            if (!digits || colon + 1u >= item.size()) { lines.push_back("CHILI_PAGE_AT: '" + std::string(item) + "' is not <frame>:<step>, ignored"); continue; }
            steps_.push_back({frame, std::string(settings::trimmed(item.substr(colon + 1u)))});
        }
        std::stable_sort(steps_.begin(), steps_.end(), [](const Step &a, const Step &b) { return a.frame < b.frame; });
        shots_ = wide_variable(L"CHILI_PAGE_SHOTS");
        if (!shots_.empty()) { std::error_code status; std::filesystem::create_directories(shots_, status); }
        lines.push_back(std::string("the settings page is in this run") + (start_.gpu ? "" : ", but only the Direct3D 12 renderer draws it: it will not open") +
            (steps_.empty() ? std::string() : "; " + std::to_string(steps_.size()) + " steps of CHILI_PAGE_AT"));
        return lines;
    }
    [[nodiscard]] bool is_open() const noexcept { return page_.open(); }

    // What the game's menus call: the page opens now, so that the sample that chose the line is the page's already.
    void request(PageFrame frame) {
        if (!enabled_) {
            if (!std::exchange(said_absent_, true)) std::cout << "[settings] the page was asked for: this run has none (a scripted run asks with CHILI_PAGE=1)\n";
            return;
        }
        (void)open(frame, Page::Top, "the game's menu");
    }

    void frame(std::uint64_t frame, const psprecomp::GuestMemory &memory) {
        if (!enabled_) return;
        frame_ = frame; memory_ = &memory;
        if (!font_.loaded() && start_.gpu && frame >= next_scan_ && vcs::ge_gpu_backend_overlay_texture_ready(page_texture::font_fill)) {
            next_scan_ = frame + 15u;
            if (load_page_font(memory, font_)) say("the game's font for the page: " + std::to_string(font_.glyphs().size()) + " glyphs");
        }
        if (pending_window_) take_window();
        page_.tick(seconds());
        while (!steps_.empty() && steps_.front().frame <= frame) {
            const std::string step = std::move(steps_.front().text);
            steps_.erase(steps_.begin());
            run(step);
        }
        // The menu the page was opened over is gone (the game left it by itself): the page goes too.
        if (page_.open() && window_at_open_ && menu_top(memory).window != window_at_open_) {
            say("the game's menu changed under the page");
            page_.close();
        }
        settle();
        draw();
    }

    [[nodiscard]] ControllerInput sample(const ControllerInput &in) {
        if (!enabled_) return in;
        if (page_.open()) {
            page_.tick(seconds());
            page_.pad(in.buttons, in.x, in.y);
            ++samples_;
            settle();
            // Buttons 0 and both sticks in the middle: the game's menu stays where it is.
            return ControllerInput{};
        }
        // What is held when the page has closed (the button that closed it, a direction) is not the game's
        // until it is let go.
        if (std::exchange(mask_due_, false)) { mask_ = in.buttons; stick_mask_ = off_centre(in); }
        if (!mask_ && !stick_mask_) return in;
        ControllerInput out = in;
        out.buttons &= ~mask_;
        mask_ &= in.buttons;
        if (stick_mask_) { if (off_centre(in)) out.x = out.y = 128u; else stick_mask_ = false; }
        return out;
    }

    bool message(HWND window, UINT message, WPARAM w, LPARAM l, LRESULT &result) {
        if (!enabled_ || !page_.open()) { activating_ = false; return false; }
        // The player's mouse. While a replay or a script owns the pad it is theirs only in a scripted run,
        // whose mouse messages are a test's.
        if (!start_.scripted && PcInput::instance().scripted()) return false;
        // [Input.Mouse] Enabled=false: no mouse at all. MenuClicks=false: the pointer and the wheel, no clicks.
        const auto &input = PcInput::instance().config();
        if (!input.mouse_enabled) return false;
        const bool clicks = input.mouse_menu_clicks;
        float x = 0.0f, y = 0.0f;
        switch (message) {
        case WM_MOUSEACTIVATE:
            // The click that brings the window to the front is not a click on the page.
            if (LOWORD(l) == HTCLIENT) activating_ = true;
            return false;
        case WM_MOUSEMOVE: {
            // Windows sends the message again for a pointer that rests (a window changed under it): only a
            // pointer that moved takes the focus from the keys.
            if (l == last_move_ && !page_.dragging()) return false;
            last_move_ = l;
            const bool inside = pointer_of(window, l, x, y);
            if (inside || page_.dragging()) page_.pointer(x, y);
            settle();
            // The input layer still counts the movement: the mouse is the device in use.
            return false;
        }
        case WM_LBUTTONDOWN:
            if (!clicks) return false;
            if (std::exchange(activating_, false)) { result = 0; return true; }
            if (pointer_of(window, l, x, y)) {
                page_.press(x, y);
                if (page_.dragging() && !captured_) { SetCapture(window); captured_ = true; }
            }
            PcInput::instance().note_mouse_used();
            ++clicks_;
            settle();
            result = 0;
            return true;
        case WM_LBUTTONUP:
            activating_ = false;
            (void)pointer_of(window, l, x, y);
            page_.release(x, y);
            release_capture();
            settle();
            return false;
        case WM_RBUTTONDOWN:
            if (!clicks) return false;
            if (std::exchange(activating_, false)) { result = 0; return true; }
            page_.right_click();
            release_capture();
            PcInput::instance().note_mouse_used();
            ++clicks_;
            settle();
            result = 0;
            return true;
        case WM_MOUSEWHEEL: {
            wheel_ += GET_WHEEL_DELTA_WPARAM(w);
            const int notches = wheel_ / WHEEL_DELTA;
            wheel_ -= notches * WHEEL_DELTA;
            if (notches) page_.wheel(notches);
            PcInput::instance().note_mouse_used();
            settle();
            // Taken: a wheel notch is a D-pad press with the default bindings, and would change a value.
            result = 0;
            return true;
        }
        case WM_CAPTURECHANGED:
            if (captured_ && reinterpret_cast<HWND>(l) != window) { captured_ = false; page_.cancel_drag(); settle(); }
            return false;
        case WM_KILLFOCUS:
            activating_ = false;
            page_.cancel_drag();
            release_capture();
            settle();
            return false;
        default: return false;
        }
    }

    std::vector<std::string> report() {
        std::vector<std::string> lines;
        if (!enabled_ || std::exchange(reported_, true)) return lines;
        const auto stats = vcs::ge_gpu_backend_overlay_stats();
        char text[400];
        std::snprintf(text, sizeof(text), "the settings page: opened %llu times, %llu pad samples and %llu clicks taken, %llu draw lists built (mean %.1f us, longest %.1f us), "
            "%llu presents with a page, %llu runs left out for a texture not in the cache",
            static_cast<unsigned long long>(opened_), static_cast<unsigned long long>(samples_), static_cast<unsigned long long>(clicks_),
            static_cast<unsigned long long>(builds_), builds_ ? static_cast<double>(build_ns_) / 1000.0 / static_cast<double>(builds_) : 0.0,
            static_cast<double>(build_max_ns_) / 1000.0, static_cast<unsigned long long>(stats.presents), static_cast<unsigned long long>(stats.runs_without_texture));
        lines.emplace_back(text);
        return lines;
    }

    void stop() {
        if (!enabled_) return;
        if (page_.open()) page_.close();
        settle();
        release_capture();
        if (shown_) { vcs::ge_gpu_backend_set_overlay(nullptr); shown_ = false; }
        page_.attach(nullptr);
        enabled_ = false;
    }

private:
    struct Step { std::uint64_t frame; std::string text; };

    // A deterministic run's clock is its frame count, so that the pulse and a reason's three seconds repeat.
    [[nodiscard]] double seconds() const {
        return start_.frame_clock ? static_cast<double>(frame_) / 30.0 : std::chrono::duration<double>(std::chrono::steady_clock::now() - began_).count();
    }
    static bool off_centre(const ControllerInput &in) noexcept {
        return in.x < 80u || in.x > 176u || in.y < 80u || in.y > 176u;
    }
    void say(const std::string &line) const {
        std::cout << "[settings] frame " << frame_ << ": " << line << '\n';
        std::cout.flush();
    }
    // The pointer of a mouse message in PSP pixels, through the rectangle the renderer puts the picture in.
    bool pointer_of(HWND window, LPARAM l, float &x, float &y) {
        const PictureRect picture = window_picture_rect(window);
        if (!picture.empty()) page_.set_pointer_scale(picture.scale_x());
        return picture.to_picture(static_cast<std::int32_t>(static_cast<short>(LOWORD(l))), static_cast<std::int32_t>(static_cast<short>(HIWORD(l))), x, y);
    }
    void release_capture() {
        if (!std::exchange(captured_, false)) return;
        ReleaseCapture();
    }

    bool open(PageFrame frame, Page page, const char *by) {
        if (page_.open()) return true;
        if (!start_.gpu) {
            if (!std::exchange(said_software_, true)) say("the page was asked for: the software renderer draws none");
            return false;
        }
        if (!font_.loaded() && memory_) (void)load_page_font(*memory_, font_);
        if (!font_.loaded()) { say("the page was asked for before the game's font was loaded: not opened"); return false; }
        page_.tick(seconds());
        if (!page_.open_page(frame, page)) return false;
        const MenuTop top = memory_ ? menu_top(*memory_) : MenuTop{};
        window_at_open_ = top.known ? top.window : 0u;
        ++opened_;
        mask_ = 0u; stick_mask_ = false; mask_due_ = false; wheel_ = 0; last_move_ = -1;
        say(std::string("the page opens (") + by + "); " + menu_text(top));
        settle();
        return true;
    }
    // After anything that may have changed the page: its lines into the log, and what closing takes.
    void settle() {
        for (const auto &line : page_.take_events()) say(line);
        if (page_.open() || !was_open_) { was_open_ = page_.open(); return; }
        was_open_ = false;
        release_capture();
        if (shown_) { vcs::ge_gpu_backend_set_overlay(nullptr); shown_ = false; }
        mask_due_ = true;
        window_at_open_ = 0u;
        if (memory_) say(menu_text(menu_top(*memory_)));
    }
    void draw() {
        if (!page_.open() || !font_.loaded()) {
            if (shown_) { vcs::ge_gpu_backend_set_overlay(nullptr); shown_ = false; }
            return;
        }
        const auto started = std::chrono::steady_clock::now();
        PageDraw list;
        const int pictures = start_.dpad_pictures ? start_.dpad_pictures() : 0;
        page_.set_dpad_pictures(pictures == 2 ? SettingsPage::DpadPictures::Psp : pictures == 1 ? SettingsPage::DpadPictures::Pad : SettingsPage::DpadPictures::Keys);
        page_.build(list, font_, [](std::uint64_t key) { return vcs::ge_gpu_backend_overlay_texture_ready(key); });
        vcs::ge_gpu_backend_set_overlay(list.finish());
        const auto ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
        build_ns_ += ns; build_max_ns_ = std::max(build_max_ns_, ns); ++builds_;
        shown_ = true;
    }

    // The page as one line: where it is, the row in focus and what it says.
    [[nodiscard]] std::string state_text() const {
        if (!page_.open()) return "page closed";
        std::string text = std::string(page_.frame() == PageFrame::Pause ? "pause" : "menu") + " > " + std::string(settings::page_name(page_.page()));
        if (page_.rows().empty()) return text + " (no rows)";
        const auto &row = page_.rows()[page_.focus()];
        text += " > row " + std::to_string(page_.focus() + 1u) + " of " + std::to_string(page_.rows().size()) + " '" + row.name + "'";
        if (!row.value.empty()) text += " = " + row.value;
        if (row.pending) text += " (waits for a restart)";
        if (row.lock != settings::Lock::None) text += " (locked)";
        text += "; rows " + std::to_string(page_.first_visible() + 1u) + " to " + std::to_string(std::min(page_.rows().size(), page_.first_visible() + page_.layout().visible)) + " shown";
        const auto legends = page_.legends();
        text += "; legends " + std::string(legends.left) + (legends.right.empty() ? std::string() : " / " + std::string(legends.right));
        if (page_.asking()) text += "; asks '" + std::string(settings::page_word::reset_question) + "'";
        if (!page_.reason().empty()) text += "; says '" + std::string(page_.reason()) + "'";
        return text;
    }

    void run(const std::string &step) {
        const auto parts = split(step, ':');
        const std::string &verb = parts[0];
        const auto place = [&](std::size_t first, float &x, float &y) { return parts.size() >= first + 2u && number_of(parts[first], x) && number_of(parts[first + 1u], y); };
        float x = 0.0f, y = 0.0f;
        bool done = true, tell = true;
        if (verb == "open") {
            // The frame by the game's menu unless the step names one.
            const MenuTop top = memory_ ? menu_top(*memory_) : MenuTop{};
            MenuScene scene;
            PageFrame frame = top.known ? (top.name == "SubMenu_IngameMain" ? PageFrame::Pause : PageFrame::MainMenu)
                                        : memory_ && scene.loaded(*memory_) ? PageFrame::MainMenu : PageFrame::Pause;
            Page page = Page::Top;
            for (std::size_t i = 1; i < parts.size(); ++i) {
                static constexpr std::pair<std::string_view, Page> names[]{{"top", Page::Top}, {"display", Page::Display}, {"graphics", Page::Graphics}, {"controls", Page::Controls},
                    {"keyboard", Page::KeyboardMouse}, {"gamepad", Page::Gamepad}, {"textures", Page::TexturesMods}, {"sound", Page::Sound}};
                if (parts[i] == "main") frame = PageFrame::MainMenu;
                else if (parts[i] == "pause") frame = PageFrame::Pause;
                else { bool found = false; for (const auto &[name, which] : names) if (parts[i] == name) { page = which; found = true; } done = done && found; }
            }
            if (done) { if (page_.open()) page_.close(); settle(); done = open(frame, page, "a test step"); }
        } else if (verb == "up") page_.move(-1);
        else if (verb == "down") page_.move(+1);
        else if (verb == "left") page_.change(-1);
        else if (verb == "right") page_.change(+1);
        else if (verb == "ok") page_.ok();
        else if (verb == "back") page_.back();
        else if (verb == "close") page_.close();
        else if (verb == "pad" && parts.size() >= 2u) {
            const auto buttons = static_cast<std::uint32_t>(std::strtoul(parts[1].c_str(), nullptr, 16));
            const bool sticks = place(2u, x, y);
            page_.pad(buttons, sticks ? static_cast<std::uint8_t>(x) : std::uint8_t{128u}, sticks ? static_cast<std::uint8_t>(y) : std::uint8_t{128u});
        } else if (verb == "move" && place(1u, x, y)) page_.pointer(x, y);
        else if (verb == "press" && place(1u, x, y)) page_.press(x, y);
        else if (verb == "release" && place(1u, x, y)) page_.release(x, y);
        else if (verb == "click" && place(1u, x, y)) { page_.pointer(x, y); page_.press(x, y); page_.release(x, y); }
        else if (verb == "rclick") page_.right_click();
        else if (verb == "wheel" && parts.size() >= 2u) page_.wheel(std::atoi(parts[1].c_str()));
        else if (verb == "post" && parts.size() >= 2u && place(2u, x, y)) { done = post(parts[1], x, y); tell = false; }
        else if (verb == "set" && step.size() > 4u) {
            const auto out = start_.live->set_text(std::string_view(step).substr(4u), page_.open() ? page_.frame() : PageFrame::MainMenu);
            say(out.line);
            tell = false;
        } else if (verb == "state") { say("state: " + state_text() + "; " + (memory_ ? menu_text(menu_top(*memory_)) : std::string("no game memory yet"))); tell = false; }
        else if (verb == "shot" && parts.size() >= 2u) {
            settle(); draw();
            const auto path = shots_ / (parts[1] + ".png");
            const bool saved = !shots_.empty() && start_.save_frame && start_.save_frame(path);
            say("picture " + path.string() + (saved ? " written: " : " NOT written: ") + state_text());
            tell = false;
        } else if (verb == "window" && parts.size() >= 2u) {
            settle(); draw();
            if (start_.request_window_capture && !shots_.empty() && !pending_window_) { start_.request_window_capture(); pending_window_ = parts[1]; window_waits_ = 0u; }
            else done = false;
            tell = false;
        } else if (verb == "size" && parts.size() >= 2u) { done = resize(parts[1]); tell = false; }
        else if (verb == "fullscreen" && parts.size() >= 2u && start_.set_fullscreen) { start_.set_fullscreen(parts[1] == "on"); done = resize({}); tell = false; }
        else done = false;
        if (!done) { say("step '" + step + "' was not done"); return; }
        settle();
        if (tell) say("step '" + step + "': " + state_text());
    }

    // A real window message at a place of the picture, for the path a player's mouse takes.
    bool post(const std::string &what, float x, float y) {
        const HWND window = start_.window ? start_.window() : nullptr;
        const PictureRect picture = window_picture_rect(window);
        if (!window || picture.empty()) return false;
        float cx = 0.0f, cy = 0.0f;
        picture.to_client(x, y, cx, cy);
        const auto place = MAKELPARAM(static_cast<short>(std::lround(cx)), static_cast<short>(std::lround(cy)));
        RECT client{};
        GetClientRect(window, &client);
        bool posted = false;
        if (what == "move") posted = PostMessageW(window, WM_MOUSEMOVE, 0u, place) != FALSE;
        else if (what == "down") posted = PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, place) != FALSE;
        else if (what == "up") posted = PostMessageW(window, WM_LBUTTONUP, 0u, place) != FALSE;
        else if (what == "rdown") posted = PostMessageW(window, WM_RBUTTONDOWN, MK_RBUTTON, place) != FALSE && PostMessageW(window, WM_RBUTTONUP, 0u, place) != FALSE;
        // The wheel's own place is the screen's; its notches are the step's x.
        else if (what == "wheel") posted = PostMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<short>(std::lround(x) * WHEEL_DELTA)), 0) != FALSE;
        char text[240];
        std::snprintf(text, sizeof(text), "window message '%s' %s for (%.1f, %.1f) of the picture: client (%ld, %ld); the picture is at (%d, %d) %dx%d of a client area %ldx%ld",
            what.c_str(), posted ? "posted" : "NOT posted", x, y, std::lround(cx), std::lround(cy), picture.x, picture.y, picture.width, picture.height, client.right, client.bottom);
        say(text);
        return posted;
    }
    // "<width>x<height>": the client area gets that size; empty: only the line for the log.
    bool resize(const std::string &size) {
        const HWND window = start_.window ? start_.window() : nullptr;
        if (!window) return false;
        unsigned width = 0u, height = 0u;
        if (!size.empty()) {
            if (std::sscanf(size.c_str(), "%ux%u", &width, &height) != 2 || !width || !height) return false;
            RECT frame{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
            AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), FALSE, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)), GetDpiForWindow(window));
            SetWindowPos(window, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        RECT client{};
        GetClientRect(window, &client);
        const PictureRect picture = window_picture_rect(window);
        char text[200];
        std::snprintf(text, sizeof(text), "the window's client area is %ldx%ld, the picture at (%d, %d) %dx%d; the window in front is %s", client.right, client.bottom,
            picture.x, picture.y, picture.width, picture.height, GetForegroundWindow() == window ? "this one" : "another one");
        say(text);
        return true;
    }
    void take_window() {
        vcs::GeGpuWindowCapture capture;
        if (!start_.take_window_capture || !start_.take_window_capture(capture)) {
            if (++window_waits_ > 60u) { say("no present came for the window picture '" + *pending_window_ + "'"); pending_window_.reset(); }
            return;
        }
        std::vector<unsigned> pixels(static_cast<std::size_t>(capture.width) * capture.height);
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            const std::byte *p = capture.rgba8.data() + i * 4u;
            pixels[i] = 0xFF000000u | (std::to_integer<unsigned>(p[0]) << 16u) | (std::to_integer<unsigned>(p[1]) << 8u) | std::to_integer<unsigned>(p[2]);
        }
        const auto path = shots_ / (*pending_window_ + ".png");
        const bool written = write_png(path, capture.width, capture.height, pixels);
        char text[200];
        std::snprintf(text, sizeof(text), ": %ux%u, the game's picture at (%d, %d) %dx%d", capture.width, capture.height, capture.picture_x, capture.picture_y, capture.picture_width, capture.picture_height);
        say("window picture " + path.string() + (written ? " written" : " NOT written") + text);
        pending_window_.reset();
    }

    PageStart start_;
    SettingsPage page_;
    PageFont font_;
    const psprecomp::GuestMemory *memory_{};
    std::chrono::steady_clock::time_point began_{};
    std::vector<Step> steps_;
    std::filesystem::path shots_;
    std::optional<std::string> pending_window_;
    std::uint64_t frame_{}, next_scan_{}, opened_{}, samples_{}, clicks_{}, builds_{}, build_ns_{}, build_max_ns_{};
    // The window the page was opened over, the buttons kept from the game after it closed.
    std::uint32_t window_at_open_{}, mask_{};
    int wheel_{};
    LPARAM last_move_{-1};
    unsigned window_waits_{};
    bool enabled_{}, shown_{}, was_open_{}, mask_due_{}, stick_mask_{}, activating_{}, captured_{}, reported_{}, said_absent_{}, said_software_{};
};
Host &host() { static Host instance; return instance; }
}

void page_request_open(PageFrame frame) { host().request(frame); }
bool page_is_open() { return host().is_open(); }
std::vector<std::string> page_start(PageStart start) { return host().start(std::move(start)); }
void page_frame(std::uint64_t frame, const psprecomp::GuestMemory &memory) { host().frame(frame, memory); }
ControllerInput page_sample(const ControllerInput &sample) { return host().sample(sample); }
bool page_window_message(HWND window, UINT message, WPARAM w, LPARAM l, LRESULT &result) { return host().message(window, message, w, l, result); }
std::vector<std::string> page_report() { return host().report(); }
void page_stop() { host().stop(); }
}
