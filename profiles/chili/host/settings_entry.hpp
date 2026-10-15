#pragma once
// The ways into the settings page from the game's own menus, while the game runs
// (CONTROLS.md, "The settings page inside the game"). The blocks are
// changed where they are served (menu_entry_blocks.hpp); this is the other half: once per pad sample the
// host looks at the game's menu, with reads of guest memory and no function over guest code, and asks for
// the page (settings_page_api.hpp) when the player chose the entry.
//
//   main menu       the line where Fiesta stood sends command 9001 to its window, which keeps it in its member
//                   m_iCommand: read there, -1 written back (what the window starts with), the page asked for
//   pause menu      the appended line `Settings` has command -1, on which the game does nothing: a new press of
//                   Cross with that line selected is the choice. The Cross is kept from the game until it is
//                   let go. A pause window without the line (a kind the host does not append to, or
//                   CHILI_PAGE_ENTRY=legend) has the legend instead: Triangle, which no pause window uses,
//                   and the host draws "<Triangle> Settings" under the pause box (settings_entry_legend.hpp).
//                   A left click on the legend is its button too (in a menu a click anywhere else is Cross)
//   Profile Options Sync Highscores starts the PSP's wireless play, which this port does not have: its line
//                   is switched off (one word of the line's members, written whenever it reads on while the
//                   list is up, because the game's script switches it on each time it shows the list). The
//                   game then draws it dark and the cursor skips it
//   a hidden door   F2 in any of these menus: for tests, in no legend. (Select was a second one until the
//                   first version shipped: Tab is Select by default, and a player who pressed it by habit got
//                   the page without knowing why.)
//
// What does need a function over guest code, and is therefore never in a plain deterministic run:
//
//   NetworkInterface::cmd_start_network (0x08D751AC) answers "false", so no line of any menu can reach the
//   network imports the port lacks (without it choosing Fiesta or Sync Highscores ends the program)
//
//   CHILI_PAGE_ENTRY=hook, or a main menu block that could not be changed (a mod's own): the main menu's
//   entry by two host functions instead of the changed block. TextBox::SetTextSlotIndex (0x088F4744) gives
//   the Fiesta line and its two boxes string 119 for 13, and MenuWidgetButton::on_MenuWidgetButtonEvent
//   (0x08BACA84) asks for the page when that line is chosen and hands the game a button code no line knows
//
// The window stack: master_control = [[[0x08DFBC04]+0x14]+0x278]; its member block (+0x14) has the list of
// open windows at +0x12C (count at [list+8], entries at [[list+4]+4]); the last one has the focus. A logic
// node can be the last for a moment, so the top's name is checked before any of its members is read. A
// window's member block: +0x28 columns, +0x4C m_iCommand, +0x50 the list of lines, +0x60 row, +0x64 column.
// A line's: +0x04 MenuCommand, +0x08 m_tIsActive. A node: +0x14 member block, +0x3C parent, +0x48 name.
#include "menu_entry_blocks.hpp"
#include "settings_page_api.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
namespace chili {
// Whether this run has the entries: a player's run has; a deterministic, replay or script run only with
// CHILI_PAGE=1, so such a run is otherwise exactly what it was before the page existed.
inline bool settings_entry_enabled(bool scripted_run) {
    char value[8]{};
    std::size_t length = 0u;
    return !scripted_run || (getenv_s(&length, value, sizeof(value), "CHILI_PAGE") == 0 && std::string_view(value) == "1");
}

// The player's switch: `in_game` is [Game] SettingsInGame of ChiliNative.ini (true when the file does not
// say). CHILI_PAGE=0 switches the lines and the page off for one run, CHILI_PAGE=1 asks for them whatever
// the file says.
inline bool settings_switched_off(bool in_game) {
    char value[8]{};
    std::size_t length = 0u;
    const std::string_view asked = getenv_s(&length, value, sizeof(value), "CHILI_PAGE") == 0 ? std::string_view(value) : std::string_view();
    return asked == "0" || (!in_game && asked != "1");
}

class SettingsEntry {
public:
    // PSP buttons of a pad sample.
    static constexpr std::uint32_t select_button = 0x0001u, triangle_button = 0x1000u, cross_button = 0x4000u;
    static constexpr std::uint32_t globals_address = 0x08DFBC04u, self_address = 0x08DFB450u;
    static constexpr std::uint32_t start_network_address = 0x08D751ACu, set_text_slot_index_address = 0x088F4744u, widget_button_event_address = 0x08BACA84u;
    // Pad samples a pause window without the line stands before its legend is drawn: its lines slide in for
    // about a fifth of a second.
    static constexpr unsigned legend_delay = 6u;

    // CHILI_PAGE_ENTRY=<word>[,<word>]: `hook` takes the main menu's entry by host functions and leaves its
    // block alone; `legend` appends no pause line, so every pause window gets the legend; `sync` leaves Sync
    // Highscores on, for the one test that has to reach the game's network start. Nothing: the blocks.
    struct Route { bool hook{}, legend{}, sync{}; std::string problem; };
    static Route route(std::string_view text) {
        Route out;
        while (!text.empty()) {
            const auto end = std::min(text.find(','), text.size());
            const auto word = text.substr(0u, end);
            text.remove_prefix(std::min(end + 1u, text.size()));
            if (word == "hook") out.hook = true;
            else if (word == "legend") out.legend = true;
            else if (word == "sync") out.sync = true;
            else if (!word.empty() && word != "data") out.problem = "CHILI_PAGE_ENTRY knows data, hook, legend and sync: '" + std::string(word) + "' ignored";
        }
        return out;
    }
    static Route route_from_environment() {
        char value[64]{};
        std::size_t length = 0u;
        return getenv_s(&length, value, sizeof(value), "CHILI_PAGE_ENTRY") == 0 ? route(value) : Route{};
    }

    struct Options {
        bool main_menu_poll{};    // the main menu's block carries the entry: its window's command is read
        bool main_menu_hooks{};   // the main menu's entry by the two host functions
        bool leave_sync{};        // Sync Highscores stays on (a test of the network answer)
    };
    // What the legend's painter is told: draw it, take it away, or leave the picture alone (the page is up
    // and the overlay is the page's). Hidden and PageOwns are said once, Shown with every pad sample while
    // the legend is due, so a painter can put back a legend that something else took away.
    enum class Legend : std::uint8_t { Hidden, Shown, PageOwns };

    explicit SettingsEntry(Options options = {}) : options_(options) {}
    SettingsEntry(const SettingsEntry &) = delete;
    SettingsEntry &operator=(const SettingsEntry &) = delete;
    ~SettingsEntry() { if (current() == this) current() = nullptr; }

    // Puts the host functions in place (before the game runs). Returns the lines for the log.
    std::vector<std::string> install(psprecomp::Runtime &runtime) {
        std::vector<std::string> lines;
        current() = this;
        const auto over = [&](std::uint32_t address, psprecomp::Runtime::RecompiledFunction function, psprecomp::Runtime::RecompiledFunction &original, const char *label) {
            original = runtime.function_at(address);
            if (!original) { lines.push_back("no generated code at " + psprecomp::hex32(address) + ": " + label + " is not put in place"); return false; }
            runtime.register_function(address, function, label);
            return true;
        };
        if (over(start_network_address, &network_thunk, original(0), "chili_no_network_start"))
            lines.push_back("the game's network start (" + psprecomp::hex32(start_network_address) + ") answers false: the PSP's wireless play is not there");
        if (options_.main_menu_hooks) {
            const bool label = over(set_text_slot_index_address, &label_thunk, original(1), "chili_settings_label");
            const bool choice = over(widget_button_event_address, &choice_thunk, original(2), "chili_settings_choice");
            if (label && choice) lines.push_back("main menu: the Fiesta line is Settings by two host functions (its label at " + psprecomp::hex32(set_text_slot_index_address) +
                ", its choice at " + psprecomp::hex32(widget_button_event_address) + ")");
        }
        return lines;
    }
    void set_legend(std::function<void(Legend)> painter) { legend_painter_ = std::move(painter); }
    void set_log(std::ostream *log) noexcept { log_ = log; }
    // F2 in the game's window (the game's thread): the page at the next pad sample that finds a menu.
    void hotkey() noexcept { hotkey_ = true; }
    // The same for the window's message hook, which has no object at hand. Nothing when no entry is installed.
    static void hotkey_pressed() noexcept { if (auto *entry = current()) entry->hotkey(); }
    // Where the legend is in the picture, in PSP pixels (its painter says).
    void set_legend_area(float x0, float y0, float x1, float y1) noexcept { area_[0] = x0; area_[1] = y0; area_[2] = x1; area_[3] = y1; }
    // A left click at that place of the picture (the game's thread). True when the legend is up and the click
    // is on it: the click is the legend's button and must not also become the menu's Cross.
    bool click(float x, float y) noexcept {
        if (legend_ != Legend::Shown || !(x >= area_[0] && x < area_[2] && y >= area_[1] && y < area_[3])) return false;
        clicked_ = true;
        return true;
    }
    static bool legend_click(float x, float y) noexcept { auto *entry = current(); return entry && entry->click(x, y); }

    // Once per pad sample, with the buttons the game is about to get; it may take some out. `sample` is the
    // sample's number, for the log.
    void sampled(psprecomp::GuestMemory &memory, std::uint32_t &buttons, std::uint64_t sample) {
        const std::uint32_t raw = buttons, pressed = raw & ~previous_;
        previous_ = raw;
        // What opened the page stays away from the game until it is let go.
        const auto keep_back = [&] { buttons &= ~held_; held_ &= raw; };
        if (page_is_open()) {
            // The page has the pad and the picture; the game's menu is held by the page.
            hotkey_ = clicked_ = false;
            paint(Legend::PageOwns);
            keep_back();
            return;
        }
        const std::uint32_t top = top_window(memory);
        const std::uint32_t block = top ? word(memory, top + 0x14u) : 0u;
        const char *how = nullptr;
        PageFrame frame = PageFrame::MainMenu;
        bool legend = false, waiting = false;
        if (block && named(memory, top, menu_entry::main_menu_window)) {
            if (options_.main_menu_poll && word(memory, block + 0x4Cu) == menu_entry::main_menu_command) {
                memory.store32(block + 0x4Cu, 0xFFFFFFFFu);
                how = "the main menu's line";
            }
        } else if (block && named(memory, top, "SubMenu_Profile")) {
            if (!options_.leave_sync) switch_off(memory, block, "Syncronize", sample);
        } else if (block && named(memory, top, menu_entry::pause_window)) {
            frame = PageFrame::Pause;
            const std::uint32_t line = line_named(memory, block, menu_entry::pause_line);
            if (line) {
                if ((pressed & cross_button) && selected_line(memory, block) == line) { how = "the pause menu's line"; held_ |= cross_button; }
            } else {
                waiting = true;
                legend = ++pause_samples_ > legend_delay;
                if (pressed & triangle_button) { how = "Triangle in the pause menu (the legend)"; held_ |= triangle_button; }
                else if (clicked_) how = "a click on the pause menu's legend";
            }
            pause_line_ = line != 0u;
        } else if (!(block && named(memory, top, "SubMenu_Settings"))) {
            // No menu the page knows: F2 opens nothing here.
            hotkey_ = false;
        }
        if (!waiting) pause_samples_ = 0u;
        if (!how && hotkey_ && top) how = "F2 (the hidden door)";
        hotkey_ = clicked_ = false;
        // The legend goes before the page is asked for: the page may open inside the call and put its own
        // picture up at once.
        paint(legend && !how ? Legend::Shown : Legend::Hidden);
        if (how) {
            ++asked_;
            if (log_) *log_ << "[menu] sample " << sample << ": Settings by " << how << ", the page is asked for (" << (frame == PageFrame::Pause ? "pause menu" : "main menu") << ")\n";
            page_request_open(frame);
        }
        keep_back();
    }

    // The label of a text box as the game sets it (hook route): 119 for 13 on the Fiesta line of the main
    // menu and on the two boxes below it. True when the index was changed.
    bool label(const psprecomp::GuestMemory &memory, std::uint32_t box, std::uint32_t &index) {
        if (index != menu_entry::fiesta_text || !box) return false;
        const std::uint32_t above = word(memory, box + 0x3Cu);
        const std::uint32_t line = named(memory, box, menu_entry::main_menu_line) ? box : above && named(memory, above, menu_entry::main_menu_line) ? above : 0u;
        if (!line || !named(memory, word(memory, line + 0x3Cu), menu_entry::main_menu_window)) return false;
        index = menu_entry::settings_text;
        ++labels_;
        return true;
    }
    // A button event on a menu line as the game's handler gets it (hook route): `self` the line, `arguments`
    // the two words button and state. True when it was the choice of the Fiesta line: the page is asked for
    // and the game's handler is left a button code no line knows.
    bool choice(psprecomp::GuestMemory &memory, std::uint32_t self, std::uint32_t arguments) {
        if (!self || !memory.contains(arguments, 8u) || memory.load32(arguments) != 5u || memory.load32(arguments + 4u) != 1u) return false;
        if (!named(memory, self, menu_entry::main_menu_line) || !named(memory, word(memory, self + 0x3Cu), menu_entry::main_menu_window)) return false;
        memory.store32(arguments, 99u);
        if (page_is_open()) return true;
        ++asked_;
        if (log_) *log_ << "[menu] Settings by the main menu's line (host function), the page is asked for (main menu)\n";
        page_request_open(PageFrame::MainMenu);
        return true;
    }

    // What happened so far, for the end of the run and for tests.
    [[nodiscard]] unsigned asked() const noexcept { return asked_; }
    [[nodiscard]] unsigned switched_off() const noexcept { return switched_off_; }
    [[nodiscard]] unsigned network_answers() const noexcept { return network_answers_; }
    [[nodiscard]] unsigned labels() const noexcept { return labels_; }
    [[nodiscard]] Legend legend() const noexcept { return legend_; }
    // Whether the pause window seen last had the line (false: the legend's kind).
    [[nodiscard]] bool pause_line() const noexcept { return pause_line_; }
    [[nodiscard]] std::string report() const {
        return "the page was asked for " + std::to_string(asked_) + " time" + (asked_ == 1u ? "" : "s") + "; Sync Highscores switched off " + std::to_string(switched_off_) +
            " time" + (switched_off_ == 1u ? "" : "s") + "; the game's network start answered false " + std::to_string(network_answers_) + " time" + (network_answers_ == 1u ? "" : "s");
    }

    // ---- Reads of the game's menu (no writes) ----
    static std::uint32_t word(const psprecomp::GuestMemory &memory, std::uint32_t address) {
        return address && memory.contains(address, 4u) ? memory.load32(address) : 0u;
    }
    static bool named(const psprecomp::GuestMemory &memory, std::uint32_t node, std::string_view text) {
        const std::uint32_t at = node ? word(memory, node + 0x48u) : 0u;
        if (!at || !memory.contains(at, text.size() + 1u)) return false;
        for (std::size_t i = 0; i <= text.size(); ++i)
            if (memory.load8(at + static_cast<std::uint32_t>(i)) != (i < text.size() ? static_cast<std::uint8_t>(text[i]) : 0u)) return false;
        return true;
    }
    // The entries of a script's list member: how many, and where the words are.
    struct List { std::uint32_t count{}, data{}; };
    static List list(const psprecomp::GuestMemory &memory, std::uint32_t object, std::uint32_t limit) {
        if (!object) return {};
        const std::uint32_t count = word(memory, object + 8u), holder = word(memory, object + 4u), data = holder ? word(memory, holder + 4u) : 0u;
        if (!count || count > limit || !data || !memory.contains(data, 4u * count)) return {};
        return {count, data};
    }
    // The window that has the focus: the last of master_control's stack. 0 when no menu is up.
    static std::uint32_t top_window(const psprecomp::GuestMemory &memory) {
        const std::uint32_t globals = word(memory, globals_address), globals_block = globals ? word(memory, globals + 0x14u) : 0u;
        const std::uint32_t master = globals_block ? word(memory, globals_block + 0x278u) : 0u, master_block = master ? word(memory, master + 0x14u) : 0u;
        const auto stack = list(memory, master_block ? word(memory, master_block + 0x12Cu) : 0u, 32u);
        return stack.count ? memory.load32(stack.data + 4u * (stack.count - 1u)) : 0u;
    }
    // The line of that name among a window's lines; 0 when it has none.
    static std::uint32_t line_named(const psprecomp::GuestMemory &memory, std::uint32_t window_block, std::string_view name) {
        const auto lines = list(memory, word(memory, window_block + 0x50u), 64u);
        for (std::uint32_t i = 0; i < lines.count; ++i)
            if (const std::uint32_t line = memory.load32(lines.data + 4u * i); named(memory, line, name)) return line;
        return 0u;
    }
    static std::uint32_t selected_line(const psprecomp::GuestMemory &memory, std::uint32_t window_block) {
        const auto lines = list(memory, word(memory, window_block + 0x50u), 64u);
        const std::uint32_t row = word(memory, window_block + 0x60u), column = word(memory, window_block + 0x64u), columns = word(memory, window_block + 0x28u);
        const std::uint64_t index = std::uint64_t{row} * (columns ? columns : 1u) + column;
        return index < lines.count ? memory.load32(lines.data + 4u * static_cast<std::uint32_t>(index)) : 0u;
    }

private:
    // m_tIsActive of a line of the window, held at false.
    void switch_off(psprecomp::GuestMemory &memory, std::uint32_t window_block, std::string_view name, std::uint64_t sample) {
        const std::uint32_t line = line_named(memory, window_block, name), line_block = line ? word(memory, line + 0x14u) : 0u;
        if (!line_block || !memory.contains(line_block + 8u, 4u) || memory.load32(line_block + 8u) == 0u) return;
        memory.store32(line_block + 8u, 0u);
        if (!switched_off_++ && log_) *log_ << "[menu] sample " << sample << ": Profile Options, Sync Highscores switched off (it starts the PSP's wireless play)\n";
    }
    void paint(Legend state) {
        if (state == legend_ && state != Legend::Shown) return;
        legend_ = state;
        if (legend_painter_) legend_painter_(state);
    }
    static SettingsEntry *&current() { static SettingsEntry *entry = nullptr; return entry; }
    static psprecomp::Runtime::RecompiledFunction &original(unsigned which) { static psprecomp::Runtime::RecompiledFunction functions[3]{}; return functions[which]; }
    // NetworkInterface::cmd_start_network: a1 points at the truth it answers with.
    static void network_thunk(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
        if (runtime.memory().contains(ctx.gpr[5], 4u)) runtime.memory().store32(ctx.gpr[5], 0u);
        if (auto *entry = current()) {
            if (!entry->network_answers_++ && entry->log_) *entry->log_ << "[menu] the game asked for its network: answered false\n";
        }
        ctx.pc = ctx.gpr[31];
    }
    // TextBox::SetTextSlotIndex(a0 = box, a1 = index).
    static void label_thunk(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
        if (auto *entry = current()) (void)entry->label(runtime.memory(), ctx.gpr[4], ctx.gpr[5]);
        original(1)(runtime, ctx);
    }
    // MenuWidgetButton::on_MenuWidgetButtonEvent: a script handler, self at self_address, a1 = the argument words.
    static void choice_thunk(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
        if (auto *entry = current()) (void)entry->choice(runtime.memory(), word(runtime.memory(), self_address), ctx.gpr[5]);
        original(2)(runtime, ctx);
    }

    Options options_;
    std::function<void(Legend)> legend_painter_;
    std::ostream *log_{&std::cout};
    std::uint32_t previous_{}, held_{};
    unsigned pause_samples_{}, asked_{}, switched_off_{}, network_answers_{}, labels_{};
    Legend legend_{Legend::Hidden};
    float area_[4]{};
    bool hotkey_{}, clicked_{}, pause_line_{};
};
}
