#pragma once
#include "disc_image.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace chili::launcher {
// What a player's start needs before the game can run: the folders beside the executable, the game
// disc, a log file, and a way to say what went wrong to someone who has no console.
//
// A start with no positional argument is a player's start (a double-click). Its layout is portable:
//
//   ChiliConCarnage.exe
//   ChiliNative.ini              settings, written with the defaults on the first start
//   saves\                       the memory stick: PSP\SAVEDATA\... and pc-system-settings.txt
//   mods\                        mod folders (mod_files.hpp)
//   logs\ChiliConCarnage.log     this run's log; the run before it is ChiliConCarnage.previous.log
//
// The disc is the player's own copy: an .iso file or the folder of an unpacked disc, named by
// [Disc] Path of ChiliNative.ini. Without a usable one the game asks, checks the answer and writes it
// to the file. The disc is read where it is; nothing of it is copied.
//
// A start with the two positional arguments is a scripted or developer run and gets none of this: no
// dialog, no message box, no log file, no instance check; output goes to stdout and stderr as before.
//
//   --disc <folder or .iso>      a player's start on this disc, for this run only
//   --ms0 <folder>               the memory stick folder instead of saves\ (either kind of start)
//   --write-default-ini <file>   writes the settings file with the defaults and exits
//   CHILI_UNATTENDED=1           nobody is there to answer: no dialog or message box opens (messages go
//                                to the log) and a second instance is not refused. For test scripts;
//                                CHILI_DETERMINISTIC, CHILI_AUTO_DIALOGS, CHILI_INPUT_REPLAY and
//                                PSPRECOMP_LIVE_CONTROL imply it.
//   CHILI_DISC_ANSWER=<path>     the answer to the disc question of an unattended start
//   CHILI_CRASH_TEST=1           a player's start crashes on purpose once its log is open (tests the crash report)

// The one disc this program is the recompilation of.
inline constexpr char disc_id[] = "ULUS-10216";
inline constexpr std::uint64_t boot_size = 8616992u;
inline constexpr std::uint64_t boot_hash = 0xA18A6D5249286C4Aull;   // FNV-1a, 64 bits, of BOOT.BIN
inline constexpr wchar_t window_title[] = L"Chili Con Carnage";

inline constexpr char usage_text[] =
    "Chili Con Carnage, native PC build\n"
    "\n"
    "  ChiliConCarnage.exe [--disc <folder or .iso>] [--ms0 <save folder>]\n"
    "      plays; settings, saves, mods and logs are beside the program\n"
    "  ChiliConCarnage.exe <BOOT.BIN> <disc0 directory> [dispatch limit] [--ms0 <memory stick directory>]\n"
    "      scripted and developer runs\n"
    "  ChiliConCarnage.exe --write-default-ini <file>\n"
    "      writes the settings file with the defaults";

struct Start {
    // The command line.
    std::vector<std::wstring> positional;          // <BOOT.BIN> <disc0 directory> [dispatch limit]
    std::filesystem::path ms0, disc, write_ini;    // --ms0, --disc, --write-default-ini
    bool usage{};
    // A player's start (no positional argument) and where its files are.
    bool player{}, unattended{};
    std::filesystem::path base, ini, saves, log;
    // Why the run failed, for finish(); empty after a clean run.
    std::string failure;
};

struct Disc {
    std::filesystem::path path;         // the folder, or the .iso file
    std::shared_ptr<IsoImage> image;    // set for an .iso
    std::vector<std::uint8_t> boot;     // PSP_GAME/SYSDIR/BOOT.BIN
};

std::string utf8(const std::filesystem::path &path);
Start parse_command_line();
// Opens a disc folder or image and checks that it is this game. Throws std::runtime_error with a
// sentence the player can act on.
Disc open_disc(const std::filesystem::path &path);
// A player's start: makes the folders, opens the log, refuses a second instance. `scripted` is
// RunOptions::scripted(). False when this process should end quietly (the game is already running).
bool begin(Start &start, bool scripted);
// The disc of a player's start: --disc, [Disc] Path, a disc beside the executable, or the player's
// answer, which is written to the ini. Empty when the player chose to quit.
std::optional<Disc> choose_disc(const Start &start);
// Adds the commented [Disc] section to an ini that has none.
void add_default_disc_settings(const std::filesystem::path &ini);
// After everything of the run is destroyed: tells the player why it failed, and returns the exit code.
int finish(const Start &start, int code);
}
