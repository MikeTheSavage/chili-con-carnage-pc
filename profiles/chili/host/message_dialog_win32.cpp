#include "message_dialog_win32.hpp"
#include "dialog_dpi.hpp"
#include "background_window.hpp"
#include <stdexcept>
#include <utility>
#include <algorithm>
#include <vector>

namespace chili {
namespace {
constexpr wchar_t class_name[] = L"ChiliNativePspMessageDialog";
constexpr std::uint32_t up=0x10u,right=0x20u,down=0x40u,left=0x80u,circle=0x2000u,cross=0x4000u;
std::wstring wide(const std::string &text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (!count) throw std::runtime_error("Dialog text is not valid UTF-8");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), count) != count)
        throw std::runtime_error("Dialog UTF-8 conversion failed");
    return result;
}
}
struct Win32MessageDialogPresenter::Impl {
    Parent parent_provider;
    Buttons buttons_provider;
    MessageDialogRequest request;
    std::optional<MessageDialogSelection> selection;
    HWND window{}, parent{};
    HHOOK message_hook{};
    DWORD thread{};
    bool parent_disabled{};
    bool visible{true};
    UINT default_button{};
    bool filtering_message{};
    bool completed{},sample_valid{};
    std::uint32_t previous_buttons{},native_button_bits{};
    std::vector<UINT> buttons;
    inline static thread_local Impl *active_ui{};

    void require_thread() const {
        if (thread && thread != GetCurrentThreadId())
            throw std::runtime_error("Dialog presenter called from a different native UI thread");
    }
    void choose(MessageDialogChoice choice) {
        if (completed) return;
        selection = MessageDialogSelection{request.generation, choice};completed=true;
    }
    void choose_button(UINT id) {
        if(id==IDYES&&request.yes_no)choose(MessageDialogChoice::Yes);
        else if(id==IDNO&&request.yes_no)choose(MessageDialogChoice::No);
        else if(id==IDOK&&request.okay)choose(MessageDialogChoice::Okay);
        else if(id==IDCANCEL&&request.allow_back)choose(MessageDialogChoice::Back);
    }
    void set_default(UINT id) {
        if(default_button!=id)if(auto old=GetDlgItem(window,default_button))SendMessageW(old,BM_SETSTYLE,BS_PUSHBUTTON,TRUE);
        default_button=id;
        if(auto current=GetDlgItem(window,id)){SendMessageW(current,BM_SETSTYLE,BS_DEFPUSHBUTTON,TRUE);if(visible)SetFocus(current);}
    }
    void device_edges() {
        if(!buttons_provider||completed)return;
        const auto sample=buttons_provider();
        if(!sample){sample_valid=false;native_button_bits=0;return;}
        const auto current=*sample;
        if(!sample_valid){previous_buttons=current;sample_valid=true;native_button_bits&=current;return;}
        auto edges=current&~previous_buttons;previous_buttons=current;
        edges&=~native_button_bits;native_button_bits&=current;
        const auto confirm=request.button_swap?cross:circle,back_button=request.button_swap?circle:cross;
        if(edges&back_button){if(request.allow_back)choose(MessageDialogChoice::Back);return;}
        const bool backward=(edges&(left|up))!=0u,forward=(edges&(right|down))!=0u;
        if(backward!=forward){
            const auto found=std::find(buttons.begin(),buttons.end(),default_button);
            if(found==buttons.end())throw std::runtime_error("Native message default button is unavailable");
            const auto index=static_cast<std::size_t>(found-buttons.begin());
            set_default(buttons[backward?(index?index-1u:0u):std::min(index+1u,buttons.size()-1u)]);
        }
        if(edges&confirm)choose_button(default_button);
    }
    static LRESULT CALLBACK message_filter(int code, WPARAM wp, LPARAM lp) {
        auto *self = active_ui;
        if (code >= 0 && wp == PM_REMOVE && self && self->window && !self->filtering_message) {
            auto *message = reinterpret_cast<MSG *>(lp);
            if (message->hwnd == self->window || IsChild(self->window, message->hwnd)) {
                self->filtering_message = true;
                if(message->message==WM_KEYDOWN){
                    if(message->wParam==VK_UP)self->native_button_bits|=up;
                    if(message->wParam==VK_DOWN)self->native_button_bits|=down;
                    if(message->wParam==VK_LEFT)self->native_button_bits|=left;
                    if(message->wParam==VK_RIGHT)self->native_button_bits|=right;
                    const auto id=static_cast<UINT>(GetDlgCtrlID(message->hwnd));
                    if(message->wParam==VK_SPACE&&std::find(self->buttons.begin(),self->buttons.end(),id)!=self->buttons.end())self->native_button_bits|=cross;
                }
                const auto handled = IsDialogMessageW(self->window, message);
                self->filtering_message = false;
                if (handled) {
                    // The existing game pump can continue without processing
                    // this dialog navigation/input a second time.
                    message->hwnd = nullptr; message->message = WM_NULL;
                    message->wParam = 0; message->lParam = 0;
                }
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    static LRESULT CALLBACK procedure(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto *self = reinterpret_cast<Impl *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        if (msg == WM_CLOSE) {
            if (self->request.allow_back) self->choose(MessageDialogChoice::Back);
            return 0;
        }
        if (msg == WM_COMMAND && (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == 0u)) {
            switch (LOWORD(wp)) {
            case IDYES: if (self->request.yes_no) self->choose(MessageDialogChoice::Yes); return 0;
            case IDNO: if (self->request.yes_no) self->choose(MessageDialogChoice::No); return 0;
            case IDOK: if (self->request.okay) self->choose(MessageDialogChoice::Okay); return 0;
            case IDCANCEL: if (self->request.allow_back) self->choose(MessageDialogChoice::Back); return 0;
            }
        }
        if (msg == DM_GETDEFID) return MAKELONG(self->default_button, DC_HASDEFID);
        if (msg == DM_SETDEFID) { self->default_button = static_cast<UINT>(wp); return 1; }
        // Unexpected host destruction is not confirmation/cancellation.
        if (msg == WM_NCDESTROY) {
            self->window = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    void close() noexcept {
        if (message_hook) { UnhookWindowsHookEx(message_hook); message_hook = nullptr; }
        if (active_ui == this) active_ui = nullptr;
        if (window) {
            const auto closing = std::exchange(window, nullptr);
            SetWindowLongPtrW(closing, GWLP_USERDATA, 0);
            if (thread == GetCurrentThreadId()) DestroyWindow(closing);
            else PostMessageW(closing, WM_CLOSE, 0, 0);
        }
        if (parent_disabled && parent && IsWindow(parent)) EnableWindow(parent, TRUE);
        parent_disabled = false; parent = nullptr; selection.reset(); thread = 0u;completed=sample_valid=false;native_button_bits=0;buttons.clear();
    }
};
Win32MessageDialogPresenter::Win32MessageDialogPresenter(Parent parent, bool visible, Buttons buttons) : impl_(std::make_unique<Impl>()) {
    impl_->parent_provider = std::move(parent);
    impl_->visible = visible;
    impl_->buttons_provider=std::move(buttons);
}
Win32MessageDialogPresenter::~Win32MessageDialogPresenter() { impl_->close(); }

void Win32MessageDialogPresenter::open(const MessageDialogRequest &request) {
    impl_->require_thread();
    if (impl_->window) throw std::runtime_error("A native message window is already open");
    if (Impl::active_ui) throw std::runtime_error("A native message presenter is already active on this UI thread");
    if(!request.generation||request.button_swap>1u||(request.yes_no&&request.okay)||(!request.yes_no&&!request.okay&&!request.allow_back))
        throw std::runtime_error("Invalid native message choices/button mapping");
    const auto body = wide(request.text);
    const auto okay_label = wide(request.okay_label);
    const auto back_label = wide(request.back_label);
    const auto held=impl_->buttons_provider?impl_->buttons_provider():std::optional<std::uint32_t>{};
    static const ATOM atom = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Impl::procedure;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = class_name;
        return RegisterClassW(&wc);
    }();
    if (!atom) throw std::runtime_error("Cannot register native message window class");
    impl_->request = request; impl_->selection.reset(); impl_->thread = GetCurrentThreadId();
    impl_->completed=false;impl_->sample_valid=held.has_value();impl_->previous_buttons=held.value_or(0u);impl_->native_button_bits=0;impl_->buttons.clear();
    impl_->parent = impl_->parent_provider ? impl_->parent_provider() : nullptr;
    if (impl_->parent && !IsWindow(impl_->parent)) throw std::runtime_error("Invalid game window parent");
    DWORD parent_thread{};
    if (impl_->parent) parent_thread = GetWindowThreadProcessId(impl_->parent, nullptr);
    if (parent_thread && parent_thread != impl_->thread)
        throw std::runtime_error("Game window and message window must share the native UI thread");
    const DialogDpiScope dialog_dpi;
    RECT rect{0, 0, 520, 260};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    const DWORD extra = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    AdjustWindowRectEx(&rect, style, FALSE, extra);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (impl_->parent) {
        RECT parent_rect{};
        if (GetWindowRect(impl_->parent, &parent_rect)) {
            x = parent_rect.left + ((parent_rect.right - parent_rect.left) - (rect.right - rect.left)) / 2;
            y = parent_rect.top + ((parent_rect.bottom - parent_rect.top) - (rect.bottom - rect.top)) / 2;
        }
    }
    impl_->window = CreateWindowExW(extra, class_name, L"Chili Con Carnage - PSP message",
        style, x, y, rect.right - rect.left, rect.bottom - rect.top, impl_->parent,
        nullptr, GetModuleHandleW(nullptr), impl_.get());
    if (!impl_->window) throw std::runtime_error("Cannot create native message window");
    const auto font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    const auto child = [&](const wchar_t *type, const wchar_t *caption, DWORD flags,
        int cx, int cy, int width, int height, UINT id) {
        auto hwnd = CreateWindowExW(0, type, caption, WS_CHILD | WS_VISIBLE | flags,
            cx, cy, width, height, impl_->window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
            GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) throw std::runtime_error("Cannot create native message control");
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return hwnd;
    };
    child(L"EDIT", body.c_str(), ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
        16, 16, 488, 188, 100u);
    unsigned count = (request.yes_no ? 2u : 0u) + (request.okay ? 1u : 0u) + (request.allow_back ? 1u : 0u);
    if (!count) throw std::runtime_error("Native message has no actionable buttons");
    const int first = (520 - static_cast<int>(count) * 112 + 8) / 2;
    unsigned ordinal{};
    HWND focus{};
    const auto button = [&](const wchar_t *caption, UINT id, bool is_default) {
        const auto hwnd = child(L"BUTTON", caption, WS_TABSTOP | (is_default ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
            first + static_cast<int>(ordinal++) * 112, 218, 104, 28, id);
        impl_->buttons.push_back(id);
        if (is_default) { impl_->default_button = id; focus = hwnd; }
    };
    if (request.yes_no) {
        button(L"Yes", IDYES, !request.default_no);
        button(L"No", IDNO, request.default_no);
    } else if (request.okay) button(okay_label.empty() ? L"OK" : okay_label.c_str(), IDOK, true);
    if (request.allow_back)
        button(back_label.empty() ? L"Back" : back_label.c_str(), IDCANCEL, !request.yes_no && !request.okay);
    // PcWindow pumps thread messages without dialog navigation. A local thread
    // hook preserves Enter/Escape/Tab behavior through either pump, without any
    // change to the game window or global keyboard interception.
    Impl::active_ui = impl_.get();
    impl_->message_hook = SetWindowsHookExW(WH_GETMESSAGE, Impl::message_filter, nullptr, impl_->thread);
    if (!impl_->message_hook) throw std::runtime_error("Cannot install native dialog message routing");
    if (impl_->parent && IsWindowEnabled(impl_->parent)) {
        EnableWindow(impl_->parent, FALSE); impl_->parent_disabled = true;
    }
    if (impl_->visible) {
        ShowWindow(impl_->window, background_windows ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
        if (focus && !background_windows) SetFocus(focus);
        if (!request.suppress_sound) MessageBeep(request.text_sound ? MB_OK : MB_ICONERROR);
    }
}
std::optional<MessageDialogSelection> Win32MessageDialogPresenter::poll(std::uint64_t generation) {
    impl_->require_thread();
    if (generation != impl_->request.generation) return {};
    if (!impl_->window) throw std::runtime_error("Native message window disappeared without a user response");
    MSG message{};
    // Only consume this window's messages and child messages. Other game-window
    // events remain available to Presentation/PcWindow's normal pump.
    while (PeekMessageW(&message, impl_->window, 0, 0, PM_REMOVE)) {
        if (!IsDialogMessageW(impl_->window, &message)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (!impl_->window) throw std::runtime_error("Native message window disappeared without a user response");
    }
    try{impl_->device_edges();}catch(...){impl_->close();throw;}
    return std::exchange(impl_->selection, {});
}
void Win32MessageDialogPresenter::close(std::uint64_t generation) noexcept {
    if (generation == impl_->request.generation) impl_->close();
}
} // namespace chili
