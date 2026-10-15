#pragma once
#include "psprecomp/runtime.hpp"
#include "ge_sprites.hpp"
#include "ge_triangle_raster.hpp"
#include "ge_triangles.hpp"
#include "ge_frame_capture.hpp"
#include "frame_costs.hpp"
#include "host_log.hpp"
#include "thread_runtime.hpp"
#include "performance_telemetry.hpp"
#include <unordered_map>
#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <optional>
#include <iostream>
#include <bit>
#include <unordered_set>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>
#include <cstring>
#include <xmmintrin.h>
namespace chili {
class GeRuntime {
public:
    struct Callback {unsigned signal,signal_arg,finish,finish_arg,gp;};
    // Lists run to END inside the enqueue call and take the ids 1,2,3,..., so the
    // completed ids are exactly [1,count]. Same answers as a set of every id ever
    // issued, without a node per list.
    struct CompletedLists {
        unsigned count{};
        void insert(unsigned id) noexcept {if(id>count)count=id;}
        bool contains(unsigned id) const noexcept {return id-1u<count;}
        std::size_t size() const noexcept {return count;}
        bool empty() const noexcept {return !count;}
        operator std::unordered_set<unsigned>() const {
            std::unordered_set<unsigned> ids;
            for(unsigned id=1;id<=count;++id)ids.insert(id);
            return ids;
        }
    };
    // What one execute_list() call did. `end` is the address after the END word;
    // `truncated` means the draw limit ended the list early (tools only).
    struct ListRun {bool completed{},truncated{};unsigned id{},commands{},draws{},end{};};
    std::array<std::optional<Callback>,16> callbacks;
    std::array<unsigned,256> commands{};
    std::array<float,96> bones{};
    std::array<float,12> world{},view{},texture{};
    std::array<float,16> projection{};
    std::array<float,8> morph{};
    unsigned bone_cursor{},world_cursor{},view_cursor{},texture_cursor{},projection_cursor{};
    CompletedLists completed;
    unsigned next_list{1},offset_address{},vertex_address{},index_address{};
    unsigned sprite_copy_count{};
    // Source address of the last CLUT load, and the number of lists handed to the
    // executor by sceGeListEnQueue (the capture/trace ordinal).
    unsigned clut_address{},enqueued_lists{};
    // Offline tools stop a list after this many draws to bisect a frame.
    unsigned draw_limit{0xFFFFFFFFu};
    // Seam for another renderer. When set, every PRIM goes to it instead of the
    // software rasterizer: it returns true once the draw is consumed and writes the
    // cursors the draw advanced to (it receives the current ones), or returns false
    // with `error` set, which stops the list like a failed software draw.
    using DrawBackend=std::function<bool(psprecomp::GuestMemory &memory,const GeRuntime &ge,unsigned prim_data,
                                         unsigned &vertices,unsigned &indices,std::string &error)>;
    DrawBackend draw_backend;
    // Runs after every list sceGeListEnQueue completed, with the framebuffer the
    // list left selected and the number of draws it made. A list that ends on
    // the displayed buffer is the end of a picture: the host presents there.
    // `plan` is what list_plan returned when the game handed the list over: the
    // part of the decision that needs the game's side at that moment. list_end
    // runs on the thread that ran the list, list_plan always on the game thread.
    std::function<std::uint64_t()> list_plan;
    std::function<void(unsigned framebuffer,unsigned draws,std::uint64_t plan)> list_end;
    // Called every few milliseconds while the game thread waits for the list
    // thread: the window's messages (start_worker).
    std::function<void()> wait_pump;
    // Change counters for a renderer that caches decoded state. draw_revision
    // moves when a register the draw descriptor is built from changes value
    // (targets, texture, blend, tests, scissor, vertex type); lighting_revision
    // when a light or material register does. Matrices, VADDR, IADDR and PRIM
    // move neither. Both start at 1; 0 means "do not cache" to the renderer.
    std::uint64_t draw_revision{1},lighting_revision{1};
    static constexpr std::uint8_t kAffectsDraw=1,kAffectsLighting=2;
    static constexpr std::array<std::uint8_t,256> kRevisionClass=[]{
        std::array<std::uint8_t,256> table{};
        for(unsigned c=0xA0;c<=0xAF;++c)table[c]|=kAffectsDraw;
        for(unsigned c=0xB8;c<=0xBF;++c)table[c]|=kAffectsDraw;
        for(unsigned c:{0x12,0x1E,0x1F,0x21,0x22,0x23,0x24,0x27,0x28,0x9C,0x9D,0x9E,0x9F,0xB0,0xB1,
                        0xC2,0xC3,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xCD,0xCE,0xCF,0xD0,0xD2,0xD3,0xD4,0xD5,
                        0xDB,0xDC,0xDD,0xDE,0xDF,0xE0,0xE1,0xE6,0xE7,0xE8,0xE9})table[c]|=kAffectsDraw;
        for(unsigned c=0x17;c<=0x1B;++c)table[c]|=kAffectsLighting;
        for(unsigned c=0x53;c<=0x58;++c)table[c]|=kAffectsLighting;
        for(unsigned c=0x5B;c<=0x5D;++c)table[c]|=kAffectsLighting;
        for(unsigned c=0x5F;c<=0x9A;++c)table[c]|=kAffectsLighting;
        return table;
    }();
    GeClutCache clut;
    bool snapshot_copy(unsigned data){
        if((data&0xFFFFu)!=30u)return false;
        ++sprite_copy_count;
        return !dump_prefix_.empty() && sprite_copy_count==dump_selected_;
    }
    void dump_draw(psprecomp::Runtime &rt,unsigned vertices,unsigned data,const char *phase){
        if(dump_prefix_.empty())return;
        try{
            const std::filesystem::path root(dump_prefix_+"_"+std::to_string(sprite_copy_count)+"_"+phase);
            auto reg=[&](unsigned i){return commands[i]&0xFFFFFFu;};
            const unsigned tex=(reg(0xA0)&0xFFFFF0u)|((reg(0xA8)<<8)&0x0F000000u);
            const unsigned ts=reg(0xA8)&0x7FFu,tw=1u<<(reg(0xB8)&15u),th=1u<<((reg(0xB8)>>8)&15u);
            const unsigned fb=0x04000000u|(reg(0x9C)&0x1FFFF0u),db=0x04000000u|(reg(0x9E)&0x1FFFF0u);
            std::ofstream metadata(root.string()+".txt",std::ios::trunc);
            metadata<<"copy="<<sprite_copy_count<<" phase="<<phase<<"\n"<<std::hex;
            metadata<<"primitive="<<data<<" vertices="<<vertices<<" vtype="<<reg(0x12)<<"\n";
            metadata<<"texture="<<tex<<" stride="<<ts<<" width="<<tw<<" height="<<th<<"\n";
            metadata<<"framebuffer="<<fb<<" stride="<<(reg(0x9D)&0x7FCu)<<" depth="<<db<<" stride="<<(reg(0x9F)&0x7FCu)<<"\n";
            for(unsigned i=0;i<commands.size();++i)metadata<<"reg "<<i<<" "<<commands[i]<<"\n";
            metadata.precision(9);
            auto matrix_dump=[&](const char *name,const auto &values){
                metadata<<std::dec<<name;for(float value:values)metadata<<" "<<value;
                metadata<<"\n"<<std::hex;
            };
            matrix_dump("world",world);matrix_dump("view",view);matrix_dump("projection",projection);
            matrix_dump("texture_matrix",texture);matrix_dump("bones",bones);matrix_dump("morph",morph);
            auto range=[&](const char *kind,unsigned address,unsigned long long length){
                if(!length || length>4u*1024u*1024u || !rt.memory().contains(address,static_cast<std::size_t>(length))){
                    metadata<<kind<<" unavailable address="<<address<<" length="<<length<<"\n";return;
                }
                std::vector<unsigned char> bytes(static_cast<std::size_t>(length));rt.memory().copy_out(address,bytes);
                std::ofstream binary(root.string()+"_"+kind+".bin",std::ios::binary|std::ios::trunc);
                binary.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));binary.close();
                metadata<<kind<<" address="<<address<<" length="<<length<<" saved="<<!binary.fail()<<"\n";
            };
            try {
                const auto layout=ge::make_layout(reg(0x12));
                const unsigned count=data&0xFFFFu;
                unsigned records=count;
                metadata<<"vertex_stride="<<layout.vertex_stride<<" sample_stride="<<layout.sample_stride
                        <<" indices="<<index_address<<" index_bytes="<<layout.index_bytes<<"\n";
                if(layout.index_bytes){
                    const auto index_bytes=static_cast<unsigned long long>(count)*layout.index_bytes;
                    range("indices",index_address,index_bytes);
                    records=0;
                    if(rt.memory().contains(index_address,static_cast<std::size_t>(index_bytes)))
                        for(unsigned n=0;n<count;++n){
                            const unsigned index=layout.index_bytes==1?rt.memory().load8(index_address+n):rt.memory().load16(index_address+n*2u);
                            records=std::max(records,index+1u);
                        }
                }
                range("vertices",vertices,static_cast<unsigned long long>(records)*layout.vertex_stride);
            }catch(const std::exception &failure){metadata<<"vertex_layout_unavailable="<<failure.what()<<"\n";}
            range("vram",psprecomp::GuestMemory::kVramPhysicalBase,psprecomp::GuestMemory::kVramSize);
            // Snapshots remain raw guest bytes, including indexed texture layouts.
            const unsigned format=reg(0xC3)&15u;
            const bool compressed=format>=8u && format<=10u;
            const unsigned pitch=compressed?((ts+3u)/4u)*(format==8u?8u:16u):
                format==4u?ts/2u:format==5u?ts:(format<=2u || format==6u)?ts*2u:ts*4u;
            const unsigned rows=compressed?((th+3u)/4u):(reg(0xC2)&1u)?((th+7u)&~7u):th;
            range("texture",tex,static_cast<unsigned long long>(rows)*pitch);
            const unsigned palette=(reg(0xB0)&0xFFFFF0u)|((reg(0xB1)<<8)&0x0F000000u);
            range("palette_source",palette,std::min((reg(0xC4)&63u)*32u,1024u));
            metadata.close();
            std::cout<<"[ge-dump] "<<root.string()<<" metadata="<<!metadata.fail()<<'\n';
        }catch(const std::exception &e){std::cerr<<"[ge-dump] capture failed: "<<e.what()<<'\n';}
    }
    void matrix(unsigned command,unsigned data){
        const float value=std::bit_cast<float>(data<<8);
        auto write=[&](auto &values,unsigned &cursor,unsigned mask){
            if(cursor<values.size())values[cursor]=value;
            cursor=(cursor+1)&mask;
        };
        switch(command){
        case 0x2A:bone_cursor=data&127;break;
        case 0x2B:write(bones,bone_cursor,127);break;
        case 0x3A:world_cursor=data&15;break;
        case 0x3B:write(world,world_cursor,15);break;
        case 0x3C:view_cursor=data&15;break;
        case 0x3D:write(view,view_cursor,15);break;
        case 0x3E:projection_cursor=data&15;break;
        case 0x3F:write(projection,projection_cursor,15);break;
        case 0x40:texture_cursor=data&15;break;
        case 0x41:write(texture,texture_cursor,15);break;
        default:if(command>=0x2C && command<=0x33)morph[command-0x2C]=value;break;
        }
    }
    // Everything a list can read or change besides guest memory, for frame capture
    // and the replay tool.
    GeExecutorState snapshot() const {
        GeExecutorState s;
        s.commands=commands;s.bones=bones;s.world=world;s.view=view;s.texture=texture;s.projection=projection;s.morph=morph;
        s.bone_cursor=bone_cursor;s.world_cursor=world_cursor;s.view_cursor=view_cursor;
        s.texture_cursor=texture_cursor;s.projection_cursor=projection_cursor;
        s.offset_address=offset_address;s.vertex_address=vertex_address;s.index_address=index_address;
        s.next_list=next_list;s.sprite_copy_count=sprite_copy_count;s.clut_address=clut_address;
        s.clut_bytes=clut.raw_bytes();
        for(std::size_t i=0;i<s.clut_loaded.size();++i)s.clut_loaded[i]=clut.raw_loaded()[i];
        return s;
    }
    void restore(const GeExecutorState &s){
        commands=s.commands;bones=s.bones;world=s.world;view=s.view;texture=s.texture;projection=s.projection;morph=s.morph;
        ++draw_revision;++lighting_revision;
        bone_cursor=s.bone_cursor;world_cursor=s.world_cursor;view_cursor=s.view_cursor;
        texture_cursor=s.texture_cursor;projection_cursor=s.projection_cursor;
        offset_address=s.offset_address;vertex_address=s.vertex_address;index_address=s.index_address;
        next_list=s.next_list;sprite_copy_count=s.sprite_copy_count;clut_address=s.clut_address;
        completed.count=s.next_list?s.next_list-1u:0u;
        clut.restore(s.clut_bytes,s.clut_loaded);
    }
    GeDrawTrace &trace() noexcept {return trace_;}
    // Trace every list instead of only captured ones (replay tool).
    void set_trace_all(bool enabled) noexcept {trace_all_=enabled;}
    // Capture the next `lists` enqueued lists; needs CHILI_GE_CAPTURE_DIR.
    void capture_next(unsigned lists) noexcept {capture_.capture_next(lists);}
    // Optional same-runtime-thread diagnostic. Registered storage outlives GE.
    ParallelLiveObserver *parallel_observer_{};
    void set_parallel_live_observer(ParallelLiveObserver *observer) noexcept { parallel_observer_=observer; }
    PerformanceTelemetry *telemetry_{};
    ThreadRuntime *scheduler{};
    unsigned interrupt_stack{};const unsigned *sdk_version{};
    std::optional<psprecomp::AllegrexContext> interrupt_resume;
    inline static std::unordered_map<psprecomp::Runtime*,GeRuntime*> instances;
    static constexpr unsigned interrupt_return=0x08000014;
    ~GeRuntime(){stop_worker();for(auto it=instances.begin();it!=instances.end();)if(it->second==this)it=instances.erase(it);else ++it;}

    // ---- Lists on a thread of their own ------------------------------------
    //
    // A PSP's GE draws while the CPU goes on: the game hands a frame's scene list
    // over, runs the next frame's logic, and only then waits for the list
    // (sceGeListSync, sceGeDrawSync at the start of its frame-end function). Its
    // scene lists alternate between two buffers for that reason. With the worker
    // on, sceGeListEnQueue only looks for the list's end and its FINISH argument
    // (which the finish callback gets, at once, as before), queues the list and
    // returns; the worker runs it, with every draw, texture decode, submission
    // and present, and the two sync calls wait for it. What a list reads (its
    // own words, vertices, textures, palettes) is read when the worker gets to
    // it: memory the game rewrites before it has synced is seen as rewritten,
    // which is the PSP's own rule.
    //
    // Everything the lists touch belongs to the worker while it runs: the GE
    // state in this object and the whole renderer. Other threads reach the
    // renderer through render_call().
    //
    // Not for runs that capture, trace, dump or log lists, nor for the tools:
    // can_use_worker(). Deterministic runs keep the synchronous path; nothing a
    // list does changes guest state either way.
    bool can_use_worker() const noexcept {
        return !telemetry_ && !log_ && dump_prefix_.empty() && !dump_failure_ && !trace_.available() && !trace_all_ &&
            !trace_lists_active_ && !list_index_on_ && !capture_.enabled() && draw_limit==0xFFFFFFFFu;
    }
    bool worker_running() const noexcept {return worker_on_;}
    // `setup` runs first on the new thread (its priority, its name).
    void start_worker(psprecomp::Runtime &r,std::function<void()> setup={}){
        if(worker_on_ || !can_use_worker())return;
        scout_base_=commands[0x10];scout_offset_=offset_address;scout_finish_=commands[0x0F]&0xFFFFu;
        id_tickets_.fill(0u);
        worker_on_=true;list_takes_id_=false;frame_costs.ge_on_worker=true;
        worker_=std::thread([this,&r,setup=std::move(setup)]{worker_main(r,setup);});
    }
    // Finishes what is queued and ends the thread; the synchronous path is back afterwards.
    void stop_worker(){
        if(!worker_on_)return;
        // Waited for like a sync call, so the window's messages keep moving while the last lists are drawn.
        const auto ticket=push_job({Job::Kind::Stop,0u,0u,{}});
        wait_for_worker([this,ticket]{return jobs_finished_>=ticket;});
        if(worker_.joinable())worker_.join();
        worker_on_=false;list_takes_id_=true;frame_costs.ge_on_worker=false;
        completed.count=next_list-1u;
    }
    // Runs `call` where the lists run, after everything handed over so far, and
    // with `wait` returns when it has run. Without the worker it runs here.
    void render_call(std::function<void()> call,bool wait){
        if(!worker_on_ || on_worker_thread_){call();return;}
        const auto ticket=push_job({Job::Kind::Call,0u,0u,std::move(call)});
        if(wait)wait_for_worker([this,ticket]{return jobs_finished_>=ticket;});
    }

    // ---- A list run ahead of its enqueue -----------------------------------
    //
    // The game closes a frame's scene list, waits for the vertical blank (twice
    // at 30 frames per second) and only then enqueues the list: the whole wait
    // sits between the pad read and the picture. A host that knows which list
    // the game is about to enqueue calls run_early(address) before the wait.
    // The list is run at once (handed to the worker when that is on), and the
    // game's sceGeListEnQueue of that address then only gets its id and its
    // finish callback: what the game sees is the same as without, list ids
    // included. False means nothing was run: the list is not a finished one
    // (no FINISH and END within reach), or this run captures or traces lists.
    bool run_early(psprecomp::Runtime &r,unsigned address){
        if(!address || (address&3u) || interrupt_resume || !can_use_worker())return false;
        if(worker_on_ && !worker_healthy(r))return false;
        // The executor is idle on the synchronous path: its registers are the list's starting state.
        if(!worker_on_){scout_base_=commands[0x10];scout_offset_=offset_address;scout_finish_=commands[0x0F]&0xFFFFu;}
        unsigned end{},finish{};
        if(!scout_list(r,address,end,finish,false))return false;
        ++enqueued_lists;
        const std::uint64_t plan=list_plan?list_plan():0u;
        if(worker_on_){
            early_={true,address,end,finish,push_job({Job::Kind::List,address,plan,{}})};
            return true;
        }
        ListRun run;
        list_takes_id_=false;
        const bool ok=execute_list(r,address,run);
        list_takes_id_=true;
        if(!ok || !run.completed)return false;
        if(list_end){CostTimer cost(frame_costs.submit_ns);list_end(0x04000000u|(commands[0x9C]&0x1FFFF0u),run.draws,plan);}
        early_={true,address,run.end,commands[0x0F]&0xFFFFu,0u};
        return true;
    }
    std::uint64_t early_lists() const noexcept {return early_hits_;}
    static void returned(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c){
        auto &self=*instances.at(&rt);
        if(!self.interrupt_resume){rt.stop("GE interrupt return without continuation");return;}
        c=*self.interrupt_resume;self.interrupt_resume.reset();
        if(self.log_)std::cout<<"[ge] finish interrupt returned continuation="<<std::hex<<c.pc<<std::dec<<'\n';
        self.scheduler->leave_interrupt(c);
    }
    // One PRIM command, through draw_backend when set and the software rasterizer
    // otherwise. True means the draw took effect and the vertex/index cursors
    // advanced; false leaves both untouched.
    bool draw_primitive(psprecomp::Runtime &r,unsigned address,unsigned word,unsigned draw_index){
        const unsigned data=word&0xFFFFFFu;
        const unsigned original_vertices=vertex_address,original_indices=index_address;
        const bool capture=snapshot_copy(data);
        if(capture)dump_draw(r,original_vertices,data,"before");
        unsigned advanced{},advanced_indices=index_address,written_pixels{};std::string error;
        const unsigned primitive=(data>>16)&7u;
        const bool triangles=primitive>=3u && primitive<=5u && !(commands[0xD3]&1u);
        PerformanceTelemetry::DrawScope draw_scope(telemetry_,data&0xFFFFu,triangles);
        bool rendered=false;
        if(draw_backend){
            advanced=vertex_address;
            rendered=draw_backend(r.memory(),*this,data,advanced,advanced_indices,error);
        }else if(triangles){
            // Coincident geometry never samples textures or targets.
            // Prove its zero coverage before validating fragment state.
            std::string geometry_error;
            rendered=primitive==3u && render_ge_triangles(r.memory(),commands,world,view,projection,vertex_address,data,advanced,geometry_error);
#ifdef CHILI_PARALLEL_SHADE_OBSERVATION
            if(rendered && parallel_observer_)parallel_observer_->note_caller_coincident();
#endif
            if(!rendered){
                ge::DeformState deformation;const ge::DeformState *deformation_state=nullptr;
                const unsigned vtype=commands[0x12]&0xFFFFFFu;
                if((vtype&0x1C0000u) || ((vtype&0x600u) && !(vtype&0x800000u))){
                    deformation.morph_weights=morph;
                    for(unsigned bone=0;bone<8;++bone)
                        std::copy_n(bones.begin()+bone*12,12,deformation.bones[bone].begin());
                    deformation_state=&deformation;
                }
#ifdef CHILI_PARALLEL_SHADE_OBSERVATION
                if(parallel_observer_)
                    rendered=render_ge_triangle_raster_observed(r.memory(),commands,world,view,projection,vertex_address,data,advanced,error,clut,&written_pixels,index_address,&advanced_indices,deformation_state,parallel_observer_);
                else
#endif
                rendered=render_ge_triangle_raster(r.memory(),commands,world,view,projection,vertex_address,data,advanced,error,clut,&written_pixels,index_address,&advanced_indices,deformation_state);
            }
        }else rendered=render_ge_sprites(r.memory(),commands,vertex_address,data,advanced,error,&clut,telemetry_);
        draw_scope.finish(rendered,written_pixels);
        if(rendered){
            if(capture)dump_draw(r,original_vertices,data,"after");
            vertex_address=advanced;index_address=advanced_indices;commands[4]=word;
            if(log_){
                std::cout<<(triangles?"[ge] triangle batch processed vertices=":(commands[0xD3]&1)?"[ge] clear sprites rendered vertices=":"[ge] sprites rendered vertices=")<<(data&0xFFFFu);
                if(triangles)std::cout<<" written pixels="<<written_pixels;
                std::cout<<'\n';
            }
        }else{
            std::cout<<"[ge] native renderer: "<<error<<'\n';
            if(dump_failure_)dump_draw(r,original_vertices,data,"failure");
        }
        if(trace_list_){
            auto info=describe_ge_draw(commands,word,original_vertices,original_indices,clut_address);
            info.list=enqueued_lists;info.index=draw_index;info.pc=address;
            info.rendered=rendered;info.written_pixels=written_pixels;info.error=std::move(error);
            trace_.draw(info);
        }
        return rendered;
    }
    // Runs one display list to END, exactly as sceGeListEnQueue does. False means
    // the stream needed something unimplemented and Runtime::stop() holds the reason.
    // Guest state is whatever the caller put in memory and in this object.
    bool execute_list(psprecomp::Runtime &r,unsigned start,ListRun &run){
        trace_list_=(trace_all_ || trace_selected_) && trace_.available();
        if(trace_list_)trace_.list_begin(enqueued_lists,start);
        bool ok;
        {CostTimer cost(frame_costs.ge_ns);ok=run_list(r,start,run);}
        ++frame_costs.lists;frame_costs.draws+=run.draws;
        if(trace_list_){
            trace_.list_end(enqueued_lists,run.completed,run.id,run.commands,run.draws,
                ok?(run.truncated?"draw limit":""):std::string_view(r.stop_reason()));
            trace_list_=false;
        }
        return ok;
    }
    explicit GeRuntime(psprecomp::Runtime &rt,ThreadRuntime *threads=nullptr,unsigned stack=0,const unsigned *sdk=nullptr,PerformanceTelemetry *telemetry=nullptr):telemetry_(telemetry),scheduler(threads),interrupt_stack(stack),sdk_version(sdk){
        configure_from_environment();
        instances[&rt]=this;rt.register_function(interrupt_return,&returned,"chili_ge_interrupt_return");
        rt.register_hle("sceGe_user",0xA4FC06A4u,[this](auto &r,auto &c){
            unsigned id=0;while(id<callbacks.size() && callbacks[id])++id;
            if(id==callbacks.size()){c.set_gpr(2,0x80000022u);return;}
            const auto address=c.gpr[4];if(!address || !r.memory().contains(address,16)){c.set_gpr(2,0x800200D3u);return;}
            callbacks[id]=Callback{r.memory().load32(address),r.memory().load32(address+4),r.memory().load32(address+8),r.memory().load32(address+12),c.gpr[28]};
            std::cout<<"[ge] callback="<<id<<" signal="<<callbacks[id]->signal<<" finish="<<callbacks[id]->finish<<'\n';c.set_gpr(2,id);
        });
        rt.register_hle("sceGe_user",0x05DB22CEu,[this](auto &,auto &c){
            const unsigned id=c.gpr[4];
            if(id>=callbacks.size()){c.set_gpr(2,0x80000100u);return;}
            // Remove both registered handlers. An already entered finish IRQ
            // retains its copied continuation until the existing return path.
            callbacks[id].reset();c.set_gpr(2,0u);
        });
        rt.register_hle("sceGe_user",0xAB49E76Au,[this](auto &r,auto &c){
            if(log_)std::cout<<"[ge] enqueue list="<<std::hex<<c.gpr[4]<<" stall="<<c.gpr[5]<<" callback="<<c.gpr[6]<<" options="<<c.gpr[7]<<std::dec<<'\n';
            if((c.gpr[4]|c.gpr[5])&3u){c.set_gpr(2,0x80000103u);return;}
            if(c.gpr[5] || c.gpr[7]){
                r.stop("GE stalled lists and option contexts require implementation");return;
            }
            const unsigned callback_id=c.gpr[6];
            if(callback_id!=0xFFFFFFFFu && (callback_id>=callbacks.size() || !callbacks[callback_id])){
                c.set_gpr(2,0x80000100u);return;
            }
            if(interrupt_resume){r.stop("Nested GE submission inside interrupt requires deferred queue support");return;}
            if(early_.valid && early_.address==c.gpr[4]){
                // Run at the frame boundary already (run_early): the id and the finish callback are left.
                early_.valid=false;++early_hits_;
                const unsigned id=next_list++;
                if(worker_on_)id_tickets_[id&63u]=early_.ticket;else completed.insert(id);
                c.set_gpr(2,id);
                deliver_finish(r,c,callback_id,early_.finish,early_.end);
                return;
            }
            if(worker_on_){
                // The list goes to the worker; the caller gets the id and its finish callback now.
                if(!worker_healthy(r))return;
                unsigned end{},finish{};
                if(!scout_list(r,c.gpr[4],end,finish))return;
                ++enqueued_lists;
                const unsigned id=next_list++;
                id_tickets_[id&63u]=push_job({Job::Kind::List,c.gpr[4],list_plan?list_plan():0u,{}});
                c.set_gpr(2,id);
                deliver_finish(r,c,callback_id,finish,end);
                return;
            }
            // Execute state streams; never acknowledge a draw or transfer without its effects.
            const unsigned ordinal=++enqueued_lists;
            const bool capturing=capture_.wants(ordinal) && begin_capture(r,c,callback_id);
            trace_selected_=capturing || (trace_lists_active_ && trace_lists_.contains(ordinal));
            ListRun run;
            const bool ok=execute_list(r,c.gpr[4],run);
            if(capturing)
                capture_.end({run.completed,run.id,run.commands,run.draws,run.end,ok?std::string():r.stop_reason()},snapshot(),r.memory());
            if(list_index_on_)
                list_index_<<ge_json::Object().num("list",ordinal).hex("addr",c.gpr[4]).num("completed",run.completed)
                    .num("commands",run.commands).num("draws",run.draws).hex("fb",0x04000000u|(commands[0x9C]&0x1FFFF0u)).finish()<<'\n';
            if(!ok || !run.completed)return;
            if(list_end){
                const std::uint64_t plan=list_plan?list_plan():0u;
                CostTimer cost(frame_costs.submit_ns);list_end(0x04000000u|(commands[0x9C]&0x1FFFF0u),run.draws,plan);
            }
            c.set_gpr(2,run.id);
            deliver_finish(r,c,callback_id,commands[0x0F]&0xFFFFu,run.end);
        });
        rt.register_hle("sceGe_user",0x03444EB4u,[this](auto &r,auto &c){
            if(log_)std::cout<<"[ge] list sync id="<<c.gpr[4]<<" mode="<<c.gpr[5]<<'\n';
            if(c.gpr[5]>1){c.set_gpr(2,0x80000107u);return;}
            if(worker_on_){
                // Mode 0 waits for the list; mode 1 asks: 0 done, 2 still being drawn.
                const unsigned id=c.gpr[4];
                if(id-1u>=next_list-1u){c.set_gpr(2,0x80000100u);return;}
                // Ids older than the table are long done.
                const std::uint64_t ticket=next_list-id<=id_tickets_.size()?id_tickets_[id&63u]:0u;
                if(c.gpr[5]==0u)wait_for_worker([this,ticket]{return jobs_finished_>=ticket;});
                if(!worker_healthy(r))return;
                bool done;
                {std::lock_guard<std::mutex> lock(worker_mutex_);done=jobs_finished_>=ticket;}
                c.set_gpr(2,done?0u:2u);
                return;
            }
            c.set_gpr(2,completed.contains(c.gpr[4])?0u:0x80000100u);
        });
        rt.register_hle("sceGe_user",0xB287BD61u,[this](auto &r,auto &c){
            if(log_)std::cout<<"[ge] draw sync mode="<<c.gpr[4]<<'\n';
            if(c.gpr[4]>1){c.set_gpr(2,0x80000107u);return;}
            if(worker_on_){
                if(c.gpr[4]==0u)wait_for_worker([this]{return jobs_finished_>=jobs_pushed_;});
                if(!worker_healthy(r))return;
                bool idle;
                {std::lock_guard<std::mutex> lock(worker_mutex_);idle=jobs_finished_>=jobs_pushed_;}
                c.set_gpr(2,idle?0u:2u);
                return;
            }
            // Enqueue is synchronous and unsupported effects stop before it returns.
            c.set_gpr(2,0u);
        });
        rt.register_hle("sceGe_user",0xE47E40E4u,[](auto &,auto &c){c.set_gpr(2,psprecomp::GuestMemory::kVramPhysicalBase);});
    }
private:
    // All configuration is read here, once; the list and draw paths only test the
    // cached values.
    void configure_from_environment(){
        log_=log_enabled(LogChannel::Ge);
        if(const char *prefix=std::getenv("PSPRECOMP_GE_DUMP"))dump_prefix_=prefix;
        if(const char *number=std::getenv("PSPRECOMP_GE_DUMP_DRAW")){
            char *end{};const auto n=std::strtoul(number,&end,10);
            if(end!=number && !*end && n>0 && n<=0xFFFFFFFFul)dump_selected_=static_cast<unsigned>(n);
        }
        if(const char *failure=std::getenv("PSPRECOMP_GE_DUMP_FAILURE"))dump_failure_=*failure!=0;
        // CHILI_GE_NULL_DRAW=1 consumes every PRIM without drawing. The game then
        // runs on game code and firmware services alone, which is what a replay
        // needs when it measures or checks only those.
        if(const char *null_draw=std::getenv("CHILI_GE_NULL_DRAW"))
            if(*null_draw=='1')draw_backend=[](psprecomp::GuestMemory &,const GeRuntime &,unsigned,unsigned &,unsigned &,std::string &){return true;};
        if(const char *path=std::getenv("CHILI_GE_TRACE"))
            if(*path && !trace_.open(path))std::cerr<<"[ge-trace] cannot open "<<path<<'\n';
        if(const char *lists=std::getenv("CHILI_GE_TRACE_LISTS")){
            std::string error;
            if(*lists && !GeListSelection::parse(lists,trace_lists_,error))std::cerr<<"[ge-trace] CHILI_GE_TRACE_LISTS: "<<error<<'\n';
            trace_lists_active_=!trace_lists_.empty();
        }
        if(const char *path=std::getenv("CHILI_GE_LIST_INDEX")){
            if(*path)list_index_.open(path,std::ios::out|std::ios::trunc);
            list_index_on_=list_index_.is_open();
            if(*path && !list_index_on_)std::cerr<<"[ge-trace] cannot open "<<path<<'\n';
        }
    }
    bool begin_capture(psprecomp::Runtime &r,const psprecomp::AllegrexContext &c,unsigned callback_id){
        GeFrameCapture::Request request;
        request.ordinal=enqueued_lists;request.list=c.gpr[4];request.stall=c.gpr[5];request.callback=callback_id;request.options=c.gpr[7];
        request.sdk_version=sdk_version?*sdk_version:0u;request.host_mxcsr=_mm_getcsr();
        if(callback_id!=0xFFFFFFFFu){request.callback_finish=callbacks[callback_id]->finish;request.callback_signal=callbacks[callback_id]->signal;}
        return capture_.begin(request,snapshot(),r.memory());
    }
    void capture_stop(psprecomp::Runtime &r,unsigned address){
        GeFrameCapture::Request request;
        request.ordinal=enqueued_lists;request.list=address;request.callback=0xFFFFFFFFu;
        request.sdk_version=sdk_version?*sdk_version:0u;request.host_mxcsr=_mm_getcsr();
        capture_.stop(request,snapshot(),r.memory());
    }
    bool run_list(psprecomp::Runtime &r,unsigned start,ListRun &run){
        PerformanceTelemetry::GeListScope list_scope(telemetry_);
        struct ReturnFrame {unsigned pc,offset;};
        std::array<ReturnFrame,32> stack{};unsigned depth=0;
        unsigned previous=0,cursor=start;
        // List words are read through one resolved pointer per stretch of guest memory, not through a
        // bounds-checked load each: the window is [window_base, window_base+window_bytes) and is
        // resolved again only when a jump, a call or a long list leaves it.
        const std::uint8_t *window=nullptr;unsigned window_base=0,window_bytes=0;
        for(unsigned i=0;i<1000000;++i){const auto address=cursor;cursor+=4;
            if(address-window_base>=window_bytes){
                window=nullptr;window_bytes=0;
                for(const unsigned bytes:{1u<<20,1u<<14,1u<<8,4u}){
                    window=r.memory().raw_pointer(address,bytes);
                    if(window){window_base=address;window_bytes=bytes;break;}
                }
                if(!window)return stop_list(r,"GE list reached invalid memory");
            }
            std::uint32_t word;std::memcpy(&word,window+(address-window_base),sizeof(word));
            const unsigned command=word>>24,data=word&0xFFFFFFu;
            list_scope.word_read();
            if(command==0x0C){
                if(previous!=0x0F)return stop_list(r,"GE END without FINISH requires control-flow handling");
                // On the worker and ahead of its enqueue a list has no id yet: sceGeListEnQueue issues it.
                const unsigned id=list_takes_id_?next_list++:0u;
                if(list_takes_id_)completed.insert(id);
                list_scope.completed();
                if(log_)std::cout<<"[ge] state list completed id="<<id<<" commands="<<i+1<<'\n';
                run.completed=true;run.id=id;run.commands=i+1;run.end=cursor;
                return true;
            }
            if(command==8){
                cursor=((((commands[0x10]&0xFF0000u)<<8)|data)+offset_address)&0x0FFFFFFCu;
                previous=8;commands[8]=word;continue;
            }
            if(command==0x0A){
                if(depth==stack.size())return stop_list(r,"GE CALL stack overflow");
                stack[depth++]={cursor,offset_address};
                cursor=((((commands[0x10]&0xFF0000u)<<8)|data)+offset_address)&0x0FFFFFFCu;
                commands[command]=word;previous=command;continue;
            }
            if(command==0x0B){
                if(!depth)return stop_list(r,"GE RET with empty stack");
                const auto frame=stack[--depth];cursor=frame.pc;offset_address=frame.offset;
                commands[command]=word;previous=command;continue;
            }
            if(command==0xC4){
                std::string error;
                if(clut.load(r.memory(),commands[0xB0]&0xFFFFFFu,commands[0xB1]&0xFFFFFFu,data,error)){
                    if(data&63u)clut_address=(commands[0xB0]&0x00FFFFF0u)|((commands[0xB1]<<8)&0x0F000000u);
                    commands[command]=word;previous=command;
                    if(log_)std::cout<<"[ge] CLUT loaded blocks="<<(data&63u)<<'\n';
                    continue;
                }
                std::cout<<"[ge] CLUT loader: "<<error<<'\n';
            }
            if(command==4){
                if(run.draws==draw_limit){run.truncated=true;run.commands=i;run.end=address;return true;}
                if(draw_primitive(r,address,word,run.draws)){previous=4;++run.draws;continue;}
            }
            if((command>=4 && command<=0x0B) || command==0x0E ||
               command==0xEA || (command==0xC4 && data)){
                std::cout<<"[ge] unimplemented effect pc="<<std::hex<<address<<" word="<<word<<std::dec<<'\n';
                std::cout<<"[ge] vertices="<<std::hex<<vertex_address<<" indices="<<index_address<<'\n';
                for(unsigned reg=0;reg<256;++reg)if(commands[reg]&0xFFFFFFu)
                    std::cout<<"[ge-state] "<<reg<<" "<<commands[reg]<<'\n';
                if(r.memory().contains(vertex_address,64))for(unsigned j=0;j<16;++j)
                    std::cout<<"[ge-vertex] "<<vertex_address+j*4<<" "<<r.memory().load32(vertex_address+j*4)<<'\n';
                std::cout<<std::dec;
                run.commands=i;run.end=address;
                if(capture_.enabled())capture_stop(r,address);
                return stop_list(r,"GE drawing, control flow, or transfer requires implementation");
            }
            if(commands[command]!=word){
                const auto kind=kRevisionClass[command];
                if(kind&kAffectsDraw)++draw_revision;
                if(kind&kAffectsLighting)++lighting_revision;
            }
            commands[command]=word;matrix(command,data);
            if(command==0x13)offset_address=word<<8;
            if(command==0x14)offset_address=address;
            if(command==1 || command==2){
                const unsigned relative=(((commands[0x10]&0xFF0000u)<<8)|data)+offset_address;
                if(command==1)vertex_address=relative&0x0FFFFFFFu;
                else index_address=relative&0x0FFFFFFFu;
            }
            if(command==0xCB || command==0xCC)r.memory().memory_barrier();
            previous=command;
        }
        return stop_list(r,"GE list exceeded command safety limit");
    }
    // A list that cannot go on. On the worker the reason waits for the game
    // thread, which stops the runtime at its next GE call.
    bool stop_list(psprecomp::Runtime &r,const char *reason){
        if(on_worker_thread_){std::lock_guard<std::mutex> lock(worker_mutex_);if(worker_error_.empty())worker_error_=reason;}
        else r.stop(reason);
        return false;
    }
    struct Job {
        enum class Kind {List,Call,Stop} kind{};
        unsigned start{};
        std::uint64_t plan{};
        std::function<void()> call;
    };
    // Returns the job's ticket: jobs_finished_ reaches it when the job is done.
    std::uint64_t push_job(Job job){
        std::uint64_t ticket;
        {std::lock_guard<std::mutex> lock(worker_mutex_);jobs_.push_back(std::move(job));ticket=++jobs_pushed_;}
        worker_wake_.notify_one();
        return ticket;
    }
    template<class Done> void wait_for_worker(Done done){
        CostTimer cost(frame_costs.ge_wait_ns);
        std::unique_lock<std::mutex> lock(worker_mutex_);
        while(!done()){
            if(worker_done_.wait_for(lock,std::chrono::milliseconds(2),done))break;
            // The worker may be inside a call that needs the window's thread, which is this one.
            lock.unlock();
            if(wait_pump)wait_pump();
            lock.lock();
        }
    }
    // The worker's error, if any, stops the runtime. False when it did.
    bool worker_healthy(psprecomp::Runtime &r){
        std::string error;
        {std::lock_guard<std::mutex> lock(worker_mutex_);error=worker_error_;}
        if(error.empty())return true;
        r.stop(error);
        return false;
    }
    void worker_main(psprecomp::Runtime &r,const std::function<void()> &setup){
        on_worker_thread_=true;
        if(setup)setup();
        for(;;){
            Job job;
            {
                std::unique_lock<std::mutex> lock(worker_mutex_);
                worker_wake_.wait(lock,[this]{return !jobs_.empty();});
                job=std::move(jobs_.front());jobs_.pop_front();
            }
            try{
                if(job.kind==Job::Kind::List){
                    ListRun run;
                    const bool ok=execute_list(r,job.start,run);
                    if(ok && run.completed){
                        if(list_end){CostTimer cost(frame_costs.submit_ns);list_end(0x04000000u|(commands[0x9C]&0x1FFFF0u),run.draws,job.plan);}
                    }else stop_list(r,"GE list did not complete");
                }else if(job.kind==Job::Kind::Call && job.call)job.call();
            }catch(const std::exception &failure){
                std::lock_guard<std::mutex> lock(worker_mutex_);
                if(worker_error_.empty())worker_error_=std::string("GE list thread: ")+failure.what();
            }
            {
                std::lock_guard<std::mutex> lock(worker_mutex_);
                ++jobs_finished_;
            }
            worker_done_.notify_all();
            if(job.kind==Job::Kind::Stop)break;
        }
    }
    // The game thread's look at a list it hands to the worker: where the list
    // ends and the argument of its FINISH, which the finish callback is called
    // with. Follows JUMP, CALL and RET the way run_list does, with its own copy
    // of BASE and the offset register, which carry over from list to list.
    // With `stop` a list that does not end stops the runtime, as running it would.
    bool scout_list(psprecomp::Runtime &r,unsigned start,unsigned &end,unsigned &finish,bool stop=true){
        const auto fail=[&](const char *reason){if(stop)r.stop(reason);return false;};
        struct ReturnFrame {unsigned pc,offset;};
        std::array<ReturnFrame,32> stack{};unsigned depth=0;
        unsigned previous=0,cursor=start,base=scout_base_,offset=scout_offset_;
        finish=scout_finish_;
        const std::uint8_t *window=nullptr;unsigned window_base=0,window_bytes=0;
        for(unsigned i=0;i<1000000;++i){const auto address=cursor;cursor+=4;
            if(address-window_base>=window_bytes){
                window=nullptr;window_bytes=0;
                for(const unsigned bytes:{1u<<20,1u<<14,1u<<8,4u}){
                    window=r.memory().raw_pointer(address,bytes);
                    if(window){window_base=address;window_bytes=bytes;break;}
                }
                if(!window)return fail("GE list reached invalid memory");
            }
            std::uint32_t word;std::memcpy(&word,window+(address-window_base),sizeof(word));
            const unsigned command=word>>24,data=word&0xFFFFFFu;
            switch(command){
            case 0x0C:
                if(previous!=0x0F)return fail("GE END without FINISH requires control-flow handling");
                end=cursor;scout_base_=base;scout_offset_=offset;scout_finish_=finish;
                return true;
            case 0x08:cursor=((((base&0xFF0000u)<<8)|data)+offset)&0x0FFFFFFCu;break;
            case 0x0A:
                if(depth==stack.size())return fail("GE CALL stack overflow");
                stack[depth++]={cursor,offset};
                cursor=((((base&0xFF0000u)<<8)|data)+offset)&0x0FFFFFFCu;break;
            case 0x0B:
                if(!depth)return fail("GE RET with empty stack");
                {const auto frame=stack[--depth];cursor=frame.pc;offset=frame.offset;}
                break;
            case 0x0F:finish=data&0xFFFFu;break;
            case 0x10:base=word;break;
            case 0x13:offset=word<<8;break;
            case 0x14:offset=address;break;
            default:break;
            }
            previous=command;
        }
        return fail("GE list exceeded command safety limit");
    }
    // The finish callback of a list, entered as an interrupt of the calling thread.
    void deliver_finish(psprecomp::Runtime &r,psprecomp::AllegrexContext &c,unsigned callback_id,unsigned token,unsigned end){
        if(callback_id==0xFFFFFFFFu || !callbacks[callback_id]->finish)return;
        const auto cb=*callbacks[callback_id];
        if(!scheduler || !interrupt_stack || !r.memory().contains(interrupt_stack,16384) || !scheduler->enter_interrupt()){
            r.stop("GE finish interrupt requires valid stack and enabled interrupt context");return;
        }
        c.pc=c.gpr[31];interrupt_resume=c;
        c.gpr[4]=token;c.gpr[5]=cb.finish_arg;
        c.gpr[6]=sdk_version && *sdk_version<=0x02000010u?0u:end;
        c.gpr[28]=cb.gp;c.gpr[29]=interrupt_stack+16384-64;
        c.gpr[31]=interrupt_return;c.pc=cb.finish;
        if(log_)std::cout<<"[ge] finish interrupt token="<<c.gpr[4]<<'\n';
    }
    std::thread worker_;
    std::mutex worker_mutex_;
    std::condition_variable worker_wake_,worker_done_;
    // Under worker_mutex_.
    std::deque<Job> jobs_;
    std::uint64_t jobs_pushed_{},jobs_finished_{};
    std::string worker_error_;
    // Game thread: the worker is on; BASE, the offset register and the last FINISH argument as they
    // will be when the next list starts.
    bool worker_on_{};
    unsigned scout_base_{},scout_offset_{},scout_finish_{};
    // The ticket of each of the last 64 list ids, for sceGeListSync (game thread).
    std::array<std::uint64_t,64> id_tickets_{};
    // A list takes its id when it ends: the synchronous sceGeListEnQueue. Off while the worker is
    // on and while a list runs ahead of its enqueue.
    bool list_takes_id_{true};
    // The list run_early ran or queued, until the game enqueues it.
    struct Early {bool valid{};unsigned address{},end{},finish{};std::uint64_t ticket{};} early_;
    std::uint64_t early_hits_{};
    inline static thread_local bool on_worker_thread_{};
    GeFrameCapture capture_;
    GeDrawTrace trace_;
    GeListSelection trace_lists_;
    std::ofstream list_index_;
    std::string dump_prefix_;
    unsigned dump_selected_{1};
    bool dump_failure_{},log_{},trace_all_{},trace_lists_active_{},trace_selected_{},trace_list_{},list_index_on_{};
};
}
