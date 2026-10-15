#include "message_dialog_win32.hpp"
#include "pc_window.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using Presenter = chili::Win32MessageDialogPresenter;
using Choice = chili::MessageDialogChoice;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
std::wstring desktop_name(HDESK desktop) {
    DWORD bytes{};
    GetUserObjectInformationW(desktop, UOI_NAME, nullptr, 0, &bytes);
    require(bytes != 0u, "Cannot query desktop name length");
    std::vector<wchar_t> name(bytes / sizeof(wchar_t));
    require(GetUserObjectInformationW(desktop, UOI_NAME, name.data(), bytes, &bytes) != FALSE,
        "Cannot query desktop name");
    return name.data();
}
std::wstring input_desktop_name() {
    const auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    require(input != nullptr, "Cannot inspect the current input desktop");
    const auto name = desktop_name(input); CloseDesktop(input); return name;
}
struct PrivateDesktop {
    HDESK original{}, isolated{};
    std::wstring input_before;
    PrivateDesktop() {
        original = GetThreadDesktop(GetCurrentThreadId());
        require(original != nullptr, "Cannot inspect fixture thread desktop");
        input_before = input_desktop_name();
        const auto name = L"ChiliMsgAcceptance_" + std::to_wstring(GetCurrentProcessId());
        isolated = CreateDesktopW(name.c_str(), nullptr, nullptr, 0,
            DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_READOBJECTS |
            DESKTOP_WRITEOBJECTS | DESKTOP_HOOKCONTROL, nullptr);
        require(isolated != nullptr, "Cannot create the private fixture desktop");
        if (!SetThreadDesktop(isolated)) {
            CloseDesktop(isolated); isolated = nullptr;
            throw std::runtime_error("Cannot attach fixture thread to its private desktop");
        }
        require(input_desktop_name() == input_before, "Private desktop setup changed input desktop");
    }
    void verify() const {
        require(GetThreadDesktop(GetCurrentThreadId()) == isolated, "Fixture unexpectedly left its private desktop");
        require(input_desktop_name() == input_before, "Fixture changed the interactive input desktop");
    }
    ~PrivateDesktop() {
        if (isolated) { SetThreadDesktop(original); CloseDesktop(isolated); }
    }
};
HWND dialog_window() {
    struct Search { HWND window{}; unsigned count{}; } search;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM context) -> BOOL {
        wchar_t name[128]{}; GetClassNameW(hwnd, name, 128);
        if (std::wstring(name) == L"ChiliNativePspMessageDialog") {
            auto &search = *reinterpret_cast<Search *>(context); search.window = hwnd; ++search.count;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    require(search.count <= 1u, "Presenter left multiple native windows on fixture thread");
    return search.window;
}
void all_hidden(HWND dialog, HWND parent) {
    require(dialog && IsWindow(dialog) && !IsWindowVisible(dialog) && !IsWindowVisible(parent),
        "Acceptance fixture created a visible top-level window");
}
void hidden_children(HWND dialog) {
    bool hidden = true;
    EnumChildWindows(dialog, [](HWND child, LPARAM context) -> BOOL {
        if (IsWindowVisible(child)) *reinterpret_cast<bool *>(context) = false;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&hidden));
    require(hidden, "A presenter child became effectively visible");
}
chili::MessageDialogRequest request(unsigned generation, bool yes_no = true, bool okay = false,
    bool back = true, bool default_no = false) {
    chili::MessageDialogRequest r;
    r.generation = generation; r.size = 708u; r.mode = 1u; r.language = 1u; r.button_swap = 1u;
    r.text = "Synthetic fixture event. No human input or game dialog acceptance is claimed.";
    r.yes_no = yes_no; r.okay = okay; r.allow_back = back; r.default_no = default_no;
    r.suppress_sound = true; return r;
}
void key(HWND target, UINT code) {
    require(PostMessageW(target, WM_KEYDOWN, code, 1) != FALSE, "Cannot enqueue test key down");
    require(PostMessageW(target, WM_KEYUP, code, 0xC0000001u) != FALSE, "Cannot enqueue test key up");
}
void pump(chili::PcWindow &window) {
    require(window.pump(), "Actual hidden PcWindow pump reported closed");
}
void selected(Presenter &presenter, unsigned generation, Choice expected) {
    const auto selection = presenter.poll(generation);
    require(selection && selection->generation == generation && selection->choice == expected,
        "Native synthetic event did not yield the expected choice/token");
    require(!presenter.poll(generation), "Presenter returned the same event twice");
}
void require_error(const std::function<void()> &call, const char *message) {
    bool threw{}; try { call(); } catch (const std::runtime_error &) { threw = true; }
    require(threw, message);
}
void pending_parent_focus() {
    chili::PcWindow game(false);
    const auto initial_focus = GetFocus();
    Presenter presenter([&] { return game.handle(); }, false);
    require(!dialog_window() && IsWindowEnabled(game.handle()), "Presenter construction launched UI or disabled parent");
    presenter.open(request(1u, true, false, true, true));
    const auto dialog = dialog_window();
    all_hidden(dialog, game.handle()); hidden_children(dialog);
    require(GetWindow(dialog, GW_OWNER) == game.handle() && !IsWindowEnabled(game.handle()),
        "Message parent ownership/disable contract differs");
    require(GetFocus() == initial_focus, "Hidden open stole native focus");
    const auto def = SendMessageW(dialog, DM_GETDEFID, 0, 0);
    require(HIWORD(def) == DC_HASDEFID && LOWORD(def) == IDNO, "DEFAULT_NO did not target the No default button");
    require((GetWindowLongPtrW(GetDlgItem(dialog, IDNO), GWL_STYLE) & BS_TYPEMASK) == BS_DEFPUSHBUTTON &&
        (GetWindowLongPtrW(GetDlgItem(dialog, IDYES), GWL_STYLE) & BS_TYPEMASK) == BS_PUSHBUTTON,
        "Initial native default button styles disagree with DEFAULT_NO");
    for (unsigned i = 0; i < 16u; ++i) { pump(game); require(!presenter.poll(1u), "Window pump fabricated a default response"); }
    presenter.close(999u);
    require(IsWindow(dialog) && !IsWindowEnabled(game.handle()), "Stale-token close affected current native window");
    presenter.close(1u);
    require(!IsWindow(dialog) && !dialog_window() && IsWindowEnabled(game.handle()), "close failed native teardown/parent restoration");
    require_error([&] { presenter.poll(1u); }, "Programmatic close returned a fabricated selection");
    std::cout << "Hidden open/pending: owner, default-No target/style, no focus/response, parent restore PASS\n";
}
void button_messages() {
    struct Case { UINT id; Choice expected; bool yes_no, okay, back; };
    const Case cases[] = {{IDYES, Choice::Yes, true, false, true}, {IDNO, Choice::No, true, false, true},
        {IDOK, Choice::Okay, false, true, true}, {IDCANCEL, Choice::Back, false, false, true}};
    unsigned generation = 10u;
    for (const auto &c : cases) {
        chili::PcWindow game(false); Presenter presenter([&] { return game.handle(); }, false);
        presenter.open(request(generation, c.yes_no, c.okay, c.back));
        const auto dialog = dialog_window(); all_hidden(dialog, game.handle());
        const auto button = GetDlgItem(dialog, c.id); require(button != nullptr, "Expected native button missing");
        // Standard BUTTON receives synthetic mouse messages; no cursor movement,
        // SendInput, visible UI, foreground change or human action is involved.
        require(PostMessageW(button, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(8, 8)) != FALSE &&
            PostMessageW(button, WM_LBUTTONUP, 0, MAKELPARAM(8, 8)) != FALSE, "Cannot enqueue synthetic button messages");
        pump(game); selected(presenter, generation, c.expected);
        presenter.close(generation++); require(IsWindowEnabled(game.handle()), "Mouse event teardown left parent disabled");
    }
    std::cout << "Actual PcWindow pump + native BUTTON synthetic mouse down/up: Yes/No/OK/Back PASS\n";
}
void keyboard_messages() {
    for (bool default_no : {false, true}) {
        chili::PcWindow game(false); Presenter presenter([&] { return game.handle(); }, false);
        presenter.open(request(20u, true, false, true, default_no));
        const auto dialog = dialog_window(); all_hidden(dialog, game.handle());
        const auto initial = GetDlgItem(dialog, default_no ? IDNO : IDYES);
        SetFocus(initial);
        require(GetFocus() == initial, "Cannot establish test-only focus on the private hidden desktop");
        key(initial, VK_RETURN); pump(game);
        selected(presenter, 20u, default_no ? Choice::No : Choice::Yes); presenter.close(20u);
    }
    { chili::PcWindow game(false); Presenter presenter([&] { return game.handle(); }, false);
      presenter.open(request(21u)); const auto dialog = dialog_window();
      const auto yes = GetDlgItem(dialog, IDYES); SetFocus(yes); key(yes, VK_ESCAPE); pump(game);
      selected(presenter, 21u, Choice::Back); presenter.close(21u); }
    { chili::PcWindow game(false); Presenter presenter([&] { return game.handle(); }, false);
      presenter.open(request(22u, false, true, false)); const auto dialog = dialog_window();
      const auto okay = GetDlgItem(dialog, IDOK); SetFocus(okay); key(okay, VK_ESCAPE); pump(game);
      require(!presenter.poll(22u) && IsWindow(dialog), "Disallowed Escape fabricated Back");
      key(okay, VK_RETURN); pump(game); selected(presenter, 22u, Choice::Okay); presenter.close(22u); }
    { chili::PcWindow game(false); Presenter presenter([&] { return game.handle(); }, false);
      presenter.open(request(23u)); const auto dialog = dialog_window();
      const auto yes = GetDlgItem(dialog, IDYES), no = GetDlgItem(dialog, IDNO), back = GetDlgItem(dialog, IDCANCEL);
      SetFocus(yes); require(GetFocus() == yes, "Cannot establish Tab start focus");
      key(yes, VK_TAB); pump(game);
      require(GetFocus() == no && !presenter.poll(23u), "Native Tab did not move Yes->No without selecting");
      key(no, VK_TAB); pump(game);
      require(GetFocus() == back && !presenter.poll(23u), "Native Tab did not move No->Back without selecting");
      presenter.close(23u); }
    std::cout << "Actual PcWindow pump + synthetic Enter/Escape/Tab, DEFAULT_NO and no-cancel PASS\n";
}
void close_destroy_and_failures() {
    for (bool back : {false, true}) {
        chili::PcWindow game(false); Presenter presenter([&] { return game.handle(); }, false);
        presenter.open(request(30u, false, true, back)); const auto dialog = dialog_window();
        require(PostMessageW(dialog, WM_CLOSE, 0, 0) != FALSE, "Cannot enqueue test close"); pump(game);
        if (back) selected(presenter, 30u, Choice::Back);
        else require(!presenter.poll(30u), "Disallowed close fabricated Back");
        require(IsWindow(dialog), "WM_CLOSE destroyed dialog before guest consumes explicit action");
        presenter.close(30u); require(IsWindowEnabled(game.handle()), "Close test did not restore parent");
    }
    { chili::PcWindow game(false); Presenter presenter([&] { return game.handle(); }, false);
      presenter.open(request(31u)); const auto dialog = dialog_window();
      require(DestroyWindow(dialog) != FALSE, "Cannot perform test-only unexpected destruction"); pump(game);
      require_error([&] { presenter.poll(31u); }, "Window destruction silently returned a choice");
      presenter.close(31u); require(IsWindowEnabled(game.handle()), "Unexpected destruction did not restore parent"); }
    { chili::PcWindow game(false); HWND dialog{};
      { Presenter presenter([&] { return game.handle(); }, false); presenter.open(request(32u)); dialog = dialog_window(); }
      require(!IsWindow(dialog) && IsWindowEnabled(game.handle()), "Presenter destruction leaked UI or disabled parent");
      Presenter restart([&] { return game.handle(); }, false); restart.open(request(33u));
      require(!restart.poll(33u), "Destroyed presenter's event answered new instance"); restart.close(33u); }
    { chili::PcWindow game(false); EnableWindow(game.handle(), FALSE);
      Presenter presenter([&] { return game.handle(); }, false); presenter.open(request(34u)); presenter.close(34u);
      require(!IsWindowEnabled(game.handle()), "Presenter enabled a parent it did not disable"); }
    { chili::PcWindow game(false); Presenter presenter([&] { return game.handle(); }, false);
      auto r = request(35u); r.text = std::string("\xC0\xAF", 2u);
      require_error([&] { presenter.open(r); }, "Invalid UTF-8 silently displayed replacement text");
      presenter.close(35u); require(!dialog_window() && IsWindowEnabled(game.handle()), "UTF-8 failure leaked native state"); }
    { chili::PcWindow game(false); Presenter presenter([&] { return game.handle(); }, false);
      require_error([&] { presenter.open(request(36u, false, false, false)); }, "No-action native request silently succeeded");
      presenter.close(36u); require(!dialog_window() && IsWindowEnabled(game.handle()), "Partial open failure leaked window/state"); }
    std::cout << "Allowed/disallowed close, destruction without choice, restart/hook cleanup, failure rollback PASS\n";
}
}
int main() {
    try {
        PrivateDesktop desktop;
        pending_parent_focus(); desktop.verify(); button_messages(); desktop.verify();
        keyboard_messages(); desktop.verify(); close_destroy_and_failures(); desktop.verify();
        require(!dialog_window(), "Final presenter window leaked");
        std::cout << "Presenter acceptance PASS: private desktop never activated; hidden windows only; synthetic events only\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
