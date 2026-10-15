#pragma once
// The "Settings" entries in the game's own menus, as changes to the blocks the game loads them from
// (CONTROLS.md, "The settings page inside the game"). Nothing here
// touches the game or a file: a block's bytes are read through a function and the result is a list of
// ranges to serve differently (file_runtime.cpp serves them when the page is enabled).
//
// Main menu (mainmenuscene.map, fragment SceneMode_MainMenu): the line `Fiesta` of the window `Menu_Main`
// starts the PSP's wireless play, which this port does not have. Five values turn it into the entry, in
// place, the block keeps its size:
//
//   text index (built-in property 432) of the line and of its `- outline` and `- dropshadow` boxes   13 -> 119
//       ("Settings", "Paramètres", "Ajustes": the game's own word, in its own text table)
//   MenuCommand of the line                                                                           302 -> 9001
//   Action of its Action_MenuWindowStack child (0 pushes the Fiesta window; the script acts on 0, 1, 2) 0 -> 3
//
// The game then runs its own handler to the end when the line is chosen: nothing is pushed, and the window's
// member m_iCommand holds 9001, which the host reads (settings_entry.hpp).
//
// Pause menu (every mission block, the window `SubMenu_IngameMain` in its scene-mode fragment): a real line
// `Settings` under the last one. Five nodes (the line, its two text boxes, its two animations) are clones of
// the line `Quit` with new node numbers behind the fragment's last one, text index 119 and MenuCommand -1;
// their records are a new chunk at the end of the fragment, and the fragment's size in the block's size
// table grows by the chunk (so does "largest asset" when the fragment is the block's largest).
//
//   MenuCommand is -1, not a number of its own: the pause window's handler (0x08BD3F60) closes the window for
//   every command it does not know and the game stays paused with no menu; -1 is the one number it returns
//   on at once. The choice is seen in the pad sample instead (settings_entry.hpp).
//
//   window under            lines the game has (y)                          what is done
//   GringoMode, MachoMode   Resume 119, Rewind 136, Restart 153, Quit 170   the four move up 12, the new one at 175
//   DonkeyMode (Hangman)    Resume 129, Quit 150                            nothing moves, the new one at 171
//   anything else           (six lines in a Macho or style challenge,       left as it is: such a window gets
//                           three in Fiesta)                                the legend (settings_entry.hpp)
//
// A window is known by its lines and by the names of the two nodes above it, never by its own name alone: an
// El Macho block has two windows named SubMenu_IngameMain in one fragment.
#include "block_props.hpp"
#include <algorithm>
#include <bit>
#include <functional>
#include <initializer_list>
namespace chili::menu_entry {
inline constexpr std::uint32_t parent_property = 6u, position_property = 13u, text_index_property = 432u;
inline constexpr std::uint32_t settings_text = 119u, fiesta_text = 13u;
inline constexpr std::uint32_t fiesta_command = 302u, main_menu_command = 9001u, pause_command = 0xFFFFFFFFu;
inline constexpr std::uint32_t push_action = 0u, no_action = 3u;
// The names the host knows its nodes by while the game runs (a node keeps its name as a C string).
inline constexpr char main_menu_window[] = "Menu_Main", main_menu_line[] = "Fiesta";
inline constexpr char pause_window[] = "SubMenu_IngameMain", pause_logic[] = "Menu_IngameMain", pause_line[] = "Settings";
inline constexpr float story_lift = -12.0f, story_drop = 5.0f, hangman_drop = 21.0f;

// Reads `out.size()` bytes of the block at `offset`; returns how many it could.
using Read = std::function<std::size_t(std::uint64_t offset, std::span<std::uint8_t> out)>;

// A range of the block that is served with other bytes (as many, or more when a fragment grew).
struct Edit { std::size_t offset{}, old_size{}; std::vector<std::uint8_t> bytes; };
struct FragmentResult {
    bool changed{};
    // Pause windows: given the line (four-line and two-line kinds), left alone, already carrying the line.
    unsigned story{}, hangman{}, other{}, already{};
    std::string problem;
};
struct BlockResult {
    std::vector<Edit> edits;   // by offset, apart from each other; empty: the block is served as it is
    bool main_menu{};          // the Fiesta line is the entry (changed now, or found changed)
    unsigned story{}, hangman{}, other{}, already{};
    std::size_t read_bytes{};  // how much of the block was read
    std::string problem;       // why a block that was meant to change did not
    // The size of the block with the edits.
    [[nodiscard]] std::size_t size_after(std::size_t before) const {
        for (const auto &edit : edits) before += edit.bytes.size() - edit.old_size;
        return before;
    }
};
enum class What : std::uint8_t { MainMenu, PauseLine };

namespace detail {
inline std::vector<std::uint8_t> words(std::initializer_list<std::uint32_t> values) {
    std::vector<std::uint8_t> out;
    for (const auto value : values) naz::put32(out, value);
    return out;
}
inline float number(std::span<const std::uint8_t> data, std::size_t at) { return std::bit_cast<float>(props::word(data, at)); }
// The nodes of a fragment with what the entries need of them.
struct Tree {
    const std::vector<props::Property> &database;
    props::Fragment fragment;
    std::unordered_map<std::uint32_t, std::uint32_t> parent;
    std::unordered_map<std::uint32_t, std::size_t> order;
    Tree(std::span<const std::uint8_t> bytes, const std::vector<props::Property> &properties)
        : database(properties), fragment(props::read_fragment(bytes, 0u, bytes.size(), properties)) {
        for (std::size_t i = 0; i < fragment.entities.size(); ++i) order.emplace(fragment.entities[i], i);
        for (const auto &value : fragment.values)
            if (value.index == parent_property && value.words == 1u) parent[value.entity] = props::word(bytes, value.offset);
    }
    // The node's name in lower case; empty when it has none.
    [[nodiscard]] std::string_view name(std::uint32_t entity) const {
        const auto found = fragment.names.find(entity);
        return found == fragment.names.end() ? std::string_view() : std::string_view(found->second);
    }
    [[nodiscard]] std::uint32_t above(std::uint32_t entity) const {
        const auto found = parent.find(entity);
        return found == parent.end() ? 0u : found->second;
    }
    // Whether `index` is the database property of that name (a name can be declared more than once).
    [[nodiscard]] bool is(std::uint32_t index, std::string_view property) const {
        return index >= props::builtin_properties && index - props::builtin_properties < database.size() &&
               database[index - props::builtin_properties].name == property;
    }
    // The one-word value of that database property on the node; null when it sets none.
    [[nodiscard]] const props::Value *value(std::uint32_t entity, std::string_view property) const {
        for (const auto &item : fragment.values) if (item.entity == entity && item.words == 1u && is(item.index, property)) return &item;
        return nullptr;
    }
};
// The chunks of a fragment: where each header is and where its records end.
struct Chunk { std::size_t header{}, first{}, stop{}; };
inline std::vector<Chunk> chunks(std::span<const std::uint8_t> bytes) {
    std::vector<Chunk> out;
    const std::size_t end = bytes.size(), first_chunk = props::word(bytes, 0u);
    for (std::size_t chunk = first_chunk ? first_chunk : end; chunk < end && end - chunk >= 12u;) {
        const std::size_t following = props::word(bytes, chunk), length = props::word(bytes, chunk + 4u);
        const std::size_t first = chunk + 8u + std::size_t{props::word(bytes, chunk + 8u)};
        if (first < chunk + 12u || first > end || length > end - first) return {};
        out.push_back({chunk, first, first + length});
        if (!following || following == 0x80000000u || following > end - chunk || out.size() > 4096u) break;
        chunk += following;
    }
    return out;
}
}

// Turns the main menu's Fiesta line into the entry, in place. True when the five values are the entry's
// afterwards; `changed` says whether anything was written (false: the fragment already carried them).
inline bool settings_for_fiesta(std::span<std::uint8_t> bytes, const std::vector<props::Property> &database, bool &changed, std::string &problem) {
    changed = false;
    try {
        const detail::Tree tree(bytes, database);
        std::uint32_t line = 0u;
        for (const auto entity : tree.fragment.entities)
            if (tree.name(entity) == props::lower(main_menu_line) && tree.name(tree.above(entity)) == props::lower(main_menu_window)) { line = entity; break; }
        if (!line) { problem = "no line Fiesta in the window Menu_Main"; return false; }
        std::vector<std::size_t> texts, actions;
        for (const auto &value : tree.fragment.values) {
            if (value.words != 1u) continue;
            const bool box = value.entity == line || (tree.above(value.entity) == line && (tree.name(value.entity) == "- outline" || tree.name(value.entity) == "- dropshadow"));
            if (box && value.index == text_index_property) texts.push_back(value.offset);
            if (tree.above(value.entity) == line && tree.is(value.index, "Action")) actions.push_back(value.offset);
        }
        const auto *command = tree.value(line, "MenuCommand");
        if (texts.size() != 3u || actions.size() != 1u || !command) {
            problem = "the Fiesta line has " + std::to_string(texts.size()) + " text indexes, " + std::to_string(actions.size()) + " actions and " + (command ? "a" : "no") + " MenuCommand; expected 3, 1 and one";
            return false;
        }
        const auto all = [&](std::uint32_t text, std::uint32_t number, std::uint32_t action) {
            return std::all_of(texts.begin(), texts.end(), [&](std::size_t at) { return props::word(bytes, at) == text; }) &&
                   props::word(bytes, command->offset) == number && props::word(bytes, actions[0]) == action;
        };
        if (all(settings_text, main_menu_command, no_action)) return true;
        if (!all(fiesta_text, fiesta_command, push_action)) { problem = "the Fiesta line does not have the game's own values (text 13, command 302, action 0)"; return false; }
        for (const auto at : texts) props::store(bytes, at, settings_text);
        props::store(bytes, command->offset, main_menu_command);
        props::store(bytes, actions[0], no_action);
        changed = true;
        return true;
    } catch (const std::runtime_error &error) { problem = error.what(); return false; }
}

// Gives every pause window of the fragment that the host knows its `Settings` line. The fragment grows by one
// chunk per window; on any doubt it is left exactly as it was.
inline FragmentResult add_pause_line(std::vector<std::uint8_t> &bytes, const std::vector<props::Property> &database) {
    FragmentResult result;
    const auto original = bytes;
    const auto give_up = [&](std::string why) { bytes = original; result = {}; result.problem = std::move(why); return result; };
    try {
        const detail::Tree tree(bytes, database);
        const auto &fragment = tree.fragment;
        if (fragment.entities.empty()) return result;
        std::uint32_t next_id = *std::max_element(fragment.entities.begin(), fragment.entities.end()) + 1u;
        std::size_t expected_entities = fragment.entities.size();
        struct Line { std::uint32_t entity{}; std::string_view name; std::uint32_t command{}; };
        for (const auto window : fragment.entities) {
            if (tree.name(window) != props::lower(pause_window) || tree.name(tree.above(window)) != props::lower(pause_logic)) continue;
            // Its lines, in the order they register: the children that have a MenuCommand and no TriggerButton
            // (a child with both is a button listener: Back).
            std::vector<Line> lines;
            for (const auto entity : fragment.entities) {
                if (tree.above(entity) != window || tree.value(entity, "TriggerButton")) continue;
                if (const auto *command = tree.value(entity, "MenuCommand")) lines.push_back({entity, tree.name(entity), props::word(bytes, command->offset)});
            }
            if (std::any_of(lines.begin(), lines.end(), [](const Line &line) { return line.name == props::lower(pause_line); })) { ++result.already; continue; }
            const auto mode = tree.name(tree.above(tree.above(window)));
            const auto are = [&](std::initializer_list<std::pair<std::string_view, std::uint32_t>> wanted) {
                return lines.size() == wanted.size() && std::equal(lines.begin(), lines.end(), wanted.begin(),
                    [](const Line &line, const auto &want) { return line.name == want.first && line.command == want.second; });
            };
            const bool story = (mode == "gringomode" || mode == "machomode") && are({{"resume", 1u}, {"rewind", 400u}, {"restart", 201u}, {"quit", 200u}});
            const bool hangman = mode == "donkeymode" && are({{"resume", 1u}, {"quit", 200u}});
            if (!story && !hangman) { ++result.other; continue; }
            // The clone of the last line with everything below it.
            const std::uint32_t source = lines.back().entity;
            std::vector<std::uint32_t> group{source};
            for (bool grown = true; grown;) {
                grown = false;
                for (const auto entity : fragment.entities)
                    if (std::find(group.begin(), group.end(), entity) == group.end() && std::find(group.begin(), group.end(), tree.above(entity)) != group.end()) { group.push_back(entity); grown = true; }
            }
            std::sort(group.begin(), group.end(), [&](std::uint32_t a, std::uint32_t b) { return tree.order.at(a) < tree.order.at(b); });
            std::unordered_map<std::uint32_t, std::uint32_t> renumbered;
            for (const auto entity : group) renumbered.emplace(entity, next_id++);
            std::vector<std::uint8_t> added;
            std::size_t creates = 0u, positions = 0u, names = 0u, commands = 0u, texts = 0u;
            for (const auto &created : fragment.created) {
                const auto renumber = renumbered.find(created.entity);
                if (renumber == renumbered.end()) continue;
                const auto at = added.size();
                added.insert(added.end(), original.begin() + static_cast<std::ptrdiff_t>(created.offset), original.begin() + static_cast<std::ptrdiff_t>(created.offset + created.size));
                props::store(added, at, renumber->second);
                ++creates;
            }
            if (creates != group.size()) return give_up("a node of the line Quit has no create record of its own");
            for (const auto &value : fragment.values) {
                const auto renumber = renumbered.find(value.entity);
                if (renumber == renumbered.end()) continue;
                naz::put32(added, renumber->second);
                naz::put32(added, value.index);
                const auto at = added.size();
                if (value.entity == source && value.index == props::name_property) {
                    // The name as a string value: a word count, then the text with its end and padding.
                    const std::string_view name(pause_line);
                    const std::size_t text_words = name.size() / 4u + 1u;
                    naz::put32(added, static_cast<std::uint32_t>(text_words));
                    added.resize(added.size() + 4u * text_words);
                    std::copy(name.begin(), name.end(), added.begin() + static_cast<std::ptrdiff_t>(at + 4u));
                    ++names;
                    continue;
                }
                added.insert(added.end(), original.begin() + static_cast<std::ptrdiff_t>(value.offset), original.begin() + static_cast<std::ptrdiff_t>(value.offset + 4u * value.words));
                // A reference to a node of the group (the parent, most of all) is one to its clone.
                if (value.words == 1u)
                    if (const auto inside = renumbered.find(props::word(added, at)); inside != renumbered.end()) props::store(added, at, inside->second);
                if (value.entity == source && value.index == position_property && value.words == 3u) {
                    props::store(added, at + 4u, std::bit_cast<std::uint32_t>(detail::number(added, at + 4u) + (story ? story_drop : hangman_drop)));
                    ++positions;
                }
                if (value.entity == source && value.words == 1u && tree.is(value.index, "MenuCommand")) { props::store(added, at, pause_command); ++commands; }
                if (value.index == text_index_property && value.words == 1u) { props::store(added, at, settings_text); ++texts; }
            }
            if (positions != 1u || names != 1u || commands != 1u || texts < 1u)
                return give_up("the line Quit sets " + std::to_string(positions) + " positions, " + std::to_string(names) + " names, " + std::to_string(commands) + " commands and " + std::to_string(texts) + " text indexes");
            // The four lines of a story mission make room: every position the line sets, moved up.
            if (story)
                for (const auto &line : lines) {
                    std::size_t moved = 0u;
                    for (const auto &value : fragment.values)
                        if (value.entity == line.entity && value.index == position_property && value.words == 3u) {
                            props::store(bytes, value.offset + 4u, std::bit_cast<std::uint32_t>(detail::number(bytes, value.offset + 4u) + story_lift));
                            ++moved;
                        }
                    if (!moved) return give_up("a line of the pause window sets no position");
                }
            // A new chunk behind the last one: next 0, the records' length, the records right behind the header.
            const auto all = detail::chunks(bytes);
            if (all.empty() || all.back().stop != bytes.size()) return give_up("the last chunk does not end the fragment");
            props::store(bytes, all.back().header, static_cast<std::uint32_t>(bytes.size() - all.back().header));
            for (const auto value : {std::uint32_t{0u}, static_cast<std::uint32_t>(added.size()), std::uint32_t{4u}}) naz::put32(bytes, value);
            bytes.insert(bytes.end(), added.begin(), added.end());
            expected_entities += group.size();
            ++(story ? result.story : result.hangman);
            result.changed = true;
        }
        if (!result.changed) return result;
        // What was made is read back as the mod layer reads any fragment: the nodes are there, the lines are named.
        const detail::Tree after(bytes, database);
        std::size_t named = 0u;
        for (const auto entity : after.fragment.entities)
            if (after.name(entity) == props::lower(pause_line) && after.name(after.above(entity)) == props::lower(pause_window)) ++named;
        if (after.fragment.entities.size() != expected_entities || named != result.story + result.hangman + result.already)
            return give_up("the fragment with the new line does not read back");
        return result;
    } catch (const std::runtime_error &error) { return give_up(error.what()); }
}

// The entry of `what` for one block (a .map: mainmenuscene.map, or a mission's). Reads the block's head (the
// asset records and sizes, which lie behind the block's stream) and the scene-mode fragments, not the whole
// block; nothing is changed when the block is not what the game ships in the way this code checks.
inline BlockResult edit_block(const Read &read, std::uint64_t whole, const std::vector<props::Property> &database, What what) {
    BlockResult result;
    const auto fail = [&](std::string why) { result.edits.clear(); result.problem = std::move(why); return result; };
    constexpr std::size_t base = props::map_base;
    if (whole < base + 37u || whole > 0x7FFFFFFFu) return fail("not a block");
    const auto size = static_cast<std::size_t>(whole);
    // The block with only its head filled in, so the mod layer's own reader can list its assets.
    std::vector<std::uint8_t> head(size);
    const auto fetch = [&](std::size_t at, std::size_t count) {
        if (at > size || count > size - at) return false;
        result.read_bytes += count;
        return read(at, std::span(head).subspan(at, count)) == count;
    };
    if (!fetch(0u, base + 37u)) return fail("cannot be read");
    const std::size_t count = props::word(head, base + 16u), info_size = props::word(head, base + 20u);
    const std::size_t stream_start = props::word(head, base + 29u), stream_end = props::word(head, base + 33u);
    if (stream_end < stream_start || stream_end - stream_start > size - base - 37u) return fail("bad stream range");
    std::size_t at = base + 37u + (stream_end - stream_start), total = 0u;
    for (unsigned runs = 0u; total < count; ++runs) {
        if (runs > 4096u || !fetch(at, 4u)) return fail("the run-length words run past the file");
        total += props::word(head, at) & 0x7FFFFFFFu;
        at += 4u;
    }
    if (total != count || info_size > size - at || count > (size - at - info_size) / 4u) return fail("the header does not describe this file");
    const std::size_t sizes = at + info_size;
    if (!fetch(at, info_size + 4u * count)) return fail("cannot be read");
    std::vector<props::Asset> assets;
    try { assets = props::read_block(head, true); }
    catch (const std::runtime_error &error) { return fail(error.what()); }
    const std::size_t largest = props::word(head, base + 24u);
    std::size_t new_largest = largest;
    for (std::size_t index = 0; index < assets.size(); ++index) {
        const auto &asset = assets[index];
        if (asset.type != props::fragment_type || !asset.size) continue;
        const auto name = props::lower(asset.name);
        const bool wanted = what == What::MainMenu ? name.ends_with("/scenemode_mainmenu.fragment") : name.find("/scenemode_") != std::string::npos && name.ends_with(".fragment");
        if (!wanted) continue;
        std::vector<std::uint8_t> fragment(asset.size);
        result.read_bytes += asset.size;
        if (read(asset.offset, fragment) != asset.size) return fail(asset.name + " cannot be read");
        if (what == What::MainMenu) {
            bool changed = false;
            std::string problem;
            if (!settings_for_fiesta(fragment, database, changed, problem)) return fail(asset.name + ": " + problem);
            result.main_menu = true;
            if (changed) result.edits.push_back({asset.offset, asset.size, std::move(fragment)});
            continue;
        }
        const auto made = add_pause_line(fragment, database);
        if (!made.problem.empty()) return fail(asset.name + ": " + made.problem);
        result.story += made.story; result.hangman += made.hangman; result.other += made.other; result.already += made.already;
        if (!made.changed) continue;
        new_largest = std::max(new_largest, fragment.size());
        result.edits.push_back({sizes + 4u * index, 4u, detail::words({static_cast<std::uint32_t>(fragment.size())})});
        result.edits.push_back({asset.offset, asset.size, std::move(fragment)});
    }
    if (what == What::MainMenu && !result.main_menu) return fail("no fragment SceneMode_MainMenu");
    if (new_largest != largest) result.edits.push_back({base + 24u, 4u, detail::words({static_cast<std::uint32_t>(new_largest)})});
    std::sort(result.edits.begin(), result.edits.end(), [](const Edit &a, const Edit &b) { return a.offset < b.offset; });
    return result;
}

// The block's bytes with the edits (tests, and tools that want the whole file).
inline std::vector<std::uint8_t> apply(std::span<const std::uint8_t> block, const std::vector<Edit> &edits) {
    std::vector<std::uint8_t> out;
    std::size_t at = 0u;
    for (const auto &edit : edits) {
        out.insert(out.end(), block.begin() + static_cast<std::ptrdiff_t>(at), block.begin() + static_cast<std::ptrdiff_t>(edit.offset));
        out.insert(out.end(), edit.bytes.begin(), edit.bytes.end());
        at = edit.offset + edit.old_size;
    }
    out.insert(out.end(), block.begin() + static_cast<std::ptrdiff_t>(at), block.end());
    return out;
}
}
