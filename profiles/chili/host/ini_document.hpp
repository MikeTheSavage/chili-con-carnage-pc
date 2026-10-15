#pragma once
// ChiliNative.ini as the player wrote it: every line is kept byte for byte, and a change touches only
// the line of the key that changed. The C++ twin of the launcher's launcher/src/IniDocument.cs: the same
// three cases in the same order, so a change made by the game and the same change made by the launcher
// give the same bytes (tools/ini_document_test.cpp holds the two against each other).
//
//   set   the last live line of the key keeps its spelling, its spacing and its trailing comment and gets
//         the new value; else the first switched-off "; Key=value" line of the section becomes live; else
//         the line is added after the last line of its section; else the section is added at the end.
//   unset every live line of the key gets "; " in front.
//
// The file is the launcher's as much as the game's, so nobody writes it whole: ini_change_file() reads it
// fresh, applies only the keys that changed, writes a temporary file beside it and swaps that in
// (ReplaceFileW), all under the named mutex ini_gate_name. The launcher's Model.Save does the same under
// the same mutex. A writer that does not know the mutex (an older launcher) is still not overwritten: the
// file is read once more right before the swap, and when it changed the change is made again on the new text.
//
// A file that is gone for a moment is not a file that was never there. While another program swaps its own
// text in, or a scanner or a sync client holds the name, the file can be missing or closed to others for a
// few milliseconds. A writer that took that for "no file yet" would start from nothing and put its own two
// lines in the place of the player's whole file. So a writer says what it last saw of the file (IniKnown): a
// file it has seen is waited for (half a second), and if it stays away it is written again whole, from the
// text last seen, with the change. A swap that fails is tried again before the file is written in place,
// and a swap that took the old file away without putting the new one there is finished by a move.
//
// Text: the file's bytes. A file that is not UTF-8 is taken as Latin-1, as the launcher takes it; values
// are given in UTF-8 and written in the file's encoding. Section and key names are compared without case
// for A to Z only (the launcher compares all letters so; every name of ChiliNative.ini is ASCII).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace chili {
// Taken by the game and by the launcher around read, change and swap of ChiliNative.ini.
inline constexpr wchar_t ini_gate_name[] = L"Local\\ChiliConCarnage.PC.Settings";

class IniDocument {
public:
    enum class Kind : std::uint8_t { Blank, Comment, Section, KeyValue, Other };
    struct Line {
        std::string text;            // without its line end
        std::string eol;             // "\r\n", "\n", "\r" or "" (the last line without one)
        Kind kind{Kind::Blank};
        std::string section;         // the section the line is in; for a header, the section it opens
        bool has_key{};              // KeyValue: always. Comment: a switched-off "; Key=value" line
        std::string key;
        std::string value;           // without the inline comment, trimmed
        std::string raw_value;       // everything after '=', trimmed (paths keep ';' and '#')
        std::size_t value_start{}, value_length{};   // where the value is in `text` (KeyValue only)
    };
    std::vector<Line> lines;
    bool has_bom{};
    bool utf8{true};
    std::string new_line{"\r\n"};

    static IniDocument parse(std::string_view bytes) {
        IniDocument doc;
        if (bytes.size() >= 3u && bytes.substr(0u, 3u) == "\xEF\xBB\xBF") { doc.has_bom = true; bytes.remove_prefix(3u); }
        doc.utf8 = valid_utf8(bytes);
        doc.parse_text(bytes);
        return doc;
    }
    std::string bytes() const {
        std::string out = has_bom ? "\xEF\xBB\xBF" : "";
        for (const auto &line : lines) { out += line.text; out += line.eol; }
        return out;
    }

    // The value in force, or nothing when no live line names the key.
    std::optional<std::string> get(std::string_view section, std::string_view key) const {
        const auto *line = find_live(section, key);
        return line ? std::optional<std::string>(line->value) : std::nullopt;
    }
    // Everything after '=' (for paths: ';' and '#' are part of it; one pair of quotes round it is removed).
    std::optional<std::string> get_raw(std::string_view section, std::string_view key) const {
        const auto *line = find_live(section, key);
        if (!line) return std::nullopt;
        std::string_view v = line->raw_value;
        if (v.size() >= 2u && v.front() == '"' && v.back() == '"' && v.find('"', 1u) == v.size() - 1u) v = v.substr(1u, v.size() - 2u);
        return std::string(v);
    }
    bool has(std::string_view section, std::string_view key) const { return find_live(section, key) != nullptr; }
    // True when the file names the key at all, live or switched off with ';'.
    bool mentions(std::string_view section, std::string_view key) const {
        for (const auto &line : lines)
            if ((line.kind == Kind::KeyValue || line.kind == Kind::Comment) && line.has_key && same(line.section, section) && same(line.key, key)) return true;
        return false;
    }
    bool has_section(std::string_view section) const {
        for (const auto &line : lines) if (line.kind == Kind::Section && same(line.section, section)) return true;
        return false;
    }

    // `value` in UTF-8. `raw`: the value replaces everything after '=' (a path: a trailing comment is not kept).
    void set(std::string_view section, std::string_view key, std::string_view value_utf8, bool raw = false) {
        const std::string value = encoded(value_utf8);
        if (auto *live = find_live(section, key)) {
            std::string text;
            if (raw) text = live->text.substr(0u, live->text.find('=') + 1u) + value;
            else text = live->text.substr(0u, live->value_start) + value + live->text.substr(live->value_start + live->value_length);
            replace(*live, text);
            return;
        }
        for (auto &line : lines)
            if (line.kind == Kind::Comment && line.has_key && same(line.section, section) && same(line.key, key)) {
                replace(line, leading_space(line.text) + line.key + "=" + value);
                return;
            }
        std::ptrdiff_t header = -1, last = -1;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const auto &line = lines[i];
            if (line.kind == Kind::Section) {
                if (same(line.section, section)) { header = static_cast<std::ptrdiff_t>(i); last = header; }
                continue;
            }
            if (header >= 0 && same(line.section, section) && line.kind != Kind::Blank) last = static_cast<std::ptrdiff_t>(i);
        }
        if (header < 0) {
            if (!lines.empty()) {
                if (lines.back().eol.empty()) lines.back().eol = new_line;
                if (lines.back().kind != Kind::Blank) {
                    Line blank; blank.eol = new_line; blank.section = lines.back().section;
                    lines.push_back(std::move(blank));
                }
            }
            auto head = parse_line("[" + encoded(section) + "]", "", utf8);
            head.eol = new_line;
            lines.push_back(std::move(head));
            last = static_cast<std::ptrdiff_t>(lines.size()) - 1;
        }
        if (lines[static_cast<std::size_t>(last)].eol.empty()) lines[static_cast<std::size_t>(last)].eol = new_line;
        auto added = parse_line(encoded(key) + "=" + value, encoded(section), utf8);
        added.eol = new_line;
        lines.insert(lines.begin() + last + 1, std::move(added));
    }
    // Switches every live line of the key off by putting "; " in front of it.
    void unset(std::string_view section, std::string_view key) {
        for (auto &line : lines)
            if (line.kind == Kind::KeyValue && same(line.section, section) && same(line.key, key)) {
                const auto lead = leading_space(line.text);
                replace(line, lead + "; " + line.text.substr(lead.size()));
            }
    }

    // A to Z without case, everything else byte for byte.
    static bool same(std::string_view a, std::string_view b) noexcept {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
    }
    // Whether the bytes are UTF-8 as the launcher's strict decoder takes it: no overlong form, no surrogate,
    // nothing above U+10FFFF, no cut-off sequence.
    static bool valid_utf8(std::string_view text) noexcept {
        for (std::size_t i = 0; i < text.size();) {
            const auto b = static_cast<unsigned char>(text[i]);
            std::size_t more = 0; std::uint32_t code = b, least = 0;
            if (b < 0x80u) { ++i; continue; }
            else if (b >= 0xC2u && b <= 0xDFu) { more = 1u; code = b & 0x1Fu; least = 0x80u; }
            else if (b >= 0xE0u && b <= 0xEFu) { more = 2u; code = b & 0x0Fu; least = 0x800u; }
            else if (b >= 0xF0u && b <= 0xF4u) { more = 3u; code = b & 0x07u; least = 0x10000u; }
            else return false;
            if (i + more >= text.size()) return false;
            for (std::size_t k = 1; k <= more; ++k) {
                const auto c = static_cast<unsigned char>(text[i + k]);
                if ((c & 0xC0u) != 0x80u) return false;
                code = code << 6u | (c & 0x3Fu);
            }
            if (code < least || code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu)) return false;
            i += more + 1u;
        }
        return true;
    }

private:
    static char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
    static bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r'; }
    // The length of the white space at the start (or, `back`, at the end) of the text, as .NET's
    // String.Trim counts it: U+0009 to U+000D, U+0020, U+0085, U+00A0, U+1680, U+2000 to U+200A, U+2028,
    // U+2029, U+202F, U+205F, U+3000. In a Latin-1 file a character is a byte.
    static std::size_t white(std::string_view text, bool back, bool utf8) noexcept {
        if (text.empty()) return 0u;
        const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
        const unsigned char a = byte(back ? text.size() - 1u : 0u);
        if (a == ' ' || (a >= 0x09u && a <= 0x0Du)) return 1u;
        if (!utf8) return a == 0x85u || a == 0xA0u ? 1u : 0u;
        if (text.size() >= 2u) {
            const std::size_t at = back ? text.size() - 2u : 0u;
            if (byte(at) == 0xC2u && (byte(at + 1u) == 0x85u || byte(at + 1u) == 0xA0u)) return 2u;       // U+0085, U+00A0
        }
        if (text.size() >= 3u) {
            const std::size_t at = back ? text.size() - 3u : 0u;
            const unsigned char e0 = byte(at), e1 = byte(at + 1u), e2 = byte(at + 2u);
            if (e0 == 0xE1u && e1 == 0x9Au && e2 == 0x80u) return 3u;                                      // U+1680
            if (e0 == 0xE2u && e1 == 0x80u && ((e2 >= 0x80u && e2 <= 0x8Au) || e2 == 0xA8u || e2 == 0xA9u || e2 == 0xAFu)) return 3u;   // U+2000 to U+200A, U+2028, U+2029, U+202F
            if (e0 == 0xE2u && e1 == 0x81u && e2 == 0x9Fu) return 3u;                                      // U+205F
            if (e0 == 0xE3u && e1 == 0x80u && e2 == 0x80u) return 3u;                                      // U+3000
        }
        return 0u;
    }
    static std::string_view trim(std::string_view text, bool utf8) noexcept {
        for (std::size_t n; (n = white(text, false, utf8)) != 0u;) text.remove_prefix(n);
        for (std::size_t n; (n = white(text, true, utf8)) != 0u;) text.remove_suffix(n);
        return text;
    }
    // A key as a switched-off line may name one: letters, digits, '_' and '.'. (The launcher also takes
    // letters outside ASCII; no key of the game has one, and a name with one is never asked for here.)
    static bool is_key_name(std::string_view text) noexcept {
        if (text.empty()) return false;
        for (const char c : text)
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return false;
        return true;
    }
    static std::string strip_comment(std::string_view text) {
        const auto cut = text.find_first_of(";#");
        return std::string(cut == std::string_view::npos ? text : text.substr(0u, cut));
    }
    static std::string leading_space(std::string_view text) {
        std::size_t i = 0;
        while (i < text.size() && is_space(text[i])) ++i;
        return std::string(text.substr(0u, i));
    }
    static Line parse_line(std::string text_in, std::string section, bool utf8) {
        Line line; line.text = std::move(text_in); line.section = std::move(section);
        const std::string_view text = line.text;
        std::size_t first = 0;
        while (first < text.size() && is_space(text[first])) ++first;
        if (first == text.size()) { line.kind = Kind::Blank; return line; }
        if (text[first] == ';' || text[first] == '#') {
            line.kind = Kind::Comment;
            std::size_t p = first;
            while (p < text.size() && (text[p] == ';' || text[p] == '#')) ++p;
            while (p < text.size() && is_space(text[p])) ++p;
            const auto eq = text.find('=', p);
            if (eq != std::string_view::npos && eq > p) {
                const auto key = trim(text.substr(p, eq - p), utf8);
                if (is_key_name(key)) {
                    line.has_key = true;
                    line.key = std::string(key);
                    line.raw_value = std::string(trim(text.substr(eq + 1u), utf8));
                    const auto stripped = strip_comment(text.substr(eq + 1u));
                    line.value = std::string(trim(stripped, utf8));
                }
            }
            return line;
        }
        const auto comment = text.find_first_of(";#", first);
        std::size_t body_end = comment == std::string_view::npos ? text.size() : comment;
        while (body_end > first && is_space(text[body_end - 1u])) --body_end;
        if (text[first] == '[') {
            line.kind = Kind::Section;
            line.section = body_end - first >= 2u && text[body_end - 1u] == ']' ? std::string(trim(text.substr(first + 1u, body_end - first - 2u), utf8)) : std::string();
            return line;
        }
        const auto equals = text.find('=', first);
        if (equals == std::string_view::npos || equals >= body_end) { line.kind = Kind::Other; return line; }
        line.kind = Kind::KeyValue;
        line.has_key = true;
        line.key = std::string(trim(text.substr(first, equals - first), utf8));
        std::size_t vs = equals + 1u;
        while (vs < body_end && is_space(text[vs])) ++vs;
        line.value_start = vs;
        line.value_length = body_end - vs;
        line.value = std::string(text.substr(vs, body_end - vs));
        line.raw_value = std::string(trim(text.substr(equals + 1u), utf8));
        return line;
    }
    void parse_text(std::string_view text) {
        std::size_t crlf = 0, lf = 0, pos = 0;
        std::string section;
        while (pos < text.size()) {
            std::size_t end = pos;
            while (end < text.size() && text[end] != '\n' && text[end] != '\r') ++end;
            std::string eol;
            if (end < text.size()) {
                if (text[end] == '\r' && end + 1u < text.size() && text[end + 1u] == '\n') { eol = "\r\n"; ++crlf; }
                else if (text[end] == '\n') { eol = "\n"; ++lf; }
                else eol = "\r";
            }
            auto line = parse_line(std::string(text.substr(pos, end - pos)), section, utf8);
            line.eol = eol;
            if (line.kind == Kind::Section) section = line.section;
            lines.push_back(std::move(line));
            pos = end + eol.size();
        }
        new_line = lf > crlf ? "\n" : "\r\n";
    }
    const Line *find_live(std::string_view section, std::string_view key) const {
        const Line *found = nullptr;   // the game takes the last line that names the key
        for (const auto &line : lines)
            if (line.kind == Kind::KeyValue && same(line.section, section) && same(line.key, key)) found = &line;
        return found;
    }
    Line *find_live(std::string_view section, std::string_view key) { return const_cast<Line *>(std::as_const(*this).find_live(section, key)); }
    void replace(Line &line, const std::string &text) {
        auto parsed = parse_line(text, line.section, utf8);
        // The section stays the one the line was in, as in the launcher.
        parsed.section = std::move(line.section);
        parsed.eol = std::move(line.eol);
        line = std::move(parsed);
    }
    // UTF-8 text in the file's encoding: as it is, or for a Latin-1 file one byte per character, '?' for a
    // character Latin-1 does not have (two for one outside the first 65,536, which the launcher holds as a pair).
    std::string encoded(std::string_view text) const {
        if (utf8 || !valid_utf8(text)) return std::string(text);
        std::string out;
        for (std::size_t i = 0; i < text.size();) {
            const auto b = static_cast<unsigned char>(text[i]);
            const std::size_t more = b < 0x80u ? 0u : b < 0xE0u ? 1u : b < 0xF0u ? 2u : 3u;
            std::uint32_t code = more == 0u ? b : more == 1u ? b & 0x1Fu : more == 2u ? b & 0x0Fu : b & 0x07u;
            for (std::size_t k = 1; k <= more; ++k) code = code << 6u | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
            if (code <= 0xFFu) out.push_back(static_cast<char>(static_cast<unsigned char>(code)));
            else out += code > 0xFFFFu ? "??" : "?";
            i += more + 1u;
        }
        return out;
    }
};

// ---- the file --------------------------------------------------------------------------------------

struct IniChange {
    std::string section, key, value;   // the value in UTF-8
    bool raw{};                        // a path: replaces everything after '='
    bool unset{};                      // the key is switched off instead (value unused)
};
struct IniWriteResult {
    bool ok{};            // the file holds the changes (also when it held them already)
    bool changed{};       // the file was written
    bool swapped{};       // through the temporary file and the swap; false: written in place (a folder that refuses the swap)
    bool gate{};          // the mutex was held
    bool remade{};        // the file was gone although this program had seen it: written again whole from what it knew
    unsigned attempts{};  // 1, or more when the file changed between the read and the swap
    std::string error;    // why it failed
};
// What a writer last saw of the file: whether it was there, and its bytes. ini_change_file keeps it current.
struct IniKnown {
    bool existed{};
    std::string text;
};

namespace ini_file {
// The bytes of the file; nothing when it cannot be opened (`exists` says whether it is there).
inline std::optional<std::string> read(const std::filesystem::path &file, bool *exists = nullptr) {
    if (exists) *exists = false;
    const HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (exists) *exists = error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
        return std::nullopt;
    }
    if (exists) *exists = true;
    std::string bytes;
    char buffer[16384];
    bool good = true;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(handle, buffer, sizeof(buffer), &got, nullptr)) { good = false; break; }
        if (!got) break;
        bytes.append(buffer, got);
    }
    CloseHandle(handle);
    return good ? std::optional<std::string>(std::move(bytes)) : std::nullopt;
}
inline bool write(const std::filesystem::path &file, std::string_view bytes) {
    const HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    bool good = true;
    for (std::size_t done = 0; good && done < bytes.size();) {
        DWORD put = 0;
        good = WriteFile(handle, bytes.data() + done, static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1u << 20)), &put, nullptr) && put != 0u;
        done += put;
    }
    good = FlushFileBuffers(handle) && good;
    return CloseHandle(handle) && good;
}
// How long a file that should be there is waited for, and how often it is looked for.
inline constexpr DWORD patience_ms = 500u, patience_step_ms = 20u;
// read(), patient: a file that is there and cannot be opened (another program has it to itself for a moment)
// is tried again for up to `wait_ms`; with `expected`, so is a file that is not there.
inline std::optional<std::string> read_patiently(const std::filesystem::path &file, bool *exists, bool expected, DWORD wait_ms = patience_ms) {
    const ULONGLONG started = GetTickCount64();
    for (;;) {
        bool found = false;
        auto bytes = read(file, &found);
        if (exists) *exists = found;
        if (bytes || (!found && !expected) || GetTickCount64() - started >= wait_ms) return bytes;
        Sleep(patience_step_ms);
    }
}
// write(), patient: tried again for up to `wait_ms` when the file cannot be opened for writing.
inline bool write_patiently(const std::filesystem::path &file, std::string_view bytes, DWORD wait_ms = patience_ms / 2u) {
    const ULONGLONG started = GetTickCount64();
    for (;;) {
        if (write(file, bytes)) return true;
        if (GetTickCount64() - started >= wait_ms) return false;
        Sleep(patience_step_ms);
    }
}
inline bool there(const std::filesystem::path &file) noexcept { return GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES; }
// The gate of ChiliNative.ini, held while the object lives. A wait that runs out is not an error: the
// writer goes on without the gate, and the check before the swap is what is left.
class Gate {
public:
    explicit Gate(DWORD wait_ms = 2000u) : handle_(CreateMutexW(nullptr, FALSE, ini_gate_name)) {
        if (!handle_) return;
        const DWORD result = WaitForSingleObject(handle_, wait_ms);
        held_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
    ~Gate() { if (handle_) { if (held_) ReleaseMutex(handle_); CloseHandle(handle_); } }
    Gate(const Gate &) = delete;
    Gate &operator=(const Gate &) = delete;
    bool held() const noexcept { return held_; }
private:
    HANDLE handle_{};
    bool held_{};
};
inline std::string apply(std::string_view before, std::span<const IniChange> changes) {
    auto doc = IniDocument::parse(before);
    for (const auto &change : changes) {
        if (change.unset) doc.unset(change.section, change.key);
        else doc.set(change.section, change.key, change.value, change.raw);
    }
    return doc.bytes();
}
}

// Reads the file as it is on disk now, makes the changes, and swaps the result in. A missing file is made
// (with its folder); a file `known` says this program has seen is waited for and, if it stays away, made
// again whole from `known` with the changes. Nothing is written when the file holds the changes already.
// `between`, for tests, runs after the new text is in the temporary file and before the swap. `known`, when
// given, is what the caller last saw of the file; it holds what the file holds when this returns.
template <typename Between>
IniWriteResult ini_change_file(const std::filesystem::path &file, std::span<const IniChange> changes, Between between, IniKnown *known) {
    IniWriteResult result;
    for (const auto &change : changes)
        if ((change.section + change.key + (change.unset ? std::string() : change.value)).find_first_of("\r\n") != std::string::npos) {
            result.error = "a setting cannot hold a line break";
            return result;
        }
    ini_file::Gate gate;
    result.gate = gate.held();
    auto temporary = file;
    temporary += L".game.tmp";
    constexpr unsigned most = 5u;
    const auto seen = [&](const std::string &text) { if (known) { known->existed = true; known->text = text; } };
    for (result.attempts = 1u;; ++result.attempts) {
        const bool expected = known && known->existed;
        bool exists = false;
        const auto read = ini_file::read_patiently(file, &exists, expected);
        if (!read && exists) { result.error = "the settings file cannot be read"; return result; }
        // Gone although this program has seen it: never taken for a file that was not there yet.
        const bool remade = !read && expected;
        const std::string before = read ? *read : remade ? known->text : std::string();
        if (read) seen(before);
        const std::string after = ini_file::apply(before, changes);
        if (after == before && !remade) { result.ok = true; return result; }
        std::error_code status;
        if (!exists && file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), status);
        if (!ini_file::write(temporary, after)) {
            DeleteFileW(temporary.c_str());
            // A folder that takes no second file: in place.
            if (!ini_file::write_patiently(file, after)) { result.error = "the settings file cannot be written"; return result; }
            seen(after);
            result.ok = result.changed = true; result.remade = remade;
            return result;
        }
        between();
        // Somebody who does not take the gate wrote in the meantime, or the file came back or went: once
        // more, on what is there now.
        bool still = false;
        const auto now = ini_file::read(file, &still);
        if (still != exists || (exists && now.value_or(std::string()) != before)) {
            DeleteFileW(temporary.c_str());
            if (result.attempts < most) continue;
            result.error = "the settings file keeps changing";
            return result;
        }
        // Written beside the file first, then swapped in, so a crash cannot leave half a settings file.
        bool swapped = false;
        if (exists) {
            // A program that holds the file for a moment makes the swap fail: tried again before anything else.
            for (unsigned turn = 0u; turn < 4u && !swapped && ini_file::there(file); ++turn) {
                if (turn) Sleep(ini_file::patience_step_ms);
                swapped = ReplaceFileW(file.c_str(), temporary.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE;
            }
            // A swap can fail after it has moved the old file away: the new text is then moved into its place.
            if (!swapped && !ini_file::there(file) && ini_file::there(temporary))
                swapped = MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
        } else {
            swapped = MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
            // Somebody made the file in the meantime: once more, on what is there now.
            if (!swapped && ini_file::there(file)) {
                DeleteFileW(temporary.c_str());
                if (result.attempts < most) continue;
                result.error = "the settings file keeps changing";
                return result;
            }
        }
        if (swapped) { seen(after); result.ok = result.changed = result.swapped = true; result.remade = remade; return result; }
        // Some folders (network shares, synced drives) refuse the swap: in place. `after` is the whole file.
        const bool written = ini_file::write_patiently(file, after);
        DeleteFileW(temporary.c_str());
        if (!written) { result.error = "the settings file cannot be written"; return result; }
        seen(after);
        result.ok = result.changed = true; result.remade = remade;
        return result;
    }
}
template <typename Between>
    requires std::invocable<Between &>
IniWriteResult ini_change_file(const std::filesystem::path &file, std::span<const IniChange> changes, Between between) {
    return ini_change_file(file, changes, std::move(between), static_cast<IniKnown *>(nullptr));
}
inline IniWriteResult ini_change_file(const std::filesystem::path &file, std::span<const IniChange> changes, IniKnown *known = nullptr) {
    return ini_change_file(file, changes, [] {}, known);
}
inline IniWriteResult ini_change_file(const std::filesystem::path &file, const IniChange &change, IniKnown *known = nullptr) {
    return ini_change_file(file, std::span<const IniChange>(&change, 1u), known);
}
}
