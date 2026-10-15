#include "ge_runtime.hpp"
#include <iostream>
#include <limits>
using namespace chili;
static unsigned assertions{};
static void require(bool result,const std::string &why){++assertions;if(!result)throw std::runtime_error(why);}
struct Fixture {
    psprecomp::Runtime rt;GeClutCache clut;std::array<unsigned,256> r{};
    std::array<float,12> world{1,0,0,0,1,0,0,0,1,0,0,0},view=world;
    std::array<float,16> projection{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    unsigned vertices=0x08804000,indices=0x08807000,end=0xDEADBEEF,iend=0xBAADF00D,written=0xABCD;
    ge::DeformState deformation{};std::string error;
    Fixture(){
        r[0x12]=0x1FF;r[0xD2]=3;r[0x9D]=512;r[0x9E]=0x80000;r[0x9F]=512;r[0xD5]=r[0x16]=7|(7<<10);r[0xD7]=65535;
        r[0x58]=r[0x5D]=255;r[0x4C]=r[0x4D]=2046*16;r[0x1C]=1;r[0x17]=r[0x18]=1;
        r[0x56]=0xFFFFFF;r[0x90]=0x4080FF;regf(0x65,1);regf(0x7B,1);regf(0x71,1);regf(0x87,1);regf(0x5B,1);
        regf(0x42,2);regf(0x43,2);regf(0x44,100);regf(0x45,2048);regf(0x46,2048);regf(0x47,100);regf(0x48,1);regf(0x49,1);
        for(auto &bone:deformation.bones)bone=world;deformation.morph_weights[0]=1;source();reset();
    }
    void regf(unsigned at,float f){r[at]=std::bit_cast<unsigned>(f)>>8;}
    void vector(unsigned at,ge::Vec3 v){for(unsigned n=0;n<3;++n)regf(at+n,v[n]);}
    void reset(){for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){rt.memory().store32(0x04000000+(y*512+x)*4,0xAA112233);rt.memory().store16(0x04080000+(y*512+x)*2,1000);}end=0xDEADBEEF;iend=0xBAADF00D;written=0xABCD;}
    unsigned pixel(unsigned x,unsigned y){return rt.memory().load32(0x04000000+(y*512+x)*4);}
    void encode(unsigned vertex,unsigned morph,ge::Vec3 p,ge::Vec3 n={0,0,1},ge::Color color={0,0,0,255},ge::Vec2 uv={0,0}){
        auto l=ge::make_layout(r[0x12]);unsigned a=vertices+vertex*l.vertex_stride+morph*l.sample_stride;
        rt.memory().zero(a,l.sample_stride);
        if(l.weight_type)for(unsigned w=0;w<l.weight_count;++w)rt.memory().store32(a+l.weight_offset+w*4,std::bit_cast<unsigned>(w?0.f:1.f));
        if(l.uv_type)for(unsigned k=0;k<2;++k)rt.memory().store32(a+l.uv_offset+k*4,std::bit_cast<unsigned>(uv[k]));
        if(l.color_type){unsigned packed=0;for(unsigned k=0;k<4;++k)packed|=unsigned(color[k])<<(k*8);rt.memory().store32(a+l.color_offset,packed);}
        if(l.normal_type)for(unsigned k=0;k<3;++k)rt.memory().store32(a+l.normal_offset+k*4,std::bit_cast<unsigned>(n[k]));
        for(unsigned k=0;k<3;++k)rt.memory().store32(a+l.position_offset+k*4,std::bit_cast<unsigned>(p[k]));
    }
    void source(){auto l=ge::make_layout(r[0x12]);for(unsigned v=0;v<3;++v)for(unsigned m=0;m<l.morph_count;++m)encode(v,m,{v==1?1.f:-1.f,v==2?1.f:-1.f,0});}
    bool draw(unsigned count=3,unsigned primitive=3,const ge::DeformState *state=nullptr){return render_ge_triangle_raster(rt.memory(),r,world,view,projection,vertices,(primitive<<16)|count,end,error,clut,&written,indices,&iend,state);}
    std::vector<std::uint8_t> vram(){std::vector<std::uint8_t> bytes(psprecomp::GuestMemory::kVramSize);rt.memory().copy_out(0x04000000,bytes);return bytes;}
    void exact(unsigned color,bool clipped=false){for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){bool inside=x+y<=3&&(!clipped||x+y>=2);require(pixel(x,y)==(inside?color:0xAA112233),"coverage/color "+std::to_string(x)+","+std::to_string(y));}}
    void black_texture(){r[0x1E]=1;r[0xC3]=3;r[0xC9]=0x100;r[0xC7]=0x101;r[0xA0]=0x820000;r[0xA8]=0x080001;r[0xB8]=0;rt.memory().store32(0x08820000,0xFF000000);}
    void separate(){r[0x5F]=1;r[0x56]=r[0x90]=0;r[0x57]=0x800000;r[0x91]=0xFFFFFF;r[0x5E]=1;black_texture();}
    void runtime_draw(unsigned count=3,unsigned primitive=3){
        GeRuntime gpu(rt);unsigned cursor=0x08809000;
        auto emit=[&](unsigned command,unsigned value){rt.memory().store32(cursor,(command<<24)|(value&0xFFFFFF));cursor+=4;};
        auto matrix=[&](unsigned command,const auto &values){emit(command,0);for(float value:values)emit(command+1,std::bit_cast<unsigned>(value)>>8);};
        for(unsigned command=0x12;command<256;++command)if(r[command])emit(command,r[command]);
        matrix(0x3A,world);matrix(0x3C,view);matrix(0x3E,projection);
        if(ge::make_layout(r[0x12]).needs_skinning()){emit(0x2A,0);for(const auto &bone:deformation.bones)for(float value:bone)emit(0x2B,std::bit_cast<unsigned>(value)>>8);}
        if(ge::make_layout(r[0x12]).needs_morphing())for(unsigned n=0;n<8;++n)emit(0x2C+n,std::bit_cast<unsigned>(deformation.morph_weights[n])>>8);
        emit(0x10,0x080000);emit(1,vertices&0xFFFFFF);emit(2,indices&0xFFFFFF);emit(4,(primitive<<16)|count);emit(0x0F,0xBEEF);emit(0x0C,0);
        rt.cpu().gpr[4]=0x08809000;rt.cpu().gpr[6]=0xFFFFFFFF;rt.invoke_import("sceGe_user",0xAB49E76Au,rt.cpu());
        auto l=ge::make_layout(r[0x12]);require(!rt.stopped()&&gpu.completed.size()==1&&gpu.vertex_address==(l.index_bytes?vertices:vertices+count*l.vertex_stride)&&gpu.index_address==(l.index_bytes?indices+count*l.index_bytes:indices),"streamed lighting runtime stopped or changed cursor policy: "+rt.stop_reason());
    }
};
static void visible_light_types(){
    const unsigned expected[]={0xAA4080FF,0xAA1A3469,0xAA152B55};
    for(unsigned type=0;type<3;++type){Fixture f;f.r[0x5F]=type<<8;f.vector(0x63,{0,0,2});f.vector(0x7B,{2,0,0});f.regf(0x8B,0.5f);
        require(f.draw()&&f.written==10,"native directional/point/spot: "+f.error);f.exact(expected[type]);
        f.reset();f.runtime_draw();f.exact(expected[type]);}
    Fixture f;f.r[0x55]=0x80;f.r[0x5C]=0xFFFFFF;f.r[0x56]=f.r[0x90]=0;f.separate();f.runtime_draw();f.exact(0xAA800000);
    std::cout<<"PASS visible directional/point/spot streamed GE state and post-black-texture separate specular\n";
}
static void transformed_normals_and_deformation(){
    {Fixture f;f.world[8]=-1;f.world[11]=50;f.projection[10]=0;f.vector(0x63,{0,0,-1});
        f.runtime_draw();f.exact(0xAA4080FF);f.reset();f.r[0x51]=1;require(f.draw(),"NORMAL_REVERSE: "+f.error);f.exact(0xAA000000);}
    {Fixture f;f.r[0x12]|=3u<<9;f.source();f.deformation.bones[0][8]=-1;f.vector(0x63,{0,0,-1});f.runtime_draw();f.exact(0xAA4080FF);}
    {Fixture f;f.r[0x12]|=1u<<18;f.deformation.morph_weights[0]=f.deformation.morph_weights[1]=0.5f;
        for(unsigned v=0;v<3;++v){ge::Vec3 p{v==1?1.f:-1.f,v==2?1.f:-1.f,0};f.encode(v,0,p,{0,0,1});f.encode(v,1,p,{0,0,3});}
        f.runtime_draw();f.exact(0xAA4080FF);}
    {Fixture f;f.r[0x12]=(f.r[0x12]&~0x1Cu)|0x1000;f.source();f.r[0x53]=7;
        for(unsigned n=0;n<3;++n)f.rt.memory().store16(f.indices+n*2,(n+1)%3);
        f.runtime_draw();f.exact(0xAA4080FF);}
    std::cout<<"PASS world linear normals without translation, one reversal, streamed skin/morph normals, absent source color and index16 dual cursors\n";
}
static void varying_secondary_and_clipping(){
    for(unsigned varying_w : {0u,1u})for(unsigned flat : {0u,1u}){
        Fixture f;f.separate();f.r[0x53]=4;f.r[0x50]=!flat;
        if(varying_w){f.projection[10]=f.projection[15]=0;f.projection[11]=1;}
        for(unsigned n=0;n<3;++n){float w=varying_w?float(1u<<n):1;f.encode(n,0,{(n==1?1.f:-1.f)*w,(n==2?1.f:-1.f)*w,varying_w?w:0},
            {0,0,1},{0,0,std::uint8_t(n*64),255});}
        f.runtime_draw();for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){unsigned blue=flat?128:16*x+32*y+21;
            require(f.pixel(x,y)==(x+y<=3?(0xAA000000|(blue<<16)):0xAA112233),"affine separate Gouraud/flat with varying clip W");}
    }
    for(unsigned flat : {0u,1u}){Fixture f;f.separate();f.r[0x53]=4;f.r[0x50]=!flat;
        for(unsigned n=0;n<3;++n)f.encode(n,0,{n==1?1.f:-1.f,n==2?1.f:-1.f,n?0.f:-2.f},{0,0,1},{0,0,std::uint8_t(n*64),255});
        require(f.draw()&&f.written==7,"near-clipped lit triangle: "+f.error);
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){bool inside=x+y>=2&&x+y<=3;unsigned blue=flat?128:16*x+32*y+21;
            require(f.pixel(x,y)==(inside?(0xAA000000|(blue<<16)):0xAA112233),"near clipping secondary or original provoking color");}}
    for(unsigned primitive : {4u,5u}){Fixture f;f.separate();f.r[0x53]=4;
        const ge::Vec3 strip[]={{-1,-1,0},{1,-1,0},{-1,1,0},{1,1,0}},fan[]={{-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0}};
        for(unsigned n=0;n<4;++n)f.encode(n,0,primitive==4?strip[n]:fan[n],{0,0,1},{0,0,std::uint8_t(n*64),255});
        f.runtime_draw(4,primitive);require(f.pixel(3,0)==0xAA800000&&f.pixel(primitive==4?3:0,3)==0xAAC00000,"strip/fan original secondary provoking vertex");
        unsigned coverage=0;for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)coverage+=f.pixel(x,y)!=0xAA112233;require(coverage==16,"lit strip/fan square coverage");}
    std::cout<<"PASS varying secondary affine Gouraud/flat, perspective UV policy untouched, near clipping and strip/fan provoking attributes\n";
}
static void fragment_order(){
    {Fixture f;f.separate();f.r[0x58]=128;f.r[0x22]=1;f.r[0xDB]=0xFF0000|(128<<8)|2;f.r[0x23]=1;f.r[0xDE]=4;
        require(f.draw()&&f.written==10,"specular fragment alpha/depth: "+f.error);f.exact(0xAA800000);require(f.rt.memory().load16(0x04080000)==100,"lighting fragment depth");
        f.reset();f.r[0xDB]=0xFF0000|(129<<8)|2;require(f.draw()&&f.written==0,"alpha must ignore RGB specular");f.exact(0xAA112233);}
    {Fixture f;f.separate();f.r[0x58]=128;f.r[0x21]=1;f.r[0xDF]=0x32;require(f.draw()&&f.written==10,"specular before blending: "+f.error);f.exact(0xAA481119);}
    {Fixture f;f.separate();f.r[0xE8]=0xFF0000;require(f.draw(),"specular before write mask: "+f.error);f.exact(0xAA110000);}
    {Fixture f;f.separate();f.r[0xC9]=0x103;f.rt.memory().store32(0x08820000,0xFFFAFFFF);require(f.draw(),"secondary after REPLACE with RGB saturation: "+f.error);f.exact(0xAAFFFFFF);}
    {Fixture f;f.r[0x12]&=~0x60u;f.source();f.r[0x56]=f.r[0x90]=0;f.r[0x55]=128;f.r[0x5C]=0xFFFFFF;
        require(f.draw(),"ambient-only does not invent or require normal: "+f.error);f.exact(0xAA000080);}
    std::cout<<"PASS secondary before alpha/depth/blend/masks, RGB saturation and alpha preservation\n";
}
static void unsupported_atomic(){
    auto check=[](Fixture &f,const char *label){auto before=f.vram();require(!f.draw(),label);
        require(f.vram()==before&&f.end==0xDEADBEEF&&f.iend==0xBAADF00D&&f.written==0xABCD,std::string(label)+" changed VRAM/cursors/count");};
    {Fixture f;f.r[0x12]&=~0x60u;f.source();check(f,"absent required source normal");}
    {Fixture f;f.encode(2,0,{-1,1,0},{0,0,0});check(f,"late zero normal after valid first two");}
    {Fixture f;f.r[0x5F]=3;check(f,"unknown light enum");}
    {Fixture f;f.r[0x53]=8;check(f,"unknown material update");}
    {Fixture f;f.r[0x5E]=2;check(f,"unknown lighting color mode");}
    {Fixture f;f.r[0x5F]=0x200;f.vector(0x63,{0,0,2});f.vector(0x6F,{0,0,0});check(f,"uncertain zero spot direction");}
    {Fixture f;f.r[0x5F]=0x100;f.vector(0x7B,{0,0,0});check(f,"uncertain zero attenuation");}
    {Fixture f;f.regf(0x65,std::numeric_limits<float>::quiet_NaN());check(f,"active nonfinite light");}
    {Fixture f;f.r[0x1E]=1;f.r[0xC0]=2;check(f,"environment UV remains strict");}
    {Fixture f;f.r[0x1F]=1;f.r[0xCE]=0x7FC000;check(f,"nonfinite fog remains strict");}
    {Fixture f;f.separate();f.r[0xA0]=0xFFFFF0;f.r[0xA8]=0x090008;f.r[0xB8]=3;check(f,"invalid texture with lighting");}
    {Fixture f;f.r[0x12]|=0x1000;f.source();for(unsigned n=0;n<3;++n)f.rt.memory().store16(f.indices+n*2,n);f.encode(2,0,{-1,1,0},{0,0,0});auto before=f.vram();
        GeRuntime gpu(f.rt);gpu.commands=f.r;gpu.commands[4]=0x04123456;gpu.world=f.world;gpu.view=f.view;gpu.projection=f.projection;gpu.vertex_address=f.vertices;gpu.index_address=f.indices;
        f.rt.memory().store32(0x08809000,0x04030003);f.rt.cpu().gpr[4]=0x08809000;f.rt.cpu().gpr[6]=0xFFFFFFFF;f.rt.invoke_import("sceGe_user",0xAB49E76Au,f.rt.cpu());
        require(f.rt.stopped()&&gpu.completed.empty()&&gpu.commands[4]==0x04123456&&gpu.vertex_address==f.vertices&&gpu.index_address==f.indices&&f.vram()==before,"failed runtime lighting published draw or partial effects");}
    std::cout<<"PASS uncertain/unsupported and late-source failures atomic over full2MiB VRAM, dual cursors and runtime completion\n";
}
int main(){try{visible_light_types();transformed_normals_and_deformation();varying_secondary_and_clipping();fragment_order();unsupported_atomic();
    std::cout<<"PASS "<<assertions<<" lighting integration assertions; hardware intermediate precision remains limited\n";
}catch(const std::exception &failure){std::cerr<<"FAIL "<<failure.what()<<'\n';return 1;}}
