#include "savedata_runtime.hpp"
#include "thread_runtime.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
using Bytes=std::vector<std::uint8_t>;
constexpr unsigned capture_base=0x08DF0000u,params=0x08DF68C0u,ram_base=0x08000000u;
constexpr unsigned init_nid=0x50C4CD57u,update_nid=0xD4B95FFBu,status_nid=0x8874DBE0u,shutdown_nid=0x9790B33Cu;
constexpr unsigned bad_status=0x80110001u,bad_address=0x80110002u,bad_size=0x80110004u,wrong_type=0x80110005u;
unsigned checks{};
void require(bool v,const char* message){++checks;if(!v)throw std::runtime_error(message);}
Bytes read(const std::filesystem::path &p,std::size_t size){
    std::ifstream in(p,std::ios::binary);require(bool(in),"Missing private actual pointer-followed capture");
    Bytes b{std::istreambuf_iterator<char>{in},std::istreambuf_iterator<char>{}};
    require(b.size()==size,"Unexpected actual capture length");return b;
}
struct Captures {
    Bytes ram,payload;
    std::array<Bytes,4> media;
    explicit Captures(const std::filesystem::path&root):ram(read(root/"listsave_pointer_actual_request_ram.bin",65536)),payload(read(root/"listsave_pointer_actual_payload.bin",1066)){
        media[0]=read(root/"listsave_pointer_actual_icon0.bin",20905);
        media[2]=read(root/"listsave_pointer_actual_pic1.bin",112530);
        media[3]=read(root/"listsave_pointer_actual_snd0.bin",251708);
    }
};
void pattern(psprecomp::AllegrexContext &c){
    for(unsigned i=1;i<c.gpr.size();++i)c.gpr[i]=0xF00D0000u+i;
    c.gpr[28]=0x08D9AB00u;c.gpr[29]=0x09FFF000u;c.gpr[31]=0x08830178u;c.pc=0x08D98D98u;
    c.hi=0x12345678u;c.lo=0x89ABCDEFu;c.fcr31=0x76543210u;
    for(unsigned i=0;i<c.fpr.size();++i)c.fpr[i]=float(i)*1.25f;
    for(unsigned i=0;i<c.vfpu.size();++i)c.vfpu[i]=-float(i)*0.75f;
    for(unsigned i=0;i<c.vfpu_ctrl.size();++i)c.vfpu_ctrl[i]=0xC0DE0000u+i;
    c.fpr[31]=std::bit_cast<float>(0x7FC12345u);c.vfpu[127]=std::bit_cast<float>(0xFFC54321u);
}
bool same_cpu(const psprecomp::AllegrexContext&a,const psprecomp::AllegrexContext&b){
    for(unsigned i=0;i<a.fpr.size();++i)if(std::bit_cast<std::uint32_t>(a.fpr[i])!=std::bit_cast<std::uint32_t>(b.fpr[i]))return false;
    for(unsigned i=0;i<a.vfpu.size();++i)if(std::bit_cast<std::uint32_t>(a.vfpu[i])!=std::bit_cast<std::uint32_t>(b.vfpu[i]))return false;
    return a.gpr==b.gpr&&a.pc==b.pc&&a.hi==b.hi&&a.lo==b.lo&&a.fcr31==b.fcr31&&a.vfpu_ctrl==b.vfpu_ctrl;
}
struct Presenter:chili::SaveSlotPresenter {
    unsigned opens{},polls{},closes{};
    chili::SaveSlotRequest request;
    std::deque<chili::SaveSlotSelection> queued;
    bool fail_open{},fail_poll{};
    void open(const chili::SaveSlotRequest&r)override{++opens;request=r;if(fail_open)throw std::runtime_error("Injected presenter open failure");}
    std::optional<chili::SaveSlotSelection> poll(std::uint64_t g)override{
        ++polls;require(g==request.generation,"Presenter poll generation differs from open");
        if(fail_poll)throw std::runtime_error("Injected presenter poll failure");
        if(queued.empty())return{};auto s=queued.front();queued.pop_front();return s;
    }
    void close(std::uint64_t g)noexcept override{if(g==request.generation)++closes;}
    // confirmed: the player answered the dialog's separate question about a slot that holds a save.
    void choose(std::size_t i,bool confirmed=false){queued.push_back({request.generation,chili::SaveSlotChoice::Choose,i,confirmed});}
    void cancel(){queued.push_back({request.generation,chili::SaveSlotChoice::Cancel,0});}
};
struct Backend:chili::SavedataBackend {
    psprecomp::GuestMemory&m;const Captures&capture;
    unsigned inspections{},saves{},loads{},removes{};
    std::set<std::string> existing;
    std::string last_slot;
    bool last_overwrite{},committed{},collision{},unexpected{},reorder{};
    std::uint32_t inspect_error{},save_error{},load_error{},remove_error{};
    chili::SavedataLoadedProfile loaded;
    Backend(psprecomp::GuestMemory&memory,const Captures&c):m(memory),capture(c){
        loaded.payload=c.payload;loaded.media=c.media;loaded.title="Loaded title";loaded.savedata_title="Loaded RAM";loaded.detail="12%";loaded.parental=8;
    }
    std::vector<chili::SavedataSlotInfo> inspect(const chili::save_request::Request&r)override{
        ++inspections;if(inspect_error)throw chili::SavedataBackendError(inspect_error,"Injected typed inspect error");
        const auto names=r.interactive()?r.slots:std::vector<std::string>{r.slot};std::vector<chili::SavedataSlotInfo> out;
        for(std::size_t i=0;i<names.size();++i)out.push_back({{names[i],existing.contains(names[i])?"Existing RAM":"New slot",existing.contains(names[i])},100u+i});
        if(reorder&&out.size()>1)std::swap(out[0],out[1]);return out;
    }
    chili::SavedataSaveReport save(const chili::save_request::Request&r,const std::string&slot,bool overwrite)override{
        ++saves;last_slot=slot;last_overwrite=overwrite;
        require(chili::save_request::header_unchanged(m,r),"Backend observed changed save header");
        require(r.game=="ULUS10216"&&r.file=="SECURE.BIN"&&r.title=="RAM"&&r.savedata_title=="RAM"&&r.detail=="0%"&&r.parental==9u,"Captured save metadata changed");
        require(chili::save_request::bytes(m,r.data)==capture.payload,"Backend received synthetic/mismatched payload");
        for(unsigned i=0;i<4;++i)require(chili::save_request::bytes(m,r.media[i])==capture.media[i],"Backend received synthetic/mismatched media");
        std::array<std::uint8_t,16> actual{};m.copy_out(params+1500u,actual);require(r.key==actual,"Private actual key was not forwarded unchanged");
        require(m.load32(params+28u)==0xA55A5AA5u,"Runtime wrote success before backend commit");
        if(unexpected)throw std::runtime_error("Injected unexpected backend failure");
        if(save_error)throw chili::SavedataBackendError(save_error,"Injected typed save error");
        if(collision&&!overwrite)throw chili::SavedataBackendError(0x80110383u,"New-slot collision rejected");
        committed=true;return {existing.contains(slot),existing.contains(slot)?"kept/"+slot:std::string{},{}};
    }
    std::string remove(const chili::save_request::Request&r,const std::string&slot)override{
        ++removes;last_slot=slot;require(chili::save_request::header_unchanged(m,r),"Backend observed changed delete header");
        require(r.is_delete()&&r.game=="ULUS10216","Delete request reached the backend changed");
        require(m.load32(params+28u)==0xA55A5AA5u,"Runtime wrote a result before the delete was done");
        if(remove_error)throw chili::SavedataBackendError(remove_error,"Injected typed delete error");
        existing.erase(slot);return "kept/"+slot;
    }
    std::string previous_place()const override{return "PSP\\SAVEDATA_PREVIOUS";}
    chili::SavedataLoadedProfile load(const chili::save_request::Request&r,const std::string&slot)override{
        ++loads;last_slot=slot;require(chili::save_request::header_unchanged(m,r),"Backend observed changed load header");
        require(m.load32(params+28u)==0xA55A5AA5u,"Runtime wrote success before load completed");
        if(unexpected)throw std::runtime_error("Injected unexpected load failure");
        if(load_error)throw chili::SavedataBackendError(load_error,"Injected typed load error");return loaded;
    }
};
struct Snapshot {Bytes ram,vram,scratch;};
struct Fixture {
    psprecomp::Runtime rt;
    chili::ThreadRuntime threads{rt};chili::UtilityDialogOwner owner;Presenter presenter;Backend backend;
    std::unique_ptr<chili::SavedataRuntime> savedata;
    Fixture(const Captures&c,const std::filesystem::path&mount):backend(rt.memory(),c){
        threads.initialize_bootstrap();pattern(rt.cpu());auto&m=rt.memory();m.copy_in(capture_base,c.ram);
        require(m.load32(params)==1536u&&m.load32(params+48u)==5u,"Actual capture is not1536-byte LISTSAVE");
        m.copy_in(m.load32(params+116u),c.payload);
        for(unsigned i=0;i<4;++i)if(!c.media[i].empty())m.copy_in(m.load32(params+1412u+16u*i),c.media[i]);
        m.store32(params+28u,0xA55A5AA5u);
        savedata=std::make_unique<chili::SavedataRuntime>(rt,mount,&threads,&owner,&presenter,&backend);
    }
    unsigned call(unsigned nid,unsigned a0=0u,bool may_stop=false){
        auto&c=rt.cpu();c.gpr[4]=a0;auto expected=c;auto uid=threads.current_uid();auto published_uid=psprecomp::runtime_thread_uid();
        auto generation=psprecomp::runtime_thread_switch_generation();auto time=threads.now_us();rt.invoke_import("sceUtility",nid,c);expected.gpr[2]=c.gpr[2];
        require(may_stop||!rt.stopped(),"Utility HLE stopped unexpectedly");require(same_cpu(expected,c),"Utility HLE changed full CPU/FPU/VFPU/GP except V0");
        require(threads.current_uid()==uid&&psprecomp::runtime_thread_uid()==published_uid&&psprecomp::runtime_thread_switch_generation()==generation&&threads.now_us()==time,"Utility HLE changed UID/scheduler time/generation");return c.gpr[2];
    }
    void delay(unsigned us){auto before=threads.now_us();auto uid=threads.current_uid();rt.cpu().gpr[4]=us;rt.cpu().gpr[31]=0x08830178u;
        rt.invoke_import("ThreadManForUser",0xCEADEB47u,rt.cpu());require(!rt.stopped()&&threads.current_uid()==uid&&threads.now_us()==before+us,"Actual scheduler DelayThread failed");pattern(rt.cpu());}
    Snapshot snapshot(){Snapshot s{Bytes(rt.memory().size()),Bytes(psprecomp::GuestMemory::kVramSize),Bytes(psprecomp::GuestMemory::kScratchpadSize)};
        rt.memory().copy_out(ram_base,s.ram);rt.memory().copy_out(psprecomp::GuestMemory::kVramPhysicalBase,s.vram);rt.memory().copy_out(psprecomp::GuestMemory::kScratchpadBase,s.scratch);return s;}
    void unchanged(const Snapshot&s){auto after=snapshot();require(s.ram==after.ram&&s.vram==after.vram&&s.scratch==after.scratch,"Utility changed guest RAM/VRAM/scratch before allowed output");}
    void footprint(const Snapshot&s,const std::vector<std::pair<unsigned,std::size_t>>&ranges){
        auto after=snapshot();require(s.vram==after.vram&&s.scratch==after.scratch,"Savedata changed VRAM/scratch");
        std::vector<std::pair<unsigned,std::size_t>> sorted=ranges;std::sort(sorted.begin(),sorted.end());std::size_t range{};
        for(std::size_t i=0;i<s.ram.size();++i){const auto a=ram_base+static_cast<unsigned>(i);while(range<sorted.size()&&std::uint64_t(a)>=std::uint64_t(sorted[range].first)+sorted[range].second)++range;
            bool allowed=range<sorted.size()&&a>=sorted[range].first&&std::uint64_t(a)<std::uint64_t(sorted[range].first)+sorted[range].second;
            if(!allowed&&s.ram[i]!=after.ram[i])throw std::runtime_error("Savedata output escaped exact atomic footprint");}
        ++checks;
    }
    void result_only(const Snapshot&s,unsigned result){require(rt.memory().load32(params+28u)==result,"Unexpected common.result");footprint(s,{{params+28u,4u}});}
    void start(){require(call(init_nid,params)==0u&&call(status_nid)==1u,"Operation Init failed");delay(200000u);require(call(status_nid)==2u,"Operation Running deadline failed");}
    void pending(){require(call(update_nid,1u)==0u&&call(status_nid)==2u,"Interactive no-input Update did not remain Running");}
    void finished(){require(call(update_nid,1u)==0u&&call(status_nid)==3u,"Operation failed Finished transition");}
    void text(unsigned address,const std::string&value,unsigned capacity){Bytes b(capacity);require(value.size()<capacity,"Fixture text too long");std::copy(value.begin(),value.end(),b.begin());rt.memory().copy_in(address,b);}
    void mode(unsigned mode){rt.memory().store32(params+48u,mode);if(mode<4u)text(params+76u,"DATA03",20u);}
    void load_buffers(){auto&m=rt.memory();for(unsigned off:{116u,1412u,1428u,1444u,1460u}){auto size=m.load32(params+off+4u);if(size)m.copy_in(m.load32(params+off),Bytes(size,0xCDu));m.store32(params+off+8u,0u);}}
};

void lifecycle(const Captures&c,const std::filesystem::path&mount){
    Fixture f(c,mount);const auto before=f.snapshot();require(f.call(status_nid)==wrong_type,"Unselected utility wrong-type result");
    require(f.call(init_nid,params)==0u&&f.owner.busy()&&f.backend.inspections==1u,"Init did not acquire exactly one utility slot");
    require(f.call(update_nid,1u)==bad_status&&f.presenter.opens==0u&&f.backend.saves==0u,"Init performed premature UI/save");f.unchanged(before);
    f.delay(199999u);require(f.call(status_nid)==1u,"Init deadline completed too early");f.savedata->tick();f.unchanged(before);
    f.delay(1u);require(f.call(status_nid)==2u,"Init deadline did not complete");
    for(unsigned i=0;i<3u;++i)f.pending();require(f.presenter.opens==1u&&f.presenter.polls==3u&&f.backend.saves==0u&&f.backend.loads==0u,"Pending UI fabricated an action or reopened");f.unchanged(before);
    require(f.presenter.request.slots.size()==20u&&f.presenter.request.initial_index==0u&&f.presenter.request.operation==chili::SaveSlotOperation::Save,"Captured20-slot focus/default operation mismatch");
    for(unsigned i=0;i<20u;++i)require(f.presenter.request.slots[i].name=="DATA"+(i<10u?std::string("0"):std::string{})+std::to_string(i)&&!f.presenter.request.slots[i].exists,"Presenter invented or reordered actual slots");
    require(f.call(shutdown_nid)==bad_status,"Pending UI accepted Shutdown");f.presenter.cancel();f.finished();f.result_only(before,1u);
    require(f.backend.saves==0u&&f.backend.loads==0u&&f.presenter.closes==1u,"Genuine cancel performed IO or did not close UI");
    const auto after=f.snapshot();require(f.call(shutdown_nid)==0u&&f.call(status_nid)==4u,"Shutdown failed");
    f.delay(1999u);f.savedata->tick();require(f.owner.busy(),"Shutdown released shared owner too early");f.delay(1u);
    auto cpu=f.rt.cpu();auto uid=f.threads.current_uid();auto time=f.threads.now_us();auto generation=psprecomp::runtime_thread_switch_generation();
    f.savedata->tick();require(!f.owner.busy()&&same_cpu(cpu,f.rt.cpu())&&f.threads.current_uid()==uid&&f.threads.now_us()==time&&psprecomp::runtime_thread_switch_generation()==generation,"tick changed guest context/time or failed owner release");f.unchanged(after);
    require(f.call(status_nid)==0u&&f.owner.try_acquire(chili::UtilityDialogType::Message),"Shutdown tick did not permit another utility");
    require(f.call(init_nid,0xDEADBEEFu)==bad_status&&f.owner.is_current_type(chili::UtilityDialogType::Message),"Busy invalid-pointer precedence failed");f.owner.release(chili::UtilityDialogType::Message);
    // A second dialog rejects the first dialog's delayed device event.
    f.rt.memory().store32(params+28u,0xA55A5AA5u);f.start();f.pending();const auto again=f.snapshot();
    f.presenter.queued.push_back({f.presenter.request.generation-1u,chili::SaveSlotChoice::Choose,4u});f.pending();f.unchanged(again);
    f.presenter.choose(4u);f.finished();require(f.backend.committed&&f.backend.last_slot=="DATA04"&&!f.backend.last_overwrite,"Save did not select/commit actual empty DATA04");
    require(f.rt.memory().read_c_string(params+76u,20u)=="DATA04"&&f.rt.memory().load32(params+28u)==0u,"Save success/name missing after real mock commit");f.footprint(again,{{params+76u,20u},{params+28u,4u}});
    std::cout<<"Actual LISTSAVE: real privately read payload/media/key,20 actual slots, no-input pending, genuine Cancel, timed Init/Shutdown/tick/shared ownership, stale generation, committed save + exact output PASS\n";
}

void selection_errors(const Captures&c,const std::filesystem::path&mount){
    for(unsigned type=0;type<4u;++type){Fixture f(c,mount);f.start();f.pending();
        if(type==0u)f.presenter.choose(20u);
        if(type==1u)f.presenter.queued.push_back({f.presenter.request.generation,static_cast<chili::SaveSlotChoice>(17),0});
        if(type==2u)f.rt.memory().store32(params+48u,1u);
        if(type==3u)f.rt.memory().store32(params+116u,0x09FFFFFCu);
        const auto before=f.snapshot();f.call(update_nid,1u,true);require(f.rt.stopped()&&f.backend.saves==0u&&f.presenter.closes==1u,"Invalid selection/header did not strict-stop without backend IO");f.unchanged(before);require(f.call(status_nid,0,true)==2u,"Invalid selection/header falsely Finished");}
    {Fixture f(c,mount);f.start();f.pending();f.backend.collision=true;f.presenter.choose(7u);const auto before=f.snapshot();f.finished();
        require(!f.backend.committed&&f.backend.saves==1u&&f.backend.last_slot=="DATA07"&&!f.backend.last_overwrite,"New-slot collision enabled overwrite");f.result_only(before,0x80110383u);}
    {Fixture f(c,mount);f.backend.existing.insert("DATA09");f.start();f.pending();require(f.presenter.request.initial_index==9u,"Focus LATEST did not select existing slot");f.presenter.choose(9u,true);f.finished();require(f.backend.last_overwrite&&f.backend.committed,"Explicit existing-slot overwrite flag not forwarded");
        require(f.presenter.request.confirmation_note.find("SAVEDATA_PREVIOUS")!=std::string::npos,"The overwrite question does not say where the replaced save is kept");}
    // The request's word at offset 56 was read as the permission to write over a
    // save. It is none: the game writes 1 there in its list save and 0 in its
    // automatic save, and a PSP replaces the save in both. What permits a list
    // save over a save is the player's answer in the dialog, and only that.
    for(const auto word:{0u,1u}){Fixture f(c,mount);f.rt.memory().store32(params+56u,word);f.backend.existing.insert("DATA09");f.start();f.pending();f.presenter.choose(9u,true);f.finished();
        require(f.backend.saves==1u&&f.backend.last_slot=="DATA09"&&f.backend.last_overwrite&&f.backend.committed&&f.rt.memory().load32(params+28u)==0u,"A confirmed list save over a save depends on the request's word at offset 56");}
    for(const auto word:{0u,1u}){Fixture f(c,mount);f.rt.memory().store32(params+56u,word);f.backend.existing.insert("DATA09");f.start();f.pending();f.presenter.choose(9u,false);const auto before=f.snapshot();f.finished();
        require(f.backend.saves==0u&&!f.backend.committed&&f.presenter.closes==1u,"A list save wrote over a save without the player's answer to the overwrite question");f.result_only(before,1u);}
    {Fixture f(c,mount);f.start();f.pending();auto before=f.snapshot();f.savedata.reset();require(!f.owner.busy()&&f.presenter.closes==1u,"Destructor left a live owner/presenter");f.unchanged(before);}
    for(bool fail_open:{true,false}){Fixture f(c,mount);f.start();f.presenter.fail_open=fail_open;f.presenter.fail_poll=!fail_open;auto before=f.snapshot();f.call(update_nid,1,true);require(f.rt.stopped()&&f.backend.saves==0u&&f.presenter.closes==1u,"Presenter exception fabricated output");f.unchanged(before);}
    std::cout<<"Selection errors: invalid index/choice, changed header, new-slot collision never overwrite, existing overwrite, destructor/exception closure PASS\n";
}

void backend_errors(const Captures&c,const std::filesystem::path&mount){
    for(unsigned code:{0x80110383u,0x80110384u,0x80110385u}){Fixture f(c,mount);f.backend.save_error=code;f.start();f.pending();f.presenter.choose(3u);auto before=f.snapshot();f.finished();require(!f.backend.committed&&f.backend.saves==1u,"Typed backend error committed");f.result_only(before,code);}
    {Fixture f(c,mount);f.backend.inspect_error=0x80110305u;auto before=f.snapshot();f.start();require(f.presenter.opens==0u&&f.backend.saves==0u,"Inspect error showed an invented empty-slot dialog");f.finished();f.result_only(before,0x80110305u);}
    {Fixture f(c,mount);f.backend.reorder=true;auto before=f.snapshot();f.call(init_nid,params,true);require(f.rt.stopped()&&!f.owner.busy(),"Inconsistent backend slots accepted");f.unchanged(before);}
    {Fixture f(c,mount);f.backend.unexpected=true;f.start();f.pending();f.presenter.choose(3u);auto before=f.snapshot();f.call(update_nid,1u,true);require(f.rt.stopped()&&!f.backend.committed&&f.call(status_nid,0u,true)==2u,"Unexpected backend failure became synthetic success");f.unchanged(before);}
    std::cout<<"Backend errors: typed inspect/save failures write only common.result; inconsistent/unexpected failures strict-stop without guest output PASS\n";
}

void verify_loaded(Fixture&f,const Snapshot&before,bool interactive){
    auto&m=f.rt.memory();const auto&r=f.backend.loaded;std::vector<std::pair<unsigned,std::size_t>> allowed{{params+28u,4u},{params+124u,4u},{params+128u,128u},{params+256u,128u},{params+384u,1024u},{params+1408u,1u}};
    auto equal=[&](unsigned a,const Bytes&b){Bytes actual(b.size());if(!b.empty())m.copy_out(a,actual);require(actual==b,"Loaded real bytes differ from backend");};
    equal(m.load32(params+116u),r.payload);allowed.push_back({m.load32(params+116u),r.payload.size()});require(m.load32(params+124u)==r.payload.size(),"Loaded dataSize mismatch");
    for(unsigned i=0;i<4u;++i){const auto off=1412u+16u*i;if(!m.load32(params+off+4u))continue;equal(m.load32(params+off),r.media[i]);allowed.push_back({m.load32(params+off),r.media[i].size()});allowed.push_back({params+off+8u,4u});require(m.load32(params+off+8u)==r.media[i].size(),"Loaded mediaSize mismatch");}
    require(m.read_c_string(params+128u,128u)==r.title&&m.read_c_string(params+256u,128u)==r.savedata_title&&m.read_c_string(params+384u,1024u)==r.detail&&m.load8(params+1408u)==r.parental,"Loaded SFO metadata mismatch");
    if(interactive){allowed.push_back({params+76u,20u});require(m.read_c_string(params+76u,20u)=="DATA03","LISTLOAD slot output mismatch");}
    require(m.load32(params+28u)==0u&&f.backend.loads==1u&&f.backend.saves==0u,"Load did not finish exactly once");f.footprint(before,allowed);
}
void loads_and_automatic(const Captures&c,const std::filesystem::path&mount){
    for(unsigned mode:{0u,4u}){Fixture f(c,mount);f.mode(mode);f.load_buffers();f.backend.existing.insert("DATA03");auto before=f.snapshot();f.start();
        if(mode==4u){f.pending();f.unchanged(before);require(f.presenter.request.operation==chili::SaveSlotOperation::Load,"LISTLOAD UI operation wrong");f.presenter.choose(3u);}
        f.finished();verify_loaded(f,before,mode==4u);require(f.backend.last_slot=="DATA03","Load used wrong actual slot");require(mode==4u||f.presenter.opens==0u,"AUTOLOAD opened a dialog");}
    // An automatic save replaces the save in its slot without a question, as a
    // PSP does. The game's own automatic save carries 0 in the word at offset 56
    // (its request builder at 0x08830014 clears it and the mode 1 branch leaves it);
    // the 1 this fixture inherits from the list-save capture hid the fault.
    for(const auto word:{0u,1u}){Fixture f(c,mount);f.mode(1u);f.rt.memory().store32(params+56u,word);f.backend.existing.insert("DATA03");auto before=f.snapshot();f.start();f.finished();
        require(f.backend.saves==1u&&f.backend.last_slot=="DATA03"&&f.backend.last_overwrite&&f.presenter.opens==0u&&f.backend.committed,"AUTOSAVE did not commit named slot without UI, or asked the word at offset 56");f.result_only(before,0u);}
    {Fixture f(c,mount);f.mode(4u);f.load_buffers();f.start();f.pending();f.presenter.choose(0u);auto before=f.snapshot();f.call(update_nid,1,true);require(f.rt.stopped()&&f.backend.loads==0u,"LISTLOAD selected nonexistent slot");f.unchanged(before);}
    {Fixture f(c,mount);f.mode(4u);f.load_buffers();f.backend.existing.insert("DATA03");f.start();f.pending();auto before=f.snapshot();f.presenter.cancel();f.finished();require(!f.backend.loads,"LISTLOAD cancel loaded data");f.result_only(before,1u);}
    std::cout<<"AUTOLOAD/AUTOSAVE/LISTLOAD: real payload/media bytes, metadata/sizes/name atomic output, no automatic-dialog input; empty-slot strict stop and genuine load cancel PASS\n";
}

void load_errors(const Captures&c,const std::filesystem::path&mount){
    // Oversize on the last media buffer verifies that earlier queued outputs
    // never become partially visible when a later buffer fails validation.
    for(unsigned type=0;type<7u;++type){Fixture f(c,mount);f.mode(0u);f.load_buffers();
        unsigned expected=0x80110308u;
        if(type==0u)f.backend.loaded.payload.push_back(1);
        if(type>=1u&&type<=3u)f.backend.loaded.media[type==1u?0u:type==2u?2u:3u].push_back(1);
        if(type==4u){f.backend.loaded.title.assign(128u,'T');expected=0x80110306u;}
        if(type==5u){f.backend.loaded.detail=std::string("bad\0detail",10u);expected=0x80110306u;}
        if(type==6u){f.backend.load_error=0x80110305u;expected=f.backend.load_error;}
        auto before=f.snapshot();f.start();f.finished();require(f.backend.loads==1u,"Load failure skipped typed backend");f.result_only(before,expected);}
    // All load destinations must be validated before owner acquisition, using
    // canonical spans so an uncached alias cannot bypass overlap detection.
    for(unsigned type=0;type<6u;++type){Fixture f(c,mount);f.mode(4u);f.load_buffers();auto&m=f.rt.memory();
        if(type==0u)m.store32(params+116u,m.load32(params+1412u)|0x40000000u);
        if(type==1u)m.store32(params+116u,params);
        if(type==2u)m.store32(params+116u,m.load32(params+96u));
        if(type==3u)m.store32(params+1460u,0x09FFFFFCu);
        if(type==4u)m.store32(params+116u,0x09CB35C0u|0x80000000u);
        if(type==5u)m.store32(params+1412u,m.load32(params+1444u)|0x40000000u);
        auto before=f.snapshot();require(f.call(init_nid,params)==bad_address&&!f.owner.busy()&&!f.backend.inspections,"Invalid/aliased load output reached backend or acquired owner");f.unchanged(before);}
    {Fixture f(c,mount);f.mode(0u);f.load_buffers();f.start();f.rt.memory().store32(params+124u,1u);auto before=f.snapshot();f.call(update_nid,1u,true);require(f.rt.stopped()&&f.backend.loads==0u&&f.call(status_nid,0,true)==2u,"Mutated load header caused output or Finished");f.unchanged(before);}
    {Fixture f(c,mount);f.mode(0u);f.load_buffers();f.backend.loaded.media[1].assign(16u,0xABu);auto before=f.snapshot();f.start();f.finished();verify_loaded(f,before,false);require(!f.rt.memory().load32(params+1428u)&&!f.rt.memory().load32(params+1432u)&&!f.rt.memory().load32(params+1436u),"Unrequested media buffer was populated");}
    std::cout<<"Load errors: oversized payload/all real media and late metadata failures only write result; alias/header/list overlap + invalid spans atomic; unrequested media stays untouched PASS\n";
}
// A mode the port does not implement, and a request with a field outside what
// it handles, used to stop the runtime ("Native savedata mode 7 is not
// implemented"), which ended the game from a menu. Now the request runs through
// the utility's life cycle and ends with the result of a cancelled dialog.
void declined(const Captures&c,const std::filesystem::path&mount){
    for(unsigned type=0;type<12u;++type){Fixture f(c,mount);auto&m=f.rt.memory();
        constexpr unsigned modes[]{2u,3u,6u,9u,10u,12u,13u,15u,22u,999u};
        if(type<10u)m.store32(params+48u,modes[type]);
        if(type==10u)m.store32(params+52u,1u);   // bind
        if(type==11u)m.store32(params+4u,12u);   // a language past the twelve of the firmware
        const auto before=f.snapshot();
        require(f.call(init_nid,params)==0u&&f.call(status_nid)==1u&&f.owner.busy(),"A mode the port does not implement was refused at Init");
        require(f.call(update_nid,1u)==bad_status,"A declined request ran before its Init delay");f.unchanged(before);
        f.delay(200000u);require(f.call(status_nid)==2u,"A declined request did not reach Running");f.unchanged(before);
        f.finished();f.result_only(before,1u);
        require(f.backend.saves==0u&&f.backend.loads==0u&&f.backend.removes==0u&&f.backend.inspections==0u&&f.presenter.opens==0u,"A declined request reached the backend or opened a dialog");
        require(f.call(shutdown_nid)==0u&&f.call(status_nid)==4u,"A declined request did not shut down");f.delay(2000u);f.savedata->tick();
        require(!f.owner.busy()&&f.call(status_nid)==0u&&!f.rt.stopped(),"A declined request kept the utility or stopped the runtime");
        // The next request is carried out as usual.
        m.copy_in(capture_base,c.ram);m.store32(params+28u,0xA55A5AA5u);f.start();f.pending();f.presenter.choose(4u);f.finished();
        require(f.backend.committed&&f.backend.last_slot=="DATA04","A request after a declined one was not carried out");}
    std::cout<<"Declined: modes 2,3,6,9,10,12,13,15,22,999 and unsupported bind/language end as cancelled (result 1) through Init/Running/Finished/Shutdown, nothing read or written, runtime not stopped PASS\n";
}
// Mode 7, the game's "Delete save games": the player picks a save from the
// list and answers the question; the backend takes the slot out of the list.
void deletion(const Captures&c,const std::filesystem::path&mount){
    auto prepare=[](Fixture&f){auto&m=f.rt.memory();m.store32(params+48u,7u);m.store32(params+56u,0u);m.store32(params+1480u,0u);
        // The game's delete request has no payload and no media.
        for(unsigned off:{116u,1412u,1428u,1444u,1460u})for(unsigned i=0;i<12u;i+=4u)m.store32(params+off+i,0u);
        f.backend.existing.insert("DATA03");f.backend.existing.insert("DATA07");};
    {Fixture f(c,mount);prepare(f);const auto before=f.snapshot();f.start();f.pending();f.unchanged(before);
        require(f.presenter.request.operation==chili::SaveSlotOperation::Delete&&f.presenter.request.slots.size()==20u&&f.presenter.request.initial_index==3u&&
            f.presenter.request.slots[3].exists&&f.presenter.request.slots[7].exists&&!f.presenter.request.slots[0].exists,"The delete dialog does not show the list with its saves, starting on the first one");
        require(f.presenter.request.confirmation_note.find("SAVEDATA_PREVIOUS")!=std::string::npos,"The delete question does not say where the save goes");
        f.presenter.choose(7u,true);f.finished();
        require(f.backend.removes==1u&&f.backend.last_slot=="DATA07"&&f.backend.saves==0u&&f.backend.loads==0u&&f.presenter.closes==1u,"A confirmed delete did not reach the backend once");
        f.result_only(before,1u);}
    {Fixture f(c,mount);prepare(f);f.start();f.pending();f.presenter.choose(7u,false);const auto before=f.snapshot();f.finished();
        require(f.backend.removes==0u,"A save was deleted without the player's answer to the question");f.result_only(before,1u);}
    {Fixture f(c,mount);prepare(f);f.start();f.pending();f.presenter.cancel();const auto before=f.snapshot();f.finished();
        require(f.backend.removes==0u,"A cancelled delete dialog deleted a save");f.result_only(before,1u);}
    {Fixture f(c,mount);prepare(f);f.start();f.pending();f.presenter.choose(0u,true);const auto before=f.snapshot();f.call(update_nid,1u,true);
        require(f.rt.stopped()&&f.backend.removes==0u,"A delete of an empty slot was passed on");f.unchanged(before);}
    {Fixture f(c,mount);prepare(f);f.backend.remove_error=0x80110345u;f.start();f.pending();f.presenter.choose(3u,true);const auto before=f.snapshot();f.finished();
        require(f.backend.removes==1u,"A failing delete skipped the backend");f.result_only(before,0x80110345u);}
    std::cout<<"LISTDELETE: the list with its saves, confirmed delete once, no delete without the answer or after Cancel, empty slot strict stop, typed error PASS\n";
}
void validation(const Captures&c,const std::filesystem::path&mount){
    for(unsigned type=0;type<6u;++type){Fixture f(c,mount);auto&m=f.rt.memory();unsigned expected=bad_address;
        if(type==0u)m.store32(params,0x5DCu);
        if(type==1u)m.store32(params+96u,0xDEADBEEFu);
        if(type==2u)m.store32(params+96u,params);
        if(type==3u){auto list=m.load32(params+96u);Bytes one(20u);m.copy_out(list,one);m.copy_in(list+20u,one);expected=0x80110388u;}
        if(type==4u){m.store32(params+120u,8u*1024u*1024u+1u);expected=0x80110388u;}
        if(type==5u){m.store32(params+124u,1067u);expected=0x80110388u;}
        if(type==0u)expected=bad_size;
        auto before=f.snapshot();require(f.call(init_nid,params)==expected&&!f.owner.busy()&&!f.backend.inspections,"Invalid request accepted or touched backend/owner");f.unchanged(before);}
    {Fixture f(c,mount);f.start();auto before=f.snapshot();
        for(const auto speed:{2u,1u,0u}){
            require(f.call(update_nid,speed)==0u&&f.call(status_nid)==2u&&!f.rt.stopped()&&f.backend.saves==0u,
                "Animation speed became an operation selector or automatic save");f.unchanged(before);
        }
        require(f.presenter.opens==1u,"Animation speed reopened the native dialog");}
    std::cout<<"Request validation: size, slot pointer/list overlap/duplicates, capacity/size bounds and animation-speed2/1/0 pending behavior PASS\n";
}
}
int main(int argc,char**argv){
    try{const auto root=argc>1?std::filesystem::path(argv[1]):std::filesystem::path("analysis");
        Captures c(root);const auto mount=std::filesystem::current_path()/"empty_ms0_fixture";std::filesystem::create_directories(mount);
        lifecycle(c,mount);selection_errors(c,mount);backend_errors(c,mount);loads_and_automatic(c,mount);load_errors(c,mount);declined(c,mount);deletion(c,mount);validation(c,mount);
        std::cout<<"PASS: captured native savedata operation/runtime fixture ("<<checks<<" assertions); no real saves or embedded private bytes\n";return 0;
    }catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
