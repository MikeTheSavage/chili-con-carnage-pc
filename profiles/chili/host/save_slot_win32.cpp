#include "save_slot_win32.hpp"
#include "dialog_dpi.hpp"
#include "background_window.hpp"
#include <atomic>
#include <limits>
#include <stdexcept>
#include <utility>
namespace chili {
namespace {
constexpr wchar_t class_name[]=L"ChiliNativePspSaveSlotDialog";
constexpr UINT list_id=100,summary_id=101,confirmation_id=102,teardown_message=WM_APP+57;
constexpr std::uint32_t up=0x10,down=0x40,circle=0x2000,cross=0x4000;
std::wstring wide(const std::string &text){
    if(text.size()>16384 || text.find('\0')!=std::string::npos)throw std::runtime_error("Invalid native save-slot text length/NUL");
    if(text.empty())return {};
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(!count)throw std::runtime_error("Native save-slot text is not valid UTF-8");
    std::wstring result(static_cast<std::size_t>(count),L'\0');
    if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),count)!=count)
        throw std::runtime_error("Native save-slot UTF-8 conversion failed");
    return result;
}
}
struct Win32SaveSlotPresenter::Impl {
    struct WindowContext {std::shared_ptr<Impl> state;std::uint64_t generation{};bool transferred{};};
    Parent parent_provider;Buttons buttons_provider;bool visible{true};
    const DWORD thread=GetCurrentThreadId();
    SaveSlotRequest request;std::vector<std::wstring> labels,confirmation_text;
    std::optional<SaveSlotSelection> selection;
    std::atomic<HWND> window{};std::atomic<std::uint64_t> token{};
    HWND parent{},list{},summary{},confirmation{},save{},cancel{},overwrite{},back{};
    HHOOK message_hook{};bool parent_disabled{},filtering{},confirming{},completed{},failed{};
    UINT default_button=IDOK;std::size_t overwrite_index{};std::uint32_t previous_buttons{},native_button_bits{};bool sample_valid{};
    inline static thread_local Impl *active_ui{};
    void require_thread()const{if(thread!=GetCurrentThreadId())throw std::runtime_error("Save-slot presenter requires its native UI thread");}
    void restore_parent()noexcept{
        if(parent_disabled&&parent&&IsWindow(parent))EnableWindow(parent,TRUE);
        parent_disabled=false;parent=nullptr;
    }
    void unroute()noexcept{
        if(message_hook){UnhookWindowsHookEx(message_hook);message_hook=nullptr;}
        if(active_ui==this)active_ui=nullptr;
    }
    void close_on_thread()noexcept{
        unroute();restore_parent(); // Enable the owner before destroying its dialog.
        if(const auto hwnd=window.load())DestroyWindow(hwnd);
        window=nullptr;selection.reset();confirming=completed=failed=false;
        list=summary=confirmation=save=cancel=overwrite=back=nullptr;
    }
    void request_close(std::uint64_t generation)noexcept{
        if(generation!=token.load())return;
        if(thread==GetCurrentThreadId()){close_on_thread();return;}
        // WindowContext keeps Impl alive until teardown is dispatched on the
        // owner UI thread. Never destroy a foreign-thread HWND or dangling PImpl.
        if(const auto hwnd=window.load())PostMessageW(hwnd,teardown_message,
            static_cast<WPARAM>(generation&0xFFFFFFFFu),static_cast<LPARAM>(generation>>32));
    }
    void set_default(UINT id){
        const auto hwnd=window.load();
        if(default_button!=id){if(const auto old=GetDlgItem(hwnd,default_button))SendMessageW(old,BM_SETSTYLE,BS_PUSHBUTTON,TRUE);}
        default_button=id;if(const auto button=GetDlgItem(hwnd,id))SendMessageW(button,BM_SETSTYLE,BS_DEFPUSHBUTTON,TRUE);
    }
    // confirmed: the player pressed the button of the separate question (Overwrite, Delete).
    void choose(SaveSlotChoice choice,std::size_t index=0,bool confirmed=false){
        if(completed)return;selection=SaveSlotSelection{request.generation,choice,index,confirmed};completed=true;
    }
    void update_selected(){
        const auto current=SendMessageW(list,LB_GETCURSEL,0,0);
        const bool valid=current!=LB_ERR&&static_cast<std::size_t>(current)<request.slots.size();
        const bool allowed=valid&&(request.operation==SaveSlotOperation::Save||request.slots[static_cast<std::size_t>(current)].exists);
        EnableWindow(save,allowed);
    }
    void show_main(){
        confirming=false;ShowWindow(list,SW_SHOW);ShowWindow(summary,SW_SHOW);ShowWindow(save,SW_SHOW);ShowWindow(cancel,SW_SHOW);
        EnableWindow(list,TRUE);EnableWindow(save,TRUE);EnableWindow(cancel,TRUE);
        ShowWindow(confirmation,SW_HIDE);ShowWindow(overwrite,SW_HIDE);ShowWindow(back,SW_HIDE);
        EnableWindow(overwrite,FALSE);EnableWindow(back,FALSE);set_default(IDOK);update_selected();
        if(visible)SetFocus(list);
    }
    void ask_save(){
        if(completed||confirming)return;
        const auto current=SendMessageW(list,LB_GETCURSEL,0,0);
        if(current==LB_ERR || static_cast<std::size_t>(current)>=request.slots.size())throw std::runtime_error("Native save-slot selection unavailable");
        const auto index=static_cast<std::size_t>(current);
        if(request.operation==SaveSlotOperation::Load){if(request.slots[index].exists)choose(SaveSlotChoice::Choose,index);return;}
        // An empty slot: a save goes there at once, and there is nothing to delete.
        if(!request.slots[index].exists){if(request.operation==SaveSlotOperation::Save)choose(SaveSlotChoice::Save,index);return;}
        overwrite_index=index;confirming=true;SetWindowTextW(confirmation,confirmation_text[index].c_str());
        ShowWindow(list,SW_HIDE);ShowWindow(summary,SW_HIDE);ShowWindow(save,SW_HIDE);ShowWindow(cancel,SW_HIDE);
        EnableWindow(list,FALSE);EnableWindow(save,FALSE);EnableWindow(cancel,FALSE);
        ShowWindow(confirmation,SW_SHOW);ShowWindow(overwrite,SW_SHOW);ShowWindow(back,SW_SHOW);
        EnableWindow(overwrite,TRUE);EnableWindow(back,TRUE);set_default(IDNO);
        if(visible)SetFocus(back);
    }
    void device_edges(){
        if(!buttons_provider)return;
        const auto sample=buttons_provider();
        if(!sample){sample_valid=false;native_button_bits=0;return;}
        const auto current=*sample;
        if(!sample_valid){previous_buttons=current;sample_valid=true;native_button_bits&=current;return;}
        auto edges=current&~previous_buttons;previous_buttons=current;
        // A caller may reuse the port's combined keyboard/gamepad sampler.
        // Do not apply an arrow/Space a second time after native control input.
        edges&=~native_button_bits;native_button_bits&=current;
        if(completed)return;
        const auto confirm_button=request.button_swap?cross:circle,cancel_button=request.button_swap?circle:cross;
        // A simultaneous back edge wins. Opposing direction edges do nothing.
        if(edges&cancel_button){if(confirming)show_main();else choose(SaveSlotChoice::Cancel);return;}
        if((edges&(up|down))==up || (edges&(up|down))==down){
            if(confirming){const auto id=(edges&up)?IDYES:IDNO;set_default(id);if(visible)SetFocus(GetDlgItem(window.load(),id));}
            else{const auto selected=SendMessageW(list,LB_GETCURSEL,0,0);if(selected!=LB_ERR){
                const auto last=static_cast<LRESULT>(request.slots.size()-1);
                const auto next=(edges&up)?(selected?selected-1:0):(selected<last?selected+1:last);
                SendMessageW(list,LB_SETCURSEL,next,0);update_selected();}}
        }
        if(edges&confirm_button){if(confirming){if(default_button==IDYES)choose(SaveSlotChoice::Save,overwrite_index,true);else show_main();}else ask_save();}
    }
    static LRESULT CALLBACK message_filter(int code,WPARAM wp,LPARAM lp){
        auto *self=active_ui;const auto hwnd=self?self->window.load():nullptr;
        if(code>=0&&wp==PM_REMOVE&&hwnd&&self&&!self->filtering){auto *message=reinterpret_cast<MSG*>(lp);
            if(message->hwnd==hwnd||IsChild(hwnd,message->hwnd)){
                self->filtering=true;
                if(message->message==WM_KEYDOWN){
                    if(message->wParam==VK_UP)self->native_button_bits|=up;
                    if(message->wParam==VK_DOWN)self->native_button_bits|=down;
                    const auto id=GetDlgCtrlID(message->hwnd);
                    if(message->wParam==VK_SPACE&&(id==IDOK||id==IDCANCEL||id==IDYES||id==IDNO))self->native_button_bits|=cross;
                }
                // Key auto-repeat cannot serve as a second overwrite action.
                const bool repeat=message->message==WM_KEYDOWN&&(message->lParam&(1L<<30))&&
                    (message->wParam==VK_RETURN||message->wParam==VK_SPACE||message->wParam==VK_ESCAPE);
                const bool handled=repeat||IsDialogMessageW(hwnd,message);self->filtering=false;
                if(handled){message->hwnd=nullptr;message->message=WM_NULL;message->wParam=0;message->lParam=0;}
            }
        }
        return CallNextHookEx(nullptr,code,wp,lp);
    }
    static LRESULT CALLBACK procedure(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
        auto *context=reinterpret_cast<WindowContext*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(msg==WM_NCCREATE){context=static_cast<WindowContext*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(context));}
        if(!context)return DefWindowProcW(hwnd,msg,wp,lp);
        const auto keep_alive=context->state;auto *self=keep_alive.get();
        if(msg==WM_NCDESTROY){
            self->window=nullptr;self->unroute();self->restore_parent();SetWindowLongPtrW(hwnd,GWLP_USERDATA,0);
            const bool transferred=context->transferred;const auto result=DefWindowProcW(hwnd,msg,wp,lp);if(transferred)delete context;return result;
        }
        try{
            if(msg==teardown_message){const auto generation=std::uint64_t(static_cast<std::uint32_t>(wp))|(std::uint64_t(static_cast<std::uint32_t>(lp))<<32);
                if(generation==context->generation)self->close_on_thread();return 0;}
            if(msg==WM_CLOSE){self->choose(SaveSlotChoice::Cancel);return 0;}
            if(msg==WM_COMMAND&&LOWORD(wp)==list_id&&!self->completed&&!self->confirming){
                if(HIWORD(wp)==LBN_SELCHANGE){self->update_selected();return 0;}
                if(HIWORD(wp)==LBN_DBLCLK){self->ask_save();return 0;}
            }
            if(msg==WM_COMMAND&&HIWORD(wp)==BN_CLICKED&&!self->completed){
                switch(LOWORD(wp)){
                case IDOK:if(!self->confirming)self->ask_save();return 0;
                case IDCANCEL:if(self->confirming)self->show_main();else self->choose(SaveSlotChoice::Cancel);return 0;
                case IDYES:if(self->confirming)self->choose(SaveSlotChoice::Save,self->overwrite_index,true);return 0;
                case IDNO:if(self->confirming)self->show_main();return 0;
                }
            }
            if(msg==DM_GETDEFID)return MAKELONG(self->default_button,DC_HASDEFID);
            if(msg==DM_SETDEFID){self->set_default(static_cast<UINT>(wp));return 1;}
        }catch(...){self->failed=true;return 0;}
        return DefWindowProcW(hwnd,msg,wp,lp);
    }
};
Win32SaveSlotPresenter::Win32SaveSlotPresenter(Parent parent,bool visible,Buttons buttons):impl_(std::make_shared<Impl>()){
    impl_->parent_provider=std::move(parent);impl_->visible=visible;impl_->buttons_provider=std::move(buttons);
}
Win32SaveSlotPresenter::~Win32SaveSlotPresenter(){impl_->request_close(impl_->token.load());}
void Win32SaveSlotPresenter::open(const SaveSlotRequest &request){
    impl_->require_thread();if(impl_->window.load()||Impl::active_ui)throw std::runtime_error("A native save-slot presenter is already active on this UI thread");
    if(!request.generation||request.slots.empty()||request.slots.size()>1024||request.initial_index>=request.slots.size()||request.button_swap>1||
       (request.operation!=SaveSlotOperation::Save&&request.operation!=SaveSlotOperation::Load&&request.operation!=SaveSlotOperation::Delete))
        throw std::runtime_error("Invalid native save-slot request");
    const bool deleting=request.operation==SaveSlotOperation::Delete;
    const auto note=wide(request.confirmation_note);
    const auto title=wide(request.title);std::vector<std::wstring> labels,prompts;labels.reserve(request.slots.size());prompts.reserve(request.slots.size());
    for(std::size_t index=0;index<request.slots.size();++index){const auto &slot=request.slots[index];if(slot.name.empty())throw std::runtime_error("Empty native save-slot name");
        const auto name=wide(slot.name),display=wide(slot.display_title);const auto label=display.empty()?name:display;
        labels.push_back(std::to_wstring(index+1)+L". "+label+(slot.exists?L"  (Existing save)":L"  (Empty slot)"));
        prompts.push_back((deleting?L"Delete this save?\r\n\r\n"+label:L"Overwrite this existing save?\r\n\r\n"+label+L"\r\n\r\nThe selected save will be replaced.")+
            (note.empty()?std::wstring{}:L"\r\n\r\n"+note));}
    const auto parent=impl_->parent_provider?impl_->parent_provider():nullptr;
    if(parent&&(!IsWindow(parent)||GetWindowThreadProcessId(parent,nullptr)!=impl_->thread))throw std::runtime_error("Save-slot parent must exist on the native UI thread");
    const auto held=impl_->buttons_provider?impl_->buttons_provider():std::optional<std::uint32_t>{};
    static const ATOM atom=[] {WNDCLASSW wc{};wc.lpfnWndProc=Impl::procedure;wc.hInstance=GetModuleHandleW(nullptr);wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);wc.lpszClassName=class_name;return RegisterClassW(&wc);}();
    if(!atom)throw std::runtime_error("Cannot register native save-slot window class");
    impl_->request=request;impl_->labels=std::move(labels);impl_->confirmation_text=std::move(prompts);impl_->selection.reset();impl_->token=request.generation;
    impl_->parent=parent;impl_->previous_buttons=held.value_or(0u);impl_->sample_valid=held.has_value();impl_->native_button_bits=0;impl_->completed=impl_->failed=impl_->confirming=false;
    try{
        const DialogDpiScope dialog_dpi;
        RECT rect{0,0,560,370};const DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,extra=WS_EX_DLGMODALFRAME|WS_EX_CONTROLPARENT;AdjustWindowRectEx(&rect,style,FALSE,extra);
        int x=CW_USEDEFAULT,y=CW_USEDEFAULT;if(parent){RECT owner{};if(GetWindowRect(parent,&owner)){x=owner.left+((owner.right-owner.left)-(rect.right-rect.left))/2;y=owner.top+((owner.bottom-owner.top)-(rect.bottom-rect.top))/2;}}
        auto context=std::make_unique<Impl::WindowContext>();context->state=impl_;context->generation=request.generation;
        const auto hwnd=CreateWindowExW(extra,class_name,title.empty()?L"Chili Con Carnage - Save profile":title.c_str(),style,x,y,rect.right-rect.left,rect.bottom-rect.top,parent,nullptr,GetModuleHandleW(nullptr),context.get());
        if(!hwnd)throw std::runtime_error("Cannot create native save-slot window");context->transferred=true;context.release();impl_->window=hwnd;
        const auto font=static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        auto child=[&](const wchar_t *type,const wchar_t *caption,DWORD flags,int cx,int cy,int width,int height,UINT id){
            const auto control=CreateWindowExW(type==std::wstring(L"LISTBOX")?WS_EX_CLIENTEDGE:0,type,caption,WS_CHILD|WS_VISIBLE|flags,cx,cy,width,height,hwnd,reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            if(!control)throw std::runtime_error("Cannot create native save-slot control");SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return control;};
        impl_->summary=child(L"STATIC",deleting?L"Choose a save to delete.":request.operation==SaveSlotOperation::Load?L"Choose an existing save to load.":L"Choose a save slot. Existing saves require confirmation.",0,16,16,528,36,summary_id);
        impl_->list=child(L"LISTBOX",L"",LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_VSCROLL|WS_TABSTOP,16,58,528,256,list_id);
        for(const auto &label:impl_->labels){const auto result=SendMessageW(impl_->list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));if(result==LB_ERR||result==LB_ERRSPACE)throw std::runtime_error("Cannot populate native save slots");}
        if(SendMessageW(impl_->list,LB_SETCURSEL,request.initial_index,0)==LB_ERR)throw std::runtime_error("Cannot focus initial native save slot");
        impl_->save=child(L"BUTTON",deleting?L"Delete":request.operation==SaveSlotOperation::Load?L"Load":L"Save",WS_TABSTOP|BS_DEFPUSHBUTTON,328,330,104,28,IDOK);
        impl_->cancel=child(L"BUTTON",L"Cancel",WS_TABSTOP|BS_PUSHBUTTON,440,330,104,28,IDCANCEL);
        impl_->confirmation=child(L"STATIC",L"",0,24,36,512,240,confirmation_id);
        impl_->overwrite=child(L"BUTTON",deleting?L"Delete":L"Overwrite",WS_TABSTOP|BS_PUSHBUTTON,168,330,152,28,IDYES);
        impl_->back=child(L"BUTTON",L"Back",WS_TABSTOP|BS_PUSHBUTTON,328,330,104,28,IDNO);
        impl_->show_main();Impl::active_ui=impl_.get();impl_->message_hook=SetWindowsHookExW(WH_GETMESSAGE,Impl::message_filter,nullptr,impl_->thread);
        if(!impl_->message_hook)throw std::runtime_error("Cannot install native save-slot message routing");
        if(parent&&IsWindowEnabled(parent)){EnableWindow(parent,FALSE);impl_->parent_disabled=true;}
        if(impl_->visible && background_windows)ShowWindow(hwnd,SW_SHOWNOACTIVATE);
        else if(impl_->visible){ShowWindow(hwnd,SW_SHOWNORMAL);SetFocus(impl_->list);}
    }catch(...){impl_->close_on_thread();throw;}
}
std::optional<SaveSlotSelection> Win32SaveSlotPresenter::poll(std::uint64_t generation){
    impl_->require_thread();if(generation!=impl_->token.load())return {};
    if(!impl_->window.load())throw std::runtime_error("Native save-slot window disappeared without a user response");
    MSG message{};while(PeekMessageW(&message,impl_->window.load(),0,0,PM_REMOVE)){
        if(!IsDialogMessageW(impl_->window.load(),&message)){TranslateMessage(&message);DispatchMessageW(&message);}
        if(!impl_->window.load())throw std::runtime_error("Native save-slot window disappeared without a user response");}
    if(impl_->failed){impl_->close_on_thread();throw std::runtime_error("Native save-slot input handling failed");}
    try{impl_->device_edges();}catch(...){impl_->close_on_thread();throw;}
    return std::exchange(impl_->selection,{});
}
void Win32SaveSlotPresenter::close(std::uint64_t generation)noexcept{impl_->request_close(generation);}
}
