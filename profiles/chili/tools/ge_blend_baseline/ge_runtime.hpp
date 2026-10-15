#pragma once
#include "psprecomp/runtime.hpp"
#include "ge_sprites.hpp"
#include "ge_triangle_raster.hpp"
#include "ge_triangles.hpp"
#include "thread_runtime.hpp"
#include <unordered_map>
#include <array>
#include <optional>
#include <iostream>
#include <bit>
#include <unordered_set>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
namespace chili {
class GeRuntime {
public:
    struct Callback {unsigned signal,signal_arg,finish,finish_arg,gp;};
    std::array<std::optional<Callback>,16> callbacks;
    std::array<unsigned,256> commands{};
    std::array<float,96> bones{};
    std::array<float,12> world{},view{},texture{};
    std::array<float,16> projection{};
    std::array<float,8> morph{};
    unsigned bone_cursor{},world_cursor{},view_cursor{},texture_cursor{},projection_cursor{};
    std::unordered_set<unsigned> completed;
    unsigned next_list{1},offset_address{},vertex_address{},index_address{};
    unsigned sprite_copy_count{};
    GeClutCache clut;
    bool snapshot_copy(unsigned data){
        if((data&0xFFFFu)!=30u)return false;
        ++sprite_copy_count;
        const char *prefix=std::getenv("PSPRECOMP_GE_DUMP");
        if(!prefix || !*prefix)return false;
        unsigned selected=1;
        if(const char *number=std::getenv("PSPRECOMP_GE_DUMP_DRAW")){
            char *end{};const auto n=std::strtoul(number,&end,10);
            if(end!=number && !*end && n>0 && n<=0xFFFFFFFFul)selected=static_cast<unsigned>(n);
        }
        return sprite_copy_count==selected;
    }
    void dump_draw(psprecomp::Runtime &rt,unsigned vertices,unsigned data,const char *phase){
        const char *prefix=std::getenv("PSPRECOMP_GE_DUMP");
        if(!prefix || !*prefix)return;
        try{
            const std::filesystem::path root(std::string(prefix)+"_"+std::to_string(sprite_copy_count)+"_"+phase);
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
    ThreadRuntime *scheduler{};
    unsigned interrupt_stack{};const unsigned *sdk_version{};
    std::optional<psprecomp::AllegrexContext> interrupt_resume;
    inline static std::unordered_map<psprecomp::Runtime*,GeRuntime*> instances;
    static constexpr unsigned interrupt_return=0x08000014;
    ~GeRuntime(){for(auto it=instances.begin();it!=instances.end();)if(it->second==this)it=instances.erase(it);else ++it;}
    static void returned(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c){
        auto &self=*instances.at(&rt);
        if(!self.interrupt_resume){rt.stop("GE interrupt return without continuation");return;}
        c=*self.interrupt_resume;self.interrupt_resume.reset();
        std::cout<<"[ge] finish interrupt returned continuation="<<std::hex<<c.pc<<std::dec<<'\n';
        self.scheduler->leave_interrupt(c);
    }
    explicit GeRuntime(psprecomp::Runtime &rt,ThreadRuntime *threads=nullptr,unsigned stack=0,const unsigned *sdk=nullptr):scheduler(threads),interrupt_stack(stack),sdk_version(sdk){
        instances[&rt]=this;rt.register_function(interrupt_return,&returned,"chili_ge_interrupt_return");
        rt.register_hle("sceGe_user",0xA4FC06A4u,[this](auto &r,auto &c){
            unsigned id=0;while(id<callbacks.size() && callbacks[id])++id;
            if(id==callbacks.size()){c.set_gpr(2,0x80000022u);return;}
            const auto address=c.gpr[4];if(!address || !r.memory().contains(address,16)){c.set_gpr(2,0x800200D3u);return;}
            callbacks[id]=Callback{r.memory().load32(address),r.memory().load32(address+4),r.memory().load32(address+8),r.memory().load32(address+12),c.gpr[28]};
            std::cout<<"[ge] callback="<<id<<" signal="<<callbacks[id]->signal<<" finish="<<callbacks[id]->finish<<'\n';c.set_gpr(2,id);
        });
        rt.register_hle("sceGe_user",0xAB49E76Au,[this](auto &r,auto &c){
            std::cout<<"[ge] enqueue list="<<std::hex<<c.gpr[4]<<" stall="<<c.gpr[5]<<" callback="<<c.gpr[6]<<" options="<<c.gpr[7]<<std::dec<<'\n';
            if((c.gpr[4]|c.gpr[5])&3u){c.set_gpr(2,0x80000103u);return;}
            if(c.gpr[5] || c.gpr[7]){
                r.stop("GE stalled lists and option contexts require implementation");return;
            }
            const unsigned callback_id=c.gpr[6];
            if(callback_id!=0xFFFFFFFFu && (callback_id>=callbacks.size() || !callbacks[callback_id])){
                c.set_gpr(2,0x80000100u);return;
            }
            if(interrupt_resume){r.stop("Nested GE submission inside interrupt requires deferred queue support");return;}
            // Execute state streams; never acknowledge a draw or transfer without its effects.
            struct ReturnFrame {unsigned pc,offset;};
            std::array<ReturnFrame,32> stack{};unsigned depth=0;
            unsigned previous=0,cursor=c.gpr[4];
            for(unsigned i=0;i<1000000;++i){const auto address=cursor;cursor+=4;
                if(!r.memory().contains(address,4)){r.stop("GE list reached invalid memory");return;}
                const auto word=r.memory().load32(address),command=word>>24,data=word&0xFFFFFFu;
                if(command==0x0C){
                    if(previous!=0x0F){r.stop("GE END without FINISH requires control-flow handling");return;}
                    const unsigned id=next_list++;completed.insert(id);
                    std::cout<<"[ge] state list completed id="<<id<<" commands="<<i+1<<'\n';
                    c.set_gpr(2,id);
                    if(callback_id!=0xFFFFFFFFu && callbacks[callback_id]->finish){
                        const auto cb=*callbacks[callback_id];
                        if(!scheduler || !interrupt_stack || !r.memory().contains(interrupt_stack,16384) || !scheduler->enter_interrupt()){
                            r.stop("GE finish interrupt requires valid stack and enabled interrupt context");return;
                        }
                        c.pc=c.gpr[31];interrupt_resume=c;
                        c.gpr[4]=commands[0x0F]&0xFFFFu;c.gpr[5]=cb.finish_arg;
                        c.gpr[6]=sdk_version && *sdk_version<=0x02000010u?0u:cursor;
                        c.gpr[28]=cb.gp;c.gpr[29]=interrupt_stack+16384-64;
                        c.gpr[31]=interrupt_return;c.pc=cb.finish;
                        std::cout<<"[ge] finish interrupt token="<<c.gpr[4]<<'\n';
                    }
                    return;
                }
                if(command==8){
                    cursor=((((commands[0x10]&0xFF0000u)<<8)|data)+offset_address)&0x0FFFFFFCu;
                    previous=8;commands[8]=word;continue;
                }
                if(command==0x0A){
                    if(depth==stack.size()){r.stop("GE CALL stack overflow");return;}
                    stack[depth++]={cursor,offset_address};
                    cursor=((((commands[0x10]&0xFF0000u)<<8)|data)+offset_address)&0x0FFFFFFCu;
                    commands[command]=word;previous=command;continue;
                }
                if(command==0x0B){
                    if(!depth){r.stop("GE RET with empty stack");return;}
                    const auto frame=stack[--depth];cursor=frame.pc;offset_address=frame.offset;
                    commands[command]=word;previous=command;continue;
                }
                if(command==0xC4){
                    std::string error;
                    if(clut.load(r.memory(),commands[0xB0]&0xFFFFFFu,commands[0xB1]&0xFFFFFFu,data,error)){
                        commands[command]=word;previous=command;
                        std::cout<<"[ge] CLUT loaded blocks="<<(data&63u)<<'\n';continue;
                    }
                    std::cout<<"[ge] CLUT loader: "<<error<<'\n';
                }
                if(command==4){
                    const unsigned original_vertices=vertex_address;
                    const bool capture=snapshot_copy(data);
                    if(capture)dump_draw(r,original_vertices,data,"before");
                    unsigned advanced{},advanced_indices=index_address,written_pixels{};std::string error;
                    const unsigned primitive=(data>>16)&7u;
                    const bool triangles=primitive>=3u && primitive<=5u && !(commands[0xD3]&1u);
                    bool rendered=false;
                    if(triangles){
                        // Coincident geometry never samples textures or targets.
                        // Prove its zero coverage before validating fragment state.
                        std::string geometry_error;
                        rendered=primitive==3u && render_ge_triangles(r.memory(),commands,world,view,projection,vertex_address,data,advanced,geometry_error);
                        if(!rendered){
                            ge::DeformState deformation;const ge::DeformState *deformation_state=nullptr;
                            const unsigned vtype=commands[0x12]&0xFFFFFFu;
                            if((vtype&0x1C0000u) || ((vtype&0x600u) && !(vtype&0x800000u))){
                                deformation.morph_weights=morph;
                                for(unsigned bone=0;bone<8;++bone)
                                    std::copy_n(bones.begin()+bone*12,12,deformation.bones[bone].begin());
                                deformation_state=&deformation;
                            }
                            rendered=render_ge_triangle_raster(r.memory(),commands,world,view,projection,vertex_address,data,advanced,error,clut,&written_pixels,index_address,&advanced_indices,deformation_state);
                        }
                    }else rendered=render_ge_sprites(r.memory(),commands,vertex_address,data,advanced,error,&clut);
                    if(rendered){
                        if(capture)dump_draw(r,original_vertices,data,"after");
                        vertex_address=advanced;index_address=advanced_indices;commands[4]=word;previous=4;
                        std::cout<<(triangles?"[ge] triangle batch processed vertices=":(commands[0xD3]&1)?"[ge] clear sprites rendered vertices=":"[ge] sprites rendered vertices=")<<(data&0xFFFFu);
                        if(triangles)std::cout<<" written pixels="<<written_pixels;
                        std::cout<<'\n';continue;
                    }
                    std::cout<<"[ge] native renderer: "<<error<<'\n';
                    if(const char *failure=std::getenv("PSPRECOMP_GE_DUMP_FAILURE"))
                        if(*failure)dump_draw(r,original_vertices,data,"failure");
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
                    r.stop("GE drawing, control flow, or transfer requires implementation");return;
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
            r.stop("GE list exceeded command safety limit");
        });
        rt.register_hle("sceGe_user",0x03444EB4u,[this](auto &,auto &c){
            if(c.gpr[5]>1){c.set_gpr(2,0x80000107u);return;}
            c.set_gpr(2,completed.contains(c.gpr[4])?0u:0x80000100u);
        });
        rt.register_hle("sceGe_user",0xB287BD61u,[](auto &,auto &c){
            // Enqueue is synchronous and unsupported effects stop before it returns.
            c.set_gpr(2,c.gpr[4]>1?0x80000107u:0u);
        });
        rt.register_hle("sceGe_user",0xE47E40E4u,[](auto &,auto &c){c.set_gpr(2,psprecomp::GuestMemory::kVramPhysicalBase);});
    }
};
}
