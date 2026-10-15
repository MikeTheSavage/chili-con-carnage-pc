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
