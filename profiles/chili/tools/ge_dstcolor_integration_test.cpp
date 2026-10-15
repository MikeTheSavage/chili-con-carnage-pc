#include "ge_runtime.hpp"
#define chili chili_baseline
#include "ge_blend_baseline/ge_sprites.hpp"
#include "ge_blend_baseline/ge_triangle_raster.hpp"
#undef chili
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
unsigned checks{};
void check(bool condition,const char *label){++checks;if(!condition)throw std::runtime_error(label);}
constexpr unsigned framebuffer=0x04000000,depthbuffer=0x04080000;
struct Vector {unsigned mode,destination,source,a,b,rgb;};
// Independently stated numeric facts from the recorded upstream PSP blend
// autotest. These validate RGB only, not its separate stencil-REPLACE operation.
constexpr Vector recorded[]{
    {0xA0,0x50404040,0x90808080,0,0,0x202020},
    {0xAA,0x44444444,0xEEDDCCBB,0xFFFFFF,0,0xDDCCBB},
    {0xAA,0x44444444,0xEEDDCCBB,0,0xFFFFFF,0x444444},
    {0xAA,0x50404040,0x90808080,0x808080,0,0x404040},
    {0xA2,0x50404040,0x90808080,0,0,0x484848},
    {0xAA,0xFFFFFFFF,0xFFFFFFFF,0xF0F0F0,0x0F0F0F,0xFFFFFF},
    {0xAA,0xFFFFFFFF,0xFFFFFFFF,0x0F0F0F,0xF0F0F0,0xFFFFFF},
    {0x1AA,0xFFFFFFFF,0xFFFFFFFF,0xF0F0F0,0x0F0F0F,0xE1E1E1},
    {0x1AA,0xFFFFFFFF,0xFFFFFFFF,0x0F0F0F,0xF0F0F0,0},
    {0x2AA,0xFFFFFFFF,0xFFFFFFFF,0xF0F0F0,0x0F0F0F,0},
    {0x2AA,0xFFFFFFFF,0xFFFFFFFF,0x0F0F0F,0xF0F0F0,0xE1E1E1},
};
struct Fixture {
    psprecomp::GuestMemory m;
    std::array<unsigned,256> r{};
    chili::GeClutCache clut;
    chili_baseline::GeClutCache original_clut;
    std::array<float,12> world{1,0,0,0,1,0,0,0,1,0,0,0},view=world;
    std::array<float,16> projection{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    unsigned v=0x08804000,texture=0x08810000,index=0x08818000;
    unsigned end=0xDEADBEEF,index_end=0xDEADF00D,written=0xFEEDF00D;
    std::string error;
    Fixture(){r[0x12]=0x80011E;r[0xD2]=3;r[0x9D]=8;r[0x9E]=0x80000;r[0x9F]=8;
        r[0x16]=r[0xD5]=3|(3<<10);r[0xD7]=65535;r[0xE7]=1;
        r[0xC3]=3;r[0xC9]=0x103;r[0xC7]=0x101;r[0xA0]=texture&0xFFFFFF;
        r[0xA8]=0x080004;r[0xB8]=0;
    }
    void vertex(unsigned i,unsigned x,unsigned y,unsigned color){const unsigned p=v+i*16;
        m.store16(p,0);m.store16(p+2,0);m.store32(p+4,color);
        m.store16(p+8,x);m.store16(p+10,y);m.store16(p+12,7);m.store16(p+14,0xDEAD);}
    void reset(unsigned destination,unsigned source,bool sprite){
        end=0xDEADBEEF;index_end=0xDEADF00D;written=0xFEEDF00D;error.clear();
        for(unsigned y=0;y<4;++y)for(unsigned x=0;x<8;++x){
            m.store32(framebuffer+(y*8+x)*4,destination);
            m.store16(depthbuffer+(y*8+x)*2,100);}
        r[0x1E]=sprite;
        vertex(0,0,0,source);vertex(1,2,sprite?2:0,source);
        vertex(2,2,2,source);vertex(3,0,2,source);
        m.store32(texture,source);
    }
    bool draw(bool sprite,bool baseline=false){
        if(sprite)return baseline?chili_baseline::render_ge_sprites(m,r,v,0x060002,end,error):
            chili::render_ge_sprites(m,r,v,0x060002,end,error);
        return baseline?chili_baseline::render_ge_triangle_raster(m,r,world,view,projection,v,0x050004,end,error,original_clut,&written,index,&index_end):
            chili::render_ge_triangle_raster(m,r,world,view,projection,v,0x050004,end,error,clut,&written,index,&index_end);
    }
    void pixels(unsigned destination,unsigned expected){
        for(unsigned y=0;y<4;++y)for(unsigned x=0;x<8;++x)
            check(m.load32(framebuffer+(y*8+x)*4)==(x<2&&y<2?expected:destination),"RGB/alpha/coverage mismatch");
    }
};
void recorded_pixels(){
    Fixture f;
    for(bool sprite:{false,true})for(const auto &v:recorded){
        f.r[0x21]=1;f.r[0xDF]=v.mode;f.r[0xE0]=v.a;f.r[0xE1]=v.b;
        f.r[0xE8]=0;f.r[0xE9]=0;f.reset(v.destination,v.source,sprite);
        check(f.draw(sprite),"recorded RGB draw failed");
        check(f.end==f.v+(sprite?32:64),"recorded draw vertex cursor");
        if(!sprite)check(f.written==4&&f.index_end==f.index,"recorded triangle write/index count");
        f.pixels(v.destination,(v.destination&0xFF000000)|v.rgb);
    }
    std::cout<<"Sprite and triangle: eleven recorded numeric RGB vectors each, destination stencil preservation PASS\n";
}
void masks_and_fragments(){
    Fixture f;
    // Original command branch 08820D20: DF0001AA / E0FFFFFF / E17F7F7F.
    // These inputs below are fixture data, not a captured in-game draw.
    for(bool sprite:{false,true})for(unsigned alpha_mask:{0u,0x55u,255u}){
        f.r[0x21]=1;f.r[0xDF]=0x1AA;f.r[0xE0]=0xFFFFFF;f.r[0xE1]=0x7F7F7F;
        f.r[0xE8]=0x0F55AA;f.r[0xE9]=alpha_mask;f.reset(0x5C102040,0x91FF807F,sprite);
        check(f.draw(sprite),"masked fixed subtraction draw");
        // Endpoint term subtraction: source FF807F minus destination*127.
        constexpr unsigned rgb=0xF7705F,mask=0x0F55AA,destination=0x5C102040;
        f.pixels(destination,0x5C000000|(rgb&~mask)|(destination&mask));
    }
    for(bool sprite:{false,true}){
        f.r[0x21]=1;f.r[0xDF]=0xA2;f.r[0xE0]=0;f.r[0xE1]=0xFFFFFF;
        f.r[0xE8]=f.r[0xE9]=0;f.r[0x23]=1;f.r[0xDE]=4;f.r[0xE7]=0;
        f.reset(0xAA01FF80,0xFF100101,sprite);
        check(f.draw(sprite),"A2 white destination/depth draw");f.pixels(0xAA01FF80,0xAA11FF81);
        for(unsigned y=0;y<4;++y)for(unsigned x=0;x<8;++x)
            check(f.m.load16(depthbuffer+(y*8+x)*2)==(x<2&&y<2?7:100),"depth write integration");
        f.r[0xDE]=6;f.reset(0xAA01FF80,0xFF100101,sprite);
        check(f.draw(sprite),"depth rejecting A2 draw");f.pixels(0xAA01FF80,0xAA01FF80);
        f.r[0x23]=0;f.r[0xE7]=1;f.r[0x22]=1;f.r[0xDB]=0xFF0000;
        f.reset(0xAA01FF80,0xFF100101,sprite);check(f.draw(sprite),"alpha rejecting A2 draw");
        f.pixels(0xAA01FF80,0xAA01FF80);f.r[0x22]=0;
    }
    std::cout<<"Original fixed-color command constants, RGB masks, PMSKA-independent alpha, depth/alpha tests PASS\n";
}
void legacy_equivalence(){
    Fixture f;
    for(bool sprite:{false,true})for(bool enabled:{false,true})for(unsigned mask:{0u,0xA50F55AAu})
        for(unsigned source:{0x01010101u,0x80FFFFFFu,0xEE123456u}){
            f.r[0x21]=enabled;f.r[0xDF]=enabled?0x32u:0x1000AAu;
            f.r[0xE0]=0x123456;f.r[0xE1]=0xFEDCBA;f.r[0xE8]=mask&0xFFFFFF;f.r[0xE9]=mask>>24;
            f.reset(0x79102030,source,sprite);check(f.draw(sprite,true),"baseline 32/OFF draw");
            const auto original=f.m.vram_bytes();const auto end=f.end,index_end=f.index_end,written=f.written;
            f.reset(0x79102030,source,sprite);check(f.draw(sprite),"prepared 32/OFF draw");
            check(f.m.vram_bytes()==original&&f.end==end&&f.index_end==index_end&&f.written==written,"32/OFF byte-for-byte baseline equivalence");
        }
    std::cout<<"Unchanged sprite/triangle 32 and OFF paths: whole-VRAM and cursor baseline comparison PASS\n";
}
void failures(){
    Fixture f;
    for(bool sprite:{false,true})for(unsigned mode:{0xBAu,0x3AAu,0x1000AAu,0x3A0u,0x4A0u,0x5A0u,0xB0u}){
        f.r[0x21]=1;f.r[0xDF]=mode;f.r[0xE0]=f.r[0xE1]=0xFFFFFF;
        f.r[0xE7]=0;f.r[0x23]=1;f.r[0xDE]=4;f.reset(0xAB010203,0xFF112233,sprite);
        const auto before=f.m.vram_bytes();
        check(!f.draw(sprite),"unknown blend accepted");
        check(f.m.vram_bytes()==before&&f.end==0xDEADBEEF&&f.index_end==0xDEADF00D&&f.written==0xFEEDF00D,"unknown blend preflight is atomic");
    }
    f.r[0xDF]=0xAA;
    for(bool sprite:{false,true})for(unsigned reg:{0x24u,0x27u}){
        f.r[reg]=1;f.reset(0xAB010203,0xFF112233,sprite);const auto before=f.m.vram_bytes();
        check(!f.draw(sprite)&&f.m.vram_bytes()==before&&f.end==0xDEADBEEF,"stencil/logic gate atomicity");f.r[reg]=0;
    }
    std::cout<<"Unknown/reserved equation, stencil and logic state rejected before color/depth/cursor mutation PASS\n";
}
void indexed_cursor(){
    Fixture f;f.r[0x12]|=0x800; // Documented index8 field.
    f.r[0x21]=1;f.r[0xDF]=0xAA;f.r[0xE0]=0xFFFFFF;f.r[0xE1]=0;
    f.reset(0xAA112233,0xFF010203,false);
    for(unsigned i=0;i<4;++i)f.m.store8(f.index+i,i);
    check(f.draw(false),"indexed fixed-color fan");
    check(f.end==f.v&&f.index_end==f.index+4&&f.written==4,"indexed new-mode success cursor accounting");
    f.pixels(0xAA112233,0xAA010203);
    f.r[0xDF]=0x3AA;f.reset(0xAA112233,0xFF010203,false);const auto before=f.m.vram_bytes();
    check(!f.draw(false)&&f.m.vram_bytes()==before&&f.end==0xDEADBEEF&&f.index_end==0xDEADF00D&&f.written==0xFEEDF00D,
        "indexed unknown-mode failure preserves both cursors");
    std::cout<<"Indexed new-mode success and rejected-draw VADDR/IADDR atomicity PASS\n";
}
unsigned independent_rgb(unsigned mode,unsigned s,unsigned d,unsigned fa,unsigned fb){
    unsigned out{};
    for(unsigned c=0;c<3;++c){const unsigned sh=c*8,sc=(s>>sh)&255,dc=(d>>sh)&255,sa=s>>24,da=d>>24;
        const double src[]{double(dc),255.0-dc,double(sa),255.0-sa,double(da),255.0-da,2.0*sa,std::max(255.0-2.0*sa,0.0),2.0*da,std::max(255.0-2.0*da,0.0),double((fa>>sh)&255)};
        const double dst[]{double(sc),255.0-sc,double(sa),255.0-sa,double(da),255.0-da,2.0*sa,std::max(255.0-2.0*sa,0.0),2.0*da,std::max(255.0-2.0*da,0.0),double((fb>>sh)&255)};
        const double a=std::floor((sc+0.5)*(src[mode&15]+0.5)/256.0),b=std::floor((dc+0.5)*(dst[(mode>>4)&15]+0.5)/256.0);
        const unsigned eq=mode>>8;out|=unsigned(std::clamp(eq==1?a-b:eq==2?b-a:a+b,0.0,255.0))<<sh;
    }return out;
}
void general_pixels(){
    Fixture f;
    for(bool sprite:{false,true})for(unsigned eq=0;eq<3;++eq)for(unsigned sf=0;sf<11;++sf)for(unsigned df=0;df<11;++df){
        const unsigned mode=(eq<<8)|(df<<4)|sf;if(mode==0x32)continue;
        constexpr unsigned source=0x7F19C7FF,destination=0x80D07F01,mask=0x0F55AA;
        f.r[0x21]=1;f.r[0xDF]=mode;f.r[0xE0]=0x20FF80;f.r[0xE1]=0x7F01FE;
        f.r[0xE8]=mask;f.r[0xE9]=(sf+df)%3==0?0:(sf+df)%3==1?0x55:255;
        f.reset(destination,source,sprite);check(f.draw(sprite),"general supported mode sprite/triangle rejected");
        const auto rgb=independent_rgb(mode,source,destination,f.r[0xE0],f.r[0xE1]);
        f.pixels(destination,0x80000000|(rgb&~mask)|(destination&mask));
        check(f.end==f.v+(sprite?32:64),"general mode vertex cursor mismatch");
        if(!sprite)check(f.written==4&&f.index_end==f.index,"general mode fragment/index accounting mismatch");
    }
    std::cout<<"Sprite and triangle: all362 admitted factor/equation payloads, asymmetric alpha/RGB/FIX/masks/cursors PASS\n";
}
void runtime_commands(){
    for(bool sprite:{false,true})for(unsigned mode:{0xAAu,0x1AAu,0x2AAu,0xA2u,0xA0u}){
        psprecomp::Runtime rt;chili::GeRuntime ge(rt);auto &m=rt.memory();
        const unsigned vertex=0x08804000,list=0x08808000,texture=0x08810000;
        auto &r=ge.commands;r[0x12]=0x80011E;r[0xD2]=3;r[0x9D]=8;
        r[0x16]=r[0xD5]=3|(3<<10);r[0xD7]=65535;r[0xE7]=1;
        r[0x1E]=sprite;r[0xC3]=3;r[0xC9]=0x103;r[0xC7]=0x101;
        r[0xA0]=texture&0xFFFFFF;r[0xA8]=0x080004;
        constexpr unsigned source=0xFF100101,destination=0xAA01FF80;
        const unsigned x[]{0,2,2,0},y[]{0,0,2,2};
        for(unsigned i=0;i<4;++i){const unsigned p=vertex+i*16;m.store16(p,0);m.store16(p+2,0);m.store32(p+4,source);
            m.store16(p+8,x[i]);m.store16(p+10,sprite&&i==1?2:y[i]);m.store16(p+12,7);}
        m.store32(texture,source);
        for(unsigned row=0;row<4;++row)for(unsigned col=0;col<8;++col)m.store32(framebuffer+(row*8+col)*4,destination);
        // The command payloads and decoder are real. Geometry/destination are
        // synthetic fixtures; no actual mission draw for these modes is claimed.
        const unsigned primitive=sprite?0x04060002u:0x04050004u;
        const unsigned words[]{0x10080000,0x01000000|(vertex&0xFFFFFF),0x21000001,
            0xDF000000|mode,0xE0FFFFFF,0xE1FFFFFF,primitive,0x0F000000,0x0C000000};
        for(unsigned i=0;i<std::size(words);++i)m.store32(list+i*4,words[i]);
        rt.cpu().gpr[4]=list;rt.cpu().gpr[6]=0xFFFFFFFF;
        rt.invoke_import("sceGe_user",0xAB49E76Au,rt.cpu());
        check(!rt.stopped()&&ge.completed.size()==1&&ge.vertex_address==vertex+(sprite?32:64),"runtime real command stream completion/cursor");
        check(ge.commands[0xDF]==(0xDF000000|mode)&&ge.commands[4]==primitive,"runtime blend/PRIM command state");
        const unsigned expected=mode==0xA0?0xAA01FF80:mode==0xAA||mode==0xA2?0xAA11FF81:mode==0x1AA?0xAA0F0000:0xAA00FE7F;
        check(m.load32(framebuffer)==expected,"runtime command RGB result");
        // A failed second PRIM cannot advance/completely acknowledge that draw.
        const auto before=m.vram_bytes();const unsigned old_v=ge.vertex_address,old_prim=ge.commands[4];
        m.store32(list,0xDF0003AA);m.store32(list+4,primitive);m.store32(list+8,0x0F000000);m.store32(list+12,0x0C000000);
        rt.cpu().gpr[4]=list;rt.invoke_import("sceGe_user",0xAB49E76Au,rt.cpu());
        check(rt.stopped()&&ge.completed.size()==1&&ge.vertex_address==old_v&&ge.commands[4]==old_prim&&m.vram_bytes()==before,"failed PRIM runtime cursor/framebuffer/completion atomicity");
    }
    std::cout<<"Runtime command decoding for AA/1AA/2AA/A2/A0 and failed PRIM cursor/completion atomicity PASS\n";
}
}
int main(){try{recorded_pixels();masks_and_fragments();legacy_equivalence();failures();indexed_cursor();general_pixels();runtime_commands();
    std::cout<<"GE fixed-color integration fixture PASS: "<<checks<<" assertions\n";return 0;}
catch(const std::exception &e){std::cerr<<"GE fixed-color fixture FAIL: "<<e.what()<<'\n';return 1;}}
