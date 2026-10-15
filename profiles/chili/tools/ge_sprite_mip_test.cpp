#include "ge_runtime.hpp"
#include "ge_sprites_reference.hpp"
#include <bit>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using Regs=std::array<unsigned,256>;
constexpr unsigned vertex=0x08808000,palette_address=0x08818000;
constexpr std::array<unsigned,4> levels{0xFF010203,0xFF102030,0xFF506070,0xFF90A0B0};
unsigned checks{};
void require(bool value,const std::string &why){++checks;if(!value)throw std::runtime_error(why);}
std::vector<std::uint8_t> read(const std::filesystem::path &path){std::ifstream f(path,std::ios::binary);
    if(!f)throw std::runtime_error("fixture file missing: "+path.string());
    return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};}
std::vector<std::uint8_t> vram(const psprecomp::GuestMemory &m){std::vector<std::uint8_t> out(m.vram_size());m.copy_out(0x04000000,out);return out;}
std::vector<std::uint8_t> all(const psprecomp::GuestMemory &m){std::vector<std::uint8_t> out(m.size()+m.vram_size()+psprecomp::GuestMemory::kScratchpadSize);
    m.copy_out(0x08000000,std::span(out).first(m.size()));m.copy_out(0x04000000,std::span(out).subspan(m.size(),m.vram_size()));
    m.copy_out(psprecomp::GuestMemory::kScratchpadBase,std::span(out).subspan(m.size()+m.vram_size()));return out;}
std::vector<unsigned> cpu(const psprecomp::AllegrexContext &c){std::vector<unsigned> out(c.gpr.begin(),c.gpr.end());out.insert(out.end(),{c.pc,c.hi,c.lo,c.fcr31});
    for(float f:c.fpr)out.push_back(std::bit_cast<unsigned>(f));for(float f:c.vfpu)out.push_back(std::bit_cast<unsigned>(f));
    out.insert(out.end(),c.vfpu_ctrl.begin(),c.vfpu_ctrl.end());return out;}
void seed_cpu(psprecomp::AllegrexContext &c){for(unsigned n=1;n<32;++n)c.gpr[n]=0xABCDE000+n;
    for(unsigned n=0;n<32;++n)c.fpr[n]=std::bit_cast<float>(0x7FC00000u+n);
    for(unsigned n=0;n<128;++n)c.vfpu[n]=std::bit_cast<float>(0x80000000u+n);c.pc=0x08801000;c.gpr[31]=0x08801234;}
void write_vertex(psprecomp::GuestMemory &m,unsigned n,unsigned u,unsigned v,int x,int y,unsigned z=7,unsigned color=0xFFFFFFFF){
    const unsigned p=vertex+n*16;m.store16(p,u);m.store16(p+2,v);m.store32(p+4,color);m.store16(p+8,x);m.store16(p+10,y);m.store16(p+12,z);m.store16(p+14,0xBEEF);}
unsigned storage_offset(unsigned x,unsigned y,unsigned stride,unsigned format,bool swizzle){
    const unsigned pitch=format==3?stride*4:format==4?stride/2:format==5?stride:stride*2;
    const unsigned bx=format==3?x*4:format==4?x/2:format==5?x:x*2;
    return swizzle?(y/8)*pitch*8+(bx/16)*128+(y%8)*16+bx%16:y*pitch+bx;
}
void texel(psprecomp::GuestMemory &m,const Regs &r,unsigned level,unsigned x,unsigned y,unsigned value){
    const unsigned format=r[0xC3],stride=r[0xA8+level]&0x7FF,address=(r[0xA0+level]&0xFFFFF0)|((r[0xA8+level]<<8)&0xF000000);
    const unsigned p=address+storage_offset(x,y,stride,format,r[0xC2]&1);
    if(format==3)m.store32(p,value);else if(format<3)m.store16(p,value);else if(format==5)m.store8(p,value);else{const unsigned shift=(x&1)*4;
        m.store8(p,(m.load8(p)&~(15u<<shift))|((value&15)<<shift));}
}
struct Setup{Regs regs{};chili::GeClutCache palette;};
Setup setup(psprecomp::GuestMemory &m,unsigned format=3,bool swizzle=false){Setup out;auto &r=out.regs;
    std::vector<std::uint8_t> initial(m.vram_size());for(unsigned i=0;i<initial.size();++i)initial[i]=std::uint8_t(i*37+13);m.copy_in(0x04000000,initial);
    r[0x12]=0x80011E;r[0x16]=31|(31<<10);r[0x1E]=1;r[0xD2]=3;r[0xD5]=r[0x16];r[0x9D]=512;r[0xE7]=1;
    r[0xC2]=(3<<16)|unsigned(swizzle);r[0xC3]=format;r[0xC5]=0xFF03;r[0xC6]=0x107;r[0xC8]=0xE80000;r[0xC9]=0x103;
    r[0x55]=0xFFFFFF;r[0x58]=255;r[0xD7]=65535;
    for(unsigned i=0;i<256;++i)m.store32(palette_address+i*4,i<4?levels[i]:0xFF000000u|(i*0x010305u&0xFFFFFFu));
    std::string error;require(out.palette.load(m,palette_address&0xFFFFFF,((palette_address>>8)&0xF0000),32,error),error);
    for(unsigned l=0;l<4;++l){const unsigned width=64>>l,stride=std::max(width,format==4?32u:format==5?16u:4u),address=0x08820000+l*0x10000;
        r[0xA0+l]=address&0xFFFFFF;r[0xA8+l]=((address>>8)&0xF0000)|stride;r[0xB8+l]=(6-l)|((6-l)<<8);
        for(unsigned y=0;y<width;++y)for(unsigned x=0;x<width;++x)texel(m,r,l,x,y,format==3?levels[l]:l);
    }
    write_vertex(m,0,0,0,0,0,9);write_vertex(m,1,128,64,16,16,7);return out;
}
void paint(std::vector<std::uint8_t> &expected,unsigned x,unsigned y,unsigned rgb){const unsigned p=(y*512+x)*4;
    for(unsigned c=0;c<3;++c)expected[p+c]=std::uint8_t(rgb>>(c*8));}
void constant_cases(){for(unsigned format:{3u,4u,5u})for(bool swizzle:{false,true})for(unsigned filter=4;filter<=7;++filter){
    psprecomp::Runtime rt;auto s=setup(rt.memory(),format,swizzle);s.regs[0xC6]=0x100|filter;auto expected=vram(rt.memory());
    const unsigned color=filter&2?0xFF304050:levels[2];for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)paint(expected,x,y,color);
    unsigned cursor=0x12345678;std::string error;
    require(chili::render_ge_sprites(rt.memory(),s.regs,vertex,0x60002,cursor,error,&s.palette),error);
    require(cursor==vertex+32,"visible mip cursor");require(vram(rt.memory())==expected,"constant mip entire2MiB oracle");
    // UV/rectangle reversal keeps source endpoint color/Z provenance and LOD.
    rt.memory().copy_in(0x04000000,expected);write_vertex(rt.memory(),0,128,64,16,16,9);write_vertex(rt.memory(),1,0,0,0,0,7);
    require(chili::render_ge_sprites(rt.memory(),s.regs,vertex,0x60002,cursor,error,&s.palette),error);
    require(vram(rt.memory())==expected,"reversed mip rectangle mismatch");
}}
void selection_cases(){for(unsigned which=0;which<5;++which){psprecomp::Runtime rt;auto s=setup(rt.memory());
    unsigned color=0xFF304050;
    if(which==0){write_vertex(rt.memory(),1,1024,1024,16,16);color=levels[3];}
    if(which==1){write_vertex(rt.memory(),1,8,8,16,16);color=levels[0];}
    if(which==2){s.regs[0xC8]=0xE80002;s.regs[0xD0]=0x400000;color=0xFF081119;}
    if(which==3)s.regs[0xC8]=0x180001;
    if(which==4)s.regs[0xC8]=0x180003;
    auto expected=vram(rt.memory());for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)paint(expected,x,y,color);
    unsigned cursor{};std::string error;require(chili::render_ge_sprites(rt.memory(),s.regs,vertex,0x60002,cursor,error,&s.palette),error);
    require(vram(rt.memory())==expected,"AUTO/max/MAG/SLOPE/CONST wholeVRAM oracle");
}}
void material_color_case(){psprecomp::Runtime rt;auto s=setup(rt.memory());auto &m=rt.memory();
    s.regs[0x12]=0x800102;s.regs[0x55]=0x804020;s.regs[0x58]=0x80;s.regs[0xC9]=0;
    // Actual ten-byte no-color layout; material RGB/alpha supply the source.
    const unsigned u[]{0,128},v[]{0,64},xy[]{0,16};
    for(unsigned n=0;n<2;++n){const unsigned p=vertex+n*10;m.store16(p,u[n]);m.store16(p+2,v[n]);
        m.store16(p+4,xy[n]);m.store16(p+6,xy[n]);m.store16(p+8,n?7:9);}
    auto expected=vram(m);for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)paint(expected,x,y,0x8018100A);
    unsigned cursor{};std::string error;require(chili::render_ge_sprites(m,s.regs,vertex,0x60002,cursor,error),error);
    require(cursor==vertex+20 && vram(m)==expected,"no-color material/modulate/texture-alpha-off oracle");
}
void extra_formats(){for(unsigned format:{0u,1u,2u,8u,9u,10u})for(unsigned swizzle=0;swizzle<2;++swizzle){if(format>=8 && swizzle)continue;
    psprecomp::Runtime rt;auto s=setup(rt.memory(),3,false);s.regs[0xC3]=format;s.regs[0xC2]=(3<<16)|swizzle;
    for(unsigned level=0;level<4;++level){const unsigned width=64>>level,address=0x08820000+level*0x10000;
        if(format<3){const unsigned red=format==0?0x001Fu:format==1?0x801Fu:0xF00Fu;
            const unsigned blue=format==0?0xF800u:format==1?0xFC00u:0xFF00u;
            for(unsigned y=0;y<width;++y)for(unsigned x=0;x<width;++x)texel(rt.memory(),s.regs,level,x,y,level==2?blue:red);
        }else{const unsigned bytes=format==8?8u:16u;
            for(unsigned y=0;y<width/4;++y)for(unsigned x=0;x<width/4;++x){const unsigned p=address+(y*(width/4)+x)*bytes;
                rt.memory().store32(p,0);rt.memory().store16(p+4,level==2?0x001F:0xF800);rt.memory().store16(p+6,0);
                if(format==9){rt.memory().store32(p+8,0xFFFFFFFF);rt.memory().store32(p+12,0xFFFFFFFF);}
                if(format==10){rt.memory().store32(p+8,0);rt.memory().store16(p+12,0);rt.memory().store8(p+14,255);rt.memory().store8(p+15,255);}
            }
        }
    }
    auto expected=vram(rt.memory());const unsigned color=format<3?0xFF7F007Fu:0xFF7C007Cu;
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)paint(expected,x,y,color);
    unsigned cursor{};std::string error;require(chili::render_ge_sprites(rt.memory(),s.regs,vertex,0x60002,cursor,error,&s.palette),error);
    require(cursor==vertex+32 && vram(rt.memory())==expected,"565/5551/4444/DXT1/3/5 mip decode wholeVRAM oracle");
}}
void bilinear_cases(){for(unsigned format:{3u,4u,5u})for(bool swizzle:{false,true}){
    psprecomp::Runtime rt;auto s=setup(rt.memory(),format,swizzle);s.regs[0xC8]=0x180001;
    constexpr unsigned colors[]{0xFF000000,0xFF202020,0xFF404040,0xFF606060,0xFF808080,0xFFA0A0A0,0xFFC0C0C0,0xFFE0E0E0};
    for(unsigned n=0;n<8;++n)rt.memory().store32(palette_address+n*4,colors[n]);std::string error;
    require(s.palette.load(rt.memory(),palette_address&0xFFFFFF,(palette_address>>8)&0xF0000,32,error),error);
    // Midpoint UV64/64 crosses both wrapping/tile axes. Each selected mip has
    // unequal four taps and exact four-bit fractions8/8. Arithmetic oracle:
    // L1=FF303030, L2=FFB0B0B0, mip half=FF707070.
    for(unsigned l=1;l<=2;++l){const unsigned edge=(64>>l)-1;
        const unsigned xs[]{edge,0,edge,0},ys[]{edge,edge,0,0};
        for(unsigned n=0;n<4;++n)texel(rt.memory(),s.regs,l,xs[n],ys[n],format==3?colors[(l-1)*4+n]:(l-1)*4+n);
    }
    write_vertex(rt.memory(),0,60,60,0,0);write_vertex(rt.memory(),1,68,68,1,1);
    auto expected=vram(rt.memory());paint(expected,0,0,0xFF707070);unsigned cursor{};
    require(chili::render_ge_sprites(rt.memory(),s.regs,vertex,0x60002,cursor,error,&s.palette),error);
    require(vram(rt.memory())==expected,"unequal fourtap two-stage mip oracle");
}}
void failures(){const std::function<void(Setup &,psprecomp::GuestMemory &)> bad[]{
    [](auto &s,auto &){s.regs[0xC6]=0x207;},[](auto &s,auto &){s.regs[0xC3]=6;},
    [](auto &s,auto &){s.regs[0xA1]=0xFFFFF0;s.regs[0xA9]=0x90020;},
    [](auto &s,auto &){s.regs[0x9C]=0x1FF000;},[](auto &s,auto &){s.regs[0x24]=1;},
    [](auto &s,auto &){s.regs[0xDF]=0x300;s.regs[0x21]=1;},
    [](auto &s,auto &){s.regs[0xC8]=0xE80002;s.regs[0xD0]=0x7FC000;},
    [](auto &s,auto &){s.regs[0xA9]=31;}, // L1 is actually selected; unused L0 storage is deliberately not prepared.
    [](auto &s,auto &){s.regs[0x23]=1;s.regs[0x9F]=512;s.regs[0x9E]=0;},
    [](auto &s,auto &){s.regs[0xC3]=8;s.regs[0xC2]|=1;},
    [](auto &s,auto &){s.regs[0xC3]=8;s.regs[0xA1]=0;s.regs[0xA9]=0x40020;} // selected compressed L1 must retain the feedback rejection.
    };
    for(const auto &mutate:bad){psprecomp::Runtime rt;auto s=setup(rt.memory());mutate(s,rt.memory());const auto before=all(rt.memory());
        unsigned cursor=0xFEEDCAFE;std::string error;require(!chili::render_ge_sprites(rt.memory(),s.regs,vertex,0x60002,cursor,error,&s.palette),"invalid mip accepted");
        require(cursor==0xFEEDCAFE && all(rt.memory())==before,"invalid mip prewrite atomicity");require(!error.empty(),"invalid state lacks reason");
    }
    for(unsigned address:{0x09FFFFF0u,0xFFFFFFF0u,vertex+1u}){psprecomp::Runtime rt;auto s=setup(rt.memory());const auto before=all(rt.memory());unsigned cursor=123;std::string error;
        require(!chili::render_ge_sprites(rt.memory(),s.regs,address,0x60002,cursor,error,&s.palette),"invalid source span/alignment/overflow accepted");
        require(cursor==123 && all(rt.memory())==before,"source failure atomicity");}
    psprecomp::Runtime rt;auto s=setup(rt.memory(),4,true);chili::GeClutCache cold;const auto before=all(rt.memory());unsigned cursor=123;std::string error;
    require(!chili::render_ge_sprites(rt.memory(),s.regs,vertex,0x60002,cursor,error,&cold),"cold palette accepted");
    require(cursor==123 && all(rt.memory())==before,"cold palette failure atomicity");
}
void feedback_cases(){for(unsigned positive=0;positive<2;++positive){psprecomp::Runtime rt;auto s=setup(rt.memory());
    s.regs[0xC2]=0;s.regs[0xC6]=4;s.regs[0xC8]=1;s.regs[0xA0]=0;s.regs[0xA8]=0x40040;
    write_vertex(rt.memory(),0,0,0,0,positive?0:1);write_vertex(rt.memory(),1,positive?0:2,1,2,positive?1:2);
    const auto before=all(rt.memory());unsigned cursor=123;std::string error;
    const bool ok=chili::render_ge_sprites(rt.memory(),s.regs,vertex,0x60002,cursor,error,&s.palette);
    require(ok==!positive,"feedback disjoint/dependent classification");
    if(positive)require(cursor==123 && all(rt.memory())==before,"feedback failure changed memory/cursor");
    else require(cursor==vertex+32,"disjoint feedback cursor");
}
    psprecomp::Runtime rt;auto s=setup(rt.memory());s.regs[0xC2]=0;s.regs[0xC6]=4;s.regs[0xC8]=1;s.regs[0xA0]=0;s.regs[0xA8]=0x40040;
    write_vertex(rt.memory(),0,0,0,0,1);write_vertex(rt.memory(),1,2,1,2,2);
    write_vertex(rt.memory(),2,0,8,3,1);write_vertex(rt.memory(),3,2,9,5,2);
    const auto before=all(rt.memory());unsigned cursor=123;std::string error;
    require(!chili::render_ge_sprites(rt.memory(),s.regs,vertex,0x60004,cursor,error,&s.palette),"later feedback dependency accepted");
    require(cursor==123 && all(rt.memory())==before,"later feedback prefix changed memory/cursor");
}
void contributing_feedback(){psprecomp::Runtime rt;auto s=setup(rt.memory());s.regs[0xC2]=0;s.regs[0xC6]=7;s.regs[0xA0]=0;s.regs[0xA8]=0x40200;
    write_vertex(rt.memory(),0,0,0,0,8);write_vertex(rt.memory(),1,16,8,16,16);
    auto expected=vram(rt.memory());const auto original=expected;
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<16;++x){const unsigned p=(y*512+x)*4;
        const unsigned color=original[p]|(unsigned(original[p+1])<<8)|(unsigned(original[p+2])<<16);paint(expected,x,y+8,color);}
    unsigned cursor{};std::string error;require(chili::render_ge_sprites(rt.memory(),s.regs,vertex,0x60002,cursor,error,&s.palette),error);
    require(vram(rt.memory())==expected,"zero-weight mip bilinear bottom taps caused feedback/output regression");
    // A source/target overlap in a level not contributing to the selected LOD
    // must not turn a valid visible draw into a feedback rejection.
    auto s2=setup(rt.memory());s2.regs[0xA0]=0;s2.regs[0xA8]=0x40040;s2.regs[0xC8]=0x180001;
    expected=vram(rt.memory());for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)paint(expected,x,y,0xFF304050);
    require(chili::render_ge_sprites(rt.memory(),s2.regs,vertex,0x60002,cursor,error,&s2.palette),error);
    require(vram(rt.memory())==expected,"unused mip level wrongly treated as dependency");
}
void pipeline_cases(){for(unsigned which=0;which<4;++which){psprecomp::Runtime rt;auto s=setup(rt.memory());auto &r=s.regs;
    r[0x9E]=0x80000;r[0x9F]=512;r[0x23]=1;r[0xDE]=4;r[0xE7]=0;r[0x22]=1;r[0xDB]=0xFF8000|(which==0?6u:7u);
    r[0xC9]=which<2?0x100u:3u;r[0xE8]=which<2?0xFF00u:0u;r[0xE9]=0x55;
    if(which>=2){r[0x21]=1;r[0xDF]=which==2?0x32u:0xAAu;r[0xE0]=r[0xE1]=0xFFFFFF;}
    write_vertex(rt.memory(),1,128,64,16,16,7,which<2?0x80804020u:0x80FFFFFFu);
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){rt.memory().store32(0x04000000+(y*512+x)*4,0xA1AABBCC);rt.memory().store16(0x04080000+(y*512+x)*2,8);}
    auto expected=vram(rt.memory());if(which){const unsigned color=which==1?0xA118BB0Au:which==2?0xA16C7D8Du:0xA1DAFBFFu;
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){paint(expected,x,y,color);const unsigned p=0x80000+(y*512+x)*2;expected[p]=7;expected[p+1]=0;}}
    unsigned cursor{};std::string error;require(chili::render_ge_sprites(rt.memory(),r,vertex,0x60002,cursor,error,&s.palette),error);
    require(vram(rt.memory())==expected,"mip alpha/modulate/second-Z/depth/RGB-mask/stencil/blend ordered oracle");
}}
void old_parity(){for(unsigned format:{3u,4u,5u})for(unsigned filter:{0u,0x101u})for(unsigned blend:{0u,0x32u,0xAAu}){
    std::vector<std::uint8_t> reference;unsigned ref_cursor{};std::string ref_error;
    for(unsigned pass=0;pass<3;++pass){psprecomp::Runtime rt;auto s=setup(rt.memory(),format,true);s.regs[0xC6]=filter;
        s.regs[0x21]=blend!=0;s.regs[0xDF]=blend;s.regs[0xE0]=s.regs[0xE1]=0xFFFFFF;
        std::ostringstream sink;chili::PerformanceTelemetry telemetry(sink,chili::PerformanceTelemetry::Clock::time_point{});
        unsigned cursor=123;std::string error;const bool ok=pass==0?chili_sprite_baseline::render_ge_sprites(rt.memory(),s.regs,vertex,0x60002,cursor,error,&s.palette):
            chili::render_ge_sprites(rt.memory(),s.regs,vertex,0x60002,cursor,error,&s.palette,pass==2?&telemetry:nullptr);
        require(ok,"legacy path unexpected failure: "+error);
        if(!pass){reference=all(rt.memory());ref_cursor=cursor;ref_error=error;}
        else require(all(rt.memory())==reference && cursor==ref_cursor && error==ref_error,"legacy OFF/ON baseline wholememory/error/cursor parity");
    }
}}
struct Outcome{std::vector<std::uint8_t> memory;std::vector<unsigned> context;Regs registers;unsigned cursor,index_cursor;std::uint64_t time,generation;int uid;bool operator==(const Outcome &)const=default;};
Outcome actual(const std::filesystem::path &prefix,bool hle,bool enabled){psprecomp::Runtime rt;seed_cpu(rt.cpu());chili::ThreadRuntime threads(rt);threads.initialize_bootstrap();
    std::ostringstream sink;chili::PerformanceTelemetry telemetry(sink,chili::PerformanceTelemetry::Clock::time_point{});
    chili::GeRuntime ge(rt,&threads,0,nullptr,enabled?&telemetry:nullptr);auto &m=rt.memory();Regs regs{};
    unsigned metadata_stride{},metadata_bytes{};
    std::ifstream meta(prefix.string()+".txt");std::string line;while(std::getline(meta,line)){
        if(line.starts_with("reg ")){std::istringstream row(line);std::string word;unsigned n,value;row>>word>>std::hex>>n>>value;regs[n]=value;}
        if(line.starts_with("vertex_stride="))metadata_stride=std::stoul(line.substr(14),nullptr,16);
        if(line.starts_with("vertices address=")){const auto pos=line.find("length=");metadata_bytes=std::stoul(line.substr(pos+7),nullptr,16);}
    }
    require(metadata_stride==16 && metadata_bytes==32,"actual metadata must parse hexadecimal stride10/length20");
    const auto source=read(prefix.string()+"_vertices.bin"),captured=read(prefix.string()+"_vram.bin");require(source.size()==32,"actual source size16*2");
    m.copy_in(0x09944F2C,source);m.copy_in(0x04000000,captured);ge.commands=regs;ge.vertex_address=0x09944F2C;ge.index_address=0x08807770;
    // Entire actual rectangle is offscreen; cold cache/unmapped texture levels
    // must remain unused. Source, known state, clipping and cursor are checked.
    const auto before=all(m);const auto context=cpu(rt.cpu());const auto generation=psprecomp::runtime_thread_switch_generation();
    unsigned cursor=123;std::string error;
    if(hle){constexpr unsigned list=0x08810000;m.store32(list,0x04060002);m.store32(list+4,0x0F000000);m.store32(list+8,0x0C000000);
        rt.cpu().gpr[4]=list;rt.cpu().gpr[5]=0;rt.cpu().gpr[6]=0xFFFFFFFF;rt.cpu().gpr[7]=0;
        auto expected_context=rt.cpu();expected_context.set_gpr(2,1);const auto before_hle=all(m);
        rt.invoke_import("sceGe_user",0xAB49E76A,rt.cpu());
        require(!rt.stopped() && ge.completed.size()==1,"actual GE HLE empty mip list");cursor=ge.vertex_address;
        require(all(m)==before_hle && cpu(rt.cpu())==cpu(expected_context),"actual empty HLE fullmemory/context oracle");
    }else{
        require(chili::render_ge_sprites(m,regs,0x09944F2C,0x60002,cursor,error,nullptr,enabled?&telemetry:nullptr),error);
        require(all(m)==before && cpu(rt.cpu())==context,"actual direct no-write CPU/allmemory proof");
    }
    require(cursor==0x09944F4C && vram(m)==captured,"actual empty coverage/cursor/full2MiB proof");
    require(threads.now_us()==0 && threads.current_uid()==1 && psprecomp::runtime_thread_switch_generation()==generation,"actual empty changed scheduler");
    require(sink.str().empty(),"draw observer emitted output");
    return {all(m),cpu(rt.cpu()),ge.commands,cursor,ge.index_address,threads.now_us(),psprecomp::runtime_thread_switch_generation()-generation,threads.current_uid()};
}
Outcome visible_hle(bool enabled){psprecomp::Runtime rt;seed_cpu(rt.cpu());chili::ThreadRuntime threads(rt);threads.initialize_bootstrap();
    std::ostringstream sink;chili::PerformanceTelemetry telemetry(sink,chili::PerformanceTelemetry::Clock::time_point{});
    chili::GeRuntime ge(rt,&threads,0,nullptr,enabled?&telemetry:nullptr);auto s=setup(rt.memory(),4,true);
    ge.commands=s.regs;ge.clut=s.palette;ge.vertex_address=vertex;ge.index_address=0x08807770;
    constexpr unsigned list=0x08810000;rt.memory().store32(list,0x04060002);rt.memory().store32(list+4,0x0F000000);rt.memory().store32(list+8,0x0C000000);
    auto expected=all(rt.memory());for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){const unsigned p=unsigned(rt.memory().size())+(y*512+x)*4;
        expected[p]=0x50;expected[p+1]=0x40;expected[p+2]=0x30;}
    rt.cpu().gpr[4]=list;rt.cpu().gpr[5]=0;rt.cpu().gpr[6]=0xFFFFFFFF;rt.cpu().gpr[7]=0;
    auto expected_context=rt.cpu();expected_context.set_gpr(2,1);const auto generation=psprecomp::runtime_thread_switch_generation();
    rt.invoke_import("sceGe_user",0xAB49E76A,rt.cpu());
    require(!rt.stopped() && ge.completed.size()==1 && ge.vertex_address==vertex+32 && ge.index_address==0x08807770,"visible mip native GE list/cursors");
    require(all(rt.memory())==expected && cpu(rt.cpu())==cpu(expected_context),"visible mip HLE independent allmemory/CPU oracle");
    require(threads.now_us()==0 && threads.current_uid()==1 && psprecomp::runtime_thread_switch_generation()==generation && sink.str().empty(),"visible mip HLE scheduler/output side effect");
    if(enabled)require(telemetry.counters()[chili::PerformanceTelemetry::DrawSuccesses]==1 && telemetry.counters()[chili::PerformanceTelemetry::TexturedSpriteAttempts]==1,"visible mip telemetry attribution");
    return {all(rt.memory()),cpu(rt.cpu()),ge.commands,ge.vertex_address,ge.index_address,threads.now_us(),psprecomp::runtime_thread_switch_generation()-generation,threads.current_uid()};
}
void empty_cases(){for(unsigned which=0;which<5;++which){psprecomp::Runtime rt;auto s=setup(rt.memory());
    if(which==0){write_vertex(rt.memory(),0,0,0,100,0);write_vertex(rt.memory(),1,128,64,116,16);}
    if(which==1){write_vertex(rt.memory(),0,0,0,-20,0);write_vertex(rt.memory(),1,128,64,-4,16);}
    if(which==2){write_vertex(rt.memory(),0,0,0,0,100);write_vertex(rt.memory(),1,128,64,16,116);}
    if(which==3){write_vertex(rt.memory(),0,0,0,0,-20);write_vertex(rt.memory(),1,128,64,16,-4);}
    if(which==4){write_vertex(rt.memory(),0,0,0,4,4);write_vertex(rt.memory(),1,128,64,4,16);}
    const auto before=all(rt.memory());s.regs[0xA0]=0xFFFFF0;s.regs[0xA8]=0x90040;unsigned cursor=123;std::string error;
    require(chili::render_ge_sprites(rt.memory(),s.regs,vertex,0x60002,cursor,error,nullptr),error);
    require(cursor==vertex+32 && all(rt.memory())==before,"empty sprite did not validate/no-op");
}}
} // namespace
int main(int argc,char **argv){try{require(argc==2,"capture prefix argument required");constant_cases();selection_cases();material_color_case();extra_formats();bilinear_cases();pipeline_cases();failures();feedback_cases();contributing_feedback();old_parity();empty_cases();
    const auto direct_off=actual(argv[1],false,false),direct_on=actual(argv[1],false,true);require(direct_off==direct_on,"actual direct telemetry parity");
    const auto hle_off=actual(argv[1],true,false),hle_on=actual(argv[1],true,true);require(hle_off==hle_on,"actual HLE fullstate telemetry parity");
    const auto visible_off=visible_hle(false),visible_on=visible_hle(true);require(visible_off==visible_on,"visible mip HLE OFF/ON allstate parity");
    std::cout<<"PASS: "<<checks<<" assertions; actual empty sprite/fullmemory+HLE, visible affine LOD/T4/T8/RGBA/filter oracles, oldpath and strict atomic failures\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
