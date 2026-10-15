#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include "run_options.hpp"
namespace chili {
// In-process sampling profiler for a machine without ETW rights.
//
//   CHILI_SAMPLER=<file>          sample the thread that creates the profiler and write <file> when it stops
//   CHILI_SAMPLER_HZ=<n>          samples per second, 10 through 4000 (default 1000)
//   CHILI_SAMPLER_FRAMES=<a>-<b>  sample only while the game frame counter is in [a, b]
//
// A helper thread suspends the target, reads its context, walks its stack and
// resumes it; nothing runs on the target itself. While the target is suspended
// the helper takes no lock the target could hold: no heap, no loader, no stdio.
// Unwind tables are read straight from the loaded images for that reason
// (RtlLookupFunctionEntry takes a lock). scripts/resolve_samples.py turns the
// output into function and generated-unit shares with the linker map. A relink
// replaces bin/chili_recomp.map, so copy it beside the output to keep a profile.
class SamplingProfiler {
public:
    using FrameSource=std::function<std::uint64_t()>;
    // Null when CHILI_SAMPLER is unset.
    static std::unique_ptr<SamplingProfiler> from_environment(FrameSource frame){
        const auto file=environment_text(L"CHILI_SAMPLER");
        if(file.empty())return {};
        auto hz=environment_number(L"CHILI_SAMPLER_HZ","CHILI_SAMPLER_HZ",4000u);
        if(!hz)hz=1000u;
        if(hz<10u)throw psprecomp::Error("CHILI_SAMPLER_HZ must be 10 through 4000");
        std::uint64_t first=0u,last=std::numeric_limits<std::uint64_t>::max();
        const auto range=environment_ascii(L"CHILI_SAMPLER_FRAMES");
        if(!range.empty()){
            const auto dash=range.find('-');std::size_t used_first{},used_last{};
            try{
                if(dash!=std::string::npos){first=std::stoull(range.substr(0u,dash),&used_first);last=std::stoull(range.substr(dash+1u),&used_last);}
            }catch(const std::exception &){used_first=0u;}
            if(dash==std::string::npos || used_first!=dash || used_last!=range.size()-dash-1u || last<first)
                throw psprecomp::Error("CHILI_SAMPLER_FRAMES must be <first frame>-<last frame>");
        }
        return std::make_unique<SamplingProfiler>(file,static_cast<unsigned>(hz),first,last,std::move(frame));
    }
    SamplingProfiler(std::filesystem::path file,unsigned hz,std::uint64_t first_frame,std::uint64_t last_frame,FrameSource frame)
        :file_(std::move(file)),hz_(hz),first_frame_(first_frame),last_frame_(last_frame),frame_(std::move(frame)){
        // Fail now rather than after a ten minute run.
        std::FILE *probe=_wfopen(file_.c_str(),L"wb");
        if(!probe)throw std::runtime_error("Cannot create sampler output: "+file_.string());
        std::fclose(probe);
        if(!DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&target_,
                THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,0u))
            throw std::runtime_error("Sampler cannot open the runtime thread");
        ULONG_PTR low{},high{};
        GetCurrentThreadStackLimits(&low,&high);
        stack_high_=high;
        LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);
        ticks_per_second_=static_cast<std::uint64_t>(frequency.QuadPart);
        refresh_modules();
        pool_.reserve(1u<<22u);stacks_.reserve(1u<<16u);sequence_.reserve(1u<<21u);
        std::cout<<"[sampler] "<<hz_<<" Hz, frames "<<first_frame_<<'-';
        if(last_frame_==std::numeric_limits<std::uint64_t>::max())std::cout<<"end";else std::cout<<last_frame_;
        std::cout<<", output "<<file_.string()<<'\n';
        thread_=std::thread([this]{loop();});
    }
    SamplingProfiler(const SamplingProfiler &)=delete;
    SamplingProfiler &operator=(const SamplingProfiler &)=delete;
    ~SamplingProfiler(){
        stop_.store(true,std::memory_order_release);
        if(thread_.joinable())thread_.join();
        CloseHandle(target_);
        const bool written=write();
        std::cout<<"[sampler] samples="<<samples_<<" stacks="<<stacks_.size()<<" window_ms="<<ticks_to_ms(window_ticks_)
                 <<" suspended_ms="<<ticks_to_ms(suspended_ticks_)<<" failed="<<failed_<<" truncated="<<truncated_
                 <<(written?" written ":" NOT written ")<<file_.string()<<'\n';
    }
private:
    static constexpr unsigned max_depth=192u;
    struct Module{std::uint64_t base,size;const RUNTIME_FUNCTION *functions;std::uint32_t count;std::wstring path;};
    struct Stack{std::uint32_t offset,depth;std::uint64_t count;};
    struct Sample{std::uint32_t frame,stack;};
    // What the walk needs, as plain data, so the walk can sit in an SEH frame.
    struct ModuleView{std::uint64_t base,size;const RUNTIME_FUNCTION *functions;std::uint32_t count;};

    static std::uint64_t now_ticks() noexcept {LARGE_INTEGER value;QueryPerformanceCounter(&value);return static_cast<std::uint64_t>(value.QuadPart);}
    double ticks_to_ms(std::uint64_t ticks) const noexcept {return static_cast<double>(ticks)*1000.0/static_cast<double>(ticks_per_second_);}

    // Loader work: only ever called while the target is running.
    void refresh_modules(){
        std::vector<HMODULE> handles(512u);DWORD needed{};
        if(!EnumProcessModules(GetCurrentProcess(),handles.data(),static_cast<DWORD>(handles.size()*sizeof(HMODULE)),&needed))return;
        handles.resize(std::min<std::size_t>(handles.size(),needed/sizeof(HMODULE)));
        std::vector<Module> modules;modules.reserve(handles.size());
        for(const auto handle:handles){
            MODULEINFO info{};
            if(!GetModuleInformation(GetCurrentProcess(),handle,&info,sizeof(info)))continue;
            Module module{reinterpret_cast<std::uint64_t>(info.lpBaseOfDll),info.SizeOfImage,nullptr,0u,{}};
            const auto *dos=reinterpret_cast<const IMAGE_DOS_HEADER *>(module.base);
            if(dos->e_magic!=IMAGE_DOS_SIGNATURE)continue;
            const auto *nt=reinterpret_cast<const IMAGE_NT_HEADERS64 *>(module.base+static_cast<std::uint32_t>(dos->e_lfanew));
            if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)continue;
            if(nt->OptionalHeader.NumberOfRvaAndSizes>IMAGE_DIRECTORY_ENTRY_EXCEPTION){
                const auto &directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
                if(directory.VirtualAddress && directory.VirtualAddress+directory.Size<=module.size){
                    module.functions=reinterpret_cast<const RUNTIME_FUNCTION *>(module.base+directory.VirtualAddress);
                    module.count=directory.Size/sizeof(RUNTIME_FUNCTION);
                }
            }
            std::wstring path(1024u,L'\0');
            path.resize(GetModuleFileNameW(handle,path.data(),static_cast<DWORD>(path.size())));
            module.path=std::move(path);
            modules.push_back(std::move(module));
        }
        modules_=std::move(modules);
        views_.clear();
        for(const auto &module:modules_)views_.push_back({module.base,module.size,module.functions,module.count});
    }
    static const ModuleView *find_module(const ModuleView *views,std::size_t count,std::uint64_t address) noexcept {
        for(std::size_t i=0;i<count;++i)if(address-views[i].base<views[i].size)return views+i;
        return nullptr;
    }
    static const RUNTIME_FUNCTION *find_function(const ModuleView &module,std::uint32_t rva) noexcept {
        std::uint32_t low=0u,high=module.count;
        while(low<high){
            const auto middle=low+(high-low)/2u;
            if(module.functions[middle].BeginAddress<=rva)low=middle+1u;else high=middle;
        }
        if(!low)return nullptr;
        const auto *entry=module.functions+(low-1u);
        if(rva>=entry->EndAddress)return nullptr;
        // An odd UnwindData points at the entry that really describes the range.
        if(entry->UnwindData&1u)entry=reinterpret_cast<const RUNTIME_FUNCTION *>(module.base+(entry->UnwindData&~1u));
        return entry;
    }
    // Runs with the target suspended. Returns the frame count; frames[0] is the
    // sampled instruction, the rest are return addresses. `complete` is set when
    // the walk reached the end of the stack rather than an address it could not unwind.
    static unsigned walk(const ModuleView *views,std::size_t view_count,CONTEXT &context,std::uint64_t stack_high,
        std::uint64_t *frames,bool &complete) noexcept {
        unsigned depth=0u;complete=false;
        __try{
            while(depth<max_depth){
                const auto pc=context.Rip;
                const auto *module=find_module(views,view_count,pc);
                if(!module)break;
                frames[depth++]=pc;
                const auto sp=context.Rsp;
                if(const auto *function=find_function(*module,static_cast<std::uint32_t>(pc-module->base))){
                    void *handler_data{};DWORD64 establisher{};
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER,module->base,pc,const_cast<PRUNTIME_FUNCTION>(function),&context,&handler_data,&establisher,nullptr);
                }else{
                    // No unwind data: a leaf that never moved the stack pointer.
                    context.Rip=*reinterpret_cast<const DWORD64 *>(sp);
                    context.Rsp=sp+8u;
                }
                // The stack pointer only moves up, so every read stays inside the
                // target's committed stack and never touches its guard page.
                if(context.Rsp<=sp || context.Rsp>=stack_high){complete=context.Rsp>=stack_high-8u;break;}
                if(!context.Rip){complete=true;break;}
            }
        }__except(EXCEPTION_EXECUTE_HANDLER){}
        return depth;
    }
    std::uint32_t intern(const std::uint64_t *frames,unsigned depth){
        std::uint64_t hash=1469598103934665603ull;
        for(unsigned i=0;i<depth;++i){hash^=frames[i];hash*=1099511628211ull;}
        const auto range=index_.equal_range(hash);
        for(auto it=range.first;it!=range.second;++it){
            auto &stack=stacks_[it->second];
            if(stack.depth==depth && std::equal(frames,frames+depth,pool_.begin()+stack.offset)){++stack.count;return it->second;}
        }
        const auto id=static_cast<std::uint32_t>(stacks_.size());
        stacks_.push_back({static_cast<std::uint32_t>(pool_.size()),depth,1u});
        pool_.insert(pool_.end(),frames,frames+depth);
        index_.emplace(hash,id);
        return id;
    }
    void loop(){
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_TIME_CRITICAL);
        // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION; without it the wait is a 15.6 ms tick.
        HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,0x00000002u,TIMER_ALL_ACCESS);
        const auto interval=ticks_per_second_/hz_;
        std::uint64_t frames[max_depth];
        auto next=now_ticks()+interval,previous=now_ticks();
        bool was_inside=false;
        while(!stop_.load(std::memory_order_acquire)){
            auto now=now_ticks();
            if(next>now){
                if(timer){
                    LARGE_INTEGER due;due.QuadPart=-static_cast<LONGLONG>((next-now)*10000000ull/ticks_per_second_);
                    if(due.QuadPart<0 && SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE))WaitForSingleObject(timer,100u);
                }else Sleep(1u);
                now=now_ticks();
            }
            // No catching up: a late wake-up must not turn into a burst of samples.
            next=std::max(next+interval,now+interval/2u);
            const auto frame=frame_?frame_():0u;
            const bool inside=frame>=first_frame_ && frame<=last_frame_;
            if(inside && was_inside)window_ticks_+=now-previous;
            previous=now;was_inside=inside;
            if(!inside)continue;
            CONTEXT context;context.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER;
            bool complete=false;unsigned depth=0u;
            const auto before=now_ticks();
            if(SuspendThread(target_)==static_cast<DWORD>(-1)){++failed_;continue;}
            if(GetThreadContext(target_,&context))depth=walk(views_.data(),views_.size(),context,stack_high_,frames,complete);
            ResumeThread(target_);
            suspended_ticks_+=now_ticks()-before;
            if(!depth){
                // An address in a module loaded after the last look.
                ++failed_;
                if(now-last_refresh_>ticks_per_second_/10u){refresh_modules();last_refresh_=now;}
                continue;
            }
            if(!complete){
                ++truncated_;
                if(now-last_refresh_>ticks_per_second_/10u){refresh_modules();last_refresh_=now;}
            }
            sequence_.push_back({static_cast<std::uint32_t>(frame),intern(frames,depth)});
            ++samples_;
        }
        if(timer)CloseHandle(timer);
    }
    bool write() const {
        std::FILE *out=_wfopen(file_.c_str(),L"wb");
        if(!out)return false;
        std::wstring exe(32768u,L'\0');
        exe.resize(GetModuleFileNameW(nullptr,exe.data(),static_cast<DWORD>(exe.size())));
        // The link time says which map file these addresses belong to.
        const auto image=reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
        const auto *nt=reinterpret_cast<const IMAGE_NT_HEADERS64 *>(image+static_cast<std::uint32_t>(reinterpret_cast<const IMAGE_DOS_HEADER *>(image)->e_lfanew));
        std::fprintf(out,"chili-sampler 1\nexe %s\nexe_timestamp %08lx\nhz %u\n",utf8(exe).c_str(),nt->FileHeader.TimeDateStamp,hz_);
        std::fprintf(out,"frames %llu %llu\n",static_cast<unsigned long long>(first_frame_),static_cast<unsigned long long>(last_frame_));
        std::fprintf(out,"samples %llu failed %llu truncated %llu window_ms %.3f suspended_ms %.3f\n",
            static_cast<unsigned long long>(samples_),static_cast<unsigned long long>(failed_),static_cast<unsigned long long>(truncated_),
            ticks_to_ms(window_ticks_),ticks_to_ms(suspended_ticks_));
        for(const auto &module:modules_)
            std::fprintf(out,"module %llx %llx %s\n",static_cast<unsigned long long>(module.base),static_cast<unsigned long long>(module.size),utf8(module.path).c_str());
        // stack <id> <count> <leaf address> <return address>...
        for(std::size_t id=0;id<stacks_.size();++id){
            const auto &stack=stacks_[id];
            std::fprintf(out,"stack %zu %llu",id,static_cast<unsigned long long>(stack.count));
            for(std::uint32_t i=0;i<stack.depth;++i)std::fprintf(out," %llx",static_cast<unsigned long long>(pool_[stack.offset+i]));
            std::fputc('\n',out);
        }
        // frame <game frame> <stack id of each sample taken in it, in order>
        for(std::size_t i=0;i<sequence_.size();){
            const auto frame=sequence_[i].frame;
            std::fprintf(out,"frame %u",frame);
            for(;i<sequence_.size() && sequence_[i].frame==frame;++i)std::fprintf(out," %u",sequence_[i].stack);
            std::fputc('\n',out);
        }
        const bool good=!std::ferror(out);
        return std::fclose(out)==0 && good;
    }
    static std::string utf8(const std::wstring &text){
        if(text.empty())return {};
        std::string out(static_cast<std::size_t>(WideCharToMultiByte(CP_UTF8,0u,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr)),'\0');
        WideCharToMultiByte(CP_UTF8,0u,text.data(),static_cast<int>(text.size()),out.data(),static_cast<int>(out.size()),nullptr,nullptr);
        return out;
    }
    std::filesystem::path file_;
    unsigned hz_;
    std::uint64_t first_frame_,last_frame_;
    FrameSource frame_;
    HANDLE target_{};
    std::uint64_t stack_high_{},ticks_per_second_{},last_refresh_{};
    std::vector<Module> modules_;
    std::vector<ModuleView> views_;
    std::vector<std::uint64_t> pool_;
    std::vector<Stack> stacks_;
    std::vector<Sample> sequence_;
    std::unordered_multimap<std::uint64_t,std::uint32_t> index_;
    std::uint64_t samples_{},failed_{},truncated_{},window_ticks_{},suspended_ticks_{};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};
}
