#pragma once
#include "naz_archive.hpp"
#include <charconv>
#include <unordered_map>
#include <unordered_set>
namespace chili::props {
// Gameplay values changed by name: <mods>/<name>/properties.ini.
//
//   [DamageModDict]                         a fragment of todpsp_game.main, named by the end of its path
//   ShellDamageMod.StructureDamMod = 5      <entity name>.<property> = value
//   #2.StructureDamMod = 5                  the same entity by its number in the fragment
//   StructureDamMod = 1                     every entity of the fragment that sets the property
//
//   [mission01_1.map : Weapon_Gun_Light]    a fragment of another block of the archive
//   Gun_Light.Clipsize_m = 6
//
// The values are not in database.bin: that file declares the property names
// and types. They are "set property" records of the fragment assets inside the
// .main and .map blocks, the same layout as in the blocks of the PC game Total
// Overdose (same engine), behind a PSP block header:
//
//   block     base 0 (.main) or 299 (.map: GUID 36, fingerprint 255, two words)
//             +0   u32 time stamp, property checksum, command checksum, i32 region, u32 asset count,
//                  u32 info size, u32 largest asset
//             +28  u8 flag, u32 stream start, u32 stream end, then (end - start) stream bytes
//             then run-length words (low 31 bits a number of assets) that add up to the asset count
//             then the info buffer (the asset records), asset count sizes, the payloads in record order
//   asset     +0 type (4 is a fragment), +4 self-relative pointer to the name, +28 the value 24 or 25;
//             a fragment whose size in the table is 0 keeps its records inside its own asset record,
//             at the self-relative pointer +36
//   fragment  24 byte header whose first word is the offset of the first chunk; a chunk is u32 offset to
//             the next chunk (0 or 0x80000000 ends), u32 byte length of its records, u32 self-relative
//             pointer to the first record
//   record    u32 entity id, u32 packed; packed >> 24 is 0xFF for "create" (a word count and the type
//             name follow), 0xFE for "delete", otherwise packed is a property index and the value
//             follows: one word for number, integer, truth and Entity, three for vector, four for
//             quaternion, a word count and padded text for string
//   index     0..709 are properties the engine registers itself (16 is the entity's name);
//             710 + n is entry n of database.bin
//
// A value is written in place, so the block keeps its size. number, integer,
// truth, vector, quaternion and string (no longer than the room the old one
// has) can be changed; entity references, lists and structs cannot.
inline constexpr std::uint32_t builtin_properties = 710u, name_property = 16u, fragment_type = 4u;
inline constexpr std::size_t map_base = 299u, fragment_header = 24u;
inline constexpr char default_block[] = "todpsp_game.main";
inline constexpr char database_path[] = "data_baked/scripts/stable/database.bin";

struct Property { std::string name, type; };
struct Asset { std::uint32_t type{}; std::string name; std::size_t offset{}, size{}; };
struct Value { std::uint32_t entity{}, index{}; std::size_t offset{}, words{}; };
// A "create" record: where it starts and how many bytes it has (the id, the packed word, the word count, the type name).
struct Created { std::uint32_t entity{}; std::size_t offset{}, size{}; };
struct Fragment {
    std::vector<std::uint32_t> entities;                  // in creation order
    std::unordered_map<std::uint32_t, std::string> names; // lower case
    std::vector<Value> values;                            // in file order
    std::vector<Created> created;                         // in file order
};
struct Override {
    std::string block, fragment;   // lower case; fragment without a leading slash or ".fragment"
    std::string entity;            // lower case name, "#<number>", or empty for every entity
    std::string property, value;
    std::string origin;            // "<mod>/properties.ini line 12"
    std::string label;             // the section and the line as written, for the log
};

inline std::string lower(std::string_view text) {
    std::string out(text);
    for (auto &c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}
inline std::string_view trim(std::string_view text) {
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!text.empty() && blank(text.front())) text.remove_prefix(1u);
    while (!text.empty() && blank(text.back())) text.remove_suffix(1u);
    return text;
}
inline std::uint32_t word(std::span<const std::uint8_t> data, std::size_t at) { return naz::get32(data.data() + at); }
inline void store(std::span<std::uint8_t> data, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4u; ++i) data[at + i] = static_cast<std::uint8_t>(value >> (8u * i));
}

// The property section of database.bin, in file order.
inline std::vector<Property> read_database(std::span<const std::uint8_t> data) {
    const auto fail = [](const char *what) -> std::vector<Property> { throw std::runtime_error(std::string("database.bin ") + what); };
    if (data.size() < 4u) return fail("is too small");
    const auto count = word(data, 0u);
    std::vector<Property> out;
    std::size_t at = 4u;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (data.size() - at < 4u) return fail("ends inside its property list");
        const std::size_t length = word(data, at);
        at += 4u;
        if (!length || length > data.size() - at) return fail("has a bad entry length");
        const std::string_view text(reinterpret_cast<const char *>(data.data() + at), length - 1u);
        const auto colon = text.rfind(':');
        if (colon == std::string_view::npos) out.push_back({std::string(text), {}});
        else out.push_back({std::string(text.substr(0u, colon)), std::string(text.substr(colon + 1u))});
        at += length;
    }
    return out;
}

// The lines of a properties.ini. Only ';' starts a comment: '#' names an entity by number.
inline std::vector<Override> parse(std::string_view text, const std::string &origin, std::vector<std::string> &problems) {
    std::vector<Override> out;
    std::string block, fragment, heading;
    bool section = false;
    unsigned line_number = 0u;
    while (!text.empty()) {
        const auto end = std::min(text.find('\n'), text.size());
        auto line = text.substr(0u, end);
        text.remove_prefix(std::min(end + 1u, text.size()));
        ++line_number;
        bool quoted = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '"') quoted = !quoted;
            else if (line[i] == ';' && !quoted) { line = line.substr(0u, i); break; }
        }
        line = trim(line);
        if (line.empty()) continue;
        const auto where = origin + " line " + std::to_string(line_number);
        if (line.front() == '[') {
            section = false;
            if (line.back() != ']') { problems.push_back(where + ": expected [fragment] or [block : fragment]"); continue; }
            auto name = trim(line.substr(1u, line.size() - 2u));
            heading = std::string(name);
            block = default_block;
            if (const auto colon = name.find(':'); colon != std::string_view::npos) {
                block = lower(trim(name.substr(0u, colon)));
                name = trim(name.substr(colon + 1u));
            }
            fragment = naz::normal_name(name);
            if (fragment.ends_with(".fragment")) fragment.resize(fragment.size() - 9u);
            if (block.empty() || fragment.empty() || block.find_first_of("/\\") != std::string::npos) {
                problems.push_back(where + ": expected [fragment] or [block : fragment]");
                continue;
            }
            section = true;
            continue;
        }
        const auto equals = line.find('=');
        const auto key = trim(line.substr(0u, std::min(equals, line.size())));
        if (equals == std::string_view::npos || key.empty()) { problems.push_back(where + ": expected Entity.Property = value"); continue; }
        if (!section) { problems.push_back(where + ": the line is not inside a [fragment] section"); continue; }
        Override entry;
        entry.block = block; entry.fragment = fragment; entry.origin = where;
        entry.value = std::string(trim(line.substr(equals + 1u)));
        entry.label = '[' + heading + "] " + std::string(key) + " = " + entry.value;
        if (const auto dot = key.rfind('.'); dot != std::string_view::npos) {
            entry.entity = lower(trim(key.substr(0u, dot)));
            entry.property = std::string(trim(key.substr(dot + 1u)));
        } else entry.property = std::string(key);
        if (entry.property.empty() || entry.value.empty()) { problems.push_back(where + ": expected Entity.Property = value"); continue; }
        out.push_back(std::move(entry));
    }
    return out;
}

// The assets of a block and the place of each payload. Throws unless the header accounts for the whole file.
inline std::vector<Asset> read_block(std::span<const std::uint8_t> data, bool map) {
    const auto fail = [](const std::string &what) -> std::vector<Asset> { throw std::runtime_error(what); };
    const std::size_t base = map ? map_base : 0u;
    if (data.size() < base + 37u) return fail("too small to be a block");
    const std::size_t count = word(data, base + 16u), info_size = word(data, base + 20u);
    const std::size_t stream_start = word(data, base + 29u), stream_end = word(data, base + 33u);
    if (stream_end < stream_start || stream_end - stream_start > data.size() - base - 37u) return fail("bad stream range");
    std::size_t at = base + 37u + (stream_end - stream_start), total = 0u;
    while (total < count) {
        if (data.size() - at < 4u) return fail("the run-length words run past the file");
        total += word(data, at) & 0x7FFFFFFFu;
        at += 4u;
    }
    const auto info = at;
    if (total != count || info_size > data.size() - info || count > (data.size() - info - info_size) / 4u) return fail("the header does not describe this file");
    const auto sizes = info + info_size;
    auto payload = sizes + 4u * count;
    std::vector<Asset> out;
    std::vector<std::size_t> records;
    for (at = info; at + 32u <= sizes; at += 4u) {
        const auto type = word(data, at);
        if (type > 12u || word(data, at + 8u) || word(data, at + 16u) || word(data, at + 20u) ||
            (word(data, at + 24u) & 0xFFFFu) || (word(data, at + 28u) | 1u) != 0x19u) continue;
        const std::size_t name = at + 4u + (word(data, at + 4u) & ~3u);
        if (name >= sizes || data[name] != '/') continue;
        std::size_t end = name;
        while (end < sizes && end - name < 512u && data[end] >= 32u && data[end] < 127u) ++end;
        if (end == sizes || data[end] || end - name < 5u) continue;
        out.push_back({type, std::string(reinterpret_cast<const char *>(data.data() + name), end - name), 0u, 0u});
        records.push_back(at);
    }
    if (out.size() != count) return fail("found " + std::to_string(out.size()) + " asset records, the header says " + std::to_string(count));
    for (std::size_t i = 0; i < count; ++i) {
        out[i].offset = payload;
        out[i].size = word(data, sizes + 4u * i);
        if (out[i].size > data.size() - payload) return fail("the asset sizes run past the file");
        payload += out[i].size;
        // No payload: the fragment sits inside its own record, up to the next record.
        const std::size_t limit = i + 1u < count ? records[i + 1u] : sizes;
        if (out[i].type != fragment_type || out[i].size || limit - records[i] < 40u + fragment_header) continue;
        const std::size_t pointer = word(data, records[i] + 36u);
        if (pointer && pointer < 0x80000000u && pointer <= limit - records[i] - 36u - fragment_header) {
            out[i].offset = records[i] + 36u + pointer;
            out[i].size = limit - out[i].offset;
        }
    }
    if (payload != data.size()) return fail("the asset sizes do not add up to the file");
    return out;
}

// Walks the records of the fragment at [start, start + size). Throws when a record does not start where one is expected.
inline Fragment read_fragment(std::span<const std::uint8_t> data, std::size_t start, std::size_t size, const std::vector<Property> &database) {
    const auto fail = [](const std::string &what) -> Fragment { throw std::runtime_error(what); };
    Fragment fragment;
    if (size < fragment_header || start > data.size() || size > data.size() - start) return fail("too small to be a fragment");
    const auto end = start + size;
    const std::size_t limit = builtin_properties + database.size();
    std::vector<std::pair<std::size_t, std::size_t>> chunks;
    const std::size_t first_chunk = word(data, start);
    if (first_chunk && first_chunk < fragment_header) return fail("the first chunk is inside the header");
    for (std::size_t chunk = start + (first_chunk ? first_chunk : size); chunk < end && end - chunk >= 12u;) {
        const std::size_t following = word(data, chunk), length = word(data, chunk + 4u);
        const std::size_t first = chunk + 8u + std::size_t{word(data, chunk + 8u)};
        if (first < chunk + 12u || first > end || length > end - first) return fail("a chunk runs outside the fragment");
        chunks.emplace_back(first, first + length);
        if (!following || following == 0x80000000u || following > end - chunk || chunks.size() > 4096u) break;
        chunk += following;
    }
    std::unordered_set<std::uint32_t> known;
    for (const auto &[first, stop] : chunks) {
        for (auto at = first; stop - at >= 12u && word(data, at + 4u) >> 24u >= 0xFEu;) {
            if (word(data, at + 4u) >> 24u == 0xFEu) { at += 8u; continue; }
            const std::size_t words = word(data, at + 8u);
            if (words > (stop - at - 12u) / 4u) return fail("a create record runs past its chunk");
            if (known.insert(word(data, at)).second) fragment.entities.push_back(word(data, at));
            at += 12u + 4u * words;
        }
    }
    const auto starts_record = [&](std::size_t at, std::size_t stop) {
        if (stop - at < 8u || !known.contains(word(data, at))) return false;
        const auto packed = word(data, at + 4u);
        return packed >> 24u >= 0xFEu || packed < limit;
    };
    for (const auto &[first, stop] : chunks) {
        for (auto at = first; stop - at >= 8u;) {
            if (!starts_record(at, stop)) return fail("no record at offset " + std::to_string(at - start));
            const auto entity = word(data, at), packed = word(data, at + 4u);
            at += 8u;
            if (packed >> 24u == 0xFEu) continue;
            if (packed >> 24u == 0xFFu) {
                if (stop - at < 4u || word(data, at) > (stop - at - 4u) / 4u) return fail("a create record runs past its chunk");
                fragment.created.push_back({entity, at - 8u, 12u + 4u * std::size_t{word(data, at)}});
                at += 4u + 4u * std::size_t{word(data, at)};
                continue;
            }
            const std::string_view type = packed >= builtin_properties ? std::string_view(database[packed - builtin_properties].type) : std::string_view();
            std::size_t words = 0u;
            if (type == "number" || type == "integer" || type == "truth" || type == "Entity") words = 1u;
            else if (type == "vector") words = 3u;
            else if (type == "quaternion") words = 4u;
            else if (type == "string" && stop - at >= 4u) words = 1u + std::size_t{word(data, at)};
            // A property the engine registers itself, a list or a struct: the value runs to the next record.
            else while (stop - at - 4u * words >= 4u && !starts_record(at + 4u * words, stop)) ++words;
            if (words > (stop - at) / 4u) return fail("a value runs past its chunk");
            if (packed == name_property && words >= 2u && word(data, at) == words - 1u) {
                std::string name;
                for (std::size_t i = at + 4u; i < at + 4u * words && data[i]; ++i) name.push_back(static_cast<char>(data[i]));
                fragment.names[entity] = lower(name);
            }
            fragment.values.push_back({entity, packed, at, words});
            at += 4u * words;
        }
    }
    return fragment;
}

// The words of `text` as a value of `type` that has `words` words of room. Empty with `why` set when it does not fit.
inline std::vector<std::uint32_t> encode(std::string_view type, std::string_view text, std::size_t words, std::string &why) {
    const auto numbers = [&](std::size_t count) {
        std::vector<std::uint32_t> out;
        for (auto rest = trim(text); !rest.empty();) {
            const auto stop = std::min(rest.find_first_of(", 	"), rest.size());
            auto part = rest.substr(0u, stop);
            if (part.starts_with('+')) part.remove_prefix(1u);
            float value{};
            const auto result = std::from_chars(part.data(), part.data() + part.size(), value);
            if (part.empty() || result.ec != std::errc{} || result.ptr != part.data() + part.size()) { out.clear(); break; }
            std::uint32_t bits;
            std::memcpy(&bits, &value, 4u);
            out.push_back(bits);
            rest = trim(rest.substr(stop));
            if (!rest.empty() && rest.front() == ',') rest = trim(rest.substr(1u));
        }
        if (out.size() != count) { out.clear(); why = "expected " + std::to_string(count) + (count == 1u ? " number" : " numbers separated by commas"); }
        return out;
    };
    if (type == "number") return numbers(1u);
    if (type == "vector") return numbers(3u);
    if (type == "quaternion") return numbers(4u);
    if (type == "integer") {
        std::int64_t value{};
        const bool negative = !text.empty() && text.front() == '-';
        auto digits = text.substr(negative || (!text.empty() && text.front() == '+') ? 1u : 0u);
        const bool hex = digits.starts_with("0x") || digits.starts_with("0X");
        if (hex) digits.remove_prefix(2u);
        const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), value, hex ? 16 : 10);
        if (digits.empty() || result.ec != std::errc{} || result.ptr != digits.data() + digits.size() || value > 0xFFFFFFFFll ||
            (negative && value > 0x80000000ll)) { why = "expected a whole number"; return {}; }
        return {static_cast<std::uint32_t>(negative ? -value : value)};
    }
    if (type == "truth") {
        const auto answer = lower(text);
        if (answer == "true" || answer == "1" || answer == "yes") return {1u};
        if (answer == "false" || answer == "0" || answer == "no") return {0u};
        why = "expected true or false";
        return {};
    }
    if (type == "string") {
        if (text.size() >= 2u && text.front() == '"' && text.back() == '"') text = text.substr(1u, text.size() - 2u);
        if (words < 2u || text.size() + 1u > (words - 1u) * 4u) {
            why = "the text is longer than the " + std::to_string(words < 2u ? 0u : (words - 1u) * 4u - 1u) + " characters the old one has room for";
            return {};
        }
        std::vector<std::uint32_t> out(words);
        out[0] = static_cast<std::uint32_t>(words - 1u);
        for (std::size_t i = 0; i < text.size(); ++i) out[1u + i / 4u] |= std::uint32_t{static_cast<std::uint8_t>(text[i])} << (8u * (i % 4u));
        return out;
    }
    why = "a " + std::string(type.empty() ? "built-in" : type) + " value cannot be changed";
    return {};
}

// Writes the overrides meant for the block named `block` (lower case file name) into `data`. Returns how
// many values were written; `done` gets a line per override that changed something, `problems` one per
// override that did not.
inline std::size_t apply(std::span<std::uint8_t> data, std::string_view block, const std::vector<Property> &database,
    const std::vector<Override> &overrides, std::vector<std::string> &done, std::vector<std::string> &problems) {
    std::vector<const Override *> mine;
    for (const auto &entry : overrides) if (entry.block == block) mine.push_back(&entry);
    if (mine.empty()) return 0u;
    std::vector<Asset> assets;
    try { assets = read_block(data, block.ends_with(".map")); }
    catch (const std::runtime_error &error) {
        for (const auto *entry : mine) problems.push_back(entry->origin + ": " + std::string(block) + " is not readable (" + error.what() + ")");
        return 0u;
    }
    std::unordered_map<std::string_view, std::uint32_t> index;
    for (std::size_t i = 0; i < database.size(); ++i) index.emplace(database[i].name, builtin_properties + static_cast<std::uint32_t>(i));
    std::unordered_map<const Asset *, Fragment> fragments;
    std::size_t written = 0u;
    for (const auto *entry : mine) {
        const auto property = index.find(entry->property);
        if (property == index.end()) { problems.push_back(entry->origin + ": database.bin has no property named " + entry->property); continue; }
        const auto &type = database[property->second - builtin_properties].type;
        std::size_t matched_fragments = 0u, count = 0u;
        std::string why;
        for (const auto &asset : assets) {
            if (asset.type != fragment_type || !asset.size) continue;
            auto name = lower(asset.name);
            if (name.ends_with(".fragment")) name.resize(name.size() - 9u);
            if (!name.ends_with('/' + entry->fragment)) continue;
            ++matched_fragments;
            auto found = fragments.find(&asset);
            if (found == fragments.end()) {
                try { found = fragments.emplace(&asset, read_fragment(data, asset.offset, asset.size, database)).first; }
                catch (const std::runtime_error &error) { why = asset.name + " is not readable (" + error.what() + ")"; continue; }
            }
            const auto &fragment = found->second;
            for (const auto &value : fragment.values) {
                if (value.index != property->second) continue;
                if (!entry->entity.empty()) {
                    if (entry->entity.front() == '#') {
                        const auto number = std::find(fragment.entities.begin(), fragment.entities.end(), value.entity) - fragment.entities.begin();
                        if (entry->entity != '#' + std::to_string(number)) continue;
                    } else if (const auto named = fragment.names.find(value.entity); named == fragment.names.end() || named->second != entry->entity) continue;
                }
                const auto words = encode(type, entry->value, value.words, why);
                if (words.empty()) break;
                for (std::size_t i = 0; i < words.size(); ++i) store(data, value.offset + 4u * i, words[i]);
                ++count;
            }
        }
        if (count) {
            written += count;
            done.push_back(entry->label + (count > 1u ? " (" + std::to_string(count) + " entities)" : ""));
        } else if (!why.empty()) problems.push_back(entry->origin + ": " + why);
        else if (!matched_fragments) problems.push_back(entry->origin + ": " + std::string(block) + " has no fragment " + entry->fragment);
        else problems.push_back(entry->origin + ": no entity" + (entry->entity.empty() ? "" : " " + entry->entity) + " in " + entry->fragment + " sets " + entry->property);
    }
    return written;
}
}
