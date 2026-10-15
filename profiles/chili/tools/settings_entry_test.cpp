// The ways into the settings page while the game runs (host/settings_entry.hpp, settings_entry_legend.hpp), on
// a made-up guest memory: the game's nodes, member blocks and lists written where the host reads them. The
// page itself is two functions here that count the requests and say whether a page is open.
// Part of chili_menu_entry_test (menu_entry_test.cpp).
#include "../host/settings_entry.hpp"
#include "../host/settings_entry_legend.hpp"
#include <cstdlib>
#include <stdexcept>
namespace {
unsigned *counter = nullptr;
void require(bool value, const std::string &message) { ++*counter; if (!value) throw std::runtime_error("settings entry: " + message); }
struct PageDouble { unsigned asked{}; chili::PageFrame frame{}; bool open{}; } page;

using Entry = chili::SettingsEntry;
// The game's menu objects as far as the host reads them.
struct World {
    psprecomp::Runtime runtime;
    psprecomp::GuestMemory &memory{runtime.memory()};
    std::uint32_t next{0x09000000u};
    std::uint32_t master_block{}, stack{};
    std::uint32_t reserve(std::uint32_t bytes) { const auto at = next; next += (bytes + 15u) & ~15u; for (std::uint32_t i = 0; i < bytes; i += 4u) memory.store32(at + i, 0u); return at; }
    std::uint32_t text(const std::string &value) {
        const auto at = reserve(static_cast<std::uint32_t>(value.size()) + 1u);
        for (std::size_t i = 0; i < value.size(); ++i) memory.store8(at + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(value[i]));
        return at;
    }
    // A node with a member block of its own; `block` false: a node without one.
    std::uint32_t node(const std::string &name, std::uint32_t parent = 0u, bool block = true) {
        const auto at = reserve(0x60u);
        memory.store32(at + 0x3Cu, parent);
        memory.store32(at + 0x48u, text(name));
        if (block) memory.store32(at + 0x14u, reserve(0x140u));
        return at;
    }
    std::uint32_t block(std::uint32_t node_at) const { return memory.load32(node_at + 0x14u); }
    // A list member: the object, its holder, the words.
    std::uint32_t list(const std::vector<std::uint32_t> &entries) {
        const auto object = reserve(16u), holder = reserve(16u), data = reserve(4u * 64u);
        memory.store32(object + 4u, holder); memory.store32(holder + 4u, data);
        fill(object, entries);
        return object;
    }
    void fill(std::uint32_t object, const std::vector<std::uint32_t> &entries) {
        const auto data = memory.load32(memory.load32(object + 4u) + 4u);
        for (std::size_t i = 0; i < entries.size(); ++i) memory.store32(data + 4u * static_cast<std::uint32_t>(i), entries[i]);
        memory.store32(object + 8u, static_cast<std::uint32_t>(entries.size()));
    }
    World() {
        const auto globals = node("Globals"), master = node("MasterControl");
        memory.store32(Entry::globals_address, globals);
        const auto globals_block = reserve(0x400u);
        memory.store32(globals + 0x14u, globals_block);
        memory.store32(globals_block + 0x278u, master);
        master_block = reserve(0x200u);
        memory.store32(master + 0x14u, master_block);
        stack = list({});
        memory.store32(master_block + 0x12Cu, stack);
    }
    void open(const std::vector<std::uint32_t> &windows) { fill(stack, windows); }
    // A window with its lines; every line active, command -1 in the window.
    std::uint32_t window(const std::string &name, std::uint32_t parent, const std::vector<std::string> &lines, std::vector<std::uint32_t> *made = nullptr) {
        const auto at = node(name, parent);
        std::vector<std::uint32_t> nodes;
        for (const auto &line : lines) {
            nodes.push_back(node(line, at));
            memory.store32(block(nodes.back()) + 8u, 1u);
        }
        memory.store32(block(at) + 0x28u, 1u);
        memory.store32(block(at) + 0x4Cu, 0xFFFFFFFFu);
        memory.store32(block(at) + 0x50u, list(nodes));
        if (made) *made = nodes;
        return at;
    }
    void select(std::uint32_t window_at, std::uint32_t row) { memory.store32(block(window_at) + 0x60u, row); memory.store32(block(window_at) + 0x64u, 0u); }
    std::uint32_t command(std::uint32_t window_at) const { return memory.load32(block(window_at) + 0x4Cu); }
};
// One pad sample: returns what the game gets.
std::uint32_t sample(Entry &entry, World &world, std::uint32_t buttons) {
    static std::uint64_t number = 0u;
    entry.sampled(world.memory, buttons, number++);
    return buttons;
}

void main_menu() {
    World world;
    Entry entry({true, false});
    entry.set_log(nullptr);
    page = {};
    require(sample(entry, world, Entry::cross_button) == Entry::cross_button && page.asked == 0u, "something happened with no menu up");
    const auto menues = world.node("Menues", 0u, false);
    const auto main = world.window("Menu_Main", menues, {"Gringo", "Macho", "Donkey", "Fiesta"});
    world.open({main});
    world.select(main, 3u);
    require(sample(entry, world, 0u) == 0u && sample(entry, world, Entry::cross_button) == Entry::cross_button && page.asked == 0u, "the page was asked for before the game sent the command");
    require(sample(entry, world, Entry::select_button) == Entry::select_button && sample(entry, world, 0u) == 0u && page.asked == 0u, "Select in the main menu opened the page or was kept from the game");
    // The game's own commands leave the page alone: the member holds whatever the window was sent last.
    for (const std::uint32_t other : {302u, 106u, 800u, 0u}) {
        world.memory.store32(world.block(main) + 0x4Cu, other);
        require(sample(entry, world, 0u) == 0u && page.asked == 0u && world.command(main) == other, "another command opened the page");
    }
    world.memory.store32(world.block(main) + 0x4Cu, 9001u);
    require(sample(entry, world, Entry::cross_button) == Entry::cross_button && page.asked == 1u && page.frame == chili::PageFrame::MainMenu, "command 9001 did not ask for the page");
    require(world.command(main) == 0xFFFFFFFFu && entry.asked() == 1u, "the command was not taken back to -1");
    require(sample(entry, world, Entry::cross_button) == Entry::cross_button && sample(entry, world, 0u) == 0u && page.asked == 1u, "the page was asked for twice for one choice");
    world.memory.store32(world.block(main) + 0x4Cu, 9001u);
    require(sample(entry, world, 0u) == 0u && page.asked == 2u, "a second choice was not seen");
    // A logic node on top of the stack (it has no window members): nothing is read from it.
    const auto logic = world.node("Menu_Profile", menues, false);
    world.memory.store32(world.block(main) + 0x4Cu, 9001u);
    world.open({main, logic});
    require(sample(entry, world, 0u) == 0u && page.asked == 2u && world.command(main) == 9001u, "the main menu was read while it did not have the focus");
    // Without the changed block the command is not looked at.
    Entry hooks({false, true});
    hooks.set_log(nullptr);
    world.open({main});
    require(sample(hooks, world, 0u) == 0u && page.asked == 2u && world.command(main) == 9001u, "the command was read on the hook route");
}

void profile_options() {
    World world;
    Entry entry({true, false});
    entry.set_log(nullptr);
    page = {};
    const auto menues = world.node("Menues", 0u, false);
    const auto main = world.window("Menu_Main", menues, {"Gringo", "Fiesta"});
    const auto logic = world.node("Menu_Profile", menues, false);
    std::vector<std::uint32_t> lines;
    const auto profile = world.window("SubMenu_Profile", logic, {"Create", "Save", "Load", "Delete", "Syncronize", "Trophy", "Settings"}, &lines);
    world.open({main, logic, profile});
    const auto active = [&](std::size_t i) { return world.memory.load32(world.block(lines[i]) + 8u); };
    (void)sample(entry, world, 0u);
    require(active(4) == 0u && entry.switched_off() == 1u, "Sync Highscores is still on");
    for (const std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u}) require(active(i) == 1u, "another line of Profile Options was switched off");
    (void)sample(entry, world, 0u);
    require(entry.switched_off() == 1u, "the line was written again though it was off");
    // The game's script switches it on every time it shows the list.
    world.memory.store32(world.block(lines[4]) + 8u, 1u);
    (void)sample(entry, world, 0u);
    require(active(4) == 0u && entry.switched_off() == 2u && page.asked == 0u, "the line was not switched off again");
    // A test may leave the line on (CHILI_PAGE_ENTRY=sync), to reach the game's network start.
    {
        Entry lenient({true, false, true});
        lenient.set_log(nullptr);
        world.memory.store32(world.block(lines[4]) + 8u, 1u);
        (void)sample(lenient, world, 0u);
        require(active(4) == 1u && lenient.switched_off() == 0u, "Sync Highscores was switched off though the test asked to leave it");
        (void)sample(entry, world, 0u);
        require(active(4) == 0u, "Sync Highscores stayed on");
    }
    // Select has a use of its own nowhere but is no door here; F2 is.
    require(sample(entry, world, Entry::select_button) == Entry::select_button && page.asked == 0u, "Select opened the page in Profile Options");
    entry.hotkey();
    require(sample(entry, world, 0u) == 0u && page.asked == 1u && page.frame == chili::PageFrame::MainMenu, "F2 did not open the page in Profile Options");
    const auto settings = world.window("SubMenu_Settings", logic, {"Music", "Inverse"});
    world.open({main, logic, settings});
    entry.hotkey();
    require(sample(entry, world, 0u) == 0u && page.asked == 2u, "F2 did not open the page on the game's own Settings screen");
    // F2 where no menu the page knows is up is forgotten, not kept for the next menu.
    const auto trophies = world.window("Menu_Trophies", logic, {"One"});
    world.open({main, logic, trophies});
    entry.hotkey();
    (void)sample(entry, world, 0u);
    world.open({main});
    require(sample(entry, world, 0u) == 0u && page.asked == 2u, "F2 was kept for a later menu");
    world.open({});
    entry.hotkey();
    (void)sample(entry, world, 0u);
    world.open({main});
    require(sample(entry, world, 0u) == 0u && page.asked == 2u, "F2 in play was kept for the next menu");
    // Nothing is installed here: the window's hook finds no entry and does nothing.
    Entry::hotkey_pressed();
    require(sample(entry, world, 0u) == 0u && page.asked == 2u, "a hotkey without an installed entry");
}

void pause_menu() {
    World world;
    Entry entry({true, false});
    entry.set_log(nullptr);
    page = {};
    // What the painter is told, without the repeats, and how often it was told to show.
    std::vector<Entry::Legend> painted;
    unsigned shows = 0u;
    entry.set_legend([&](Entry::Legend state) {
        if (state == Entry::Legend::Shown) ++shows;
        if (painted.empty() || painted.back() != state) painted.push_back(state);
    });
    const auto mode = world.node("GringoMode", 0u, false);
    const auto logic = world.node("Menu_IngameMain", mode, false);
    const auto with_line = world.window("SubMenu_IngameMain", logic, {"Resume", "Rewind", "Restart", "Quit", "Settings"});
    world.open({logic, with_line});
    world.select(with_line, 4u);
    // The choice: a new press of Cross with the line selected. The game never gets that Cross.
    require(sample(entry, world, 0u) == 0u && page.asked == 0u && entry.pause_line(), "the pause window with the line");
    require(sample(entry, world, Entry::cross_button) == 0u && page.asked == 1u && page.frame == chili::PageFrame::Pause, "Cross on the Settings line did not ask for the page");
    require(sample(entry, world, Entry::cross_button | 0x40u) == 0x40u && sample(entry, world, Entry::cross_button) == 0u && page.asked == 1u, "the held Cross reached the game or asked again");
    require(sample(entry, world, 0u) == 0u && sample(entry, world, Entry::cross_button) == 0u && page.asked == 2u, "a second choice after the button was let go");
    (void)sample(entry, world, 0u);
    // On another line Cross is the game's.
    world.select(with_line, 0u);
    require(sample(entry, world, Entry::cross_button) == Entry::cross_button && page.asked == 2u, "Cross on Resume was taken");
    // Cross held from another line while the cursor comes to the Settings line is not a choice.
    world.select(with_line, 4u);
    require(sample(entry, world, Entry::cross_button) == Entry::cross_button && page.asked == 2u, "a Cross held from before chose the line");
    (void)sample(entry, world, 0u);
    // Triangle is nothing in a window that has the line; no legend there.
    require(sample(entry, world, Entry::triangle_button) == Entry::triangle_button && page.asked == 2u && painted.empty(), "Triangle or the legend in a window that has the line");
    (void)sample(entry, world, 0u);
    // Select is no door (Tab by default: pressed by habit it would open a page nobody asked for); F2 is the
    // hidden one, for tests.
    require(sample(entry, world, Entry::select_button) == Entry::select_button && page.asked == 2u, "Select in the pause menu opened the page or was kept from the game");
    (void)sample(entry, world, 0u);
    entry.hotkey();
    require(sample(entry, world, 0u) == 0u && page.asked == 3u && page.frame == chili::PageFrame::Pause, "F2 in the pause menu");
    (void)sample(entry, world, 0u);
    // The top of the stack is the logic node for a moment (the window was popped): nothing is read.
    world.open({logic});
    require(sample(entry, world, Entry::cross_button) == Entry::cross_button && page.asked == 3u, "the logic node was taken for the window");
    (void)sample(entry, world, 0u);

    // A pause window of a kind that got no line: the legend after the lines have slid in, and Triangle.
    const auto plain = world.window("SubMenu_IngameMain", logic, {"Resume", "Objectives", "Rewind", "Restart", "Skip Challenge", "Quit"});
    world.open({logic, plain});
    world.select(plain, 5u);
    for (unsigned i = 0; i < Entry::legend_delay; ++i) (void)sample(entry, world, 0u);
    require(painted.empty() && entry.legend() == Entry::Legend::Hidden && !entry.pause_line(), "the legend came before the window had settled");
    (void)sample(entry, world, 0u);
    require(painted == std::vector{Entry::Legend::Shown}, "the legend was not drawn");
    require(sample(entry, world, Entry::cross_button) == Entry::cross_button && page.asked == 3u, "Cross on the game's last line was taken");
    (void)sample(entry, world, 0u);
    // Said again with every sample while the legend is due: its painter puts back a legend that was taken away.
    require(painted.size() == 1u && shows == 3u, "the painter was not told with every sample that the legend is due");
    // A left click on the legend is its button; a click elsewhere is not the legend's (it is the menu's Cross).
    entry.set_legend_area(200.0f, 238.0f, 294.0f, 268.0f);
    require(!entry.click(100.0f, 100.0f) && !entry.click(240.0f, 230.0f) && sample(entry, world, 0u) == 0u && page.asked == 3u, "a click beside the legend opened the page");
    require(entry.click(240.0f, 250.0f) && sample(entry, world, 0u) == 0u && page.asked == 4u && page.frame == chili::PageFrame::Pause, "a click on the legend did not ask for the page");
    require(sample(entry, world, 0u) == 0u && page.asked == 4u && painted.back() == Entry::Legend::Shown, "one click asked twice, or the legend did not come back");
    page.asked = 3u;
    require(sample(entry, world, Entry::triangle_button) == 0u && page.asked == 4u && page.frame == chili::PageFrame::Pause, "Triangle did not ask for the page");
    require(painted.size() == 4u && painted.back() == Entry::Legend::Hidden && painted[2] == Entry::Legend::Shown, "the legend stayed when the page was asked for");
    require(sample(entry, world, Entry::triangle_button) == 0u && page.asked == 4u, "the held Triangle reached the game or asked again");
    // The page opens: the picture is the page's, the pad too; what opened it stays away from the game.
    page.open = true;
    require(sample(entry, world, Entry::triangle_button | Entry::cross_button) == Entry::cross_button && page.asked == 4u, "the entry worked while the page was open");
    require(painted.back() == Entry::Legend::PageOwns, "the painter was not told that the page has the picture");
    world.memory.store32(world.block(plain) + 0x4Cu, 9001u);
    require(sample(entry, world, 0u) == 0u && page.asked == 4u, "a sample while the page was open");
    page.open = false;
    // Closed again: the window is still up, the legend comes back at once.
    (void)sample(entry, world, 0u);
    require(painted.back() == Entry::Legend::Shown, "the legend did not come back after the page");
    // Play goes on: no window, no legend.
    world.open({});
    (void)sample(entry, world, 0u);
    require(painted.back() == Entry::Legend::Hidden && sample(entry, world, Entry::triangle_button) == Entry::triangle_button && page.asked == 4u, "the legend or its button outside the pause menu");
    require(!entry.click(240.0f, 250.0f) && sample(entry, world, 0u) == 0u && page.asked == 4u, "a click where the legend was, with no legend up");
}

void hook_route() {
    World world;
    Entry entry({false, true});
    entry.set_log(nullptr);
    page = {};
    const auto menues = world.node("Menues", 0u, false);
    std::vector<std::uint32_t> lines;
    const auto main = world.window("Menu_Main", menues, {"Donkey", "Fiesta"}, &lines);
    const auto outline = world.node("- outline", lines[1], false), shadow = world.node("- dropshadow", lines[1], false);
    std::vector<std::uint32_t> others;
    (void)world.window("Menu_Multiplay", menues, {"Fiesta"}, &others);
    const auto twin = others[0];
    (void)main;
    std::uint32_t index = 13u;
    require(entry.label(world.memory, lines[1], index) && index == 119u, "the line's label");
    index = 13u;
    require(entry.label(world.memory, outline, index) && index == 119u, "the outline box's label");
    index = 13u;
    require(entry.label(world.memory, shadow, index) && index == 119u && entry.labels() == 3u, "the shadow box's label");
    index = 13u;
    require(!entry.label(world.memory, twin, index) && index == 13u, "a line of the same name in another window was relabelled");
    index = 13u;
    require(!entry.label(world.memory, lines[0], index) && !entry.label(world.memory, 0u, index) && index == 13u, "another box was relabelled");
    index = 11u;
    require(!entry.label(world.memory, lines[1], index) && index == 11u, "another text of the line was changed");

    const auto arguments = world.reserve(16u);
    const auto event = [&](std::uint32_t self, std::uint32_t button, std::uint32_t state) {
        world.memory.store32(arguments, button); world.memory.store32(arguments + 4u, state);
        return entry.choice(world.memory, self, arguments);
    };
    require(!event(lines[1], 5u, 0u) && !event(lines[1], 11u, 1u) && !event(lines[0], 5u, 1u) && !event(twin, 5u, 1u) && page.asked == 0u, "an event that is not the choice was taken");
    require(event(lines[1], 5u, 1u) && page.asked == 1u && page.frame == chili::PageFrame::MainMenu && world.memory.load32(arguments) == 99u, "the choice of the line");
    // While the page is open the line is still kept from the game, and the page is not asked for again.
    page.open = true;
    require(event(lines[1], 5u, 1u) && page.asked == 1u && world.memory.load32(arguments) == 99u, "the choice while the page is open");
    page.open = false;
}

void routes_and_switch() {
    const auto none = Entry::route(""), data = Entry::route("data"), hook = Entry::route("hook"), both = Entry::route("legend,hook"), odd = Entry::route("hook,sideways");
    require(!none.hook && !none.legend && none.problem.empty() && !data.hook && !data.legend && data.problem.empty(), "the default route");
    require(hook.hook && !hook.legend && both.hook && both.legend && both.problem.empty(), "the named routes");
    require(odd.hook && !odd.legend && odd.problem.find("sideways") != std::string::npos, "an unknown route word");
    require(Entry::route("sync").sync && !Entry::route("sync").hook && !none.sync && !both.sync, "the route word that leaves Sync Highscores on");
    _putenv_s("CHILI_PAGE", "");
    require(chili::settings_entry_enabled(false) && !chili::settings_entry_enabled(true), "a scripted run has the entries without being asked, or a player's has none");
    _putenv_s("CHILI_PAGE", "1");
    require(chili::settings_entry_enabled(true), "CHILI_PAGE=1 does not give a scripted run the entries");
    _putenv_s("CHILI_PAGE", "0");
    require(!chili::settings_entry_enabled(true) && chili::settings_entry_enabled(false), "CHILI_PAGE=0");
    _putenv_s("CHILI_PAGE", "");
    _putenv_s("CHILI_PAGE_ENTRY", "legend");
    require(Entry::route_from_environment().legend && !Entry::route_from_environment().hook, "CHILI_PAGE_ENTRY from the environment");
    _putenv_s("CHILI_PAGE_ENTRY", "");
    require(!Entry::route_from_environment().legend, "CHILI_PAGE_ENTRY unset");
}

void legend_picture() {
    // A font of the letters the word needs and the Triangle picture, in the game's proportions.
    chili::PageFont font;
    std::vector<chili::PageGlyph> glyphs;
    for (const char c : std::string("Setings")) {
        chili::PageGlyph glyph;
        glyph.code = static_cast<std::uint16_t>(c); glyph.x_offset = 1.0f; glyph.y_offset = 9.0f; glyph.advance = 16.0f; glyph.u = 0.1f; glyph.v = 0.1f; glyph.u_width = 14.0f / 256.0f;
        glyphs.push_back(glyph);
    }
    chili::PageGlyph picture;
    picture.code = static_cast<std::uint16_t>(chili::page_glyph::triangle); picture.y_offset = 9.0f; picture.advance = 29.0f; picture.u = 0.5f; picture.v = 0.5f; picture.u_width = 24.0f / 256.0f; picture.picture = true;
    glyphs.push_back(picture);
    font.assign(glyphs, 30.0f);
    chili::PageDraw draw;
    const auto box = chili::build_pause_legend(draw, font);
    // The picture's two copies and three copies of eight letters.
    require(draw.quads() == 2u + 3u * 8u, "the legend's rectangles");
    require(box.width() > 60.0f && box.width() < 110.0f && std::abs((box.x0 + box.x1) * 0.5f - chili::pause_legend_centre) <= 1.0f, "the legend is not in the middle under the box");
    // Under the pause box (it ends at y 234.5), within its width (111 to 383) and on the screen.
    require(box.x0 > 111.0f && box.x1 < 383.0f && box.y0 >= 236.0f && box.y1 <= 268.0f, "the legend is not in the rows under the pause box");
    const auto list = draw.finish();
    require(!list->runs.empty() && list->runs.front().texture == chili::page_texture::font_outline, "the legend's first rectangle is not the picture's outline twin");
    for (const auto &vertex : list->vertices) require(vertex.x >= box.x0 - 1.0f && vertex.x <= box.x1 + 3.0f && vertex.y >= 236.0f && vertex.y <= 270.0f, "a corner of the legend lies outside its rows");
    // A font without the picture: no legend at all rather than a word without its button.
    chili::PageFont bare;
    glyphs.pop_back();
    bare.assign(glyphs, 30.0f);
    chili::PageDraw none;
    require(chili::build_pause_legend(none, bare).width() == 0.0f && none.empty(), "a legend without its button");
}
}

// The page, as far as the entries need one.
namespace chili {
void page_request_open(PageFrame frame) { ++page.asked; page.frame = frame; }
bool page_is_open() { return page.open; }
}

void settings_entry_tests(unsigned &checks) {
    counter = &checks;
    main_menu();
    profile_options();
    pause_menu();
    hook_route();
    routes_and_switch();
    legend_picture();
}
