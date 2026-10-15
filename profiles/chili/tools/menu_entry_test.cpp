// The "Settings" entries in the game's own menus (host/menu_entry_blocks.hpp, host/settings_entry.hpp).
//
// Without the game: fragments and blocks made here in the game's layout (the main menu's Fiesta line turned
// in place; the pause line added to the kinds of pause window the host knows and to no other; a block edited
// through a reader that is given only its head and its scene-mode fragments), and the part that runs with
// the game on a made-up guest memory (settings_entry.hpp: the window stack read, the main menu's command
// polled, the pause line's choice taken from the pad sample and kept from the game, Sync Highscores switched
// off, the Triangle legend's button, the hidden door).
//
// With the game's disc folder as second argument: every block of the real archive is edited and read back.
//
//   chili_menu_entry_test [disc0 folder]
//   chili_menu_entry_test --write <disc0 folder> <block name> <out file>     the block as the game is served it
#include "../host/menu_entry_blocks.hpp"
#include "../host/vfs.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace {
namespace props = chili::props;
namespace entry = chili::menu_entry;
unsigned checks = 0u;
void require(bool value, const std::string &message) { ++checks; if (!value) throw std::runtime_error(message); }

// ---- Builders for a block in the game's layout (block_props.hpp) ------------------------------------------
void put(std::vector<std::uint8_t> &out, std::uint32_t value) { for (unsigned i = 0; i < 4u; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8u * i))); }
std::uint32_t bits(float value) { std::uint32_t out; std::memcpy(&out, &value, 4u); return out; }
float number(std::span<const std::uint8_t> data, std::size_t at) { const auto value = props::word(data, at); float out; std::memcpy(&out, &value, 4u); return out; }
std::vector<std::uint8_t> pattern(std::size_t size, unsigned seed) {
    std::vector<std::uint8_t> out(size);
    for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<std::uint8_t>((i * 131u + seed * 17u + (i >> 8u)) & 0xFFu);
    return out;
}
std::vector<std::uint32_t> text_words(const std::string &value) {
    const std::size_t words = value.size() / 4u + 1u;
    std::vector<std::uint32_t> out(1u + words);
    out[0] = static_cast<std::uint32_t>(words);
    for (std::size_t i = 0; i < value.size(); ++i) out[1u + i / 4u] |= std::uint32_t{static_cast<std::uint8_t>(value[i])} << (8u * (i % 4u));
    return out;
}
struct Records {
    std::vector<std::uint8_t> bytes;
    void create(std::uint32_t entity, const std::string &type) {
        put(bytes, entity); put(bytes, 0xFF000000u);
        for (const auto value : text_words(type)) put(bytes, value);
    }
    void set(std::uint32_t entity, std::uint32_t index, const std::vector<std::uint32_t> &value) {
        put(bytes, entity); put(bytes, index);
        for (const auto item : value) put(bytes, item);
    }
};
std::vector<std::uint8_t> make_fragment(const std::vector<Records> &chunks) {
    std::vector<std::uint8_t> out;
    put(out, 24u); put(out, 0u); put(out, 0x80000000u); put(out, 0x2800u); put(out, 0u); put(out, 0u);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const auto size = chunks[i].bytes.size();
        put(out, i + 1u == chunks.size() ? 0x80000000u : static_cast<std::uint32_t>(12u + size));
        put(out, static_cast<std::uint32_t>(size)); put(out, 4u);
        out.insert(out.end(), chunks[i].bytes.begin(), chunks[i].bytes.end());
    }
    return out;
}
struct BlockAsset { std::uint32_t type; std::string name; std::vector<std::uint8_t> payload; };
std::vector<std::uint8_t> make_block(const std::vector<BlockAsset> &assets, std::vector<std::size_t> &payloads, std::size_t stream_size = 40000u) {
    std::vector<std::uint8_t> out = pattern(props::map_base, 55u);
    const auto stream = pattern(stream_size, 56u);
    const auto count = static_cast<std::uint32_t>(assets.size());
    put(out, 0x0DA24EFAu); put(out, 0x5E6233D4u); put(out, 0x27403369u); put(out, 1u);
    std::vector<std::uint8_t> info;
    std::size_t largest = 0u;
    for (const auto &asset : assets) {
        const auto start = info.size();
        put(info, asset.type); put(info, 44u); put(info, 0u); put(info, 0x6D696E61u); put(info, 0u); put(info, 0u); put(info, 0x69730000u); put(info, 24u);
        put(info, 0u); put(info, 0xFFFFFFDCu); put(info, 0u); put(info, 0u);
        info.insert(info.end(), asset.name.begin(), asset.name.end());
        info.resize(start + std::max<std::size_t>(128u, (asset.name.size() + 48u + 4u + 15u) & ~std::size_t{15}));
        largest = std::max(largest, asset.payload.size());
    }
    put(out, count); put(out, static_cast<std::uint32_t>(info.size())); put(out, static_cast<std::uint32_t>(largest));
    out.push_back(1u); put(out, 4096u); put(out, 4096u + static_cast<std::uint32_t>(stream.size()));
    out.insert(out.end(), stream.begin(), stream.end());
    put(out, 0x80000001u); put(out, count - 1u);
    out.insert(out.end(), info.begin(), info.end());
    for (const auto &asset : assets) put(out, static_cast<std::uint32_t>(asset.payload.size()));
    for (const auto &asset : assets) { payloads.push_back(out.size()); out.insert(out.end(), asset.payload.begin(), asset.payload.end()); }
    return out;
}
std::vector<props::Property> database() {
    // MenuCommand twice: the game's database declares a name once per script that has it.
    return {{"Speed", "number"}, {"MenuCommand", "integer"}, {"m_tIsActive", "truth"}, {"Action", "integer"}, {"TriggerButton", "integer"},
            {"MenuWindowName", "string"}, {"MenuCommand", "integer"}, {"Trigger_On_Show", "truth"}};
}
constexpr std::uint32_t base = props::builtin_properties;
constexpr std::uint32_t menu_command = base + 1u, is_active = base + 2u, action = base + 3u, trigger_button = base + 4u, window_name = base + 5u,
    listener_command = base + 6u, on_show = base + 7u;

// A scene tree written as the game's fragments are: all create records in a chunk of their own, then the values.
struct Scene {
    Records creates, values, late;
    std::uint32_t next{0x10000000u};
    std::uint32_t node(const std::string &type, const std::string &name, std::uint32_t parent) {
        const auto id = next++;
        creates.create(id, type);
        values.set(id, entry::parent_property, {parent});
        if (!name.empty()) values.set(id, props::name_property, text_words(name));
        return id;
    }
    void place(std::uint32_t id, float x, float y) { values.set(id, entry::position_property, {bits(x), bits(y), bits(0.f)}); }
    // A menu line as the game has them: the line, its two boxes, its two animations, optionally an action.
    std::uint32_t line(std::uint32_t window, const std::string &name, float x, float y, std::uint32_t command, std::uint32_t text, int with_action = -1) {
        const auto id = node("MenuWidgetButton(TextBox)", name, window);
        place(id, x, y);
        values.set(id, entry::text_index_property, {text});
        values.set(id, menu_command, {command});
        values.set(id, is_active, {1u});
        const auto outline = node("TextBox", "- outline", id);
        place(outline, 0.f, 0.f);
        values.set(outline, entry::text_index_property, {text});
        const auto shadow = node("TextBox", "- dropshadow", id);
        place(shadow, 2.f, 2.f);
        values.set(shadow, entry::text_index_property, {text});
        for (int i = 0; i < 2; ++i) {
            const auto animation = node("MenuAnimation(NodeLogical)", "", id);
            late.set(animation, on_show, {static_cast<std::uint32_t>(i)});
            late.set(animation, base + 0u, {bits(0.1f)});
        }
        if (with_action >= 0) {
            const auto act = node("Action_MenuWindowStack(NodeLogical)", "", id);
            values.set(act, action, {static_cast<std::uint32_t>(with_action)});
            values.set(act, window_name, text_words("Menu_" + name));
        }
        return id;
    }
    std::uint32_t listener(std::uint32_t window, const std::string &name, std::uint32_t button, std::uint32_t command) {
        const auto id = node("MenuButtonListener(NodeLogical)", name, window);
        values.set(id, trigger_button, {button});
        values.set(id, listener_command, {command});
        return id;
    }
    std::vector<std::uint8_t> fragment() const { return make_fragment({creates, values, late}); }
};
struct Pause { std::uint32_t window{}; std::vector<std::uint32_t> lines; };
Pause pause_window(Scene &scene, std::uint32_t root, const std::string &mode, int kind, const std::string &logic_name = "Menu_IngameMain") {
    Pause out;
    const auto mode_node = scene.node("Folder", mode, root);
    const auto logic = scene.node("MenuWindow_IngameMain(NodeLogical)", logic_name, mode_node);
    out.window = scene.node("MenuWindow(Sprite)", "SubMenu_IngameMain", logic);
    const auto headline = scene.node("TextBox", "Headline", out.window);
    scene.place(headline, 110.f, 28.f);
    scene.values.set(headline, entry::text_index_property, {66u});
    if (kind == 4) {
        out.lines = {scene.line(out.window, "Resume", 110.f, 119.f, 1u, 232u), scene.line(out.window, "Rewind", 110.f, 136.f, 400u, 67u),
                     scene.line(out.window, "Restart", 110.f, 153.f, 201u, 68u), scene.line(out.window, "Quit", 110.f, 170.f, 200u, 69u)};
    } else if (kind == 2) {
        out.lines = {scene.line(out.window, "Resume", 110.f, 129.f, 1u, 232u), scene.line(out.window, "Quit", 110.f, 150.f, 200u, 69u)};
    } else if (kind == 6) {
        out.lines = {scene.line(out.window, "Resume", 110.f, 75.f, 1u, 232u), scene.line(out.window, "Objectives", 110.f, 92.f, 205u, 70u),
                     scene.line(out.window, "Rewind", 110.f, 109.f, 400u, 67u), scene.line(out.window, "Restart", 110.f, 126.f, 201u, 68u),
                     scene.line(out.window, "Skip Challenge", 110.f, 143.f, 206u, 71u), scene.line(out.window, "Quit", 110.f, 160.f, 200u, 15u)};
    } else {   // a story window whose Rewind has another number
        out.lines = {scene.line(out.window, "Resume", 110.f, 119.f, 1u, 232u), scene.line(out.window, "Rewind", 110.f, 136.f, 401u, 67u),
                     scene.line(out.window, "Restart", 110.f, 153.f, 201u, 68u), scene.line(out.window, "Quit", 110.f, 170.f, 200u, 69u)};
    }
    (void)scene.listener(out.window, "Back", 13u, 1u);
    (void)scene.listener(out.window, "Back (START button)", 6u, 1u);
    return out;
}

// What a fragment says of a node, read back with the mod layer's own reader.
struct Read {
    props::Fragment fragment;
    std::span<const std::uint8_t> bytes;
    Read(std::span<const std::uint8_t> data, const std::vector<props::Property> &declared) : fragment(props::read_fragment(data, 0u, data.size(), declared)), bytes(data) {}
    const props::Value *value(std::uint32_t entity, std::uint32_t index) const {
        for (const auto &item : fragment.values) if (item.entity == entity && item.index == index) return &item;
        return nullptr;
    }
    std::uint32_t word(std::uint32_t entity, std::uint32_t index) const { const auto *found = value(entity, index); return found ? props::word(bytes, found->offset) : 0xDEADBEEFu; }
    float y(std::uint32_t entity) const { const auto *found = value(entity, entry::position_property); return found ? number(bytes, found->offset + 4u) : -1.f; }
    std::uint32_t named(const std::string &name, std::uint32_t parent) const {
        for (const auto entity : fragment.entities) {
            const auto found = fragment.names.find(entity);
            if (found != fragment.names.end() && found->second == name && word(entity, entry::parent_property) == parent) return entity;
        }
        return 0u;
    }
    std::vector<std::uint32_t> children(std::uint32_t parent) const {
        std::vector<std::uint32_t> out;
        for (const auto entity : fragment.entities) if (word(entity, entry::parent_property) == parent) out.push_back(entity);
        return out;
    }
};
std::size_t differing_words(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b, std::size_t count) {
    std::size_t out = 0u;
    for (std::size_t at = 0; at + 4u <= count; at += 4u) out += props::word(a, at) != props::word(b, at) ? 1u : 0u;
    return out;
}

// ---- The main menu's line ----------------------------------------------------------------------------------
void main_menu_line() {
    const auto declared = database();
    Scene scene;
    const auto root = scene.node("Folder", "Menues", 0u);
    const auto window = scene.node("MenuWindow(Sprite)", "Menu_Main", root);
    const auto gringo = scene.line(window, "Gringo", 195.f, 87.f, 800u, 7u, 0);
    const auto donkey = scene.line(window, "Donkey", 154.f, 174.f, 500u, 11u, 0);
    const auto fiesta = scene.line(window, "Fiesta", 141.f, 199.f, 302u, 13u, 0);
    // The same name under another window, and a box with the same text elsewhere: neither is the line.
    const auto other_window = scene.node("MenuWindow(Sprite)", "Menu_Multiplay", root);
    const auto twin = scene.line(other_window, "Fiesta", 10.f, 10.f, 302u, 13u, 0);
    const auto heading = scene.node("TextBox", "Heading", window);
    scene.values.set(heading, entry::text_index_property, {13u});
    const auto original = scene.fragment();

    auto bytes = original;
    bool changed = false;
    std::string problem;
    require(entry::settings_for_fiesta(bytes, declared, changed, problem) && changed && problem.empty(), "the Fiesta line was not turned: " + problem);
    require(bytes.size() == original.size() && differing_words(bytes, original, bytes.size()) == 5u, "the main menu's fragment changed in other than five words");
    const Read after(bytes, declared);
    const auto boxes = after.children(fiesta);
    require(boxes.size() == 5u, "the line's children");
    require(after.word(fiesta, entry::text_index_property) == 119u && after.word(boxes[0], entry::text_index_property) == 119u &&
            after.word(boxes[1], entry::text_index_property) == 119u, "the line and its two boxes do not show string 119");
    require(after.word(fiesta, menu_command) == 9001u && after.word(boxes[4], action) == 3u, "the line's command and action");
    require(after.word(twin, entry::text_index_property) == 13u && after.word(twin, menu_command) == 302u && after.word(heading, entry::text_index_property) == 13u &&
            after.word(gringo, menu_command) == 800u && after.word(donkey, entry::text_index_property) == 11u, "another node changed");
    // Again: nothing to do, and that is success.
    auto again = bytes;
    require(entry::settings_for_fiesta(again, declared, changed, problem) && !changed && again == bytes, "a second pass changed the fragment");
    // A line that is not the game's own is left alone.
    for (const auto &[index, entity, value] : {std::tuple{menu_command, fiesta, 303u}, std::tuple{entry::text_index_property, fiesta, 14u}}) {
        auto odd = original;
        const Read reader(odd, declared);
        props::store(odd, reader.value(entity, index)->offset, value);
        const auto before = odd;
        require(!entry::settings_for_fiesta(odd, declared, changed, problem) && !changed && odd == before && !problem.empty(), "a changed Fiesta line was turned");
    }
    Scene bare;
    (void)bare.line(bare.node("MenuWindow(Sprite)", "Menu_Main", 0u), "Donkey", 1.f, 2.f, 500u, 11u, 0);
    auto none = bare.fragment();
    require(!entry::settings_for_fiesta(none, declared, changed, problem) && problem.find("no line Fiesta") != std::string::npos, "a menu without the line");
    auto cut = original;
    cut.resize(40u);
    require(!entry::settings_for_fiesta(cut, declared, changed, problem) && !changed, "a cut fragment was turned");
}

// ---- The pause line ----------------------------------------------------------------------------------------
void check_new_line(const std::vector<std::uint8_t> &bytes, const std::vector<std::uint8_t> &original, const Pause &pause, float y, bool moved, const std::string &what) {
    const auto declared = database();
    const Read before(original, declared), after(bytes, declared);
    const auto line = after.named("settings", pause.window);
    require(line != 0u && line > *std::max_element(before.fragment.entities.begin(), before.fragment.entities.end()), what + ": no new line behind the fragment's last node");
    require(after.y(line) == y && number(bytes, after.value(line, entry::position_property)->offset) == 110.f, what + ": the new line's place");
    require(after.word(line, menu_command) == 0xFFFFFFFFu && after.word(line, entry::text_index_property) == 119u && after.word(line, is_active) == 1u, what + ": the new line's command, text or state");
    const auto below = after.children(line);
    require(below.size() == 4u, what + ": the new line has " + std::to_string(below.size()) + " nodes below it, not four");
    require(after.fragment.names.at(below[0]) == "- outline" && after.fragment.names.at(below[1]) == "- dropshadow" &&
            after.word(below[0], entry::text_index_property) == 119u && after.word(below[1], entry::text_index_property) == 119u && after.y(below[1]) == 2.f, what + ": the new line's two boxes");
    require(after.word(below[2], on_show) == 0u && after.word(below[3], on_show) == 1u && after.word(below[3], base + 0u) == bits(0.1f), what + ": the new line's animations");
    for (std::size_t i = 0; i < pause.lines.size(); ++i)
        require(after.y(pause.lines[i]) == before.y(pause.lines[i]) + (moved ? -12.f : 0.f), what + ": line " + std::to_string(i) + " is not where it should be");
    // The lines register in the order of the fragment: the new one is the last.
    std::vector<std::uint32_t> order;
    for (const auto entity : after.children(pause.window)) if (after.value(entity, menu_command)) order.push_back(entity);
    require(order.size() == pause.lines.size() + 1u && order.back() == line, what + ": the new line is not the window's last line");
}
void pause_lines() {
    const auto declared = database();
    // A story mission's fragment.
    {
        Scene scene;
        const auto root = scene.node("Folder", "SceneMode", 0u);
        const auto pause = pause_window(scene, root, "GringoMode", 4);
        const auto original = scene.fragment();
        auto bytes = original;
        const auto result = entry::add_pause_line(bytes, declared);
        require(result.changed && result.story == 1u && result.hangman == 0u && result.other == 0u && result.problem.empty(), "story: " + result.problem);
        const Read before(original, declared), after(bytes, declared);
        require(after.fragment.entities.size() == before.fragment.entities.size() + 5u, "story: not five new nodes");
        check_new_line(bytes, original, pause, 175.f, true, "story");
        // Up to the old end the fragment differs in the four positions and in the last chunk's link to the new one.
        require(bytes.size() > original.size() && differing_words(bytes, original, original.size()) == 5u, "story: the old part of the fragment changed in other than five words");
        const auto chunks = entry::detail::chunks(bytes);
        require(chunks.size() == 4u && chunks[3].header == original.size() && chunks[3].stop == bytes.size() && props::word(bytes, chunks[3].header) == 0u &&
                props::word(bytes, chunks[3].header + 8u) == 4u && props::word(bytes, chunks[2].header) == original.size() - chunks[2].header, "story: the new chunk and its link");
        require(after.word(after.named("back", pause.window), listener_command) == 1u && after.word(after.named("headline", pause.window), entry::text_index_property) == 66u, "story: a listener or the headline changed");
        // A fragment that has the line is left as it is.
        auto again = bytes;
        const auto second = entry::add_pause_line(again, declared);
        require(!second.changed && second.already == 1u && second.story == 0u && again == bytes, "story: the line was added twice");
    }
    // An El Macho block's fragment: two windows of the same name, one of them with six lines.
    {
        Scene scene;
        const auto root = scene.node("Folder", "SceneMode", 0u);
        const auto challenge = pause_window(scene, root, "MachoChallange", 6);
        const auto macho = pause_window(scene, root, "MachoMode", 4);
        const auto original = scene.fragment();
        auto bytes = original;
        const auto result = entry::add_pause_line(bytes, declared);
        require(result.changed && result.story == 1u && result.other == 1u, "el macho: " + result.problem);
        check_new_line(bytes, original, macho, 175.f, true, "el macho");
        const Read before(original, declared), after(bytes, declared);
        require(after.named("settings", challenge.window) == 0u && after.children(challenge.window).size() == before.children(challenge.window).size(), "el macho: the challenge window got a line");
        for (const auto line : challenge.lines) require(after.y(line) == before.y(line), "el macho: a line of the challenge window moved");
    }
    // Hangman: two lines, nothing moves.
    {
        Scene scene;
        const auto root = scene.node("Folder", "SceneMode", 0u);
        const auto pause = pause_window(scene, root, "DonkeyMode", 2);
        const auto original = scene.fragment();
        auto bytes = original;
        const auto result = entry::add_pause_line(bytes, declared);
        require(result.changed && result.hangman == 1u && result.story == 0u, "hangman: " + result.problem);
        check_new_line(bytes, original, pause, 171.f, false, "hangman");
        require(differing_words(bytes, original, original.size()) == 1u, "hangman: the old part of the fragment changed in other than the chunk link");
    }
    // Windows the host does not know keep their bytes: six lines, a story window with another command, the
    // four lines under a mode that is not a story's, two lines under a story's mode, another logic node.
    for (const auto &[mode, kind, logic, counted] : {std::tuple{"StyleChallangeMode", 6, "Menu_IngameMain", 1u}, std::tuple{"GringoMode", 0, "Menu_IngameMain", 1u},
             std::tuple{"FiestaMode", 4, "Menu_IngameMain", 1u}, std::tuple{"GringoMode", 2, "Menu_IngameMain", 1u}, std::tuple{"GringoMode", 4, "Menu_Other", 0u}}) {
        Scene scene;
        (void)pause_window(scene, scene.node("Folder", "SceneMode", 0u), mode, kind, logic);
        const auto original = scene.fragment();
        auto bytes = original;
        const auto result = entry::add_pause_line(bytes, declared);
        require(!result.changed && result.other == counted && result.story + result.hangman == 0u && bytes == original && result.problem.empty(), std::string("a window that is not known got a line: ") + mode);
    }
    // A fragment whose last chunk does not end it is not grown; one that cannot be read at all neither.
    {
        Scene scene;
        (void)pause_window(scene, scene.node("Folder", "SceneMode", 0u), "GringoMode", 4);
        auto odd = scene.fragment();
        const auto chunks = entry::detail::chunks(odd);
        // Two more words behind the last chunk that its length does not count.
        odd.resize(odd.size() + 8u);
        const auto before = odd;
        auto result = entry::add_pause_line(odd, declared);
        require(!result.changed && odd == before && result.problem.find("last chunk") != std::string::npos, "a fragment with bytes behind its last chunk was grown");
        auto cut = scene.fragment();
        cut.resize(chunks[1].first + 6u);
        const auto cut_before = cut;
        result = entry::add_pause_line(cut, declared);
        require(!result.changed && cut == cut_before, "a cut fragment was grown");
    }
}

// ---- Whole blocks ------------------------------------------------------------------------------------------
struct Counted {
    std::span<const std::uint8_t> block;
    std::size_t reads{}, bytes{}, highest{};
    entry::Read reader() {
        return [this](std::uint64_t offset, std::span<std::uint8_t> out) -> std::size_t {
            if (offset >= block.size()) return 0u;
            const auto count = std::min<std::size_t>(out.size(), block.size() - static_cast<std::size_t>(offset));
            std::memcpy(out.data(), block.data() + offset, count);
            ++reads; bytes += count; highest = std::max(highest, static_cast<std::size_t>(offset) + count);
            return count;
        };
    }
};
void whole_blocks() {
    const auto declared = database();
    Scene story, style, popup, menu;
    const auto pause = pause_window(story, story.node("Folder", "SceneMode", 0u), "GringoMode", 4);
    (void)pause_window(style, style.node("Folder", "SceneMode", 0u), "StyleChallangeMode", 6);
    (void)popup.node("Sprite", "Box", 0u);
    const auto fiesta = menu.line(menu.node("MenuWindow(Sprite)", "Menu_Main", 0u), "Fiesta", 141.f, 199.f, 302u, 13u, 0);
    const auto story_fragment = story.fragment(), style_fragment = style.fragment(), popup_fragment = popup.fragment(), menu_fragment = menu.fragment();
    // A mission block: the pause fragment in the middle, a large asset in front of it that nobody needs to read.
    {
        const std::vector<BlockAsset> assets{{0u, "/data/textures/a.bmp", pattern(300000u, 60u)}, {4u, "/data/Scripts/fragments/SceneMode/IngameMenu_PopUp.fragment", popup_fragment},
            {4u, "/data/Scripts/fragments/SceneMode/SceneMode_Gringo.fragment", story_fragment}, {3u, "/data/models/b.model", pattern(7777u, 61u)},
            {4u, "/data/Scripts/fragments/SceneMode/SceneMode_StyleChallange.fragment", style_fragment}, {0u, "/data/textures/c.bmp", pattern(90000u, 62u)}};
        std::vector<std::size_t> payloads;
        const auto block = make_block(assets, payloads);
        Counted counted{block};
        const auto result = entry::edit_block(counted.reader(), block.size(), declared, entry::What::PauseLine);
        require(result.problem.empty() && result.story == 1u && result.other == 1u && result.hangman == 0u && result.edits.size() == 2u, "mission block: " + result.problem);
        require(result.read_bytes == counted.bytes && counted.bytes < block.size() - 380000u, "mission block: more than the head and the scene-mode fragments was read");
        const auto served = entry::apply(block, result.edits);
        const auto grown = served.size() - block.size();
        require(grown > 0u && served.size() == result.size_after(block.size()), "mission block: the size with the edits");
        const auto listed = props::read_block(served, true), before = props::read_block(block, true);
        require(listed.size() == 6u && listed[2].size == story_fragment.size() + grown && listed[2].offset == before[2].offset, "mission block: the fragment's size in the table");
        for (const std::size_t i : {0u, 1u, 3u, 4u, 5u}) {
            require(listed[i].size == before[i].size && listed[i].offset == before[i].offset + (i > 2u ? grown : 0u) &&
                    std::equal(served.begin() + static_cast<std::ptrdiff_t>(listed[i].offset), served.begin() + static_cast<std::ptrdiff_t>(listed[i].offset + listed[i].size),
                               block.begin() + static_cast<std::ptrdiff_t>(before[i].offset)), "mission block: asset " + std::to_string(i) + " is not what it was");
        }
        // The head differs in one word: the size of the fragment (the largest asset is still the picture).
        require(differing_words(served, block, before[0].offset) == 1u && props::word(served, props::map_base + 24u) == 300000u, "mission block: the head changed in other than the fragment's size");
        const std::vector<std::uint8_t> fragment(served.begin() + static_cast<std::ptrdiff_t>(listed[2].offset), served.begin() + static_cast<std::ptrdiff_t>(listed[2].offset + listed[2].size));
        check_new_line(fragment, story_fragment, pause, 175.f, true, "mission block");
        // Served again through the same code: it has its line, nothing more to do.
        Counted second{served};
        const auto again = entry::edit_block(second.reader(), served.size(), declared, entry::What::PauseLine);
        require(again.edits.empty() && again.already == 1u && again.other == 1u && again.problem.empty(), "mission block: edited twice");
    }
    // A block whose pause fragment is its largest asset: the header's "largest asset" grows with it.
    {
        const std::vector<BlockAsset> assets{{4u, "/data/Scripts/fragments/SceneMode/SceneMode_Donkey.fragment", {}}, {0u, "/data/textures/a.bmp", pattern(100u, 63u)}};
        Scene hangman;
        (void)pause_window(hangman, hangman.node("Folder", "SceneMode", 0u), "DonkeyMode", 2);
        auto with = assets;
        with[0].payload = hangman.fragment();
        std::vector<std::size_t> payloads;
        const auto block = make_block(with, payloads);
        Counted counted{block};
        const auto result = entry::edit_block(counted.reader(), block.size(), declared, entry::What::PauseLine);
        const auto served = entry::apply(block, result.edits);
        require(result.hangman == 1u && result.edits.size() == 3u && props::word(served, props::map_base + 24u) == with[0].payload.size() + (served.size() - block.size()) &&
                props::read_block(served, true)[0].size == props::word(served, props::map_base + 24u), "a block whose largest asset grew");
    }
    // The main menu's block: one range of the same size.
    {
        const std::vector<BlockAsset> assets{{0u, "/data/textures/a.bmp", pattern(5000u, 64u)}, {4u, "/data/Scripts/fragments/SceneMode/SceneMode_MainMenu.fragment", menu_fragment},
            {4u, "/data/Scripts/fragments/SceneMode/SceneMode_Gringo.fragment", story_fragment}};
        std::vector<std::size_t> payloads;
        const auto block = make_block(assets, payloads);
        Counted counted{block};
        const auto result = entry::edit_block(counted.reader(), block.size(), declared, entry::What::MainMenu);
        require(result.main_menu && result.problem.empty() && result.edits.size() == 1u && result.edits[0].offset == payloads[1] && result.edits[0].bytes.size() == menu_fragment.size(), "main menu block: " + result.problem);
        const auto served = entry::apply(block, result.edits);
        require(served.size() == block.size() && differing_words(served, block, block.size()) == 5u, "main menu block: not five words");
        const Read after(std::span(served).subspan(payloads[1], menu_fragment.size()), declared);
        require(after.word(fiesta, menu_command) == 9001u, "main menu block: the command");
        Counted second{served};
        const auto again = entry::edit_block(second.reader(), served.size(), declared, entry::What::MainMenu);
        require(again.main_menu && again.edits.empty() && again.problem.empty(), "main menu block: a block that has the entry");
        // A mission block has no main menu: said, and nothing changes.
        std::vector<std::size_t> other_payloads;
        const auto mission = make_block({{4u, "/data/Scripts/fragments/SceneMode/SceneMode_Gringo.fragment", story_fragment}, {0u, "/data/textures/a.bmp", pattern(64u, 65u)}}, other_payloads);
        Counted third{mission};
        const auto none = entry::edit_block(third.reader(), mission.size(), declared, entry::What::MainMenu);
        require(!none.main_menu && none.edits.empty() && !none.problem.empty(), "a block without the main menu");
    }
    // Not a block, a block cut short, a reader that cannot read: no edits, a reason.
    {
        const auto noise = pattern(50000u, 70u);
        Counted counted{noise};
        const auto result = entry::edit_block(counted.reader(), noise.size(), declared, entry::What::PauseLine);
        require(result.edits.empty() && !result.problem.empty(), "noise was edited");
        std::vector<std::size_t> payloads;
        auto block = make_block({{4u, "/data/Scripts/fragments/SceneMode/SceneMode_Gringo.fragment", story_fragment}, {0u, "/data/textures/a.bmp", pattern(64u, 65u)}}, payloads);
        block.resize(block.size() - 30u);
        Counted cut{block};
        require(entry::edit_block(cut.reader(), block.size(), declared, entry::What::PauseLine).edits.empty(), "a cut block was edited");
        const auto dead = entry::edit_block([](std::uint64_t, std::span<std::uint8_t>) -> std::size_t { return 0u; }, 100000u, declared, entry::What::PauseLine);
        require(dead.edits.empty() && !dead.problem.empty(), "a block that cannot be read");
    }
}

// ---- The game's own archive --------------------------------------------------------------------------------
struct Archive {
    std::shared_ptr<chili::vfs::HostFileSource> source;
    chili::naz::Directory directory;
    std::vector<props::Property> declared;
    explicit Archive(const std::filesystem::path &disc) : source(std::make_shared<chili::vfs::HostFileSource>(disc / "PSP_GAME" / "USRDIR" / "assets00.naz")) {
        directory = chili::naz::read_directory(*source);
        const auto *entry = find(props::database_path);
        if (!entry) throw std::runtime_error("the archive has no database.bin");
        declared = props::read_database(bytes(*entry));
    }
    const chili::naz::Entry *find(const std::string &name) const {
        for (const auto &entry : directory.entries) {
            const auto normal = chili::naz::normal_name(entry.name);
            if (normal == name || normal.ends_with('/' + name)) return &entry;
        }
        return nullptr;
    }
    std::vector<std::uint8_t> bytes(const chili::naz::Entry &entry) const {
        std::vector<std::uint8_t> out(entry.size);
        if (source->read(entry.data_offset(), out) != out.size()) throw std::runtime_error("cannot read " + entry.name);
        return out;
    }
    entry::Read reader(const chili::naz::Entry &entry, std::size_t &read_bytes) const {
        return [this, &entry, &read_bytes](std::uint64_t offset, std::span<std::uint8_t> out) -> std::size_t {
            if (offset >= entry.size) return 0u;
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), entry.size - offset));
            read_bytes += count;
            return source->read(entry.data_offset() + offset, out.first(count));
        };
    }
};
void actual_disc(const std::filesystem::path &disc) {
    const Archive archive(disc);
    unsigned blocks = 0u, edited = 0u, story = 0u, hangman = 0u, other = 0u;
    std::size_t read_bytes = 0u, whole_bytes = 0u;
    bool main_menu = false;
    const auto started = std::chrono::steady_clock::now();
    double edit_ms = 0.0;
    for (const auto &item : archive.directory.entries) {
        const auto name = chili::naz::normal_name(item.name);
        if (!name.ends_with(".map") || name.ends_with("_uk.map") || name.ends_with("_fr.map") || name.ends_with("_es.map")) continue;
        const bool main = name.ends_with("/mainmenuscene.map");
        ++blocks;
        whole_bytes += item.size;
        const auto before_edit = std::chrono::steady_clock::now();
        const auto result = entry::edit_block(archive.reader(item, read_bytes), item.size, archive.declared, main ? entry::What::MainMenu : entry::What::PauseLine);
        edit_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before_edit).count();
        require(result.problem.empty(), name + ": " + result.problem);
        const auto block = archive.bytes(item);
        const auto served = entry::apply(block, result.edits);
        require(served.size() == result.size_after(block.size()), name + ": size with the edits");
        const auto assets = props::read_block(served, true), originals = props::read_block(block, true);
        require(assets.size() == originals.size(), name + ": the number of assets changed");
        if (main) {
            main_menu = result.main_menu;
            require(result.edits.size() == 1u && served.size() == block.size() && differing_words(served, block, block.size()) == 5u, name + ": not five words in place");
            continue;
        }
        story += result.story; hangman += result.hangman; other += result.other;
        if (result.edits.empty()) continue;
        ++edited;
        // Every asset is what it was, but the scene-mode fragments with a new line: five more nodes each, and
        // the mod layer's reader walks them to the end.
        for (std::size_t i = 0; i < assets.size(); ++i) {
            const bool same = assets[i].size == originals[i].size && std::equal(served.begin() + static_cast<std::ptrdiff_t>(assets[i].offset),
                served.begin() + static_cast<std::ptrdiff_t>(assets[i].offset + assets[i].size), block.begin() + static_cast<std::ptrdiff_t>(originals[i].offset));
            if (same) continue;
            require(assets[i].type == props::fragment_type && props::lower(assets[i].name).find("/scenemode_") != std::string::npos, name + ": " + assets[i].name + " changed");
            const Read after(std::span(served).subspan(assets[i].offset, assets[i].size), archive.declared), was(std::span(block).subspan(originals[i].offset, originals[i].size), archive.declared);
            require(after.fragment.entities.size() == was.fragment.entities.size() + 5u && after.fragment.values.size() > was.fragment.values.size(), name + ": " + assets[i].name + " does not have five more nodes");
            require(assets[i].size <= props::word(served, props::map_base + 24u), name + ": the largest asset is not the largest");
        }
        // The same block served twice is not edited twice.
        std::size_t unused = 0u;
        const auto again = entry::edit_block([&served, &unused](std::uint64_t offset, std::span<std::uint8_t> out) -> std::size_t {
            if (offset >= served.size()) return 0u;
            const auto count = std::min<std::size_t>(out.size(), served.size() - static_cast<std::size_t>(offset));
            std::memcpy(out.data(), served.data() + offset, count); unused += count;
            return count;
        }, served.size(), archive.declared, entry::What::PauseLine);
        require(again.edits.empty() && again.already == result.story + result.hangman && again.other == result.other, name + ": edited twice");
    }
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::cout << "the game's archive: " << blocks << " blocks, " << edited << " with a pause line (" << story << " windows with four lines, " << hangman << " with two), "
              << other << " pause windows left alone; main menu " << (main_menu ? "turned" : "NOT turned") << "; " << read_bytes / 1048576u << " of " << whole_bytes / 1048576u
              << " MB read for the edits in " << static_cast<int>(edit_ms) << " ms (" << static_cast<int>(ms) << " ms with the checks)\n";
    // The retail disc: the main menu's block and 41 others. 19 story blocks and 7 El Macho blocks (one of them
    // holds a story fragment too) and 5 Hangman blocks get the line; the challenge windows of El Macho, the 5
    // style challenge blocks and the 5 Fiesta blocks keep their lines.
    require(main_menu && blocks == 42u && edited == 31u && story == 27u && hangman == 5u && other == 17u, "the game's archive is not edited as the retail disc should be");
}
int write_block(const std::filesystem::path &disc, const std::string &block_name, const std::filesystem::path &out) {
    const Archive archive(disc);
    const auto *item = archive.find(block_name);
    if (!item) throw std::runtime_error("the archive has no " + block_name);
    std::size_t read_bytes = 0u;
    const auto result = entry::edit_block(archive.reader(*item, read_bytes), item->size, archive.declared,
        block_name == "mainmenuscene.map" ? entry::What::MainMenu : entry::What::PauseLine);
    if (!result.problem.empty()) throw std::runtime_error(result.problem);
    const auto served = entry::apply(archive.bytes(*item), result.edits);
    std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char *>(served.data()), static_cast<std::streamsize>(served.size()));
    std::cout << block_name << ": " << result.edits.size() << " ranges, " << served.size() << " bytes (was " << item->size << "), " << read_bytes << " bytes read; written to " << out.string() << '\n';
    return 0;
}
}

void settings_entry_tests(unsigned &checks);

int main(int argc, char **argv) {
    try {
        if (argc == 5 && std::string(argv[1]) == "--write") return write_block(argv[2], argv[3], argv[4]);
        if (argc > 2) throw std::runtime_error("Usage: chili_menu_entry_test [actual disc0]");
        main_menu_line();
        pause_lines();
        whole_blocks();
        settings_entry_tests(checks);
        if (argc == 2) actual_disc(argv[1]);
        std::cout << "PASS: the main menu's Fiesta line turned in place, the pause line in the known windows and in no other, blocks edited from their head and "
                     "scene-mode fragments, the entries while the game runs" << (argc == 2 ? ", the game's own archive" : "") << " (" << checks << " checks)\n";
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
