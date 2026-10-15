#include "../host/savedata_runtime.hpp"
#include "../host/thread_runtime.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {
constexpr unsigned base=0x08DF0000u,params=0x08DF68C0u,free_output=0x08DF7118u;
constexpr unsigned init=0x50C4CD57u,update=0xD4B95FFBu,status=0x8874DBE0u,shutdown=0x9790B33Cu;
constexpr unsigned bad_status=0x80110001u,bad_address=0x80110002u,bad_size=0x80110004u,wrong_type=0x80110005u;
void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
std::vector<unsigned char> capture(const std::filesystem::path &path){
    std::ifstream input(path,std::ios::binary);require(bool(input),"Cannot read actual SIZES RAM capture");
    std::vector<unsigned char> result{std::istreambuf_iterator<char>{input},std::istreambuf_iterator<char>{}};
    require(result.size()==65536u,"Actual SIZES RAM capture must be 64 KiB");return result;
}
bool same_cpu(const psprecomp::AllegrexContext &a,const psprecomp::AllegrexContext &b){
    return a.gpr==b.gpr && a.pc==b.pc && a.hi==b.hi && a.lo==b.lo && a.fpr==b.fpr &&
        a.fcr31==b.fcr31 && a.vfpu==b.vfpu && a.vfpu_ctrl==b.vfpu_ctrl;
}
void pattern(psprecomp::AllegrexContext &c){
    for(unsigned i=1;i<c.gpr.size();++i)c.gpr[i]=0xCAFE0000u+i;
    c.gpr[28]=0x08D9AB00u;c.gpr[29]=0x09FFF000u;c.gpr[31]=0x08830228u;c.pc=0x08D98D98u;
    c.hi=0x12345678;c.lo=0x89ABCDEF;c.fcr31=0x87654321;
    for(unsigned i=0;i<c.fpr.size();++i)c.fpr[i]=static_cast<float>(i)*1.25f;
    for(unsigned i=0;i<c.vfpu.size();++i)c.vfpu[i]=-static_cast<float>(i)*0.75f;
    for(unsigned i=0;i<c.vfpu_ctrl.size();++i)c.vfpu_ctrl[i]=0xF0E00000u+i;
}
struct Fixture{
    psprecomp::Runtime rt;
    chili::ThreadRuntime threads{rt};
    chili::UtilityDialogOwner owner;
    std::unique_ptr<chili::SavedataRuntime> savedata;
    Fixture(const std::vector<unsigned char> &data,const std::filesystem::path &root){
        threads.initialize_bootstrap();pattern(rt.cpu());rt.memory().copy_in(base,data);
        savedata=std::make_unique<chili::SavedataRuntime>(rt,root,&threads,&owner);
    }
    unsigned call(unsigned nid,unsigned a0=0u,bool may_stop=false){
        auto &c=rt.cpu();c.gpr[4]=a0;auto expected=c;
        const auto uid=psprecomp::runtime_thread_uid();
        const auto generation=psprecomp::runtime_thread_switch_generation();
        const auto time=threads.now_us();
        rt.invoke_import("sceUtility",nid,c);expected.gpr[2]=c.gpr[2];
        require(may_stop || !rt.stopped(),"SIZES HLE stopped unexpectedly");
        require(same_cpu(c,expected),"SIZES HLE changed full CPU/FPU/VFPU/GP outside V0");
        require(uid==psprecomp::runtime_thread_uid() && generation==psprecomp::runtime_thread_switch_generation() && time==threads.now_us(),
            "SIZES HLE changed UID or scheduler time");return c.gpr[2];
    }
    void delay(unsigned time){
        const auto before=threads.now_us();const auto uid=threads.current_uid();
        rt.cpu().gpr[4]=time;rt.cpu().gpr[31]=0x08830228u;
        rt.invoke_import("ThreadManForUser",0xCEADEB47u,rt.cpu());
        require(!rt.stopped() && threads.current_uid()==uid && threads.now_us()==before+time,"Actual scheduler delay failed");
    }
    std::vector<unsigned char> snapshot()const{
        std::vector<unsigned char> data(65536u);rt.memory().copy_out(base,data);return data;
    }
    void start(){require(call(init,params)==0u && call(status)==1u,"Actual SIZES Init/status failed");delay(200000u);require(call(status)==2u,"SIZES failed Running transition");}
    void finish(){require(call(update,1u)==0u && call(status)==3u,"Actual SIZES Update/status failed");}
};
struct Scratch{
    std::filesystem::path parent,path;
    Scratch(){
        parent=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
        const auto serial=std::chrono::steady_clock::now().time_since_epoch().count();
        path=parent/("chili-sizes-integration-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(serial));
        require(std::filesystem::create_directory(path),"Cannot create isolated empty ms0 mount");
    }
    ~Scratch(){
        std::error_code error;
        const auto absolute=std::filesystem::weakly_canonical(path,error);
        if(!error && absolute.parent_path()==parent && absolute.filename().string().starts_with("chili-sizes-integration-"))
            std::filesystem::remove_all(absolute,error);
    }
};
unsigned long long available(const std::filesystem::path &root){
    ULARGE_INTEGER free{},total{},allfree{};
    require(GetDiskFreeSpaceExW(root.c_str(),&free,&total,&allfree)!=0,"Independent real free-space query failed");return free.QuadPart;
}
void unchanged_except(const std::vector<unsigned char> &before,const std::vector<unsigned char> &after){
    for(unsigned i=0;i<before.size();++i){const auto address=base+i;
        const bool output=(address>=free_output && address<free_output+20u) || (address>=params+28u && address<params+32u);
        require(output || before[i]==after[i],"SIZES changed unrelated memory, safety word or secure key");
    }
}
void actual_query(const std::vector<unsigned char> &data,const std::filesystem::path &root){
    Fixture f(data,root);auto &m=f.rt.memory();
    require(m.load32(params)==0x600u && m.load32(params+48u)==8u && m.load32(params+1488u)==free_output &&
        !m.load32(params+1492u) && !m.load32(params+1496u),"Captured actual free-only SIZES ABI changed");
    require(m.read_c_string(params+60u,13u)=="ULUS10216" && m.read_c_string(params+100u,13u)=="SECURE.BIN","Captured game/file identity changed");
    m.store32(free_output+20u,0xDEADC0DEu);const auto before=f.snapshot();
    require(f.call(status)==wrong_type,"Unselected SIZES utility wrong-type mismatch");
    require(f.call(init,params)==0u && f.owner.busy() && f.call(status)==1u,"SIZES Init failed to own utility slot");
    require(f.call(update,1u)==bad_status && f.snapshot()==before,"SIZES performed IO/output before Running");
    f.delay(200000u);require(f.call(status)==2u,"SIZES Init deadline did not become Running");
    const auto free_before=available(root);f.finish();const auto free_after=available(root);
    const auto minimum=std::min(free_before,free_after),maximum=std::max(free_before,free_after);
    require(m.load32(free_output)==32768u,"SIZES logical MemoryStick geometry mismatch");
    const auto clusters=m.load32(free_output+4u),kilobytes=m.load32(free_output+8u);
    require(clusters>=minimum/32768u && clusters<=maximum/32768u &&
        kilobytes>=minimum/1024u && kilobytes<=maximum/1024u,"SIZES free fields disagree with independent real host queries");
    require(kilobytes>=clusters*32ull && kilobytes<clusters*32ull+32u,"SIZES cluster/freeKB quotient relation mismatch");
    require(m.load32(params+28u)==0u && m.load32(free_output+20u)==0xDEADC0DEu,"SIZES result or 20-byte safety sentinel mismatch");
    const auto text=m.read_c_string(free_output+12u,8u);require(!text.empty() && text.size()<=7u,"SIZES free-size string is not terminated");
    unchanged_except(before,f.snapshot());
    require(f.call(shutdown)==0u && f.call(status)==4u,"SIZES Shutdown transition failed");
    f.delay(2000u);auto expected=f.rt.cpu();const auto bytes=f.snapshot();const auto uid=f.threads.current_uid();const auto time=f.threads.now_us();
    f.savedata->tick();require(!f.owner.busy() && same_cpu(expected,f.rt.cpu()) && f.snapshot()==bytes && f.threads.current_uid()==uid && f.threads.now_us()==time,
        "SIZES tick changed context/output/time or failed to release owner");
    require(f.owner.try_acquire(chili::UtilityDialogType::Message),"SIZES shutdown did not release owner for another utility");
    f.owner.release(chili::UtilityDialogType::Message);
    std::cout<<"Captured SIZES: 1536-byte ABI, real free space, scheduler lifecycle/full CPU/UID, atomic 20-byte output + sentinel PASS\n";
}
void errors(const std::vector<unsigned char> &data,const std::filesystem::path &root){
    for(const auto pointer:{0xDEADBEEFu,free_output+1u,0x09FFFFFCu,params,free_output|0x80000000u}){
        Fixture f(data,root);f.rt.memory().store32(params+1488u,pointer);const auto before=f.snapshot();
        require(f.call(init,params)==bad_address && !f.owner.busy() && f.snapshot()==before,"Invalid SIZES output acquired owner or wrote memory");
    }
    {
        Fixture f(data,root);require(f.owner.try_acquire(chili::UtilityDialogType::Message),"Cannot select busy utility");const auto before=f.snapshot();
        require(f.call(init,0xDEADBEEFu)==bad_status && f.owner.is_current_type(chili::UtilityDialogType::Message) && f.owner.busy() && f.snapshot()==before,
            "Invalid-pointer SIZES Init overrode shared busy precedence");
    }
    {
        Fixture f(data,root);f.start();f.rt.memory().store32(params+1488u,0x09FFFFFCu);const auto before=f.snapshot();
        require(f.call(update,1u)==bad_address && f.call(status)==2u && f.snapshot()==before,"Late invalid SIZES pointer caused partial output/Finished");
        f.rt.memory().store32(params+1488u,free_output);f.finish();
    }
    {
        Fixture f(data,root);f.rt.memory().store32(params,0x5C8u);const auto before=f.snapshot();
        require(f.call(init,params)==bad_size && !f.owner.busy() && f.snapshot()==before,"Truncated SIZES params accepted");
    }
    {
        Fixture f(data,root);f.rt.memory().store32(params+1492u,free_output+16u);const auto before=f.snapshot();
        require(f.call(init,params)==bad_address && !f.owner.busy() && f.snapshot()==before,"Overlapping SIZES structures accepted");
    }
    {
        // Changed on purpose: mode 22 (GETSIZE) is not implemented, and a mode the
        // port does not implement no longer stops the runtime. It is accepted at
        // Init and ends as cancelled (common.result 1) with no size written.
        Fixture f(data,root);f.rt.memory().store32(params+48u,22u);const auto before=f.snapshot();
        require(f.call(init,params)==0u && !f.rt.stopped() && f.owner.busy() && f.snapshot()==before,"GETSIZE mode 22 stopped the runtime or wrote at Init");
    }
    {
        Fixture f(data,root);f.start();f.rt.memory().store32(params+48u,22u);const auto before=f.snapshot();f.call(update,1u,true);
        require(f.rt.stopped() && f.snapshot()==before && f.call(status,0u,true)==2u && f.owner.busy(),
            "Mode mutation enabled unsupported GETSIZE, partly wrote outputs, or incorrectly finished the operation");
    }
    {
        Fixture f(data,root/"missing");f.start();const auto before=f.snapshot();f.call(update,1u,true);
        require(f.rt.stopped() && f.snapshot()==before,"Unmounted native ms0 became synthetic free-space success");
    }
    std::cout<<"SIZES errors: busy precedence, Init/late pointer atomicity + retry, overlaps, truncated params, mode22, mode mutation, unmounted ms0 PASS\n";
}
}
int main(int argc,char **argv){
    try{
        const auto data=capture(argc>1 ? argv[1] : "analysis/savedata_sizes_request_ram.bin");
        Scratch scratch;actual_query(data,scratch.path);errors(data,scratch.path);
        std::cout<<"PASS: native captured savedata SIZES integration\n";return 0;
    }catch(const std::exception &error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
