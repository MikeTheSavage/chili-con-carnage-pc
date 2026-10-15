#include "save_slot_win32.hpp"
#include "pc_window.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
namespace {
using Presenter=chili::Win32SaveSlotPresenter;using Request=chili::SaveSlotRequest;
using Choice=chili::SaveSlotChoice;using Operation=chili::SaveSlotOperation;
constexpr UINT list_id=100;constexpr unsigned cross=0x4000,circle=0x2000,up=0x10,down=0x40;
unsigned assertions{};
void require(bool result,const char *why){++assertions;if(!result)throw std::runtime_error(why);}
template<class Call>void rejects(Call call,const char *why){bool rejected=false;try{call();}catch(const std::runtime_error&){rejected=true;}require(rejected,why);}
std::wstring desktop_name(HDESK desktop){DWORD size{};GetUserObjectInformationW(desktop,UOI_NAME,nullptr,0,&size);require(size!=0,"desktop name size");std::vector<wchar_t> text(size/sizeof(wchar_t));require(GetUserObjectInformationW(desktop,UOI_NAME,text.data(),size,&size)!=FALSE,"desktop name");return text.data();}
std::wstring input_desktop(){const auto desktop=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);require(desktop!=nullptr,"input desktop inspect");const auto name=desktop_name(desktop);CloseDesktop(desktop);return name;}
struct PrivateDesktop {HDESK original{},isolated{};std::wstring input_before;
    PrivateDesktop(){original=GetThreadDesktop(GetCurrentThreadId());require(original!=nullptr,"original thread desktop");input_before=input_desktop();const auto name=L"ChiliSaveSlotFixture_"+std::to_wstring(GetCurrentProcessId());
        isolated=CreateDesktopW(name.c_str(),nullptr,nullptr,0,DESKTOP_CREATEWINDOW|DESKTOP_CREATEMENU|DESKTOP_READOBJECTS|DESKTOP_WRITEOBJECTS|DESKTOP_HOOKCONTROL,nullptr);require(isolated!=nullptr,"create private desktop");
        if(!SetThreadDesktop(isolated)){CloseDesktop(isolated);isolated=nullptr;throw std::runtime_error("attach private desktop");}verify();}
    void verify(){require(GetThreadDesktop(GetCurrentThreadId())==isolated,"private thread desktop retained");require(input_desktop()==input_before,"user input desktop unchanged");}
    ~PrivateDesktop(){if(isolated){SetThreadDesktop(original);CloseDesktop(isolated);}}
};
HWND dialog_window(){struct Search{HWND window{};unsigned count{};}search;
    EnumThreadWindows(GetCurrentThreadId(),[](HWND hwnd,LPARAM data)->BOOL{wchar_t name[128]{};GetClassNameW(hwnd,name,128);if(std::wstring(name)==L"ChiliNativePspSaveSlotDialog"){auto &search=*reinterpret_cast<Search*>(data);search.window=hwnd;++search.count;}return TRUE;},reinterpret_cast<LPARAM>(&search));
    require(search.count<=1,"at most one slot window");return search.window;}
Request request(std::uint64_t generation,std::size_t initial=0){Request r;r.generation=generation;r.title="Chili Con Carnage - Save profile";r.initial_index=initial;r.language=1;r.button_swap=1;
    // Actual observed LISTSAVE shape: DATA00..19, newData title, no existing
    // saves, LATEST focus resolves to the first available empty slot.
    for(unsigned index=0;index<20;++index)r.slots.push_back({"DATA"+std::string(index<10?"0":"")+std::to_string(index),"Title New data",false});return r;}
void hidden(HWND dialog,HWND parent){require(dialog&&IsWindow(dialog)&&!IsWindowVisible(dialog)&&!IsWindowVisible(parent),"all top-level fixture windows hidden");bool hidden_children=true;
    EnumChildWindows(dialog,[](HWND child,LPARAM data)->BOOL{if(IsWindowVisible(child))*reinterpret_cast<bool*>(data)=false;return TRUE;},reinterpret_cast<LPARAM>(&hidden_children));require(hidden_children,"all fixture children effectively hidden");}
void pump(chili::PcWindow &game){require(game.pump(),"actual hidden PcWindow pump remains running");}
void key(HWND target,UINT key_value,bool repeat=false){require(PostMessageW(target,WM_KEYDOWN,key_value,repeat?0x40000001u:1)!=FALSE,"post synthetic keydown");require(PostMessageW(target,WM_KEYUP,key_value,0xC0000001u)!=FALSE,"post synthetic keyup");}
void mouse(HWND control,int x=8,int y=8){require(control!=nullptr,"native control exists");require(PostMessageW(control,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y))!=FALSE&&PostMessageW(control,WM_LBUTTONUP,0,MAKELPARAM(x,y))!=FALSE,"post synthetic mouse down/up");}
// confirmed: the choice came from the button of the separate question about a
// slot that holds a save. The caller writes over or deletes a save only with it.
void chosen(Presenter &presenter,std::uint64_t generation,Choice choice,std::size_t index=0,bool confirmed=false){const auto selected=presenter.poll(generation);
    require(selected&&selected->generation==generation&&selected->choice==choice&&(choice==Choice::Cancel||selected->index==index),"explicit input yields expected choice/index/token");
    require(selected->confirmed==confirmed,"the confirmed mark is set exactly when the separate question was answered");
    require(!presenter.poll(generation),"selection returned once");}
bool confirming(HWND dialog){return (GetWindowLongPtrW(GetDlgItem(dialog,IDYES),GWL_STYLE)&WS_VISIBLE)!=0;}
void pending_and_mouse(){chili::PcWindow game(false);const auto original_focus=GetFocus();Presenter presenter([&]{return game.handle();},false);require(!dialog_window()&&IsWindowEnabled(game.handle()),"construction does not open UI");
    presenter.open(request(1));const auto dialog=dialog_window();hidden(dialog,game.handle());require(GetWindow(dialog,GW_OWNER)==game.handle()&&!IsWindowEnabled(game.handle()),"owned dialog disables game parent");require(GetFocus()==original_focus,"hidden open does not steal focus");
    const auto list=GetDlgItem(dialog,list_id);require(SendMessageW(list,LB_GETCOUNT,0,0)==20&&SendMessageW(list,LB_GETCURSEL,0,0)==0,"actual20-slot shape/default first");
    for(unsigned n=0;n<20;++n){pump(game);require(!presenter.poll(1),"initial focus/window pump never chooses slot");}
    presenter.close(999);require(IsWindow(dialog)&&!IsWindowEnabled(game.handle()),"stale close cannot affect active dialog");
    const auto height=static_cast<int>(SendMessageW(list,LB_GETITEMHEIGHT,0,0));mouse(list,12,height*4+height/2);pump(game);
    require(SendMessageW(list,LB_GETCURSEL,0,0)==4&&!presenter.poll(1),"list mouse highlights only");mouse(GetDlgItem(dialog,IDOK));pump(game);chosen(presenter,1,Choice::Choose,4);
    presenter.close(1);require(!IsWindow(dialog)&&IsWindowEnabled(game.handle())&&!dialog_window(),"choice teardown restores parent");
    rejects([&]{presenter.poll(1);},"programmatic close does not invent choice");std::cout<<"PASS actual20-slot LISTSAVE shape, pending/default focus, native list/button mouse and parent restore\n";}
void native_keys(){
    {chili::PcWindow game(false);Presenter presenter([&]{return game.handle();},false);presenter.open(request(10,3));auto dialog=dialog_window();auto list=GetDlgItem(dialog,list_id);SetFocus(list);require(GetFocus()==list,"private test focus on list");
        key(list,VK_DOWN);pump(game);require(SendMessageW(list,LB_GETCURSEL,0,0)==4&&!presenter.poll(10),"native down arrow selection only");key(list,VK_UP);pump(game);require(SendMessageW(list,LB_GETCURSEL,0,0)==3,"native up arrow");
        key(list,VK_TAB);pump(game);require(GetFocus()==GetDlgItem(dialog,IDOK)&&!presenter.poll(10),"Tab list to Save without choice");key(GetFocus(),VK_TAB);pump(game);require(GetFocus()==GetDlgItem(dialog,IDCANCEL),"Tab Save to Cancel");
        SetFocus(list);key(list,VK_RETURN);pump(game);chosen(presenter,10,Choice::Choose,3);presenter.close(10);}
    {chili::PcWindow game(false);Presenter presenter([&]{return game.handle();},false);presenter.open(request(11));const auto list=GetDlgItem(dialog_window(),list_id);SetFocus(list);key(list,VK_ESCAPE);pump(game);chosen(presenter,11,Choice::Cancel);presenter.close(11);}
    {chili::PcWindow game(false);Presenter presenter([&]{return game.handle();},false);presenter.open(request(12));mouse(GetDlgItem(dialog_window(),IDCANCEL));pump(game);chosen(presenter,12,Choice::Cancel);presenter.close(12);}
    std::cout<<"PASS actual PcWindow hook routing for Up/Down/Enter/Escape/Tab and native Cancel button\n";}
void overwrite(){chili::PcWindow game(false);Presenter presenter([&]{return game.handle();},false);auto r=request(20,2);r.slots[2]={"DATA02","Existing profile",true};presenter.open(r);const auto dialog=dialog_window();
    mouse(GetDlgItem(dialog,IDOK));pump(game);require(confirming(dialog)&&!presenter.poll(20),"Save existing opens separate confirmation only");
    const auto def=SendMessageW(dialog,DM_GETDEFID,0,0);require(HIWORD(def)==DC_HASDEFID&&LOWORD(def)==IDNO,"overwrite defaults to Back");
    require(!IsWindowEnabled(GetDlgItem(dialog,IDOK))&&!IsWindowEnabled(GetDlgItem(dialog,list_id)),"main controls disabled during overwrite");
    key(GetDlgItem(dialog,IDNO),VK_RETURN,true);pump(game);require(confirming(dialog)&&!presenter.poll(20),"autorepeated Enter cannot overwrite or dismiss");
    require(PostMessageW(dialog,WM_COMMAND,IDOK,reinterpret_cast<LPARAM>(GetDlgItem(dialog,IDOK)))!=FALSE,"queue stale main Save command");pump(game);require(confirming(dialog)&&!presenter.poll(20),"stale Save command cannot confirm overwrite");
    SetFocus(GetDlgItem(dialog,IDNO));key(GetFocus(),VK_RETURN);pump(game);require(!confirming(dialog)&&!presenter.poll(20),"fresh Enter on Back returns to slots without Save");
    SetFocus(GetDlgItem(dialog,list_id));key(GetFocus(),VK_RETURN);pump(game);require(confirming(dialog)&&!presenter.poll(20),"second selection asks confirmation again");
    SetFocus(GetDlgItem(dialog,IDNO));key(GetFocus(),VK_TAB);pump(game);require(GetFocus()==GetDlgItem(dialog,IDYES),"confirmation Tab reaches Overwrite");key(GetFocus(),VK_RETURN);pump(game);chosen(presenter,20,Choice::Choose,2,true);presenter.close(20);
    r=request(22,1);r.slots[1].exists=true;presenter.open(r);mouse(GetDlgItem(dialog_window(),IDOK));pump(game);require(!presenter.poll(22),"overwrite still pending");mouse(GetDlgItem(dialog_window(),IDYES));pump(game);chosen(presenter,22,Choice::Choose,1,true);presenter.close(22);
    std::cout<<"PASS explicit separate overwrite view, Back default, repeat/stale protection and native keyboard/mouse confirmation\n";}
void device_edges(){for(unsigned swap:{0u,1u}){
    chili::PcWindow game(false);unsigned state=swap?cross:circle,samples=0;Presenter presenter([&]{return game.handle();},false,[&]{++samples;return state;});auto r=request(30+swap);r.button_swap=swap;presenter.open(r);
    for(unsigned n=0;n<10;++n)require(!presenter.poll(r.generation),"held confirm at open never auto-selects");const auto before=samples;require(!presenter.poll(999)&&samples==before,"stale poll does not consume device sample");
    state=0;require(!presenter.poll(r.generation),"release confirm pending");state=down;require(!presenter.poll(r.generation),"down edge only navigates");require(SendMessageW(GetDlgItem(dialog_window(),list_id),LB_GETCURSEL,0,0)==1,"device down advances one slot");
    for(unsigned n=0;n<4;++n)require(!presenter.poll(r.generation),"held down does not repeat");require(SendMessageW(GetDlgItem(dialog_window(),list_id),LB_GETCURSEL,0,0)==1,"no repeated device movement");
    state=0;presenter.poll(r.generation);state=up;presenter.poll(r.generation);require(SendMessageW(GetDlgItem(dialog_window(),list_id),LB_GETCURSEL,0,0)==0,"up edge navigates");
    state=0;presenter.poll(r.generation);state=swap?cross:circle;chosen(presenter,r.generation,Choice::Choose,0);presenter.close(r.generation);
    ++r.generation;presenter.open(r);require(!presenter.poll(r.generation),"restart baselines held confirm again");state=0;presenter.poll(r.generation);state=swap?circle:cross;chosen(presenter,r.generation,Choice::Cancel);presenter.close(r.generation);
    }
    {chili::PcWindow game(false);unsigned state{};Presenter presenter([&]{return game.handle();},false,[&]{return state;});auto r=request(40,2);r.slots[2].exists=true;presenter.open(r);state=cross;
        require(!presenter.poll(40)&&confirming(dialog_window()),"device Save existing opens confirmation only");for(unsigned n=0;n<5;++n)require(!presenter.poll(40),"held confirm cannot overwrite");
        state=0;presenter.poll(40);state=cross;require(!presenter.poll(40)&&!confirming(dialog_window()),"fresh confirm on default Back returns to list");state=0;presenter.poll(40);state=cross;presenter.poll(40);
        require(confirming(dialog_window()),"confirmation reopened");state=0;presenter.poll(40);state=up;require(!presenter.poll(40),"device up targets Overwrite without confirming");state=0;presenter.poll(40);state=cross;chosen(presenter,40,Choice::Choose,2,true);presenter.close(40);}
    {chili::PcWindow game(false);unsigned state{};Presenter presenter([&]{return game.handle();},false,[&]{return state;});presenter.open(request(41));state=cross|circle;chosen(presenter,41,Choice::Cancel);presenter.close(41);}
    std::cout<<"PASS device edge baselines, both button swaps, navigation, stale polls/restart and deliberate overwrite\n";}
void load(){chili::PcWindow game(false);unsigned state{};Presenter presenter([&]{return game.handle();},false,[&]{return state;});auto r=request(50);r.operation=Operation::Load;r.slots[1]={"DATA01","Existing profile",true};presenter.open(r);const auto dialog=dialog_window();
    require(!IsWindowEnabled(GetDlgItem(dialog,IDOK)),"Load empty slot disabled");state=cross;require(!presenter.poll(50)&&!confirming(dialog),"empty Load cannot choose or overwrite");state=0;presenter.poll(50);state=down;presenter.poll(50);
    require(IsWindowEnabled(GetDlgItem(dialog,IDOK))&&!presenter.poll(50),"Load enabled for existing selection only");state=0;presenter.poll(50);state=cross;chosen(presenter,50,Choice::Choose,1);require(!confirming(dialog),"Load never asks overwrite");presenter.close(50);
    state=0;r.generation=51;presenter.open(r);const auto list=GetDlgItem(dialog_window(),list_id);const auto height=static_cast<int>(SendMessageW(list,LB_GETITEMHEIGHT,0,0));mouse(list,12,height+height/2);pump(game);mouse(GetDlgItem(dialog_window(),IDOK));pump(game);chosen(presenter,51,Choice::Choose,1);presenter.close(51);
    std::cout<<"PASS shared Load operation chooses existing slots only, with no overwrite path\n";}
std::wstring caption(HWND control){wchar_t text[512]{};GetWindowTextW(control,text,512);return text;}
// The delete list (savedata mode 7): only a slot that holds a save can be
// chosen, never without the separate question, and Back is its default.
void deletion(){chili::PcWindow game(false);unsigned state{};Presenter presenter([&]{return game.handle();},false,[&]{return state;});auto r=request(52);r.operation=Operation::Delete;r.title="Delete profile";
    r.slots[1]={"DATA01","Existing profile",true};r.confirmation_note="Its files are moved to PSP\\SAVEDATA_PREVIOUS.";presenter.open(r);const auto dialog=dialog_window();
    require(caption(GetDlgItem(dialog,IDOK))==L"Delete"&&caption(GetDlgItem(dialog,IDYES))==L"Delete"&&caption(GetDlgItem(dialog,101))==L"Choose a save to delete.","the delete list names its buttons and its purpose");
    require(!IsWindowEnabled(GetDlgItem(dialog,IDOK)),"Delete disabled on an empty slot");state=cross;require(!presenter.poll(52)&&!confirming(dialog),"an empty slot can be neither deleted nor asked about");
    state=0;presenter.poll(52);state=down;presenter.poll(52);require(IsWindowEnabled(GetDlgItem(dialog,IDOK))&&!presenter.poll(52),"Delete enabled on a slot with a save, nothing chosen yet");
    state=0;presenter.poll(52);state=cross;require(!presenter.poll(52)&&confirming(dialog),"choosing a save to delete asks the separate question only");
    const auto question=caption(GetDlgItem(dialog,102));require(question.find(L"Delete this save?")!=std::wstring::npos&&question.find(L"Existing profile")!=std::wstring::npos&&question.find(L"SAVEDATA_PREVIOUS")!=std::wstring::npos,"the question names the save and where it goes");
    const auto def=SendMessageW(dialog,DM_GETDEFID,0,0);require(LOWORD(def)==IDNO,"the delete question defaults to Back");
    for(unsigned n=0;n<5;++n)require(!presenter.poll(52),"a held confirm cannot delete");
    state=0;presenter.poll(52);state=cross;require(!presenter.poll(52)&&!confirming(dialog),"confirm on the default Back returns to the list without deleting");
    state=0;presenter.poll(52);state=cross;presenter.poll(52);require(confirming(dialog),"the question comes again");state=0;presenter.poll(52);state=up;require(!presenter.poll(52),"up moves to Delete without deleting");
    state=0;presenter.poll(52);state=cross;chosen(presenter,52,Choice::Choose,1,true);presenter.close(52);
    state=0;r.generation=53;presenter.open(r);const auto list=GetDlgItem(dialog_window(),list_id);const auto height=static_cast<int>(SendMessageW(list,LB_GETITEMHEIGHT,0,0));
    mouse(list,12,height*3+height/2);pump(game);mouse(GetDlgItem(dialog_window(),IDOK));pump(game);require(!presenter.poll(53)&&!confirming(dialog_window()),"the mouse cannot delete an empty slot");
    mouse(list,12,height+height/2);pump(game);mouse(GetDlgItem(dialog_window(),IDOK));pump(game);require(!presenter.poll(53)&&confirming(dialog_window()),"the mouse on Delete asks the question");
    mouse(GetDlgItem(dialog_window(),IDNO));pump(game);require(!presenter.poll(53)&&!confirming(dialog_window()),"Back by mouse returns to the list");
    mouse(GetDlgItem(dialog_window(),IDOK));pump(game);mouse(GetDlgItem(dialog_window(),IDYES));pump(game);chosen(presenter,53,Choice::Choose,1,true);presenter.close(53);
    r.generation=54;presenter.open(r);state=circle;chosen(presenter,54,Choice::Cancel);presenter.close(54);
    // The save list says the same about a save it would replace.
    state=0;auto save=request(56,1);save.slots[1].exists=true;save.confirmation_note="The save it replaces is kept.";presenter.open(save);state=cross;presenter.poll(56);
    require(confirming(dialog_window())&&caption(GetDlgItem(dialog_window(),102)).find(L"The save it replaces is kept.")!=std::wstring::npos&&caption(GetDlgItem(dialog_window(),IDYES))==L"Overwrite","the overwrite question carries the caller's note");presenter.close(56);
    std::cout<<"PASS delete list: saves only, separate question with Back as default, device and mouse, cancel, and the caller's note in both questions\n";}
void combined_keyboard_provider(){chili::PcWindow game(false);unsigned state{};Presenter presenter([&]{return game.handle();},false,[&]{return state;});presenter.open(request(55,3));const auto dialog=dialog_window(),list=GetDlgItem(dialog,list_id);
    SetFocus(list);state=down;require(PostMessageW(list,WM_KEYDOWN,VK_DOWN,1)!=FALSE,"queue combined down key");pump(game);require(!presenter.poll(55)&&SendMessageW(list,LB_GETCURSEL,0,0)==4,"combined keyboard/provider moves once");
    require(PostMessageW(list,WM_KEYUP,VK_DOWN,0xC0000001u)!=FALSE,"queue combined down release");state=0;pump(game);presenter.poll(55);
    state=up;presenter.poll(55);require(SendMessageW(list,LB_GETCURSEL,0,0)==3,"later actual device edge still navigates");state=0;presenter.poll(55);
    const auto cancel=GetDlgItem(dialog,IDCANCEL);SetFocus(cancel);state=cross;require(PostMessageW(cancel,WM_KEYDOWN,VK_SPACE,1)!=FALSE,"queue combined Space down");pump(game);
    require(!presenter.poll(55),"Space on native Cancel must not prematurely choose PSP Save");require(PostMessageW(cancel,WM_KEYUP,VK_SPACE,0xC0000001u)!=FALSE,"queue combined Space release");state=0;pump(game);chosen(presenter,55,Choice::Cancel);presenter.close(55);
    std::cout<<"PASS combined keyboard/gamepad sampler avoids double arrows and Space misconfirmation\n";}
void lifecycle(){
    {chili::PcWindow game(false);Presenter presenter([&]{return game.handle();},false);presenter.open(request(60));const auto old=dialog_window();key(GetDlgItem(old,list_id),VK_RETURN);presenter.close(60);presenter.open(request(61,4));pump(game);
        require(!presenter.poll(60)&&!presenter.poll(61),"queued old keys cannot answer restarted token");presenter.close(60);require(IsWindow(dialog_window())&&!IsWindowEnabled(game.handle()),"old close leaves restarted window");presenter.close(61);}
    {chili::PcWindow game(false);Presenter presenter([&]{return game.handle();},false);presenter.open(request(62));const auto dialog=dialog_window();require(PostMessageW(dialog,WM_CLOSE,0,0)!=FALSE,"explicit user close queued");pump(game);chosen(presenter,62,Choice::Cancel);require(IsWindow(dialog),"user close waits for caller teardown");presenter.close(62);}
    {chili::PcWindow game(false);Presenter presenter([&]{return game.handle();},false);presenter.open(request(63));require(DestroyWindow(dialog_window())!=FALSE,"fixture unexpected dialog destruction");pump(game);rejects([&]{presenter.poll(63);},"unexpected destruction is not Cancel");require(IsWindowEnabled(game.handle()),"unexpected destruction restores owner");presenter.close(63);presenter.open(request(64));require(!presenter.poll(64),"restart after unexpected destruction pending");presenter.close(64);}
    {chili::PcWindow game(false);const auto parent=game.handle();Presenter presenter([&]{return parent;},false);presenter.open(request(65));require(DestroyWindow(parent)!=FALSE,"fixture parent close");require(!dialog_window(),"parent destruction removes owned slot window");rejects([&]{presenter.poll(65);},"parent close cannot fabricate Cancel/Save");presenter.close(65);}
    {chili::PcWindow game(false);EnableWindow(game.handle(),FALSE);Presenter presenter([&]{return game.handle();},false);presenter.open(request(66));presenter.close(66);require(!IsWindowEnabled(game.handle()),"preserve previously disabled owner");}
    {chili::PcWindow game(false);HWND old{};{Presenter presenter([&]{return game.handle();},false);presenter.open(request(67));old=dialog_window();}require(!IsWindow(old)&&IsWindowEnabled(game.handle()),"destructor cleans window/hook/parent");Presenter restart([&]{return game.handle();},false);restart.open(request(68));require(!restart.poll(68),"new presenter starts pending");restart.close(68);}
    {chili::PcWindow game(false);auto presenter=std::make_unique<Presenter>([&]{return game.handle();},false);presenter->open(request(69));const auto old=dialog_window();std::thread worker([owned=std::move(presenter)]()mutable{owned.reset();});worker.join();pump(game);
        require(!IsWindow(old)&&IsWindowEnabled(game.handle())&&!dialog_window(),"off-thread destructor routes teardown safely to native UI thread");}
    {chili::PcWindow game(false);Presenter presenter([&]{return game.handle();},false);presenter.open(request(70));bool open_failed=false,poll_failed=false;std::thread worker([&]{try{presenter.open(request(71));}catch(const std::runtime_error&){open_failed=true;}try{presenter.poll(70);}catch(const std::runtime_error&){poll_failed=true;}});worker.join();require(open_failed&&poll_failed,"open/poll reject foreign UI thread");require(!presenter.poll(70),"thread misuse never selects");presenter.close(70);}
    std::cout<<"PASS stale restart, explicit close, unexpected/parent destruction, prior-disabled owner, hook cleanup and UI thread lifetime\n";}
void bad_requests(){chili::PcWindow game(false);Presenter presenter([&]{return game.handle();},false);
    for(unsigned kind=0;kind<6;++kind){auto r=request(80+kind);switch(kind){case 0:r.title=std::string("\xC0\xAF",2);break;case 1:r.slots.clear();break;case 2:r.initial_index=20;break;case 3:r.button_swap=2;break;case 4:r.slots[2].name.clear();break;default:r.operation=static_cast<Operation>(9);}
        rejects([&]{presenter.open(r);},"invalid request must reject");presenter.close(r.generation);require(!dialog_window()&&IsWindowEnabled(game.handle()),"failed open leaves no UI/disabled owner");}
    Presenter throwing([&]{return game.handle();},false,[]()->std::uint32_t{throw std::runtime_error("synthetic input provider failure");});rejects([&]{throwing.open(request(90));},"provider baseline failure rejects");require(!dialog_window()&&IsWindowEnabled(game.handle()),"provider open failure rolls back");
    unsigned calls{};Presenter poll_throwing([&]{return game.handle();},false,[&]()->std::uint32_t{if(calls++)throw std::runtime_error("synthetic poll failure");return 0;});poll_throwing.open(request(93));rejects([&]{poll_throwing.poll(93);},"provider poll failure rejects");require(!dialog_window()&&IsWindowEnabled(game.handle()),"provider poll failure tears down without choice");
    presenter.open(request(91));Presenter second([&]{return game.handle();},false);rejects([&]{second.open(request(92));},"one slot presenter per UI thread");second.close(92);require(IsWindow(dialog_window())&&!IsWindowEnabled(game.handle()),"second presenter failure preserves first");presenter.close(91);
    std::cout<<"PASS typed request/UTF8/index/mapping validation, callback failure and exclusive slot routing\n";}
}
int main(){try{PrivateDesktop desktop;pending_and_mouse();desktop.verify();native_keys();desktop.verify();overwrite();desktop.verify();device_edges();desktop.verify();load();desktop.verify();deletion();desktop.verify();combined_keyboard_provider();desktop.verify();lifecycle();desktop.verify();bad_requests();desktop.verify();require(!dialog_window(),"no final native slot window leak");
    std::cout<<"PASS "<<assertions<<" save-slot presenter assertions; hidden private desktop, synthetic events only, no saves or game launched\n";
}catch(const std::exception &error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
