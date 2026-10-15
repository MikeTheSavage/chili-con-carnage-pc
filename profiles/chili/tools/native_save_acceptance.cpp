#include "savedata_runtime.hpp"
#include "native_savedata_backend.hpp"
#include "native_save_codec.hpp"
#include "param_sfo.hpp"
#include "save_slot_win32.hpp"
#include "pc_window.hpp"
#include "thread_runtime.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

// Production composition only: no codec/backend testing macros, mocked backend,
// environment-selected choices, SendInput or input-desktop activation.
namespace {
using Bytes=std::vector<std::uint8_t>;
constexpr unsigned capture_base=0x08DF0000u,params=0x08DF68C0u,ram_base=0x08000000u;
constexpr unsigned init_nid=0x50C4CD57u,update_nid=0xD4B95FFBu,status_nid=0x8874DBE0u,shutdown_nid=0x9790B33Cu;
unsigned assertions{};
void require(bool v,const char*why){++assertions;if(!v)throw std::runtime_error(why);}
Bytes read(const std::filesystem::path&p,std::size_t min,std::size_t max){
    std::ifstream in(p,std::ios::binary|std::ios::ate);require(bool(in),"Cannot read private fixture file");const auto n=in.tellg();
    require(n>=0&&static_cast<std::uint64_t>(n)>=min&&static_cast<std::uint64_t>(n)<=max,"Private fixture file length outside bound");
    Bytes b(static_cast<std::size_t>(n));in.seekg(0);if(!b.empty())in.read(reinterpret_cast<char*>(b.data()),static_cast<std::streamsize>(b.size()));require(bool(in),"Private fixture file read failed");return b;
}
void write(const std::filesystem::path&p,const Bytes&b){std::ofstream out(p,std::ios::binary|std::ios::trunc);require(bool(out),"Cannot write temporary fixture tamper file");out.write(reinterpret_cast<const char*>(b.data()),static_cast<std::streamsize>(b.size()));out.close();require(!out.fail(),"Fixture tamper write failed");}
struct Captures {
    Bytes ram,payload;std::array<Bytes,4> media;
    explicit Captures(const std::filesystem::path&p):ram(read(p/"listsave_pointer_actual_request_ram.bin",65536,65536)),payload(read(p/"listsave_pointer_actual_payload.bin",1066,1066)){
        media[0]=read(p/"listsave_pointer_actual_icon0.bin",20905,20905);media[2]=read(p/"listsave_pointer_actual_pic1.bin",112530,112530);media[3]=read(p/"listsave_pointer_actual_snd0.bin",251708,251708);
    }
};
std::wstring desktop_name(HDESK d){DWORD bytes{};GetUserObjectInformationW(d,UOI_NAME,nullptr,0,&bytes);require(bytes!=0,"Desktop name size failed");std::vector<wchar_t> text(bytes/sizeof(wchar_t));require(GetUserObjectInformationW(d,UOI_NAME,text.data(),bytes,&bytes)!=FALSE,"Desktop name query failed");return text.data();}
std::wstring input_desktop(){auto d=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);require(d!=nullptr,"Cannot inspect input desktop");auto n=desktop_name(d);require(CloseDesktop(d)!=FALSE,"Cannot close desktop inspection handle");return n;}
struct PrivateDesktop {
    HDESK original{},isolated{};std::wstring input_before;
    PrivateDesktop(){original=GetThreadDesktop(GetCurrentThreadId());require(original!=nullptr,"No original thread desktop");input_before=input_desktop();const auto n=L"ChiliCombinedSaveFixture_"+std::to_wstring(GetCurrentProcessId());
        isolated=CreateDesktopW(n.c_str(),nullptr,nullptr,0,DESKTOP_CREATEWINDOW|DESKTOP_CREATEMENU|DESKTOP_READOBJECTS|DESKTOP_WRITEOBJECTS|DESKTOP_HOOKCONTROL,nullptr);require(isolated!=nullptr,"Cannot create private desktop");
        if(!SetThreadDesktop(isolated)){CloseDesktop(isolated);isolated=nullptr;throw std::runtime_error("Cannot attach private fixture desktop");}verify();}
    void verify(){require(GetThreadDesktop(GetCurrentThreadId())==isolated,"Private thread desktop changed");require(input_desktop()==input_before,"User input desktop changed");}
    ~PrivateDesktop(){if(isolated){SetThreadDesktop(original);CloseDesktop(isolated);}}
};
struct Scratch {
    std::filesystem::path parent,mount;
    explicit Scratch(std::filesystem::path workspace){workspace=std::filesystem::canonical(workspace);parent=workspace/"fixture_roots";std::filesystem::create_directories(parent);parent=std::filesystem::canonical(parent);
        require(parent.parent_path()==workspace,"Fixture roots escaped workspace");mount=parent/("combined-save-fixture-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(!std::filesystem::exists(mount)&&std::filesystem::create_directory(mount),"Cannot create unique temporary ms0 root");mount=std::filesystem::canonical(mount);require(mount.parent_path()==parent,"Fixture mount escaped verified parent");}
    ~Scratch(){std::error_code e;const auto resolved=std::filesystem::weakly_canonical(mount,e);if(!e&&resolved.parent_path()==parent&&resolved.filename().string().starts_with("combined-save-fixture-"))std::filesystem::remove_all(resolved,e);}
};
HWND dialog_window(){struct Search{HWND hwnd{};unsigned count{};}s;EnumThreadWindows(GetCurrentThreadId(),[](HWND w,LPARAM p)->BOOL{wchar_t name[128]{};GetClassNameW(w,name,128);if(std::wstring(name)==L"ChiliNativePspSaveSlotDialog"){auto&s=*reinterpret_cast<Search*>(p);s.hwnd=w;++s.count;}return TRUE;},reinterpret_cast<LPARAM>(&s));require(s.count<=1u,"Multiple slot dialog windows exist");return s.hwnd;}
void hidden(HWND w,HWND parent){require(w&&IsWindow(w)&&!IsWindowVisible(w)&&!IsWindowVisible(parent),"Fixture window became visible");bool invisible=true;EnumChildWindows(w,[](HWND child,LPARAM p)->BOOL{if(IsWindowVisible(child))*reinterpret_cast<bool*>(p)=false;return TRUE;},reinterpret_cast<LPARAM>(&invisible));require(invisible,"Fixture dialog child became visible");require(GetWindow(w,GW_OWNER)==parent&&!IsWindowEnabled(parent),"Dialog ownership/parent disable failed");}
void pattern(psprecomp::AllegrexContext&c){for(unsigned i=1;i<c.gpr.size();++i)c.gpr[i]=0xF00D0000u+i;c.pc=0x08D98D98u;c.gpr[28]=0x08D9AB00u;c.gpr[29]=0x09FFF000u;c.gpr[31]=0x08830178u;c.hi=0x12345678u;c.lo=0x89ABCDEFu;c.fcr31=0x76543210u;
    for(unsigned i=0;i<c.fpr.size();++i)c.fpr[i]=float(i)*1.25f;for(unsigned i=0;i<c.vfpu.size();++i)c.vfpu[i]=-float(i)*0.75f;for(unsigned i=0;i<c.vfpu_ctrl.size();++i)c.vfpu_ctrl[i]=0xC0DE0000u+i;
    c.fpr[31]=std::bit_cast<float>(0x7FC12345u);c.vfpu[127]=std::bit_cast<float>(0xFFC54321u);}
bool same_cpu(const psprecomp::AllegrexContext&a,const psprecomp::AllegrexContext&b){for(unsigned i=0;i<a.fpr.size();++i)if(std::bit_cast<std::uint32_t>(a.fpr[i])!=std::bit_cast<std::uint32_t>(b.fpr[i]))return false;for(unsigned i=0;i<a.vfpu.size();++i)if(std::bit_cast<std::uint32_t>(a.vfpu[i])!=std::bit_cast<std::uint32_t>(b.vfpu[i]))return false;return a.gpr==b.gpr&&a.pc==b.pc&&a.hi==b.hi&&a.lo==b.lo&&a.fcr31==b.fcr31&&a.vfpu_ctrl==b.vfpu_ctrl;}
struct Snapshot{Bytes ram,vram,scratch;};
struct Fixture {
    const Captures&capture;psprecomp::Runtime rt;chili::ThreadRuntime threads{rt};chili::UtilityDialogOwner owner;chili::PcWindow window{false};
    unsigned sampled_buttons{},samples{},diagnostics{};
    chili::Win32SaveSlotPresenter presenter;
    chili::NativeSavedataBackend backend;
    chili::SavedataRuntime savedata;
    Fixture(const Captures&c,const std::filesystem::path&mount):capture(c),presenter([this]{return window.handle();},false,[this]{++samples;return sampled_buttons;}),backend(rt.memory(),mount,[this](const auto&){++diagnostics;}),savedata(rt,mount,&threads,&owner,&presenter,&backend){threads.initialize_bootstrap();reset(5u);}
    void text(unsigned a,const std::string&s,unsigned capacity){Bytes b(capacity);require(s.size()<capacity,"Fixture string length invalid");std::copy(s.begin(),s.end(),b.begin());rt.memory().copy_in(a,b);}
    void reset(unsigned mode){require(!owner.busy()&&!dialog_window(),"Reset attempted with live utility/UI");auto&m=rt.memory();m.copy_in(capture_base,capture.ram);require(m.load32(params)==1536u&&m.load32(params+48u)==5u,"Actual captured request ABI changed");m.copy_in(m.load32(params+116u),capture.payload);for(unsigned i=0;i<4u;++i)if(!capture.media[i].empty())m.copy_in(m.load32(params+1412u+16u*i),capture.media[i]);m.store32(params+28u,0xA55A5AA5u);m.store32(params+48u,mode);if(mode<4u)text(params+76u,"DATA00",20u);pattern(rt.cpu());}
    unsigned call(unsigned nid,unsigned a0=0){auto&c=rt.cpu();c.gpr[4]=a0;auto expected=c;auto uid=threads.current_uid();auto published=psprecomp::runtime_thread_uid();auto generation=psprecomp::runtime_thread_switch_generation();auto time=threads.now_us();rt.invoke_import("sceUtility",nid,c);expected.gpr[2]=c.gpr[2];require(!rt.stopped(),"Combined native utility strict-stopped unexpectedly");require(same_cpu(c,expected),"Combined utility changed full CPU/FPU/VFPU/GP outside V0");require(threads.current_uid()==uid&&psprecomp::runtime_thread_uid()==published&&psprecomp::runtime_thread_switch_generation()==generation&&threads.now_us()==time,"Combined utility changed UID/time/switch generation");return c.gpr[2];}
    void delay(unsigned us){auto time=threads.now_us();auto uid=threads.current_uid();rt.cpu().gpr[4]=us;rt.cpu().gpr[31]=0x08830178u;rt.invoke_import("ThreadManForUser",0xCEADEB47u,rt.cpu());require(!rt.stopped()&&threads.current_uid()==uid&&threads.now_us()==time+us,"Real scheduler delay failed");pattern(rt.cpu());}
    Snapshot snapshot(){Snapshot s{Bytes(rt.memory().size()),Bytes(psprecomp::GuestMemory::kVramSize),Bytes(psprecomp::GuestMemory::kScratchpadSize)};rt.memory().copy_out(ram_base,s.ram);rt.memory().copy_out(psprecomp::GuestMemory::kVramPhysicalBase,s.vram);rt.memory().copy_out(psprecomp::GuestMemory::kScratchpadBase,s.scratch);return s;}
    void footprint(const Snapshot&s,std::vector<std::pair<unsigned,std::size_t>> allowed){const auto after=snapshot();require(s.vram==after.vram&&s.scratch==after.scratch,"Native save touched VRAM/scratch");std::sort(allowed.begin(),allowed.end());std::size_t range{};
        for(std::size_t i=0;i<s.ram.size();++i){auto a=ram_base+static_cast<unsigned>(i);while(range<allowed.size()&&std::uint64_t(a)>=std::uint64_t(allowed[range].first)+allowed[range].second)++range;auto permitted=range<allowed.size()&&a>=allowed[range].first&&std::uint64_t(a)<std::uint64_t(allowed[range].first)+allowed[range].second;if(!permitted&&s.ram[i]!=after.ram[i])throw std::runtime_error("Combined save/load escaped exact guest output footprint");}++assertions;}
    void unchanged(const Snapshot&s){footprint(s,{});}
    void pump(){auto cpu=rt.cpu();auto uid=threads.current_uid();auto time=threads.now_us();require(window.pump(),"Hidden PcWindow pump closed");require(same_cpu(cpu,rt.cpu())&&threads.current_uid()==uid&&threads.now_us()==time,"PcWindow pump changed guest state");}
    void start(){require(call(init_nid,params)==0u&&call(status_nid)==1u&&owner.busy(),"Native save Init failed");delay(199999u);require(call(status_nid)==1u,"Init deadline completed early");delay(1u);require(call(status_nid)==2u,"Init failed Running transition");}
    void update(bool done,unsigned speed=2u){pump();require(call(update_nid,speed)==0u&&call(status_nid)==(done?3u:2u),"Combined operation pending/Finished mismatch");}
    void teardown(){require(!dialog_window()&&IsWindowEnabled(window.handle()),"Native dialog failed to restore owner after action");auto before=snapshot();require(call(shutdown_nid)==0u&&call(status_nid)==4u,"Combined Shutdown failed");delay(1999u);savedata.tick();require(owner.busy(),"Shutdown released owner early");delay(1u);auto cpu=rt.cpu();auto uid=threads.current_uid();auto time=threads.now_us();auto generation=psprecomp::runtime_thread_switch_generation();savedata.tick();require(!owner.busy()&&same_cpu(cpu,rt.cpu())&&threads.current_uid()==uid&&threads.now_us()==time&&psprecomp::runtime_thread_switch_generation()==generation,"Lifecycle tick changed guest state or retained owner");unchanged(before);require(call(status_nid)==0u,"Combined status3->4->0 did not complete");}
    void load_buffers(){auto&m=rt.memory();for(unsigned off:{116u,1412u,1428u,1444u,1460u}){auto n=m.load32(params+off+4u);if(n)m.copy_in(m.load32(params+off),Bytes(n,0xCD));m.store32(params+off+8u,0u);}text(params+128u,"",128u);text(params+256u,"",128u);text(params+384u,"",1024u);m.store8(params+1408u,0);}
};
void assert_files(const std::filesystem::path&mount,const Captures&c){const auto root=mount/"PSP"/"SAVEDATA",slot=root/"ULUS10216DATA00";require(std::filesystem::is_directory(slot),"Committed DATA00 directory missing");std::set<std::string> names;for(const auto&f:std::filesystem::directory_iterator(slot)){require(f.is_regular_file(),"Committed save contains non-file item");names.insert(f.path().filename().string());}require(names==std::set<std::string>{"PARAM.SFO","SECURE.BIN","ICON0.PNG","PIC1.PNG","SND0.AT3"},"Committed save file set differs from requested native files");unsigned roots{};for(const auto&e:std::filesystem::directory_iterator(root)){++roots;require(e.path()==slot,"Save retained unexpected staging/backup/other slot directory");}require(roots==1u,"Native save did not produce exactly one slot");
    require(read(slot/"ICON0.PNG",c.media[0].size(),c.media[0].size())==c.media[0]&&read(slot/"PIC1.PNG",c.media[2].size(),c.media[2].size())==c.media[2]&&read(slot/"SND0.AT3",c.media[3].size(),c.media[3].size())==c.media[3],"Committed real media bytes differ");
    const auto raw=read(slot/"PARAM.SFO",1,1024*1024);const auto doc=chili::sfo::parse(raw);require(doc.entries.size()==7u,"Native SFO field count differs");auto text=[&](const char*k,const char*v){auto*e=doc.find(k);require(e&&e->text_value()==v,"Native SFO text metadata mismatch");};text("TITLE","RAM");text("SAVEDATA_TITLE","RAM");text("SAVEDATA_DETAIL","0%");text("SAVEDATA_DIRECTORY","ULUS10216DATA00");text("CHILI_PC_FILE","SECURE.BIN");require(doc.find("PARENTAL_LEVEL")&&doc.find("PARENTAL_LEVEL")->integer_value()==9u&&doc.find("CHILI_PC_FORMAT")&&doc.find("CHILI_PC_FORMAT")->integer_value()==1u,"Native SFO integer metadata mismatch");require(!doc.find("SAVEDATA_PARAMS")&&!doc.find("SAVEDATA_FILE_LIST"),"Native SFO invented PSP secure metadata");
    const auto encrypted=read(slot/"SECURE.BIN",c.payload.size()+64u,c.payload.size()+64u);require(std::equal(chili::native_save::magic.begin(),chili::native_save::magic.end(),encrypted.begin()),"Committed payload does not use native container magic");require(!std::equal(c.payload.begin(),c.payload.end(),encrypted.begin()+static_cast<std::ptrdiff_t>(chili::native_save::header_bytes)),"Native payload stored plaintext");}
void assert_loaded(Fixture&f,const Snapshot&before){auto&m=f.rt.memory();std::vector<std::pair<unsigned,std::size_t>> allowed{{params+28u,4u},{params+124u,4u},{params+128u,128u},{params+256u,128u},{params+384u,1024u},{params+1408u,1u}};auto same=[&](unsigned a,const Bytes&b){Bytes out(b.size());if(!b.empty())m.copy_out(a,out);require(out==b,"Native AUTOLOAD restored mismatched real bytes");};same(m.load32(params+116u),f.capture.payload);allowed.push_back({m.load32(params+116u),f.capture.payload.size()});require(m.load32(params+124u)==1066u,"Native AUTOLOAD payload size mismatch");for(unsigned i=0;i<4u;++i){auto off=1412u+16u*i;if(!m.load32(params+off+4u))continue;same(m.load32(params+off),f.capture.media[i]);allowed.push_back({m.load32(params+off),f.capture.media[i].size()});allowed.push_back({params+off+8u,4u});require(m.load32(params+off+8u)==f.capture.media[i].size(),"Native AUTOLOAD media size mismatch");}require(m.read_c_string(params+128u,128u)=="RAM"&&m.read_c_string(params+256u,128u)=="RAM"&&m.read_c_string(params+384u,1024u)=="0%"&&m.load8(params+1408u)==9u&&m.load32(params+28u)==0u,"Native AUTOLOAD metadata/result mismatch");f.footprint(before,allowed);}
void acceptance(const Captures&c,const std::filesystem::path&mount,PrivateDesktop&desktop){Fixture f(c,mount);auto&m=f.rt.memory();require(!IsWindowVisible(f.window.handle()),"PcWindow fixture became visible");const auto before=f.snapshot();const auto confirm=m.load32(params+8u)==1u?0x4000u:0x2000u;f.sampled_buttons=confirm;f.start();f.unchanged(before);require(std::filesystem::is_empty(mount),"Init wrote native files before user input");
    f.update(false);auto dialog=dialog_window();hidden(dialog,f.window.handle());require(SendMessageW(GetDlgItem(dialog,100),LB_GETCOUNT,0,0)==20&&SendMessageW(GetDlgItem(dialog,100),LB_GETCURSEL,0,0)==0,"Captured native list/default DATA00 differs");
    f.update(false,1u); // Retain Update(1) coverage beside the actual game speed2.
    for(unsigned i=0;i<8u;++i)f.update(false);require(f.samples>=11u&&!f.backend.last_commit_result()&&std::filesystem::is_empty(mount),"Held confirm committed or backend was bypassed");f.unchanged(before);desktop.verify();
    f.sampled_buttons=0u;f.update(false);f.unchanged(before);require(std::filesystem::is_empty(mount),"Confirm release chose a slot");f.sampled_buttons=confirm;f.update(true);
    require(m.load32(params+28u)==0u&&m.read_c_string(params+76u,20u)=="DATA00","Real edge Save did not publish success/DATA00");auto commit=f.backend.last_commit_result();require(commit&&commit->outcome==chili::savedata_storage::Outcome::Committed,"Actual backend has no committed transaction");f.footprint(before,{{params+28u,4u},{params+76u,20u}});assert_files(mount,c);const auto slot=mount/"PSP"/"SAVEDATA"/"ULUS10216DATA00",cipher_path=slot/"SECURE.BIN",sfo_path=slot/"PARAM.SFO";const auto cipher1=read(cipher_path,1130,1130),sfo1=read(sfo_path,1,1024*1024);f.teardown();desktop.verify();
    std::cout<<"Captured LISTSAVE Update(2)+pending Update(1)->real Win32 sampled edges->native storage/SFO/AES: held confirm pending, release/fresh edge DATA00 commit, exact guest outputs, status3->4->0 PASS\n";
    // The game's automatic save carries 0 in the word at offset 56 (its request
    // builder clears it and only the list save sets it to 1). This step used to
    // keep the 1 of the list-save capture and so never met the fault that made
    // every automatic save after the first fail with 0x80110385.
    f.reset(1u);m.store32(params+56u,0u);auto auto_before=f.snapshot();f.start();f.update(true);require(m.load32(params+28u)==0u&&!dialog_window(),"Named AUTOSAVE failed/opened UI");f.footprint(auto_before,{{params+28u,4u}});const auto cipher2=read(cipher_path,1130,1130),sfo2=read(sfo_path,1,1024*1024);require(sfo1==sfo2&&cipher1!=cipher2&&!std::equal(cipher1.begin()+32,cipher1.begin()+44,cipher2.begin()+32),"AUTOSAVE did not replace using fresh random native nonce");commit=f.backend.last_commit_result();require(commit&&commit->outcome==chili::savedata_storage::Outcome::Committed,"AUTOSAVE replacement did not commit");assert_files(mount,c);f.teardown();desktop.verify();
    const auto kept=mount/"PSP"/"SAVEDATA_PREVIOUS"/"ULUS10216DATA00";
    require(read(kept/"SECURE.BIN",1130,1130)==cipher1&&read(kept/"PARAM.SFO",1,1024*1024)==sfo1&&read(kept/"ICON0.PNG",1,4*1024*1024)==c.media[0],"The save the automatic save replaced is not kept byte for byte in PSP\\SAVEDATA_PREVIOUS");
    std::cout<<"Native named AUTOSAVE with the game's word 0 at offset 56: real replacement without a dialog, new BCrypt nonce/ciphertext, complete file set, exact result-only output, the replaced save kept PASS\n";
    // LISTSAVE over the save through the real dialog, with either value of the word.
    const auto cancel=confirm==0x4000u?0x2000u:0x4000u;
    for(const auto word:{0u,1u}){
        const auto cipher_before=read(cipher_path,1130,1130);
        // The question is answered Back (its default), then the list is left: nothing is written.
        f.reset(5u);m.store32(params+56u,word);auto list_before=f.snapshot();f.sampled_buttons=0u;f.start();f.update(false);
        require(SendMessageW(GetDlgItem(dialog_window(),100),LB_GETCURSEL,0,0)==0,"LISTSAVE did not start on the existing DATA00");
        f.sampled_buttons=confirm;f.update(false);require((GetWindowLongPtrW(GetDlgItem(dialog_window(),IDYES),GWL_STYLE)&WS_VISIBLE)!=0,"Choosing the existing slot did not ask the overwrite question");
        f.sampled_buttons=0u;f.update(false);f.sampled_buttons=confirm;f.update(false);require((GetWindowLongPtrW(GetDlgItem(dialog_window(),IDYES),GWL_STYLE)&WS_VISIBLE)==0,"Back did not return to the list");
        f.sampled_buttons=0u;f.update(false);f.sampled_buttons=cancel;f.update(true);
        require(m.load32(params+28u)==1u&&read(cipher_path,1130,1130)==cipher_before,"A LISTSAVE left without the overwrite answer was not cancelled or changed the save");f.footprint(list_before,{{params+28u,4u}});f.teardown();desktop.verify();
        // The question is answered Overwrite: the save is replaced and the one before it kept.
        f.reset(5u);m.store32(params+56u,word);list_before=f.snapshot();f.sampled_buttons=0u;f.start();f.update(false);
        f.sampled_buttons=confirm;f.update(false);f.sampled_buttons=0u;f.update(false);f.sampled_buttons=0x10u;f.update(false);f.sampled_buttons=0u;f.update(false);
        require(std::filesystem::exists(cipher_path)&&read(cipher_path,1130,1130)==cipher_before,"The save was written before the overwrite answer");
        f.sampled_buttons=confirm;f.update(true);
        require(m.load32(params+28u)==0u&&m.read_c_string(params+76u,20u)=="DATA00","A LISTSAVE over a save failed after the player answered Overwrite");f.footprint(list_before,{{params+28u,4u},{params+76u,20u}});
        require(read(cipher_path,1130,1130)!=cipher_before&&read(kept/"SECURE.BIN",1130,1130)==cipher_before,"The confirmed LISTSAVE did not replace the save and keep the one before it");assert_files(mount,c);f.teardown();desktop.verify();
    }
    std::cout<<"Native LISTSAVE over a save through the real dialog, word 0 and 1 at offset 56: Back then Cancel writes nothing, Overwrite replaces and keeps the save before PASS\n";
    f.reset(0u);f.load_buffers();auto load_before=f.snapshot();f.start();f.unchanged(load_before);f.update(true);assert_loaded(f,load_before);require(!dialog_window(),"Native AUTOLOAD opened UI");f.teardown();desktop.verify();std::cout<<"Native AUTOLOAD: real authenticated filesystem roundtrip restored actual1066-byte payload/three media/metadata with full context + exact atomic output PASS\n";
    // The tamper checks below work on the save as it is on disk now.
    const auto cipher_now=read(cipher_path,1130,1130),sfo_now=read(sfo_path,1,1024*1024);
    for(unsigned which=0;which<2u;++which){auto guard=std::unique_lock<std::recursive_mutex>(*f.backend.serialization());if(which==0u){auto corrupt=cipher_now;corrupt.back()^=0x40u;write(cipher_path,corrupt);}else{write(cipher_path,cipher_now);auto doc=chili::sfo::parse(sfo_now);auto*title=doc.find("TITLE");require(title&&title->storage.size()==128u,"Temporary SFO title missing");title->storage[0]^=1u;write(sfo_path,chili::sfo::serialize(doc));}guard.unlock();f.reset(0u);f.load_buffers();auto tamper_before=f.snapshot();f.start();f.unchanged(tamper_before);f.update(true);require(m.load32(params+28u)==0x80110306u,"Native tamper did not return authentication/broken-data error");f.footprint(tamper_before,{{params+28u,4u}});f.teardown();desktop.verify();}
    require(f.diagnostics==0u,"Combined save unexpectedly required recovery/cleanup");std::cout<<"Real AES-tag and valid-SFO tampering: authentication errors publish only common.result, all guest payload/media/metadata/keys preserved PASS\n";
}
}
int main(int argc,char**argv){try{require(argc==3,"Expected workspace and owned analysis paths");PrivateDesktop desktop;Captures c{std::filesystem::path(argv[2])};Scratch scratch{std::filesystem::path(argv[1])};acceptance(c,scratch.mount,desktop);desktop.verify();require(!dialog_window(),"Final slot dialog leak");std::cout<<"PASS combined native save acceptance: "<<assertions<<" assertions; private hidden desktop, real temporary files, no private bytes logged/no real saves/game launched\n";return 0;}catch(const std::exception&e){std::cerr<<"FAIL combined native save acceptance: "<<e.what()<<'\n';return 1;}}
