#include "launcher.hpp"
#include "dialog_dpi.hpp"
#include "ini_document.hpp"
#include "version.hpp"
#include <shellapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <cstdio>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <io.h>
#include <iostream>
#include <sstream>
#include <string_view>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
namespace chili::launcher {
namespace {
constexpr wchar_t instance_name[] = L"Local\\ChiliConCarnage.PC.Player";
constexpr wchar_t game_window_class[] = L"ChiliNativeDisplay";   // pc_window.hpp
constexpr wchar_t question_class[] = L"ChiliDiscQuestion";
constexpr char disc_ini_text[] =
    "\n"
    "[Disc]\n"
    "; The game disc: your own copy of Chili Con Carnage (USA, ULUS-10216), as an .iso file or as the\n"
    "; folder of an unpacked disc (the one that holds PSP_GAME and UMD_DATA.BIN). The game asks for it\n"
    "; on the first start and writes the answer here. A path without a drive is taken from the folder\n"
    "; of this file. Remove the line to be asked again.\n"
    "; Path=C:\\Games\\Chili Con Carnage (USA).iso\n";
// The start the crash report belongs to; set by begin().
const Start *current = nullptr;

// UTF-8 text as UTF-16; bytes that are not UTF-8 are read in the system code page.
std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    UINT page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int count = MultiByteToWideChar(page, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) {
        page = CP_ACP; flags = 0u;
        count = MultiByteToWideChar(page, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (count <= 0) return {};
    }
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(page, flags, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}
std::wstring environment(const wchar_t *name) {
    std::wstring value(512u, L'\0');
    for (;;) {
        const auto count = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
        if (!count) return {};
        if (count < value.size()) { value.resize(count); return value; }
        value.resize(count + 1u);
    }
}
std::filesystem::path executable_file() {
    std::wstring path(32768u, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return path;
}
std::string file_text(const std::filesystem::path &file) {
    std::ifstream stream(file, std::ios::binary);
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}
std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) text.remove_prefix(1u);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1u);
    return text;
}
std::string lower(std::string_view text) {
    std::string out(text);
    for (auto &c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}
std::string grouped(std::uint64_t number) {
    auto text = std::to_string(number);
    for (auto at = static_cast<std::ptrdiff_t>(text.size()) - 3; at > 0; at -= 3) text.insert(static_cast<std::size_t>(at), 1u, ',');
    return text;
}
// True when the caller gave this process a file or a pipe to write to.
bool redirected(DWORD which) {
    const auto handle = GetStdHandle(which);
    if (!handle || handle == INVALID_HANDLE_VALUE) return false;
    const auto type = GetFileType(handle) & ~static_cast<DWORD>(FILE_TYPE_REMOTE);
    return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}

// A message for the player: a line of the log, and a message box unless nobody is there to read one.
// `release_log` closes the log first (the last message of a run), so that any program can open the
// file the message names while the box is on the screen.
void say(const Start &start, const std::string &text, UINT icon, bool release_log = false) {
    std::string line = text;
    for (std::size_t at = 0u; (at = line.find('\n', at)) != std::string::npos;) line.replace(at, 1u, " | ");
    std::cerr << "[message] " << line << '\n';
    if (release_log && !start.log.empty()) {
        std::cout.flush();
        std::fclose(stdout);
        std::fclose(stderr);
    }
    if (start.unattended) return;
    // A confined or captured pointer could not reach the button.
    ClipCursor(nullptr);
    ReleaseCapture();
    MessageBoxW(nullptr, wide(text).c_str(), window_title, MB_OK | MB_SETFOREGROUND | icon);
}

// ---- one instance at a time ----------------------------------------------------------------------

// Brings the window of the running game (a process of this same program file) to the front.
bool show_running_game() {
    struct Search { std::filesystem::path self; HWND found{}; } search{executable_file(), nullptr};
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto &state = *reinterpret_cast<Search *>(parameter);
        wchar_t name[64]{};
        if (!GetClassNameW(window, name, 64) || std::wstring_view(name) != game_window_class) return TRUE;
        DWORD process_id{};
        GetWindowThreadProcessId(window, &process_id);
        const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
        if (!process) return TRUE;
        std::wstring image(32768u, L'\0');
        auto size = static_cast<DWORD>(image.size());
        const bool known = QueryFullProcessImageNameW(process, 0u, image.data(), &size) != 0;
        CloseHandle(process);
        image.resize(known ? size : 0u);
        if (_wcsicmp(image.c_str(), state.self.c_str()) != 0) return TRUE;
        state.found = window;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    if (!search.found) return false;
    if (IsIconic(search.found)) ShowWindow(search.found, SW_RESTORE);
    SetForegroundWindow(search.found);
    return true;
}
// False when a player's game is already running. The mutex is held until the process ends.
bool claim_instance(const Start &start) {
    const auto mutex = CreateMutexW(nullptr, FALSE, instance_name);
    if (!mutex || GetLastError() != ERROR_ALREADY_EXISTS) return true;
    CloseHandle(mutex);
    if (!show_running_game()) say(start, "Chili Con Carnage is already running.", MB_ICONINFORMATION);
    return false;
}

// ---- the log and the crash report ----------------------------------------------------------------

// stdout and stderr go to logs\ChiliConCarnage.log; the log of the run before is kept beside it.
void open_log(Start &start) {
    std::error_code status;
    const auto folder = start.base / L"logs";
    std::filesystem::create_directories(folder, status);
    const auto log = folder / L"ChiliConCarnage.log", previous = folder / L"ChiliConCarnage.previous.log";
    if (std::filesystem::exists(log, status)) {
        std::filesystem::remove(previous, status);
        std::filesystem::rename(log, previous, status);
    }
    // Not the _s variants: they open the file for this process alone, and the log must be readable in
    // an editor while the game runs or while a failure message that names it is on the screen.
#pragma warning(push)
#pragma warning(disable : 4996)
    if (!_wfreopen(log.c_str(), L"w", stdout)) return;
    // Both streams share one file position, so their lines land in the order they were written.
    if (_wfreopen(L"NUL", L"w", stderr)) _dup2(_fileno(stdout), _fileno(stderr));
#pragma warning(pop)
    std::setvbuf(stderr, nullptr, _IONBF, 0u);
    // The file is current after every line: it is what a failure message points at.
    std::cout.clear(); std::cerr.clear();
    std::cout.setf(std::ios::unitbuf);
    start.log = log;
}
std::string log_note(const Start &start) {
    return start.log.empty() ? std::string() : "\n\nThe log of this run:\n" + utf8(start.log);
}
LONG WINAPI crashed(EXCEPTION_POINTERS *info) {
    static LONG entered = 0;
    if (InterlockedExchange(&entered, 1)) return EXCEPTION_CONTINUE_SEARCH;
    const auto *record = info->ExceptionRecord;
    HMODULE module = nullptr;
    wchar_t file[MAX_PATH]{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<LPCWSTR>(record->ExceptionAddress), &module);
    if (module) GetModuleFileNameW(module, file, MAX_PATH);
    const wchar_t *name = file;
    for (const wchar_t *c = file; *c; ++c) if (*c == L'\\') name = c + 1;
    char line[400];
    std::snprintf(line, sizeof(line), "exception 0x%08lX at %ls+0x%llX", record->ExceptionCode, *name ? name : L"?",
        static_cast<unsigned long long>(static_cast<const char *>(record->ExceptionAddress) - reinterpret_cast<const char *>(module)));
    // stderr is unbuffered and goes to the log.
    std::fprintf(stderr, "[crash] %s\n", line);
    if (current) say(*current, std::string("Chili Con Carnage has crashed (") + line + ")." + log_note(*current), MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---- ChiliNative.ini [Disc] -----------------------------------------------------------------------

// [Disc] Path of the ini; empty when the file names none. Read as the launcher reads it and as
// remember_disc writes it (ini_document.hpp): the last live line of the section, everything after the
// sign (a path may hold ';' and '#'), without one pair of quotes round it.
std::filesystem::path remembered_disc(const std::filesystem::path &ini) {
    const auto value = chili::IniDocument::parse(file_text(ini)).get_raw("Disc", "Path").value_or(std::string());
    if (value.empty()) return {};
    std::filesystem::path path(wide(value));
    std::error_code status;
    if (path.is_relative()) path = std::filesystem::absolute(ini, status).parent_path() / path;
    return path;
}
// Writes the disc into [Disc] Path as the launcher writes it (ini_document.hpp): over the live line that is
// there, else over the commented example, else as a new line of the section, else with a new section. The
// file is read fresh, only this line changes, and the result is swapped in under the gate the launcher
// shares, so a launcher that is open beside the game loses nothing.
void remember_disc(const Start &start, const std::filesystem::path &disc) {
    // A disc below the folder of the ini (the program's folder) is written relative to it, so the
    // folder can be moved with the disc in it.
    std::error_code status;
    auto shown = disc;
    if (const auto relative = disc.lexically_relative(std::filesystem::absolute(start.ini, status).parent_path());
        !relative.empty() && *relative.begin() != "..") shown = relative;
    const auto written = chili::ini_change_file(start.ini, chili::IniChange{"Disc", "Path", utf8(shown), true});
    if (!written.ok) std::cerr << "[disc] the choice could not be written to " << utf8(start.ini) << " (" << written.error << ")\n";
}

// ---- asking for the disc --------------------------------------------------------------------------

enum class Choice { Image, Folder, Quit };
struct Question { Choice choice{Choice::Quit}; bool done{}; };
constexpr int image_button = 101, folder_button = 102;

LRESULT CALLBACK question_procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto *state = reinterpret_cast<Question *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams));
    if (state && message == WM_COMMAND && HIWORD(w) == BN_CLICKED) {
        const int id = LOWORD(w);
        if (id == image_button || id == folder_button || id == IDCANCEL) {
            state->choice = id == image_button ? Choice::Image : id == folder_button ? Choice::Folder : Choice::Quit;
            state->done = true;
            return 0;
        }
    }
    if (state && message == WM_CLOSE) { state->choice = Choice::Quit; state->done = true; return 0; }
    return DefWindowProcW(window, message, w, l);
}
// The question window: what is needed, what was wrong with the last answer, and three buttons.
Choice ask(const std::string &problem) {
    std::wstring text =
        L"Chili Con Carnage needs the game disc.\n\n"
        L"Use your own copy of the USA release (ULUS-10216): the .iso file of the disc, or the folder of an "
        L"unpacked disc (the one that holds PSP_GAME and UMD_DATA.BIN). The game reads it where it is; "
        L"nothing is copied. The choice is kept in ChiliNative.ini.";
    if (!problem.empty()) text += L"\n\nThis one cannot be used:\n" + wide(problem);
    // Laid out in 96-dpi pixels and scaled by Windows, like the game's other dialogs.
    const DialogDpiScope dialog_dpi;
    const auto instance = GetModuleHandleW(nullptr);
    static const ATOM atom = [instance] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = question_procedure;
        wc.hInstance = instance;
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = question_class;
        return RegisterClassW(&wc);
    }();
    if (!atom) throw std::runtime_error("Cannot open the disc question window");
    const auto font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    constexpr int width = 560, margin = 16, button_height = 28;
    RECT measured{0, 0, width - 2 * margin, 0};
    if (const auto dc = GetDC(nullptr)) {
        const auto old = SelectObject(dc, font);
        DrawTextW(dc, text.c_str(), -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(nullptr, dc);
    }
    const int text_height = std::clamp(static_cast<int>(measured.bottom) + 4, 60, 420);
    const int height = margin + text_height + margin + button_height + margin;
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, extra = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT | WS_EX_APPWINDOW;
    RECT rect{0, 0, width, height};
    AdjustWindowRectEx(&rect, style, FALSE, extra);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0u, &work, 0u);
    Question state;
    const auto window = CreateWindowExW(extra, question_class, window_title, style,
        work.left + ((work.right - work.left) - (rect.right - rect.left)) / 2, work.top + ((work.bottom - work.top) - (rect.bottom - rect.top)) / 2,
        rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, &state);
    if (!window) throw std::runtime_error("Cannot open the disc question window");
    const auto child = [&](const wchar_t *type, const wchar_t *caption, DWORD flags, int x, int y, int w, int h, int id) {
        const auto control = CreateWindowExW(0u, type, caption, WS_CHILD | WS_VISIBLE | flags, x, y, w, h, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return control;
    };
    child(L"STATIC", text.c_str(), SS_LEFT | SS_NOPREFIX, margin, margin, width - 2 * margin, text_height, 100);
    const int row = margin + text_height + margin;
    const auto first = child(L"BUTTON", L"Choose the .iso file...", WS_TABSTOP | BS_DEFPUSHBUTTON, margin, row, 176, button_height, image_button);
    child(L"BUTTON", L"Choose the disc folder...", WS_TABSTOP | BS_PUSHBUTTON, margin + 184, row, 176, button_height, folder_button);
    child(L"BUTTON", L"Quit", WS_TABSTOP | BS_PUSHBUTTON, width - margin - 96, row, 96, button_height, IDCANCEL);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    if (first) SetFocus(first);
    MSG message;
    while (!state.done) {
        const auto got = GetMessageW(&message, nullptr, 0u, 0u);
        if (got <= 0) { if (!got) PostQuitMessage(static_cast<int>(message.wParam)); break; }
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    DestroyWindow(window);
    return state.choice;
}
// The system's file or folder picker; empty when it was cancelled.
std::filesystem::path pick(bool folder) {
    std::filesystem::path result;
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog *dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0u;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
        const COMDLG_FILTERSPEC types[] = {{L"PSP disc image (*.iso)", L"*.iso"}, {L"All files (*.*)", L"*.*"}};
        if (!folder) dialog->SetFileTypes(2u, types);
        dialog->SetTitle(folder ? L"Chili Con Carnage: the folder of the unpacked disc (it holds PSP_GAME)" : L"Chili Con Carnage: the .iso file of the disc");
        IShellItem *item = nullptr;
        if (SUCCEEDED(dialog->Show(nullptr)) && SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR name = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name) { result = name; CoTaskMemFree(name); }
            item->Release();
        }
        dialog->Release();
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return result;
}
}

// ---- public -------------------------------------------------------------------------------------

std::string utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

Start parse_command_line() {
    Start start;
    int count = 0;
    wchar_t **arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 1; arguments && i < count; ++i) {
        const std::wstring_view argument(arguments[i]);
        const auto value = [&](std::filesystem::path &into) {
            if (++i >= count) start.usage = true;
            else into = arguments[i];
        };
        if (argument == L"--ms0") value(start.ms0);
        else if (argument == L"--disc") value(start.disc);
        else if (argument == L"--write-default-ini") value(start.write_ini);
        else if (argument.starts_with(L"--") || argument == L"-h" || argument == L"/?") start.usage = true;
        else start.positional.emplace_back(argument);
    }
    if (arguments) LocalFree(arguments);
    if (start.positional.size() == 1u || start.positional.size() > 3u) start.usage = true;
    if (!start.positional.empty() && (!start.disc.empty() || !start.write_ini.empty())) start.usage = true;
    start.player = !start.usage && start.positional.empty() && start.write_ini.empty();
    return start;
}

Disc open_disc(const std::filesystem::path &given) {
    std::error_code status;
    Disc disc;
    disc.path = std::filesystem::absolute(given, status);
    const auto name = utf8(disc.path);
    if (given.empty() || !std::filesystem::exists(disc.path, status)) throw std::runtime_error(name + "\nwas not found.");
    // One way to read a file of the disc, whichever kind it is: nullopt when the disc has none.
    std::function<std::optional<std::vector<std::uint8_t>>(const char *, std::uint64_t)> read;
    std::function<std::optional<std::uint64_t>(const char *)> size;
    if (std::filesystem::is_directory(disc.path, status)) {
        // The folder that holds PSP_GAME. PSP_GAME itself, or a folder inside it, names the same disc.
        for (unsigned up = 0u; up < 2u && !std::filesystem::is_directory(disc.path / L"PSP_GAME", status); ++up) {
            const auto leaf = lower(utf8(disc.path.filename()));
            if (leaf != "psp_game" && leaf != "sysdir" && leaf != "usrdir") break;
            disc.path = disc.path.parent_path();
        }
        size = [&](const char *inside) -> std::optional<std::uint64_t> {
            std::error_code failed;
            const auto file = disc.path / inside;
            if (!std::filesystem::is_regular_file(file, failed)) return std::nullopt;
            return std::filesystem::file_size(file, failed);
        };
        read = [&](const char *inside, std::uint64_t limit) -> std::optional<std::vector<std::uint8_t>> {
            const auto bytes = size(inside);
            if (!bytes || *bytes > limit) return std::nullopt;
            std::vector<std::uint8_t> out(static_cast<std::size_t>(*bytes));
            std::ifstream stream(disc.path / inside, std::ios::binary);
            stream.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
            if (static_cast<std::size_t>(stream.gcount()) != out.size()) return std::nullopt;
            return out;
        };
    } else {
        try { disc.image = std::make_shared<IsoImage>(disc.path); }
        catch (const std::exception &error) { throw std::runtime_error(name + "\ncannot be read as a disc image: " + error.what() + "."); }
        size = [&](const char *inside) -> std::optional<std::uint64_t> {
            const auto *entry = disc.image->file(inside);
            return entry ? std::optional<std::uint64_t>(entry->size) : std::nullopt;
        };
        read = [&](const char *inside, std::uint64_t limit) -> std::optional<std::vector<std::uint8_t>> {
            const auto *entry = disc.image->file(inside);
            if (!entry || entry->size > limit) return std::nullopt;
            return disc.image->bytes(inside, limit);
        };
    }
    const auto id_file = read("UMD_DATA.BIN", 4096u);
    if (!id_file || !size("PSP_GAME/SYSDIR/BOOT.BIN"))
        throw std::runtime_error(name + "\nis not a PSP game disc: UMD_DATA.BIN and PSP_GAME\\SYSDIR\\BOOT.BIN are not both there.");
    std::string id(id_file->begin(), std::find(id_file->begin(), id_file->end(), static_cast<std::uint8_t>('|')));
    if (id != disc_id) {
        const bool printable = !id.empty() && id.size() <= 16u && std::all_of(id.begin(), id.end(), [](char c) { return c > 32 && c < 127; });
        throw std::runtime_error(name + "\nis " + (printable ? "the disc " + id : std::string("another disc")) +
            ". This program plays Chili Con Carnage, USA release, " + disc_id + ".");
    }
    auto boot = read("PSP_GAME/SYSDIR/BOOT.BIN", 64u << 20u);
    std::uint64_t hash = 0xCBF29CE484222325ull;
    if (boot) for (const auto byte : *boot) hash = (hash ^ byte) * 0x100000001B3ull;
    if (!boot || boot->size() != boot_size || hash != boot_hash)
        throw std::runtime_error(name + "\nhas a different game program than the one this port was made from (PSP_GAME\\SYSDIR\\BOOT.BIN, " +
            grouped(boot ? boot->size() : *size("PSP_GAME/SYSDIR/BOOT.BIN")) + " bytes; " + grouped(boot_size) +
            " expected, unchanged). An unmodified copy of the USA disc is needed.");
    for (const char *needed : {"PSP_GAME/USRDIR/assets00.naz", "PSP_GAME/USRDIR/configpsp.txt", "PSP_GAME/USRDIR/module/psmf.prx"})
        if (!size(needed).value_or(0u))
            throw std::runtime_error(name + "\nis incomplete: " + needed + " is missing.");
    disc.boot = std::move(*boot);
    return disc;
}

bool begin(Start &start, bool scripted) {
    start.base = executable_file().parent_path();
    start.unattended = scripted || environment(L"CHILI_UNATTENDED") == L"1" || !environment(L"PSPRECOMP_LIVE_CONTROL").empty();
    const auto config = environment(L"CHILI_CONFIG");
    start.ini = config.empty() ? start.base / L"ChiliNative.ini" : std::filesystem::path(config);
    start.saves = start.base / L"saves";
    current = &start;
    if (!start.unattended && !claim_instance(start)) return false;
    // A caller that took stdout (a test script) keeps it; a double-click has none and gets the log file.
    if (!redirected(STD_OUTPUT_HANDLE)) open_log(start);
    SetUnhandledExceptionFilter(crashed);
    std::cout << "[start] Chili Con Carnage PC " << CHILI_VERSION_TEXT << " (built " << __DATE__ << "), folder " << utf8(start.base) << '\n';
    // CHILI_CRASH_TEST=1: an access violation on purpose, to see the crash report.
    if (environment(L"CHILI_CRASH_TEST") == L"1") *static_cast<volatile int *>(nullptr) = 0;
    // The folders a player looks for. The saves folder is made here so that a folder that cannot be
    // written (Program Files, a read-only share) is reported in plain words.
    std::error_code status;
    if (!scripted) std::filesystem::create_directories(start.base / L"mods", status);
    if (start.ms0.empty() && environment(L"CHILI_MS0_DIR").empty() && !scripted) {
        std::filesystem::create_directories(start.saves, status);
        if (status || !std::filesystem::is_directory(start.saves, status))
            throw std::runtime_error("The game's folder cannot be written to:\n" + utf8(start.base) +
                "\nMove the game to a folder of your own (not Program Files) and start it again.");
    }
    return true;
}

std::optional<Disc> choose_disc(const Start &start) {
    const auto report = [](const Disc &disc, const char *origin) {
        std::cout << "[disc] " << utf8(disc.path) << (disc.image ? " (disc image, read in place, " + std::to_string(disc.image->files()) + " files)" : std::string(" (folder)"))
                  << ", named by " << origin << '\n';
    };
    // --disc: this disc or nothing.
    if (!start.disc.empty()) {
        auto disc = open_disc(start.disc);
        report(disc, "--disc");
        return disc;
    }
    std::string problem;
    if (const auto named = remembered_disc(start.ini); !named.empty()) {
        try { auto disc = open_disc(named); report(disc, "[Disc] Path"); return disc; }
        catch (const std::exception &error) { problem = error.what(); std::cout << "[disc] [Disc] Path cannot be used: " << problem << '\n'; }
    } else {
        // Nothing named yet: a disc beside the program is taken without asking.
        std::error_code status;
        std::vector<std::filesystem::path> beside{start.base / L"disc0", start.base};
        for (const auto &entry : std::filesystem::directory_iterator(start.base, status))
            if (entry.is_regular_file(status) && lower(utf8(entry.path().extension())) == ".iso") beside.push_back(entry.path());
        for (const auto &candidate : beside) {
            if (candidate == start.base && !std::filesystem::is_directory(candidate / L"PSP_GAME", status)) continue;
            try {
                auto disc = open_disc(candidate);
                report(disc, "its place beside the program");
                remember_disc(start, disc.path);
                return disc;
            } catch (const std::exception &) { }
        }
    }
    if (start.unattended) {
        const auto answer = environment(L"CHILI_DISC_ANSWER");
        if (answer.empty()) throw std::runtime_error(problem.empty() ? "The game disc is not set ([Disc] Path of ChiliNative.ini)." : problem);
        auto disc = open_disc(answer);
        report(disc, "CHILI_DISC_ANSWER");
        remember_disc(start, disc.path);
        return disc;
    }
    for (;;) {
        const auto choice = ask(problem);
        if (choice == Choice::Quit) { std::cout << "[disc] no disc chosen\n"; return std::nullopt; }
        const auto picked = pick(choice == Choice::Folder);
        if (picked.empty()) continue;
        try {
            auto disc = open_disc(picked);
            report(disc, "the player");
            remember_disc(start, disc.path);
            return disc;
        } catch (const std::exception &error) { problem = error.what(); std::cout << "[disc] the player's choice cannot be used: " << problem << '\n'; }
    }
}

void add_default_disc_settings(const std::filesystem::path &ini) {
    std::error_code status;
    if (!std::filesystem::is_regular_file(ini, status)) return;
    if (lower(file_text(ini)).find("[disc]") != std::string::npos) return;
    std::ofstream stream(ini, std::ios::binary | std::ios::app);
    stream << disc_ini_text;
}

int finish(const Start &start, int code) {
    std::cout.flush();
    std::cerr.flush();
    if (start.usage) {
        // A program without a console has nowhere to print the usage.
        const auto handle = GetStdHandle(STD_ERROR_HANDLE);
        if ((!handle || handle == INVALID_HANDLE_VALUE || GetFileType(handle) == FILE_TYPE_UNKNOWN) && environment(L"CHILI_UNATTENDED") != L"1")
            MessageBoxW(nullptr, wide(usage_text).c_str(), window_title, MB_OK | MB_ICONINFORMATION);
    } else if (start.player && !start.failure.empty()) {
        say(start, "Chili Con Carnage cannot go on.\n\n" + start.failure + log_note(start), MB_ICONERROR, true);
    }
    return code;
}
}
