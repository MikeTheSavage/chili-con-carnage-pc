#include "ge_runtime.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace chili;
static unsigned assertions{};
static void require(bool result,const std::string &why){++assertions;if(!result)throw std::runtime_error(why);}
struct Fixture {
    psprecomp::Runtime rt;GeClutCache clut;std::array<unsigned,256> r{};
    ge::DeformState deformation{};
    std::array<float,12> world{1,0,0,0,1,0,0,0,1,0,0,0},view=world;
    std::array<float,16> projection{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    unsigned v=0x08804000,idx=0x08807000,end=0xDEADBEEF,iend=0xBAADF00D,written=0xABCD;
    std::string error;
    Fixture(){
        r[0xD2]=3;r[0x9D]=512;r[0x9E]=0x80000;r[0x9F]=512;r[0xD5]=r[0x16]=7|(7<<10);r[0xD7]=65535;
        r[0x55]=0x223311;r[0x58]=255;r[0x4C]=r[0x4D]=2046*16;r[0x1C]=1;
        regf(0x42,2);regf(0x43,2);regf(0x44,100);regf(0x45,2048);regf(0x46,2048);regf(0x47,100);
        regf(0x48,1);regf(0x49,1);deformation.morph_weights[0]=1;
        for(auto &bone:deformation.bones)bone=world;
        reset();
    }
    void regf(unsigned at,float f){r[at]=std::bit_cast<unsigned>(f)>>8;}
    void reset(){
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){rt.memory().store32(0x04000000+(y*512+x)*4,0xAA112233);rt.memory().store16(0x04080000+(y*512+x)*2,1000);}
        end=0xDEADBEEF;iend=0xBAADF00D;written=0xABCD;
    }
    unsigned pixel(unsigned x,unsigned y){return rt.memory().load32(0x04000000+(y*512+x)*4);}
    void encode(unsigned vertex,unsigned morph,const ge::Sample &sample){
        auto l=ge::make_layout(r[0x12]);const unsigned a=v+vertex*l.vertex_stride+morph*l.sample_stride;
        for(unsigned n=0;n<l.sample_stride;++n)rt.memory().store8(a+n,0xCC);
        auto component=[&](unsigned at,unsigned type,float value,bool normalized){
            unsigned size=1u<<(type-1),bits=type==3?std::bit_cast<unsigned>(value):unsigned(int(value*(normalized?(type==1?128:32768):1)));
            for(unsigned n=0;n<size;++n)rt.memory().store8(a+at+n,std::uint8_t(bits>>(n*8)));
        };
        constexpr unsigned size[]={0,1,2,4};
        for(unsigned n=0;n<l.weight_count;++n)component(l.weight_offset+n*size[l.weight_type],l.weight_type,sample.weights[n],true);
        if(l.uv_type)for(unsigned n=0;n<2;++n)component(l.uv_offset+n*size[l.uv_type],l.uv_type,(*sample.uv)[n],!l.through);
        if(l.normal_type)for(unsigned n=0;n<3;++n)component(l.normal_offset+n*size[l.normal_type],l.normal_type,(*sample.normal)[n],true);
        if(l.color_type){unsigned color=0;if(l.color_type==7)for(unsigned n=0;n<4;++n)color|=unsigned((*sample.color)[n])<<(n*8);
            else color=l.color_type==4?31:l.color_type==5?0x801F:0xF00F;
            if(l.color_type==7)rt.memory().store32(a+l.color_offset,color);else rt.memory().store16(a+l.color_offset,color);}
        for(unsigned n=0;n<3;++n)component(l.position_offset+n*size[l.position_type],l.position_type,sample.position[n],!l.through);
    }
    void source(unsigned morphs,unsigned weight_type=0,unsigned weights=0,unsigned index_type=0,bool attrs=true){
        r[0x12]=(3u<<7)|(attrs?((7u<<2)|(3u<<5)|3u):0u)|(weight_type<<9)|
            (weights?((weights-1)<<14):0u)|((morphs-1)<<18)|(index_type<<11);
        for(unsigned vertex=0;vertex<3;++vertex)for(unsigned morph=0;morph<morphs;++morph){
            ge::Sample sample;sample.position={vertex==1?1.f:-1.f,vertex==2?1.f:-1.f,0};
            if(attrs){sample.color=ge::Color{255,0,0,255};sample.uv=ge::Vec2{0,0};sample.normal=ge::Vec3{1,2,3};}
            for(unsigned n=0;n<weights;++n)sample.weights[n]=1.f/weights;
            encode(vertex,morph,sample);
        }
        if(index_type)for(unsigned n=0;n<3;++n){if(index_type==1)rt.memory().store8(idx+n,(n+2)%3);else rt.memory().store16(idx+n*2,(n+2)%3);}
    }
    bool draw(const ge::DeformState *state,unsigned count=3,unsigned primitive=3){
        return render_ge_triangle_raster(rt.memory(),r,world,view,projection,v,(primitive<<16)|count,end,error,clut,&written,idx,&iend,state);
    }
    void exact(unsigned color,unsigned shift_x=0){
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){bool inside=x>=shift_x&&(x-shift_x)+y<=3;
            require(pixel(x,y)==(inside?color:0xAA112233),"Deformed pixel mismatch "+std::to_string(x)+","+std::to_string(y));}
    }
    std::vector<std::uint8_t> vram(){std::vector<std::uint8_t> out(psprecomp::GuestMemory::kVramSize);rt.memory().copy_out(0x04000000,out);return out;}
};
static void set_morph_sources(Fixture &f){
    f.deformation.morph_weights[0]=f.deformation.morph_weights[1]=0.5f;
    for(unsigned vertex=0;vertex<3;++vertex)for(unsigned morph=0;morph<2;++morph){
        ge::Sample sample;sample.position={float(vertex==1?1:-1)+morph,vertex==2?1.f:-1.f,0};
        sample.color=morph?ge::Color{0,255,0,255}:ge::Color{255,0,0,255};
        sample.uv=ge::Vec2{float(morph),0};sample.normal=morph?ge::Vec3{3,4,5}:ge::Vec3{1,2,3};f.encode(vertex,morph,sample);
    }
}
static void morph_pixels(){
    for(unsigned index_type : {0u,1u,2u}){
        Fixture f;f.source(2,0,0,index_type);set_morph_sources(f);
        require(f.draw(&f.deformation)&&f.written==10,"morph visible draw: "+f.error);f.exact(0xAA007F7F,1);
        const auto l=ge::make_layout(f.r[0x12]);
        require(f.end==(index_type?f.v:f.v+3*l.vertex_stride)&&f.iend==(index_type?f.idx+3*index_type:f.idx),"morph both source/index cursors");
        f.r[0x1E]=1;f.r[0xC3]=3;f.r[0xC9]=0x100;f.r[0xC7]=0x101;f.r[0xC6]=0;
        f.r[0xA0]=0x820000;f.r[0xA8]=0x080002;f.r[0xB8]=1;
        f.rt.memory().store32(0x08820000,0xFF0000FF);f.rt.memory().store32(0x08820004,0xFFFFFFFF);
        f.reset();require(f.draw(&f.deformation)&&f.written==10,"morphed UV native texel: "+f.error);f.exact(0xAA007F7F,1);
        f.r[0x1E]=0;f.r[0x17]=1;f.r[0x53]=1;f.r[0x5C]=0xFFFFFF;f.r[0x5D]=255;f.r[0x54]=0x010000;
        f.r[0x23]=1;f.r[0xDE]=4;f.reset();require(f.draw(&f.deformation),"morph ambient/depth path: "+f.error);f.exact(0xAA017F7F,1);
        require(f.rt.memory().load16(0x04080000+2)==100,"deformed fragment depth update");
    }
    Fixture missing;missing.source(2,0,0,0,false);missing.deformation.morph_weights[0]=missing.deformation.morph_weights[1]=0.5f;
    require(missing.draw(&missing.deformation),"absent morph color must retain GE material: "+missing.error);missing.exact(0xAA223311);
    std::cout<<"PASS morph pixels, color/UV/ambient/depth, absent material source, index8/16 and dual cursors\n";
}
static void skin_pixels(){
    for(unsigned type : {1u,2u,3u})for(unsigned index_type : {0u,1u,2u}){
        Fixture f;f.source(1,type,8,index_type);
        f.deformation.bones[7][9]=4; // Every weight is1/8: final translation is exactly0.5.
        f.deformation.morph_weights.fill(std::numeric_limits<float>::quiet_NaN()); // Count1 ignores morph registers.
        require(f.draw(&f.deformation)&&f.written==10,"all8 bone native draw: "+f.error);f.exact(0xAA0000FF,1);
        const auto l=ge::make_layout(f.r[0x12]);require(f.end==(index_type?f.v:f.v+3*l.vertex_stride)&&
            f.iend==(index_type?f.idx+3*index_type:f.idx),"all8 bones index/nonindex cursors");
    }
    Fixture f;f.source(1,3,1);
    for(unsigned vertex=0;vertex<3;++vertex){ge::Sample s;s.position={vertex==1?1.f:-1.f,vertex==2?1.f:-1.f,0};
        s.color=ge::Color{255,0,0,255};s.uv=ge::Vec2{0,0};s.normal=ge::Vec3{1,2,3};s.weights[0]=2;f.encode(vertex,0,s);}
    f.deformation.bones[0]={0.5f,0,0,0,0.5f,0,0,0,0.5f,0.25f,0,0};
    require(f.draw(&f.deformation)&&f.written==10,"nonunit native bone weight: "+f.error);f.exact(0xAA0000FF,1);
    Fixture absent;absent.source(1,3,1,0,false);absent.deformation.bones[0][9]=0.5f;
    require(absent.draw(&absent.deformation),"bone draw absent color/material: "+absent.error);absent.exact(0xAA223311,1);
    std::cout<<"PASS skin pixels with all8 bones and raw weight8/16/float, nonunit weights, absent attributes, index8/16\n";
}
static void ordinary_compatibility(){
    Fixture f;f.source(1);f.deformation.bones[0].fill(std::numeric_limits<float>::quiet_NaN());f.deformation.morph_weights.fill(std::numeric_limits<float>::quiet_NaN());
    require(f.draw(nullptr),"ordinary draw requires no deformation state");f.exact(0xAA0000FF);
    f.reset();require(f.draw(&f.deformation),"ordinary draw must ignore extraneous malformed deformation state");f.exact(0xAA0000FF);
    f.source(1,3,8);f.r[0x12]|=0x800000;
    for(unsigned vertex=0;vertex<3;++vertex){ge::Sample s;s.position={vertex==1?4.f:0.f,vertex==2?4.f:0.f,100};
        s.color=ge::Color{255,0,0,255};s.uv=ge::Vec2{0,0};s.normal=ge::Vec3{1,2,3};f.encode(vertex,0,s);}
    f.reset();f.world.fill(std::numeric_limits<float>::quiet_NaN());f.view=f.world;f.projection.fill(std::numeric_limits<float>::quiet_NaN());
    require(f.draw(nullptr),"through weighted records bypass bone/world state");f.exact(0xAA0000FF);
    std::cout<<"PASS optional state preserves ordinary and through bypass behavior\n";
}
static void failure_atomic(){
    auto check=[](Fixture &f,const ge::DeformState *state,const char *label){auto before=f.vram();
        require(!f.draw(state),label);require(f.vram()==before&&f.end==0xDEADBEEF&&f.iend==0xBAADF00D&&f.written==0xABCD,
            std::string(label)+" changed VRAM/cursors/counter");};
    {Fixture f;f.source(2);check(f,nullptr,"missing required morph state");}
    {Fixture f;f.source(1,3,1);check(f,nullptr,"missing required bone state");}
    {Fixture f;f.source(2,3,1);f.deformation.morph_weights[1]=std::numeric_limits<float>::quiet_NaN();check(f,&f.deformation,"nonfinite combined morph strict");}
    {Fixture f;f.source(2);f.r[0x12]|=0x800000;check(f,&f.deformation,"through morph strict");}
    {Fixture f;f.r[0x12]=(3u<<7)|(4u<<2)|(1u<<18);for(unsigned v=0;v<3;++v)for(unsigned m=0;m<2;++m){ge::Sample s;s.position={v==1?1.f:-1.f,v==2?1.f:-1.f,0};f.encode(v,m,s);}check(f,&f.deformation,"questioned565 morph alpha strict");}
    {Fixture f;f.source(1,3,1);f.deformation.bones[0][3]=std::numeric_limits<float>::quiet_NaN();check(f,&f.deformation,"active malformed bone matrix");}
    {Fixture f;f.source(2,0,0,2);set_morph_sources(f);f.deformation.morph_weights[1]=std::numeric_limits<float>::quiet_NaN();check(f,&f.deformation,"active malformed morph register");}
    {Fixture f;f.source(2,0,0,2);set_morph_sources(f);auto l=ge::make_layout(f.r[0x12]);f.rt.memory().store32(f.v+l.vertex_stride+l.sample_stride+l.position_offset,std::bit_cast<unsigned>(std::numeric_limits<float>::infinity()));check(f,&f.deformation,"late nonfinite indexed morph source");}
    {Fixture f;f.source(1,3,1);f.r[0x1F]=1;f.r[0xCE]=0x7FC000;check(f,&f.deformation,"nonfinite fog after valid bones");}
    {Fixture f;f.source(2);set_morph_sources(f);f.r[0x17]=f.r[0x18]=1;f.r[0x5F]=3;check(f,&f.deformation,"unknown individual light remains strict after morph");}
    {Fixture f;f.source(1,3,1);f.r[0x1E]=1;f.r[0xA0]=0xFFFFF0;f.r[0xA8]=0x090008;f.r[0xB8]=3;f.r[0xC3]=3;check(f,&f.deformation,"bad texture after valid deformation");}
    std::cout<<"PASS required-state/unsupported-combination/late-input/pixel-state failures preserve full2MiB VRAM and outputs\n";
}
static void streamed_runtime(bool morph,unsigned index_type){
    Fixture f;f.source(morph?2:1,morph?0:3,morph?0:8,index_type);
    if(morph)set_morph_sources(f);else f.deformation.bones[7][9]=4;
    GeRuntime gpu(f.rt);gpu.commands=f.r;gpu.world=f.world;gpu.view=f.view;gpu.projection=f.projection;
    // Deliberately start state at zero. It must be established by actual GE commands.
    const unsigned list=0x08809000;unsigned cursor=list;
    auto emit=[&](unsigned command,unsigned data){f.rt.memory().store32(cursor,(command<<24)|(data&0xFFFFFF));cursor+=4;};
    auto emitf=[&](unsigned command,float value){emit(command,std::bit_cast<unsigned>(value)>>8);};
    emit(0x12,f.r[0x12]);
    if(morph){emitf(0x2C,0.5f);emitf(0x2D,0.5f);}
    else {emit(0x2A,0);for(const auto &bone:f.deformation.bones)for(float value:bone)emitf(0x2B,value);}
    emit(0x10,0x080000);emit(1,f.v&0xFFFFFF);emit(2,f.idx&0xFFFFFF);emit(4,0x030003);emit(0x0F,0xBEEF);emit(0x0C,0);
    f.rt.cpu().gpr[4]=list;f.rt.cpu().gpr[6]=0xFFFFFFFF;f.rt.invoke_import("sceGe_user",0xAB49E76Au,f.rt.cpu());
    auto l=ge::make_layout(f.r[0x12]);require(!f.rt.stopped()&&gpu.completed.size()==1&&gpu.commands[4]==0x04030003&&
        gpu.vertex_address==(index_type?f.v:f.v+3*l.vertex_stride)&&gpu.index_address==(index_type?f.idx+3*index_type:f.idx),
        "runtime streamed state did not finish/commit dual cursors: "+f.rt.stop_reason());
    if(morph)require(gpu.morph[0]==0.5f&&gpu.morph[1]==0.5f,"runtime streamed morph state missing");
    else require(gpu.bones[93]==4&&gpu.bone_cursor==96,"runtime96 bone values/cursor missing");
    f.exact(morph?0xAA007F7F:0xAA0000FF,1);
}
static void runtime_failed_draw(){
    Fixture f;f.source(2,3,1,2);GeRuntime gpu(f.rt);gpu.bones[0]=std::numeric_limits<float>::quiet_NaN();gpu.commands=f.r;gpu.commands[4]=0x04123456;
    gpu.world=f.world;gpu.view=f.view;gpu.projection=f.projection;gpu.vertex_address=f.v;gpu.index_address=f.idx;
    const unsigned list=0x08809000;f.rt.memory().store32(list,0x04030003);f.rt.memory().store32(list+4,0x0F000000);f.rt.memory().store32(list+8,0x0C000000);
    auto before=f.vram();f.rt.cpu().gpr[4]=list;f.rt.cpu().gpr[6]=0xFFFFFFFF;f.rt.invoke_import("sceGe_user",0xAB49E76Au,f.rt.cpu());
    require(f.rt.stopped()&&gpu.completed.empty()&&gpu.commands[4]==0x04123456&&gpu.vertex_address==f.v&&gpu.index_address==f.idx&&
        f.vram()==before,"failed deformed runtime list published command/cursors/completion or VRAM");
}
int main(){try{
    morph_pixels();skin_pixels();ordinary_compatibility();failure_atomic();
    for(unsigned index_type : {0u,1u,2u}){streamed_runtime(true,index_type);streamed_runtime(false,index_type);}runtime_failed_draw();
    std::cout<<"PASS "<<assertions<<" deformation/raster/runtime assertions; visible native pixels and streamed GE state; hardware precision limits unchanged\n";
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
