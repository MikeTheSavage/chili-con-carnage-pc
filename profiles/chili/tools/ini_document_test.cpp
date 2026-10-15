// host/ini_document.hpp: ChiliNative.ini read with every line kept, one key changed in place, and the file
// written by read, change and swap under the gate the game shares with the launcher.
//
// The document is the C++ twin of the launcher's launcher/src/IniDocument.cs, and this test holds the two
// against each other. The launcher's --every-value mode (scripts/check_launcher_settings.ps1) puts every
// row of every page to each of its values and saves; since the settings-page work it also keeps every save:
// the file before, the changes in the order they were made, the file after (transitions.tsv, transitions\).
// For each of those saves the game's writer must make the same bytes from the same file and changes.
//
//   chili_ini_document_test <scratch folder> [<fixture folder>]
//       the cases written by hand, and the launcher's saves as tools/ini_fixture keeps them: the file the
//       launcher began with, each save's changes, and the length and two hashes of what the launcher wrote
//   chili_ini_document_test --launcher-files <folder>
//       the same against the launcher's own files, byte for byte (the folder --every-value wrote into)
//   chili_ini_document_test --make-fixture <folder> <fixture folder>
//       checks as --launcher-files does and then writes the fixture from that folder
#include "ini_document.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
using chili::IniChange;
using chili::IniDocument;
unsigned checks, failures;
const bool keep_going = [] { const char *value = std::getenv("CHILI_TEST_KEEP_GOING"); return value && *value == '1'; }();
void require(bool ok, const std::string &what) {
    ++checks;
    if (ok) return;
    if (!keep_going) throw std::runtime_error(what);
    ++failures;
    std::cerr << "FAILED CHECK: " << what << '\n';
}
std::string shown(std::string_view bytes) {
    std::string out;
    for (const char c : bytes) {
        if (c == '\r') out += "\\r"; else if (c == '\n') out += "\\n\n"; else if (c == '\t') out += "\\t";
        else if (static_cast<unsigned char>(c) < 0x20u || static_cast<unsigned char>(c) >= 0x7Fu) { char hex[8]; std::snprintf(hex, sizeof(hex), "\\x%02X", static_cast<unsigned char>(c)); out += hex; }
        else out.push_back(c);
    }
    return out;
}
void same_bytes(std::string_view got, std::string_view expected, const std::string &what) {
    if (got != expected && keep_going) std::cerr << "--- got ---\n" << shown(got) << "\n--- expected ---\n" << shown(expected) << '\n';
    require(got == expected, what + (got == expected ? "" : "\n--- got ---\n" + shown(got) + "\n--- expected ---\n" + shown(expected)));
}
// One change to the text, as the document makes it.
std::string changed(std::string_view text, std::string_view section, std::string_view key, std::string_view value, bool raw = false) {
    auto doc = IniDocument::parse(text);
    doc.set(section, key, value, raw);
    return doc.bytes();
}
std::string switched_off(std::string_view text, std::string_view section, std::string_view key) {
    auto doc = IniDocument::parse(text);
    doc.unset(section, key);
    return doc.bytes();
}

// ---- the document ----------------------------------------------------------------------------------

void document() {
    // Every byte comes back: a byte order mark, three kinds of line end, a last line without one, tabs,
    // comments of both kinds, a line that is neither a key nor a comment.
    const std::string messy = "\xEF\xBB\xBF; top\r\n\r\n[One]\nA=1\r\n\tB = two  ; why\r# old style\rnot a setting\n [ Two ] ; header\n  C=3";
    same_bytes(IniDocument::parse(messy).bytes(), messy, "a file comes back byte for byte");
    same_bytes(IniDocument::parse("").bytes(), "", "an empty file stays empty");
    {
        const auto doc = IniDocument::parse(messy);
        require(doc.has_bom && doc.utf8, "the byte order mark is seen");
        require(doc.lines.size() == 9u, "nine lines");
        require(doc.get("One", "A") == std::string("1") && doc.get("one", "a") == std::string("1"), "a value, names without case");
        require(doc.get("One", "B") == std::string("two"), "a value without its spaces and its comment");
        require(doc.get("Two", "C") == std::string("3"), "a header with spaces and a comment opens its section");
        require(!doc.get("One", "C") && !doc.has("Two", "A") && doc.has("One", "B"), "a key is looked for in its section only");
        require(doc.has_section("TWO") && !doc.has_section("Three"), "sections");
        require(doc.new_line == "\r\n", "as many CRLF as LF: CRLF");
    }
    require(IniDocument::parse("a=1\nb=2\n").new_line == "\n", "LF only: LF");
    require(IniDocument::parse("a=1").new_line == "\r\n" && IniDocument::parse("").new_line == "\r\n", "no line end at all: CRLF");
    require(IniDocument::parse("a=1\rb=2\r").new_line == "\r\n", "a lone CR counts for neither");

    // A live line keeps its spelling, its spacing and its comment.
    same_bytes(changed("[S]\r\n  Key =  old   ; note\r\n", "S", "Key", "new"), "[S]\r\n  Key =  new   ; note\r\n", "the value alone changes");
    same_bytes(changed("[S]\nkey=old#x\n", "s", "KEY", "longer value"), "[S]\nkey=longer value#x\n", "the file's spelling of the key stays");
    same_bytes(changed("[S]\nKey=\n", "S", "Key", "v"), "[S]\nKey=v\n", "an empty value is filled");
    same_bytes(changed("[S]\nKey=   ; c\n", "S", "Key", "v"), "[S]\nKey=v   ; c\n", "an empty value before a comment: the value goes right after the sign");
    same_bytes(changed("[S]\nKey=v\n", "S", "Key", ""), "[S]\nKey=\n", "a value can be emptied");
    // The game takes the last line that names a key, so that is the one that changes.
    same_bytes(changed("[S]\nK=1\nK=2\n[T]\nK=3\n", "S", "K", "9"), "[S]\nK=1\nK=9\n[T]\nK=3\n", "the last live line of the section");
    same_bytes(changed("[S]\nK=1\n[T]\nx=1\n[S]\nK=2\n", "S", "K", "9"), "[S]\nK=1\n[T]\nx=1\n[S]\nK=9\n", "a section written twice: the last line of both");
    // A switched-off line becomes live: the first one, with the key as the file spells it.
    same_bytes(changed("[S]\r\n; Key=old\r\n; Key=older\r\n", "S", "key", "new"), "[S]\r\nKey=new\r\n; Key=older\r\n", "the first switched-off line becomes live");
    same_bytes(changed("[S]\n  ;;# Key = old ; why\n", "S", "Key", "new"), "[S]\n  Key=new\n", "its indent stays, its comment marks and its old comment go");
    same_bytes(changed("[S]\n; a sentence = with a sign\n", "S", "Key", "v"), "[S]\n; a sentence = with a sign\nKey=v\n", "a comment that is a sentence is not a switched-off line");
    same_bytes(changed("[S]\n; Key=1\n[T]\n", "T", "Key", "v"), "[S]\n; Key=1\n[T]\nKey=v\n", "a switched-off line of another section is left alone");
    same_bytes(changed("[S]\n; K=1\nK=2\n", "S", "K", "3"), "[S]\n; K=1\nK=3\n", "a live line goes before a switched-off one");
    // A new line goes after the last line of its section that is not blank.
    same_bytes(changed("[S]\r\nA=1\r\n\r\n\r\n[T]\r\nB=2\r\n", "S", "New", "v"), "[S]\r\nA=1\r\nNew=v\r\n\r\n\r\n[T]\r\nB=2\r\n", "at the end of its section, before the blank lines");
    same_bytes(changed("[S]\n", "S", "New", "v"), "[S]\nNew=v\n", "an empty section");
    same_bytes(changed("[S]", "S", "New", "v"), "[S]\r\nNew=v\r\n", "a header without a line end gets one (CRLF when the file has none)");
    same_bytes(changed("[S]\nA=1", "S", "New", "v"), "[S]\nA=1\nNew=v\n", "a last line without a line end gets the file's");
    same_bytes(changed("[S]\nA=1\n; note\n", "S", "New", "v"), "[S]\nA=1\n; note\nNew=v\n", "after the section's last comment");
    same_bytes(changed("[S]\nA=1\n[T]\n[S]\nB=2\n", "S", "New", "v"), "[S]\nA=1\n[T]\n[S]\nB=2\nNew=v\n", "a section written twice: after the last line of the second");
    same_bytes(changed("[S]\nA=1\n[T]\nB=1\n[S]\n", "S", "New", "v"), "[S]\nA=1\n[T]\nB=1\n[S]\nNew=v\n", "a section written twice, the second empty: after its header");
    // A new section goes to the end of the file, after one blank line.
    same_bytes(changed("[S]\r\nA=1\r\n", "T", "K", "v"), "[S]\r\nA=1\r\n\r\n[T]\r\nK=v\r\n", "a new section after a blank line");
    same_bytes(changed("[S]\nA=1\n\n", "T", "K", "v"), "[S]\nA=1\n\n[T]\nK=v\n", "no second blank line");
    same_bytes(changed("[S]\nA=1", "T", "K", "v"), "[S]\nA=1\n\n[T]\nK=v\n", "the last line gets its line end first");
    same_bytes(changed("", "T", "K", "v"), "[T]\r\nK=v\r\n", "an empty file");
    same_bytes(changed("\xEF\xBB\xBF", "T", "K", "v"), "\xEF\xBB\xBF[T]\r\nK=v\r\n", "a file that is only a byte order mark");
    same_bytes(changed("K=1\n", "", "K", "2"), "K=2\n", "a key before the first section has the empty section name");
    // Raw (a path): everything after '=' is the value.
    same_bytes(changed("[Disc]\nPath = \"C:\\old;1\" ; c\n", "Disc", "Path", "D:\\new #2.iso", true), "[Disc]\nPath =D:\\new #2.iso\n", "raw: the value replaces everything after the sign");
    same_bytes(changed("[Disc]\n; Path=C:\\Games\\x.iso\n", "Disc", "Path", "disc0", true), "[Disc]\nPath=disc0\n", "raw: the switched-off example becomes the line");
    {
        const auto doc = IniDocument::parse("[Disc]\nPath = \"C:\\a;b #c\\d.iso\"  \nTwo=\"a\" \"b\"\nBare= x;y \n");
        require(doc.get_raw("Disc", "Path") == std::string("C:\\a;b #c\\d.iso"), "raw: ';' and '#' belong to a path, one pair of quotes goes");
        require(doc.get("Disc", "Path") == std::string("\"C:\\a"), "not raw: the value ends at the ';'");
        require(doc.get_raw("Disc", "Two") == std::string("\"a\" \"b\""), "raw: several quoted words keep their quotes");
        require(doc.get_raw("Disc", "Bare") == std::string("x;y") && !doc.get_raw("Disc", "None"), "raw: trimmed; nothing for a key that is not there");
    }
    // The game's own [Disc] text (host/launcher.cpp) and the disc the player chose: the commented example
    // becomes the line, and the path reads back whole.
    {
        auto doc = IniDocument::parse("Version=2\n\n[Disc]\n; The game disc: your own copy, as an .iso file or as the folder of an unpacked disc.\n; Remove the line to be asked again.\n; Path=C:\\Games\\Chili Con Carnage (USA).iso\n");
        doc.set("Disc", "Path", "D:\\dumps\\my #1; copy.iso", true);
        same_bytes(doc.bytes(), "Version=2\n\n[Disc]\n; The game disc: your own copy, as an .iso file or as the folder of an unpacked disc.\n; Remove the line to be asked again.\nPath=D:\\dumps\\my #1; copy.iso\n",
            "the disc: the example line of the game's own text becomes the line");
        require(doc.get_raw("Disc", "Path") == std::string("D:\\dumps\\my #1; copy.iso"), "the disc: read back whole");
        doc.set("Disc", "Path", "disc0", true);
        require(doc.get_raw("Disc", "Path") == std::string("disc0") && doc.lines.size() == 6u, "the disc: chosen again, the same line");
    }
    // Switching off.
    same_bytes(switched_off("[S]\n  K=1 ; c\nK=2\n[T]\nK=3\n", "S", "K"), "[S]\n  ; K=1 ; c\n; K=2\n[T]\nK=3\n", "every live line of the key gets '; ' in front, after its indent");
    {
        auto doc = IniDocument::parse("[S]\nK=1\n");
        doc.unset("S", "K");
        require(!doc.has("S", "K") && doc.mentions("S", "K") && !doc.mentions("S", "L"), "a switched-off key is mentioned and not live");
        doc.set("S", "K", "2");
        same_bytes(doc.bytes(), "[S]\nK=2\n", "and comes back in its place");
    }
    // A file that is not UTF-8 is Latin-1: one byte per character, also for what is written into it.
    {
        const std::string latin = "[S]\nName=caf\xE9\n";
        auto doc = IniDocument::parse(latin);
        require(!doc.utf8, "a lone E9 is not UTF-8");
        same_bytes(doc.bytes(), latin, "and comes back as it was");
        doc.set("S", "Name", "na\xC3\xAFve \xE2\x82\xAC \xF0\x9F\x8E\xB2");
        same_bytes(doc.bytes(), "[S]\nName=na\xEFve ? ??\n", "written in Latin-1: a character it lacks is '?', one outside the first plane two");
        auto utf = IniDocument::parse("[S]\nName=caf\xC3\xA9\n");
        require(utf.utf8, "C3 A9 is UTF-8");
        utf.set("S", "Name", "na\xC3\xAFve \xE2\x82\xAC");
        same_bytes(utf.bytes(), "[S]\nName=na\xC3\xAFve \xE2\x82\xAC\n", "written as it is into a UTF-8 file");
    }
    for (const char *bad : {"\xC0\x80", "\xE0\x80\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xF8\x88\x80\x80\x80", "\xC3", "\xE2\x82", "\x80"})
        require(!IniDocument::valid_utf8(bad), "not UTF-8: overlong forms, surrogates, beyond U+10FFFF, cut-off sequences, a lone tail byte");
    for (const char *good : {"", "plain", "\xC2\x80", "\xDF\xBF", "\xE0\xA0\x80", "\xEF\xBF\xBF", "\xF0\x90\x80\x80", "\xF4\x8F\xBF\xBF"})
        require(IniDocument::valid_utf8(good), "UTF-8: the first and last sequence of each length");
    // Spaces round a name as the launcher trims them: every Unicode space, not only the blank and the tab.
    require(IniDocument::parse("[S]\nKey\xC2\xA0=v\n").get("S", "Key") == std::string("v"), "a no-break space after a key (UTF-8)");
    require(IniDocument::parse("[S]\n\xE2\x80\x83Key\xE3\x80\x80=v\n").get("S", "Key") == std::string("v"), "an em space before a key and an ideographic space after it");
    require(IniDocument::parse("[S]\n\x0BKey\x0C=v\n").get("S", "Key") == std::string("v"), "a vertical tab and a form feed round a key");
    require(!IniDocument::parse("[S]\nKey\xE2\x80\x8B=v\n").get("S", "Key"), "a zero-width space is not a space: the key is another one");
    require(IniDocument::parse("[S]\nKey\xA0=v\xE9\n").get("S", "Key") == std::string("v\xE9"), "a no-break space after a key (Latin-1)");
    require(IniDocument::parse("[\xC2\xA0S\xE2\x80\x89]\nK=v\n").get("S", "K") == std::string("v"), "spaces inside a header's brackets");
    require(IniDocument::parse("[S]\nKey=\xC2\xA0v\n").get("S", "Key") == std::string("\xC2\xA0v"), "the value keeps what is not a blank or a tab");
    same_bytes(changed("[S]\nKey\xC2\xA0= old\n", "S", "Key", "new"), "[S]\nKey\xC2\xA0= new\n", "and such a line is changed in place");
    // Lines that are not settings stay as they are, whatever is asked.
    same_bytes(changed("[S\nK=1\n", "S", "K", "2"), "[S\nK=1\n\n[S]\nK=2\n", "a header that is not closed opens the section with no name");
    same_bytes(changed("[S]\n=v\nword\n", "S", "K", "2"), "[S]\n=v\nword\nK=2\n", "a line with no name and a line with no sign");
}

// ---- the file --------------------------------------------------------------------------------------

std::string file_bytes(const std::filesystem::path &file) { return chili::ini_file::read(file).value_or("<cannot be read>"); }
void put(const std::filesystem::path &file, std::string_view bytes) { require(chili::ini_file::write(file, bytes), "the test wrote " + file.string()); }

void file(const std::filesystem::path &scratch) {
    std::error_code status;
    std::filesystem::remove_all(scratch, status);
    std::filesystem::create_directories(scratch);
    const auto ini = scratch / "ChiliNative.ini";
    auto temporary = ini;
    temporary += L".game.tmp";
    // One change, swapped in.
    put(ini, "; mine\r\n[Rendering]\r\n; Anisotropy=16\r\n");
    auto result = chili::ini_change_file(ini, IniChange{"Rendering", "Anisotropy", "4"});
    require(result.ok && result.changed && result.swapped && result.gate && result.attempts == 1u, "a change is swapped in under the gate at the first attempt");
    same_bytes(file_bytes(ini), "; mine\r\n[Rendering]\r\nAnisotropy=4\r\n", "the file holds the change and everything else");
    require(!std::filesystem::exists(temporary), "the temporary file is gone");
    // The same again: nothing is written.
    const auto stamp = std::filesystem::last_write_time(ini);
    result = chili::ini_change_file(ini, IniChange{"Rendering", "Anisotropy", "4"});
    require(result.ok && !result.changed && std::filesystem::last_write_time(ini) == stamp, "a file that holds the change already is not written");
    // Several at once, in order; one switched off.
    const std::vector<IniChange> several{{"Rendering", "SSAO", "low"}, {"Audio", "BufferMs", "90"}, {"Rendering", "Anisotropy", "", false, true}, {"Rendering", "MSAA", "2"}};
    result = chili::ini_change_file(ini, several);
    require(result.ok && result.changed, "several changes");
    same_bytes(file_bytes(ini), "; mine\r\n[Rendering]\r\n; Anisotropy=4\r\nSSAO=low\r\nMSAA=2\r\n\r\n[Audio]\r\nBufferMs=90\r\n", "made in the order given");
    // A file that is not there is made, with its folder.
    const auto fresh = scratch / "new folder" / "ChiliNative.ini";
    result = chili::ini_change_file(fresh, IniChange{"Disc", "Path", "C:\\Games\\a;b.iso", true});
    require(result.ok && result.changed && result.swapped, "a missing file is made");
    same_bytes(file_bytes(fresh), "[Disc]\r\nPath=C:\\Games\\a;b.iso\r\n", "with the section and the line");
    result = chili::ini_change_file(scratch / "never.ini", IniChange{"Disc", "Path", "", false, true});
    require(result.ok && !result.changed && !std::filesystem::exists(scratch / "never.ini"), "switching a key off in a file that is not there makes no file");
    // A line break in a value would make two lines: refused, the file untouched.
    const auto before = file_bytes(ini);
    result = chili::ini_change_file(ini, IniChange{"Rendering", "SSAO", "high\r\nBackend=software"});
    require(!result.ok && !result.error.empty() && file_bytes(ini) == before, "a value with a line break is refused");
    // Another writer that does not take the gate changes the file between this writer's read and its swap:
    // the change is made again on the new text, and nothing of the other writer's is lost.
    put(ini, "[Input]\nPrompts=auto\nShowInput=false\n");
    unsigned meddled = 0u;
    result = chili::ini_change_file(ini, std::vector<IniChange>{{"Input", "Prompts", "keyboard"}}, [&] {
        if (meddled++ == 0u) put(ini, "[Input]\nPrompts=auto\nShowInput=true\n; the other writer was here\n");
    });
    require(result.ok && result.changed && result.attempts == 2u && meddled == 2u, "the file changed before the swap: a second attempt");
    same_bytes(file_bytes(ini), "[Input]\nPrompts=keyboard\nShowInput=true\n; the other writer was here\n", "both writers' changes are in the file");
    // A file that never stops changing: given up after five attempts, the other writer's last text stays.
    unsigned turn = 0u;
    result = chili::ini_change_file(ini, std::vector<IniChange>{{"Input", "Prompts", "psp"}}, [&] { put(ini, "[Input]\nPrompts=auto\n; turn " + std::to_string(++turn) + "\n"); });
    require(!result.ok && result.attempts == 5u && turn == 5u, "five attempts, then an error");
    same_bytes(file_bytes(ini), "[Input]\nPrompts=auto\n; turn 5\n", "the file is the other writer's");
    require(!std::filesystem::exists(temporary), "no temporary file is left behind");
    // A file that cannot be replaced or written (read-only): an error, the file as it was.
    put(ini, "[Input]\nPrompts=auto\n");
    SetFileAttributesW(ini.c_str(), FILE_ATTRIBUTE_READONLY);
    result = chili::ini_change_file(ini, IniChange{"Input", "Prompts", "gamepad"});
    SetFileAttributesW(ini.c_str(), FILE_ATTRIBUTE_NORMAL);
    require(!result.ok && !result.error.empty(), "a read-only file: an error");
    same_bytes(file_bytes(ini), "[Input]\nPrompts=auto\n", "and the file as it was");
    require(!std::filesystem::exists(temporary), "no temporary file is left behind after the failure");
    // The gate: a writer waits for whoever holds it.
    {
        std::atomic<bool> holding{false}, release{false};
        std::thread holder([&] { chili::ini_file::Gate gate; holding = gate.held(); while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(2)); });
        while (!holding) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto started = std::chrono::steady_clock::now();
        std::thread letting_go([&] { std::this_thread::sleep_for(std::chrono::milliseconds(150)); release = true; });
        result = chili::ini_change_file(ini, IniChange{"Input", "Prompts", "keyboard"});
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        holder.join(); letting_go.join();
        require(result.ok && result.gate && waited >= 140, "a writer waits while another holds the gate (" + std::to_string(waited) + " ms)");
    }
    // A file the writer has seen and that is gone is not a file that was never there: the writer waits for
    // it, and if it stays away writes it again whole, never its own lines alone.
    {
        const std::string whole = "; the player's own file\r\n[Disc]\r\nPath=C:\\Games\\Chili.iso\r\n\r\n[Input.Mouse]\r\nSensitivityX=0.08\r\nInvertY=true\r\n";
        const auto with = [&](const char *turn_speed) { std::string text = whole; text.replace(text.find("0.08"), 4u, turn_speed); return text; };
        auto away = ini;
        away += L".away";
        put(ini, whole);
        chili::IniKnown known;
        result = chili::ini_change_file(ini, IniChange{"Input.Mouse", "SensitivityX", "0.08"}, &known);
        require(result.ok && !result.changed && known.existed && known.text == whole, "a writer that found nothing to change knows the file all the same");
        // Gone for good (another writer's swap failed after it had moved the file away).
        require(DeleteFileW(ini.c_str()) != FALSE, "the test removed the file");
        auto started = std::chrono::steady_clock::now();
        result = chili::ini_change_file(ini, IniChange{"Input.Mouse", "SensitivityX", "0.12"}, &known);
        auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        require(result.ok && result.changed && result.remade && waited >= 450, "gone: waited for (" + std::to_string(waited) + " ms), then made again");
        same_bytes(file_bytes(ini), with("0.12"), "made again whole, with the change");
        require(known.text == with("0.12") && !std::filesystem::exists(temporary), "the writer knows what it wrote; no temporary file");
        // Without what the writer knew the same change makes a file of two lines: that is the fault this guards against.
        require(DeleteFileW(ini.c_str()) != FALSE, "the test removed the file");
        result = chili::ini_change_file(ini, IniChange{"Input.Mouse", "SensitivityX", "0.12"});
        same_bytes(file_bytes(ini), "[Input.Mouse]\r\nSensitivityX=0.12\r\n", "a writer that never saw the file makes one of its own lines (the first start)");
        // Gone for a moment: it comes back while the writer waits, with another writer's change in it.
        put(ini, with("0.12"));
        require(MoveFileExW(ini.c_str(), away.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE, "the test moved the file away");
        {
            std::thread back([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
                std::string text = with("0.12");
                text.replace(text.find("InvertY=true"), 12u, "InvertY=false");
                (void)chili::ini_file::write(away, text);
                MoveFileExW(away.c_str(), ini.c_str(), MOVEFILE_REPLACE_EXISTING);
            });
            result = chili::ini_change_file(ini, IniChange{"Input.Mouse", "SensitivityX", "0.2"}, &known);
            back.join();
        }
        require(result.ok && result.changed && !result.remade, "gone for a moment: waited for, changed as it came back");
        {
            std::string expected = with("0.2");
            expected.replace(expected.find("InvertY=true"), 12u, "InvertY=false");
            same_bytes(file_bytes(ini), expected, "the other writer's line and this writer's change are both in the file");
        }
        // Gone between the read and the swap, and it stays away: the next attempt makes it again whole.
        put(ini, with("0.2"));
        result = chili::ini_change_file(ini, std::vector<IniChange>{{"Input.Mouse", "SensitivityX", "0.3"}}, [&] { DeleteFileW(ini.c_str()); }, &known);
        require(result.ok && result.remade && result.attempts == 2u, "gone before the swap: a second attempt, made again");
        same_bytes(file_bytes(ini), with("0.3"), "whole, with the change");
        // A program that has the file open without sharing it for a swap or a write, and lets go after a moment.
        {
            const HANDLE holder = CreateFileW(ini.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            require(holder != INVALID_HANDLE_VALUE, "the test holds the file open");
            std::thread letting_go([&] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); CloseHandle(holder); });
            result = chili::ini_change_file(ini, IniChange{"Input.Mouse", "SensitivityX", "0.4"}, &known);
            letting_go.join();
            require(result.ok && result.changed && !result.remade, "a reader that holds the file for a moment: the writer waits");
            same_bytes(file_bytes(ini), with("0.4"), "and the file is whole, with the change");
        }
        // One that never lets go: an error, the file as it was, and the writer still knows the whole text.
        {
            const HANDLE holder = CreateFileW(ini.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            require(holder != INVALID_HANDLE_VALUE, "the test holds the file open");
            result = chili::ini_change_file(ini, IniChange{"Input.Mouse", "SensitivityX", "0.5"}, &known);
            CloseHandle(holder);
            require(!result.ok && !result.error.empty() && known.existed && known.text == with("0.4"), "a reader that never lets go: an error, and the writer knows the file as it is");
            same_bytes(file_bytes(ini), with("0.4"), "the file as it was");
            require(!std::filesystem::exists(temporary), "no temporary file is left behind");
        }
        // A file that cannot be opened at all for a moment (another program has it to itself): waited for.
        {
            const HANDLE holder = CreateFileW(ini.c_str(), GENERIC_READ | GENERIC_WRITE, 0u, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            require(holder != INVALID_HANDLE_VALUE, "the test holds the file to itself");
            std::thread letting_go([&] { std::this_thread::sleep_for(std::chrono::milliseconds(80)); CloseHandle(holder); });
            result = chili::ini_change_file(ini, IniChange{"Input.Mouse", "SensitivityX", "0.6"}, &known);
            letting_go.join();
            require(result.ok && result.changed, "a file another program has to itself for a moment: waited for (" + result.error + ")");
            same_bytes(file_bytes(ini), with("0.6"), "and changed");
        }
        DeleteFileW(away.c_str());
    }
    // Two writers, each with its own keys, 150 saves each: every key holds its writer's last value, the
    // lines the file began with are all there, and no attempt had to be repeated (the gate did its work).
    {
        std::string start = "; a comment that must stay\r\n[Rendering]\r\n; Anisotropy=16\r\n\r\n[Input.Mouse]\r\nSensitivityX=0.08\r\n";
        put(ini, start);
        std::atomic<unsigned> repeats{0u}, errors{0u};
        const auto writer = [&](bool game) {
            for (unsigned i = 1; i <= 150u; ++i) {
                const std::vector<IniChange> changes = game
                    ? std::vector<IniChange>{{"Input.Mouse", "SensitivityX", "0." + std::to_string(100u + i)}, {"Input.Gamepad", "Rumble", std::to_string(i)}}
                    : std::vector<IniChange>{{"Rendering", "Anisotropy", std::to_string(i)}, {"Audio", "BufferMs", std::to_string(20u + i)}};
                const auto done = chili::ini_change_file(ini, changes);
                if (!done.ok) ++errors;
                if (done.attempts > 1u) ++repeats;
            }
        };
        std::thread game(writer, true), launcher(writer, false);
        game.join(); launcher.join();
        const auto doc = IniDocument::parse(file_bytes(ini));
        require(errors == 0u && repeats == 0u, "two writers, 150 saves each: no error and no second attempt");
        require(doc.get("Input.Mouse", "SensitivityX") == std::string("0.250") && doc.get("Input.Gamepad", "Rumble") == std::string("150") &&
                doc.get("Rendering", "Anisotropy") == std::string("150") && doc.get("Audio", "BufferMs") == std::string("170"), "each key holds its writer's last value");
        require(doc.lines.front().text == "; a comment that must stay" && doc.lines.size() == 12u, "and the file has its lines, and no line twice");
    }
}

// ---- the launcher's saves --------------------------------------------------------------------------

std::uint64_t fnv(std::string_view bytes) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (const char c : bytes) { hash ^= static_cast<unsigned char>(c); hash *= 0x100000001B3ull; }
    return hash;
}
// A second, unrelated hash, so that two texts with one hash in common still differ in the other.
std::uint64_t mix(std::string_view bytes) {
    std::uint64_t hash = 0x9E3779B97F4A7C15ull;
    for (const char c : bytes) { hash = (hash ^ static_cast<unsigned char>(c)) * 0xFF51AFD7ED558CCDull; hash ^= hash >> 29u; }
    return hash;
}
std::string hex(std::uint64_t value) { char text[20]; std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value)); return text; }
std::string unescaped(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1u == text.size()) { out.push_back(text[i]); continue; }
        const char next = text[++i];
        out.push_back(next == 't' ? '\t' : next == 'r' ? '\r' : next == 'n' ? '\n' : next);
    }
    return out;
}
std::vector<std::string> fields(std::string_view line) {
    std::vector<std::string> out;
    for (;;) {
        const auto tab = line.find('\t');
        out.emplace_back(line.substr(0u, tab));
        if (tab == std::string_view::npos) return out;
        line.remove_prefix(tab + 1u);
    }
}
std::vector<std::string> lines_of(const std::string &text) {
    std::vector<std::string> out;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) { if (!line.empty() && line.back() == '\r') line.pop_back(); out.push_back(line); }
    return out;
}
struct Save { unsigned id{}, from{}; std::vector<IniChange> changes; std::size_t length{}; std::string hash, second; };
struct Recorded { std::vector<Save> saves; std::vector<std::pair<std::string, unsigned>> kept; std::size_t start_length{}; std::string start_hash, start_second; };
Recorded read_list(const std::string &text, bool fixture) {
    Recorded recorded;
    for (const auto &line : lines_of(text)) {
        if (line.empty() || line[0] == '#') continue;
        const auto f = fields(line);
        if (f[0] == "S" && f.size() == 4u) { recorded.start_length = std::stoull(f[1]); recorded.start_hash = f[2]; recorded.start_second = f[3]; }
        else if (f[0] == "T" && f.size() == (fixture ? 7u : 4u)) {
            Save save; save.id = static_cast<unsigned>(std::stoul(f[1])); save.from = static_cast<unsigned>(std::stoul(f[2]));
            save.changes.reserve(std::stoul(f[3]));
            if (fixture) { save.length = std::stoull(f[4]); save.hash = f[5]; save.second = f[6]; }
            recorded.saves.push_back(std::move(save));
        } else if (f[0] == "C" && f.size() == 5u && !recorded.saves.empty()) {
            recorded.saves.back().changes.push_back(IniChange{f[1], f[2], unescaped(f[4]), f[3] == "raw", f[3] == "unset"});
        } else if (f[0] == "K" && f.size() == 3u) recorded.kept.emplace_back(f[1], static_cast<unsigned>(std::stoul(f[2])));
        else throw std::runtime_error("a line of the list of saves is not understood: " + line);
    }
    return recorded;
}
std::string id4(unsigned id) { char text[8]; std::snprintf(text, sizeof(text), "%04u", id); return text; }

// The saves replayed by the game's writer. `folder`: the launcher's own files are compared byte for byte;
// without it the lengths and hashes of the fixture are. Returns what each save left.
std::map<unsigned, std::string> replay(const Recorded &recorded, const std::string &start, const std::filesystem::path *folder) {
    std::map<unsigned, std::string> states;
    states[0u] = start;
    std::size_t changes = 0u, unsets = 0u, raws = 0u, revived = 0u;
    for (const auto &save : recorded.saves) {
        require(states.count(save.from) != 0u, "save " + std::to_string(save.id) + " starts from a save that came before it");
        const std::string &before = states[save.from];
        if (folder) same_bytes(before, file_bytes(*folder / "transitions" / (id4(save.id) + ".before.ini")), "save " + std::to_string(save.id) + ": the file the launcher read is the one the save before it left");
        const std::string after = chili::ini_file::apply(before, save.changes);
        std::string what = "save " + std::to_string(save.id) + " (";
        for (const auto &change : save.changes) {
            if (&change != &save.changes.front()) what += ", ";
            what += change.section + "/" + change.key + (change.unset ? " off" : "=" + change.value);
            ++changes; unsets += change.unset; raws += change.raw;
        }
        what += "): the game's writer makes the launcher's bytes";
        if (folder) same_bytes(after, file_bytes(*folder / "transitions" / (id4(save.id) + ".after.ini")), what);
        else require(after.size() == save.length && hex(fnv(after)) == save.hash && hex(mix(after)) == save.second, what + " (length and hashes)");
        revived += IniDocument::parse(after).lines.size() == IniDocument::parse(before).lines.size();
        states[save.id] = after;
    }
    std::size_t same_as_start = 0u;
    for (const auto &[name, state] : recorded.kept) {
        require(states.count(state) != 0u, name + " is the file of a save that was replayed");
        if (folder) same_bytes(states[state], file_bytes(*folder / name), name + ": the settings file the launcher kept");
        same_as_start += states[state] == start;
    }
    std::cout << "launcher saves replayed: " << recorded.saves.size() << " saves with " << changes << " changes (" << raws << " paths, " << unsets
              << " switched off; " << revived << " saves added no line), " << recorded.kept.size() << " kept settings files (" << same_as_start
              << " equal to the file at the start), compared " << (folder ? "byte for byte with the launcher's files" : "by length and two hashes") << '\n';
    return states;
}

int launcher_files(const std::filesystem::path &folder, const std::filesystem::path *fixture) {
    const auto list = chili::ini_file::read(folder / "transitions.tsv");
    const auto start = chili::ini_file::read(folder / "transitions" / "start.ini");
    if (!list || !start) throw std::runtime_error("no transitions.tsv or transitions\\start.ini in " + folder.string() + " (the launcher's --every-value writes them)");
    const auto recorded = read_list(*list, false);
    require(!recorded.saves.empty() && !recorded.kept.empty(), "the launcher recorded saves");
    const auto states = replay(recorded, *start, &folder);
    // list.tsv has a head line and one line per kept file.
    const auto listed = lines_of(file_bytes(folder / "list.tsv")).size();
    require(listed == recorded.kept.size() + 1u, "every file of list.tsv was compared (" + std::to_string(recorded.kept.size()) + " of " + std::to_string(listed - 1u) + ")");
    if (fixture) {
        std::filesystem::create_directories(*fixture);
        std::string out =
            "# The saves of the launcher's --every-value run (launcher/src/EveryValue.cs), for tools/ini_document_test.cpp:\n"
            "# the game's writer (host/ini_document.hpp) must make the launcher's bytes from the same file and changes.\n"
            "# Written by: chili_ini_document_test --make-fixture <the folder --every-value wrote into> <this folder>.\n"
            "#   S <length> <hash> <second hash>                    start.ini, the file the launcher began with\n"
            "#   T <save> <the save its file came from, 0 = start.ini> <changes> <length> <hash> <second hash>   what the launcher wrote\n"
            "#   C <section> <key> <set|raw|unset> <value>          one change of the save, in the order made (\\t \\r \\n \\\\ escaped)\n"
            "#   K <file> <save>                                    a settings file the run kept: the file that save left\n";
        out += "S\t" + std::to_string(start->size()) + "\t" + hex(fnv(*start)) + "\t" + hex(mix(*start)) + "\n";
        std::size_t kept = 0u;
        const auto keep_until = [&](unsigned id) { for (; kept < recorded.kept.size() && recorded.kept[kept].second <= id; ++kept) out += "K\t" + recorded.kept[kept].first + "\t" + std::to_string(recorded.kept[kept].second) + "\n"; };
        for (const auto &save : recorded.saves) {
            const auto &after = states.at(save.id);
            out += "T\t" + std::to_string(save.id) + "\t" + std::to_string(save.from) + "\t" + std::to_string(save.changes.size()) + "\t" + std::to_string(after.size()) + "\t" + hex(fnv(after)) + "\t" + hex(mix(after)) + "\n";
            for (const auto &change : save.changes) {
                std::string value;
                for (const char c : change.value) { if (c == '\\') value += "\\\\"; else if (c == '\t') value += "\\t"; else if (c == '\r') value += "\\r"; else if (c == '\n') value += "\\n"; else value.push_back(c); }
                out += "C\t" + change.section + "\t" + change.key + "\t" + (change.unset ? "unset" : change.raw ? "raw" : "set") + "\t" + value + "\n";
            }
            keep_until(save.id);
        }
        keep_until(~0u);
        require(chili::ini_file::write(*fixture / "start.ini", *start) && chili::ini_file::write(*fixture / "launcher_saves.tsv", out), "the fixture was written");
        std::cout << "fixture written: " << (*fixture / "launcher_saves.tsv").string() << " (" << out.size() << " bytes) and start.ini (" << start->size() << " bytes)\n";
    }
    return 0;
}

// The fixture: one folder per run of the launcher, each with the file it began with and its saves. `default`
// is the run on the game's own first settings file, the one scripts/check_launcher_settings.ps1 makes (373
// settings files); the others began with files that lack keys and sections or are written in odd ways.
void fixture(const std::filesystem::path &folder) {
    std::vector<std::filesystem::path> runs;
    std::error_code status;
    for (const auto &entry : std::filesystem::directory_iterator(folder, status))
        if (entry.is_directory(status) && std::filesystem::exists(entry.path() / "launcher_saves.tsv", status)) runs.push_back(entry.path());
    std::sort(runs.begin(), runs.end());
    bool standard = false;
    for (const auto &run : runs) {
        const auto name = run.filename().string();
        const auto list = chili::ini_file::read(run / "launcher_saves.tsv");
        const auto start = chili::ini_file::read(run / "start.ini");
        require(list && start, "the fixture " + name + " has its list and its start.ini");
        const auto recorded = read_list(*list, true);
        require(start->size() == recorded.start_length && hex(fnv(*start)) == recorded.start_hash && hex(mix(*start)) == recorded.start_second,
            name + "/start.ini is the file the launcher began with (a checkout that changed its line ends would change it)");
        require(!recorded.saves.empty() && !recorded.kept.empty(), name + " holds saves");
        if (name == "default") {
            standard = true;
            require(recorded.kept.size() == 373u, "the run on the game's own file kept 373 settings files, as scripts/check_launcher_settings.ps1 has it");
        }
        std::cout << name << ": ";
        (void)replay(recorded, *start, nullptr);
    }
    require(standard, "the fixture folder holds the run named default: " + folder.string());
}
}

int main(int argc, char **argv) {
    try {
        const std::string first = argc > 1 ? argv[1] : "";
        if (first == "--launcher-files" && argc == 3) { launcher_files(argv[2], nullptr); }
        else if (first == "--make-fixture" && argc == 4) { const std::filesystem::path out = argv[3]; launcher_files(argv[2], &out); }
        else if (argc >= 2 && first.rfind("--", 0u) != 0u) {
            document();
            file(argv[1]);
            if (argc >= 3) fixture(argv[2]);
            else std::cout << "no fixture folder given: the launcher's saves were not replayed\n";
        } else {
            std::cerr << "usage: chili_ini_document_test <scratch folder> [<fixture folder>] | --launcher-files <folder> | --make-fixture <folder> <fixture folder>\n";
            return 2;
        }
        if (failures) { std::cerr << failures << " of " << checks << " checks FAILED\n"; return 1; }
        std::cout << "ini document: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
