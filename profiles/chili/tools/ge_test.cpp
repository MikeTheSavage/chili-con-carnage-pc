#include "../host/ge_runtime.hpp"
#include <stdexcept>

namespace {
chili::ThreadRuntime *irq_threads;unsigned irq_calls;
void finish_callback(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c){
    if(c.gpr[4]!=0x1234 || c.gpr[5]!=0x5678 || c.gpr[6]!=0x08809008 || c.gpr[28]!=0xABC || c.gpr[29]!=0x08813FC0 || irq_threads->can_wait())throw std::runtime_error("GE interrupt ABI/stack/guard failed");
    const auto back=c.gpr[31];c.gpr[4]=1;rt.invoke_import("ThreadManForUser",0xCEADEB47u,c);
    if(c.gpr[2]!=0x800201A7)throw std::runtime_error("GE interrupt permitted blocking delay");
    ++irq_calls;c.gpr[16]=0;c.fpr[2]=0;c.vfpu_ctrl[0]=0;c.pc=back;
}
void irq_done(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c){
    if(irq_calls!=1 || c.gpr[16]!=99 || c.fpr[2]!=123 || c.vfpu_ctrl[0]!=0xE4 || c.gpr[28]!=0xDEF || c.gpr[29]!=0x09FFF000 || c.gpr[2]!=1 || !irq_threads->can_wait())throw std::runtime_error("GE interrupt restoration failed");
    rt.stop("GE interrupt test completed");
}
void indexed_ge_test(){
    for(unsigned format=4;format<=5;++format){
        psprecomp::Runtime rt;chili::GeRuntime ge(rt);auto &m=rt.memory();
        const unsigned pal=0x08810000,tex=0x08820000,v=0x08808000,list=0x08809000;
        const unsigned width=format==4?64u:32u,height=16u,pitch=32u;
        auto color=[](unsigned i){return 0xFF000000u|i|(((i*3)&255)<<8)|(((i*5)&255)<<16);};
        auto index=[&](unsigned x,unsigned y){return format==4?(x+y*3)&15u:(x+y*32)&255u;};
        for(unsigned i=0;i<256;++i)m.store32(pal+i*4,color(i));
        // Exercise the actual runtime C4 path, including B0/B1 address assembly.
        m.store32(list,0xB0000000|(pal&0xFFFFFF));m.store32(list+4,0xB1000000|((pal>>8)&0xF0000));
        m.store32(list+8,0xC4000000|(format==4?2u:32u));m.store32(list+12,0x0F000000);m.store32(list+16,0x0C000000);
        rt.cpu().gpr[4]=list;rt.cpu().gpr[6]=0xFFFFFFFF;rt.invoke_import("sceGe_user",0xAB49E76Au,rt.cpu());
        if(ge.completed.size()!=1 || ge.commands[0xC4]!=(0xC4000000|(format==4?2u:32u)))
            throw std::runtime_error("GE CLUT load did not complete/state mismatch");
        // Change backing guest palette after C4. Sampling must retain cached data.
        m.store32(pal,0xDEADBEEF);
        auto &rg=ge.commands;rg[0x12]=0x800102;rg[0x1E]=1;rg[0xC3]=format;rg[0xC9]=0x103;rg[0xD2]=3;
        rg[0x55]=0xFFFFFF;rg[0x58]=255;rg[0x9D]=512;rg[0xA0]=tex&0xFFFFFF;rg[0xA8]=((tex>>8)&0xF0000)|width;
        rg[0xB8]=(format==4?6u:5u)|(4<<8);rg[0xD5]=(width-1)|((height-1)<<10);rg[0xC5]=0xFF03;rg[0xE7]=1;
        auto vertex=[&](unsigned i,unsigned u,unsigned tv,unsigned x,unsigned y){const unsigned a=v+i*10;
            m.store16(a,u);m.store16(a+2,tv);m.store16(a+4,x);m.store16(a+6,y);m.store16(a+8,0);};
        unsigned end{};std::string error;
        for(unsigned swizzle=0;swizzle<2;++swizzle){rg[0xC2]=swizzle;rg[0xC6]=0;rg[0xD5]=(width-1)|((height-1)<<10);
            unsigned write=tex;
            if(swizzle){
                for(unsigned by=0;by<2;++by)for(unsigned bx=0;bx<2;++bx)for(unsigned y=0;y<8;++y)for(unsigned x=0;x<16;++x){
                    const unsigned gx=bx*16+x,gy=by*8+y;
                    m.store8(write++,format==4?index(gx*2,gy)|(index(gx*2+1,gy)<<4):index(gx,gy));}
            }else{
                for(unsigned y=0;y<height;++y)for(unsigned x=0;x<pitch;++x)
                    m.store8(tex+y*pitch+x,format==4?index(x*2,y)|(index(x*2+1,y)<<4):index(x,y));}
            vertex(0,0,0,0,0);vertex(1,width,height,width,height);
            if(!chili::render_ge_sprites(m,rg,v,0x060002,end,error,&ge.clut) || end!=v+20)
                throw std::runtime_error("indexed sprite prepare failed: "+error);
            for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)
                if(m.load32(0x04000000+(y*512+x)*4)!=color(index(x,y)))throw std::runtime_error("indexed sprite cached palette/tile/nibble/pitch mismatch");
            // Bilinear fetch straddles both swizzle tile axes and blends decoded
            // colors, rather than blending palette indices or packed nibbles.
            const unsigned corner=format==4?31u:15u;vertex(0,corner,7,0,0);vertex(1,corner+2,9,1,1);rg[0xC6]=0x101;rg[0xD5]=0;
            const unsigned samples[]={color(index(corner,7)),color(index(corner+1,7)),color(index(corner,8)),color(index(corner+1,8))};
            unsigned expected=0;for(unsigned ch=0;ch<4;++ch){unsigned sum=0;for(unsigned sample:samples)sum+=(sample>>(ch*8))&255;expected|=((sum+2)/4)<<(ch*8);}
            if(!chili::render_ge_sprites(m,rg,v,0x060002,end,error,&ge.clut) || m.load32(0x04000000)!=expected)
                throw std::runtime_error("indexed sprite bilinear decoded-color tile corner mismatch");
        }
        // Runtime draw dispatch receives the same per-instance palette cache.
        ge.vertex_address=v;rg[0xC6]=0;m.store32(list,0x04060002);m.store32(list+4,0x0F000000);m.store32(list+8,0x0C000000);
        rt.cpu().gpr[4]=list;rt.invoke_import("sceGe_user",0xAB49E76Au,rt.cpu());
        if(ge.completed.size()!=2 || ge.vertex_address!=v+20)throw std::runtime_error("GE indexed draw cache integration failed");
        // A zero block count has no load effect even with unmapped B0/B1.
        m.store32(list,0xB0000000);m.store32(list+4,0xB1000000);m.store32(list+8,0xC4000040);m.store32(list+12,0x0F000000);m.store32(list+16,0x0C000000);
        rt.invoke_import("sceGe_user",0xAB49E76Au,rt.cpu());unsigned cached{};
        if(ge.completed.size()!=3 || !ge.clut.color(0,0xFF03,cached,error) || cached!=color(0))throw std::runtime_error("GE zero-count CLUT cache preservation failed");
        // Invalid C4 must preserve cache/command and must never finish its list.
        m.store32(list,0xB0FFFFF0);m.store32(list+4,0xB1090000);m.store32(list+8,0xC4000020);
        rt.invoke_import("sceGe_user",0xAB49E76Au,rt.cpu());
        if(ge.completed.size()!=3 || ge.commands[0xC4]!=0xC4000040 || !ge.clut.color(0,0xFF03,cached,error) || cached!=color(0))
            throw std::runtime_error("GE invalid CLUT load changed cache/state or completed");
        // Cold or insufficient cache fails before touching any pixel/cursor.
        chili::GeClutCache cold;rg[0xC6]=0;rg[0xD5]=0;m.store32(0x04000000,0x79685746);end=0x12345678;
        if(chili::render_ge_sprites(m,rg,v,0x060002,end,error,&cold) || end!=0x12345678 || m.load32(0x04000000)!=0x79685746)
            throw std::runtime_error("indexed sprite cold palette wrote pixels");
        if(format==4){rg[0xC3]=5;rg[0xA8]=((tex>>8)&0xF0000)|width;end=0x12345678;
            if(chili::render_ge_sprites(m,rg,v,0x060002,end,error,&ge.clut) || end!=0x12345678 || m.load32(0x04000000)!=0x79685746)
                throw std::runtime_error("indexed sprite partial palette preflight wrote pixels");
        }
    }
    std::cout<<"[test] GE CLUT command lifetime and T4/T8 sprite integration passed\n";
}
void clear_sprites_test(){
    psprecomp::Runtime rt;auto &m=rt.memory();std::array<unsigned,256> rg{};
    const unsigned v=0x08808000,fb=0x040E8000,db=0x04088000;
    auto vertex=[&](unsigned index,unsigned color,int x,int y,unsigned z){const unsigned a=v+index*12;
        m.store32(a,color);m.store16(a+4,x);m.store16(a+6,y);m.store16(a+8,z);m.store16(a+10,0xDEAD);};
    auto draw=[&](unsigned count,unsigned &end,std::string &error){
        return chili::render_ge_sprites(m,rg,v,0x060000|count,end,error);};
    rg[0x12]=0x80011C;rg[0xD2]=3;rg[0x9C]=0xE8000;rg[0x9D]=512;rg[0x9E]=0x88000;rg[0x9F]=512;
    rg[0x16]=rg[0xD5]=1|(1<<10);
    // Every ordinary pixel test rejects, blending is unsupported, texture storage
    // is invalid, and depth writes are masked. Clear must bypass those states.
    rg[0x1E]=rg[0x1F]=rg[0x21]=rg[0x22]=rg[0x23]=rg[0x24]=rg[0x27]=rg[0x28]=1;
    rg[0xDB]=0xFF0000;rg[0xDE]=0;rg[0xDF]=0x3AA;rg[0xE7]=1;
    rg[0xA0]=0xFFFFF0;rg[0xA8]=0x0F0000;rg[0xB8]=0x0F0F;rg[0xC3]=10;rg[0xC6]=0x707;
    vertex(0,0x01234567,0,0,0x1111);vertex(1,0xD4C3B2A1,2,2,0xABCD);
    unsigned end{};std::string error;
    for(unsigned flags=0;flags<8;++flags){
        rg[0xD3]=1|(flags<<8);rg[0xE8]=0x0F55AA;rg[0xE9]=0xA5;
        for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x){m.store32(fb+(y*512+x)*4,0x79685746);m.store16(db+(y*512+x)*2,0x2468);}
        const unsigned selected=((flags&1)?0x00FFFFFFu:0u)|((flags&2)?0xFF000000u:0u);
        const unsigned changes=selected&~0xA50F55AAu;
        const unsigned expected=(0x79685746u&~changes)|(0xD4C3B2A1u&changes);
        if(!draw(2,end,error) || end!=v+24)throw std::runtime_error("clear flag decode/stride failed: "+error);
        for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x)
            if(m.load32(fb+(y*512+x)*4)!=expected || m.load16(db+(y*512+x)*2)!=((flags&4)?0xABCD:0x2468))
                throw std::runtime_error("clear channel flag/mask/second vertex/bypass mismatch");
    }
    // Signed coordinates, reversed corners, inclusive scissor and REGION2 limit.
    rg[0xD3]=0x701;rg[0xE8]=rg[0xE9]=0;rg[0xD4]=1|(1<<10);rg[0xD5]=3|(3<<10);rg[0x16]=2|(2<<10);
    vertex(0,0x01234567,4,4,0x1111);vertex(1,0xD4C3B2A1,-1,-1,0xABCD);
    for(unsigned y=0;y<5;++y)for(unsigned x=0;x<5;++x){m.store32(fb+(y*512+x)*4,0x79685746);m.store16(db+(y*512+x)*2,0x2468);}
    if(!draw(2,end,error))throw std::runtime_error("clear clipped rectangle rejected: "+error);
    for(unsigned y=0;y<5;++y)for(unsigned x=0;x<5;++x){const bool inside=x>=1 && x<=2 && y>=1 && y<=2;
        if(m.load32(fb+(y*512+x)*4)!=(inside?0xD4C3B2A1:0x79685746) || m.load16(db+(y*512+x)*2)!=(inside?0xABCD:0x2468))
            throw std::runtime_error("clear scissor/region/reversed signed coverage mismatch");}
    rg[0xD4]=0;rg[0xD5]=rg[0x16]=1|(1<<10);vertex(0,0,0,0,0);vertex(1,0xFF000000,2,2,0);
    // Unselected or entirely masked buffers must not need valid memory or pitch.
    auto valid=rg;rg[0xD3]=0x401;rg[0x9D]=0;rg[0x9C]=0x1FFFF0;
    if(!draw(2,end,error) || m.load16(db)!=0)throw std::runtime_error("depth-only clear consulted color target: "+error);
    rg=valid;rg[0xD3]=0x301;rg[0x9F]=0;rg[0x9E]=0x1FFFF0;
    if(!draw(2,end,error) || m.load32(fb)!=0xFF000000)throw std::runtime_error("color-only clear consulted depth target: "+error);
    rg[0xD3]=0x701;rg[0x9C]=0x1FFFF0;rg[0x9D]=0;rg[0xE8]=0xFFFFFF;rg[0xE9]=255;rg[0x9E]=0x88000;rg[0x9F]=512;
    if(!draw(2,end,error))throw std::runtime_error("fully color-masked depth clear consulted color target: "+error);
    rg[0xD3]=1;rg[0x9F]=0;rg[0x9E]=0x1FFFF0;
    if(!draw(2,end,error))throw std::runtime_error("zero-flag clear consulted targets: "+error);
    // All failure checks must leave pixels, depth and vertex cursor untouched.
    auto rejected=[&](unsigned count,const char *what){m.store32(fb,0x79685746);m.store16(db,0x2468);end=0x12345678;
        if(draw(count,end,error) || end!=0x12345678 || m.load32(fb)!=0x79685746 || m.load16(db)!=0x2468)
            throw std::runtime_error(std::string("clear preflight changed memory: ")+what);};
    rg=valid;rg[0x9E]=0x1FFFF0;rejected(2,"wrapped depth target");
    rg=valid;rg[0x9C]=0x1FFFF0;rejected(2,"wrapped color target");
    rg=valid;rg[0x9F]=0;rejected(2,"zero depth stride");
    rg=valid;rg[0x9E]=rg[0x9C];rejected(2,"overlapping buffers");
    rg=valid;rejected(3,"odd sprite count");
    rg=valid;rg[0x12]=0x80011E;rejected(2,"unsupported clear vertex layout");
    rg=valid;rg[0x20]=1;rejected(2,"unsupported clear dithering");
    rg=valid;rg[0x15]=1;rejected(2,"unsupported region scaling");
    rg=valid;rg[0xD3]=0;rejected(2,"untextured ordinary draw remains unsupported");
    rg=valid;end=0x12345678;
    if(chili::render_ge_sprites(m,rg,0x09FFFFF0,0x060002,end,error) || end!=0x12345678 || m.load32(fb)!=0x79685746)
        throw std::runtime_error("clear truncated vertex preflight failed");
    // Exact game shape: eight 64-pixel strips clear the padded 512x320 target,
    // extending below the visible 272-line display while preserving row 320.
    rg=valid;rg[0x16]=rg[0xD5]=511|(319<<10);
    for(unsigned y=0;y<=320;++y)for(unsigned x=0;x<512;++x){m.store32(fb+(y*512+x)*4,0x79685746);m.store16(db+(y*512+x)*2,0x2468);}
    for(unsigned strip=0;strip<8;++strip){vertex(strip*2,0xFF000000,strip*64,0,0);vertex(strip*2+1,0xFF000000,(strip+1)*64,320,0);}
    if(!draw(16,end,error) || end!=v+192)throw std::runtime_error("game 512x320 clear rejected: "+error);
    for(unsigned y=0;y<=320;++y)for(unsigned x=0;x<512;++x)
        if(m.load32(fb+(y*512+x)*4)!=(y<320?0xFF000000:0x79685746) || m.load16(db+(y*512+x)*2)!=(y<320?0:0x2468))
            throw std::runtime_error("game 512x320 clear extent/depth mismatch");
    // Integration: only an effected clear can reach FINISH/END and advance VADDR.
    chili::GeRuntime ge(rt);ge.commands=rg;ge.vertex_address=v;const unsigned list=0x08809000;
    m.store32(list,0x04060010);m.store32(list+4,0xD3000000);m.store32(list+8,0x0F000000);m.store32(list+12,0x0C000000);
    rt.cpu().gpr[4]=list;rt.cpu().gpr[6]=0xFFFFFFFF;rt.invoke_import("sceGe_user",0xAB49E76Au,rt.cpu());
    if(ge.completed.size()!=1 || ge.vertex_address!=v+192 || ge.commands[0xD3]!=0xD3000000 || ge.commands[4]!=0x04060010)
        throw std::runtime_error("clear GE list completion/cursor progression failed");
    m.store32(list,0x04030003);rt.invoke_import("sceGe_user",0xAB49E76Au,rt.cpu());
    if(ge.completed.size()!=1 || ge.vertex_address!=v+192)throw std::runtime_error("unsupported draw completed after clear");
    std::cout<<"[test] clear sprites flags, masks, bypass, clipping, bounds and 512x320 list progression passed\n";
}
}
int main(){try{clear_sprites_test();indexed_ge_test();std::cout<<std::unitbuf;psprecomp::Runtime rt;chili::GeRuntime ge(rt);auto &c=rt.cpu();unsigned p=0x08804000;rt.memory().store32(p,0x08805000);rt.memory().store32(p+4,11);rt.memory().store32(p+8,0x08806000);rt.memory().store32(p+12,22);c.gpr[28]=99;
for(unsigned i=0;i<16;++i){c.gpr[4]=p;rt.invoke_import("sceGe_user",0xA4FC06A4u,c);if(c.gpr[2]!=i || ge.callbacks[i]->finish_arg!=22 || ge.callbacks[i]->gp!=99)throw std::runtime_error("GE callback record mismatch");}
rt.invoke_import("sceGe_user",0xA4FC06A4u,c);if(c.gpr[2]!=0x80000022)throw std::runtime_error("GE callback exhaustion failed");rt.invoke_import("sceGe_user",0xE47E40E4u,c);if(c.gpr[2]!=0x04000000)throw std::runtime_error("GE VRAM address mismatch");rt.memory().store32(c.gpr[2],0x12345678);if(rt.memory().load32(0x44000000)!=0x12345678)throw std::runtime_error("GE VRAM alias failed");
const unsigned list=p+0x100;
const unsigned words[]={0x3A000000,0x3B3F8000,0x3B400000,0x12000123,0x0F000000,0x0C000000};
for(unsigned i=0;i<6;++i)rt.memory().store32(list+i*4,words[i]);
c.gpr[4]=list;c.gpr[5]=0;c.gpr[6]=0xFFFFFFFF;c.gpr[7]=0;
rt.invoke_import("sceGe_user",0xAB49E76Au,c);const unsigned id=c.gpr[2];
if(!ge.completed.contains(id) || ge.world[0]!=1.0f || ge.world[1]!=2.0f || ge.world_cursor!=2 || ge.commands[0x12]!=0x12000123)throw std::runtime_error("GE state stream execution failed");
c.gpr[4]=id;c.gpr[5]=1;rt.invoke_import("sceGe_user",0x03444EB4u,c);if(c.gpr[2])throw std::runtime_error("GE completed list status failed");
c.gpr[5]=2;rt.invoke_import("sceGe_user",0x03444EB4u,c);if(c.gpr[2]!=0x80000107)throw std::runtime_error("GE sync mode validation failed");
ge.matrix(0x2A,95);ge.matrix(0x2B,0x3F8000);ge.matrix(0x2B,0x400000);
if(ge.bones[95]!=1.0f || ge.bones[0]!=0 || ge.bone_cursor!=97)throw std::runtime_error("GE reserved matrix cursor corrupted bones");
psprecomp::Runtime other;chili::GeRuntime isolated(other);
other.memory().store32(list,0x04030003);auto &oc=other.cpu();oc.gpr[4]=list;oc.gpr[6]=0xFFFFFFFF;
other.invoke_import("sceGe_user",0xAB49E76Au,oc);
if(!isolated.completed.empty())throw std::runtime_error("GE unsupported draw acknowledged completion");

psprecomp::Runtime pixels;std::array<unsigned,256> regs{};
regs[0x12]=0x800102;regs[0x1E]=1;regs[0xC3]=3;regs[0xC9]=3;regs[0xD2]=3;
regs[0x55]=0xFFFFFF;regs[0x58]=255;regs[0x9D]=512;regs[0xA0]=0x100000;regs[0xA8]=0x040200;regs[0xB8]=0x0101;
regs[0xD5]=1|(1<<10);regs[0xE7]=1;
unsigned vp=0x08808000;for(unsigned j=0;j<10;++j)pixels.memory().store16(vp+j*2,0);
pixels.memory().store16(vp+10,2);pixels.memory().store16(vp+12,2);
pixels.memory().store16(vp+14,2);pixels.memory().store16(vp+16,2);
for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x)pixels.memory().store32(0x04100000+(y*512+x)*4,0x00112200+y*2+x);
unsigned advanced{};std::string error;
if(!chili::render_ge_sprites(pixels.memory(),regs,vp,0x060002,advanced,error) || advanced!=vp+20)throw std::runtime_error("sprite decode failed: "+error);
for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x)if(pixels.memory().load32(0x04000000+(y*512+x)*4)!=(0xFF112200+y*2+x))throw std::runtime_error("sprite sampling mismatch");
regs[0x22]=1;regs[0xDB]=0xFF0000;pixels.memory().store32(0x04000000,0x12345678);
if(!chili::render_ge_sprites(pixels.memory(),regs,vp,0x060002,advanced,error) || pixels.memory().load32(0x04000000)!=0x12345678)throw std::runtime_error("sprite alpha rejection failed");
regs[0x22]=0;regs[0xE8]=0xFFFF;regs[0xD4]=1;pixels.memory().store32(0x04000004,0xAA556677);
if(!chili::render_ge_sprites(pixels.memory(),regs,vp,0x060002,advanced,error) || pixels.memory().load32(0x04000000)!=0x12345678 || pixels.memory().load32(0x04000004)!=0xFF116677)throw std::runtime_error("sprite scissor or mask failed");
regs[0x21]=1;regs[0xDF]=0x3AA;if(chili::render_ge_sprites(pixels.memory(),regs,vp,0x060002,advanced,error))throw std::runtime_error("unsupported sprite blending succeeded");

regs[0x21]=0;regs[0xE8]=0;regs[0xD4]=0;regs[0x9E]=0x80000;regs[0x9F]=512;regs[0xE7]=0;
pixels.memory().store16(vp+8,123);pixels.memory().store16(0x04080000,456);
regs[0x23]=1;regs[0xDE]=6;pixels.memory().store32(0x04000000,0xDEADBEEF);
if(!chili::render_ge_sprites(pixels.memory(),regs,vp,0x060002,advanced,error) || pixels.memory().load32(0x04000000)!=0xDEADBEEF || pixels.memory().load16(0x04080000)!=456)throw std::runtime_error("sprite depth rejection failed");
regs[0xDE]=4;
if(!chili::render_ge_sprites(pixels.memory(),regs,vp,0x060002,advanced,error) || pixels.memory().load16(0x04080000)!=123 || pixels.memory().load32(0x04000000)!=0xFF112200)throw std::runtime_error("sprite depth write failed");

psprecomp::Runtime jumping;chili::GeRuntime jump_ge(jumping);auto &jc=jumping.cpu();
const unsigned jumpwords[]={0x10000000,0x14000000,0x08000010,0x04030003,0x00000000,0x0F000000,0x0C000000};
for(unsigned j=0;j<7;++j)jumping.memory().store32(list+j*4,jumpwords[j]);
jc.gpr[4]=list;jc.gpr[6]=0xFFFFFFFF;jumping.invoke_import("sceGe_user",0xAB49E76Au,jc);
if(jump_ge.completed.empty() || jump_ge.offset_address!=list+4)throw std::runtime_error("GE relative jump failed to skip inline data");

{
psprecomp::Runtime interrupt_rt;chili::ThreadRuntime threads(interrupt_rt);threads.initialize_bootstrap();irq_threads=&threads;irq_calls=0;
chili::GeRuntime irq_ge(interrupt_rt,&threads,0x08810000);
interrupt_rt.register_function(0x08806000,finish_callback,"finish_test");interrupt_rt.register_function(0x08807000,irq_done,"irq_done");
auto &ic=interrupt_rt.cpu();interrupt_rt.memory().store32(p,0);interrupt_rt.memory().store32(p+4,0);interrupt_rt.memory().store32(p+8,0x08806000);interrupt_rt.memory().store32(p+12,0x5678);
ic.gpr[4]=p;ic.gpr[28]=0xABC;interrupt_rt.invoke_import("sceGe_user",0xA4FC06A4u,ic);
interrupt_rt.memory().store32(0x08809000,0x0F001234);interrupt_rt.memory().store32(0x08809004,0x0C000000);
ic.gpr[4]=0x08809000;ic.gpr[5]=0;ic.gpr[6]=0;ic.gpr[7]=0;ic.gpr[31]=0x08807000;
ic.gpr[16]=99;ic.fpr[2]=123;ic.vfpu_ctrl[0]=0xE4;ic.gpr[28]=0xDEF;ic.gpr[29]=0x09FFF000;
interrupt_rt.invoke_import("sceGe_user",0xAB49E76Au,ic);interrupt_rt.run(ic.pc,100);
if(irq_calls!=1 || irq_ge.interrupt_resume)throw std::runtime_error("GE finish callback failed to return");
}

{
psprecomp::Runtime callrt;chili::GeRuntime calls(callrt);auto &cc=callrt.cpu();const unsigned start=0x0880A000;
const unsigned code[]={0x10080000,0x13000000,0x0A80A040,0x01000123,0x0F000000,0x0C000000};
for(unsigned j=0;j<6;++j)callrt.memory().store32(start+j*4,code[j]);
callrt.memory().store32(start+0x40,0x14000000);callrt.memory().store32(start+0x44,0x10000000);
callrt.memory().store32(start+0x48,0x0A000020);callrt.memory().store32(start+0x4C,0x0B000000);
callrt.memory().store32(start+0x60,0x10090000);callrt.memory().store32(start+0x64,0x12000123);callrt.memory().store32(start+0x68,0x0B000000);
cc.gpr[4]=start;cc.gpr[6]=0xFFFFFFFF;callrt.invoke_import("sceGe_user",0xAB49E76Au,cc);
if(calls.completed.empty() || calls.offset_address || calls.vertex_address!=0x09000123 || calls.commands[0x12]!=0x12000123)throw std::runtime_error("GE nested CALL/RET state restoration failed");
}
{
psprecomp::Runtime bad;chili::GeRuntime ge_bad(bad);bad.memory().store32(list,0x0B000000);bad.cpu().gpr[4]=list;bad.cpu().gpr[6]=0xFFFFFFFF;bad.invoke_import("sceGe_user",0xAB49E76Au,bad.cpu());
if(!ge_bad.completed.empty())throw std::runtime_error("GE empty RET completed");
}
{
psprecomp::Runtime recursive;chili::GeRuntime ge_recursive(recursive);
recursive.memory().store32(list,0x10080000);recursive.memory().store32(list+4,0x0A000000|(list&0xFFFFFF));recursive.cpu().gpr[4]=list;recursive.cpu().gpr[6]=0xFFFFFFFF;
recursive.invoke_import("sceGe_user",0xAB49E76Au,recursive.cpu());if(!ge_recursive.completed.empty())throw std::runtime_error("GE stack overflow completed");
}

{
psprecomp::Runtime filtered;std::array<unsigned,256> rg{};rg[0x12]=0x800102;rg[0x1E]=1;rg[0xC3]=3;rg[0xC9]=0x100;rg[0xC6]=0x101;rg[0xD2]=3;
rg[0x55]=0x808080;rg[0x58]=255;rg[0x9D]=512;rg[0xA0]=0x100000;rg[0xA8]=0x040200;rg[0xB8]=0x0101;rg[0xE7]=1;
unsigned vtx=0x08805000;
for(unsigned j=0;j<10;++j)filtered.memory().store16(vtx+j*2,0);
filtered.memory().store16(vtx+10,2);filtered.memory().store16(vtx+12,2);filtered.memory().store16(vtx+14,1);filtered.memory().store16(vtx+16,1);
filtered.memory().store32(0x04100000,0xFF000000);filtered.memory().store32(0x04100004,0xFF0000FF);
filtered.memory().store32(0x04100800,0xFF00FF00);filtered.memory().store32(0x04100804,0xFFFF0000);
unsigned end{};std::string err;
if(!chili::render_ge_sprites(filtered.memory(),rg,vtx,0x060002,end,err) || filtered.memory().load32(0x04000000)!=0xFF202020)throw std::runtime_error("sprite bilinear/modulate pixel mismatch");
}

{
psprecomp::Runtime colored;std::array<unsigned,256> rg{};rg[0x12]=0x80011E;rg[0x1E]=1;rg[0xC3]=3;rg[0xC9]=0x100;rg[0xD2]=3;
rg[0x9D]=512;rg[0xA0]=0x100000;rg[0xA8]=0x040200;rg[0xE7]=1;
unsigned v=0x08808000;for(unsigned j=0;j<16;++j)colored.memory().store16(v+j*2,0);
colored.memory().store32(v+4,0x11223344);colored.memory().store32(v+20,0x80402010);
colored.memory().store16(v+16,1);colored.memory().store16(v+18,1);colored.memory().store16(v+24,1);colored.memory().store16(v+26,1);
colored.memory().store32(0x04100000,0x80FFFFFF);unsigned end{};std::string err;
if(!chili::render_ge_sprites(colored.memory(),rg,v,0x060002,end,err) || end!=v+32 || colored.memory().load32(0x04000000)!=0x40402010)throw std::runtime_error("colored sprite stride/second color/texture alpha mismatch");
rg[0x21]=1;rg[0xDF]=0x32;rg[0xE8]=0;rg[0xE9]=255;
colored.memory().store32(0x04000000,0xAB806040);
if(!chili::render_ge_sprites(colored.memory(),rg,v,0x060002,end,err) || colored.memory().load32(0x04000000)!=0xAB705034)throw std::runtime_error("sprite alpha blend/mask mismatch");
rg[0x21]=0;rg[0xE9]=0;
rg[0xC9]=3;
if(!chili::render_ge_sprites(colored.memory(),rg,v,0x060002,end,err) || colored.memory().load32(0x04000000)!=0x80FFFFFF)throw std::runtime_error("colored sprite REPLACE without texture alpha failed");
}
{
psprecomp::Runtime swizzle;auto &m=swizzle.memory();std::array<unsigned,256> rg{};
rg[0x12]=0x800102;rg[0x1E]=1;rg[0xC3]=3;rg[0xC9]=3;rg[0xD2]=3;
rg[0x55]=0xFFFFFF;rg[0x58]=255;rg[0x9D]=512;rg[0xA0]=0x100000;rg[0xA8]=0x090010;
rg[0xB8]=0x0404;rg[0xC2]=1;rg[0xD5]=15|(15<<10);rg[0xE7]=1;
const unsigned v=0x08808000,tex=0x09100000;
for(unsigned j=0;j<10;++j)m.store16(v+j*2,0);
m.store16(v+10,16);m.store16(v+12,16);m.store16(v+14,16);m.store16(v+16,16);
auto color=[](unsigned x,unsigned y){return 0xFF000000u|((x*10u)<<0)|((y*10u)<<8)|((x+y)<<16);};
// Build the fixture in physical block order, without calling the address mapping
// under test. Eight sequential 128-byte blocks span both tile axes.
unsigned write=tex;
for(unsigned by=0;by<2;++by)for(unsigned bx=0;bx<4;++bx)
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<4;++x){m.store32(write,color(bx*4+x,by*8+y));write+=4;}
unsigned end{};std::string err;
if(!chili::render_ge_sprites(m,rg,v,0x060002,end,err) || end!=v+20)throw std::runtime_error("swizzled sprite decode failed: "+err);
for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
    if(m.load32(0x04000000+(y*512+x)*4)!=color(x,y))throw std::runtime_error("swizzled unique texel/tile boundary mismatch");
// Bilinear taps straddle both a 4-texel block edge and an 8-row block edge.
rg[0xC6]=0x101;rg[0xD5]=0;m.store16(v,3);m.store16(v+2,7);m.store16(v+10,5);m.store16(v+12,9);m.store16(v+14,1);m.store16(v+16,1);
if(!chili::render_ge_sprites(m,rg,v,0x060002,end,err) || m.load32(0x04000000)!=0xFF0B4B23)throw std::runtime_error("swizzled bilinear tile corner mismatch");
// TEXSHADE bit 0 is unrelated to swizzling when TEXMODE bit 0 is clear.
rg[0xC1]=1;rg[0xC2]=0;rg[0xC6]=0;m.store16(v,0);m.store16(v+2,0);m.store16(v+10,16);m.store16(v+12,16);m.store16(v+14,16);m.store16(v+16,16);rg[0xD5]=15|(15<<10);
for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)m.store32(tex+(y*16+x)*4,color(x,y));
if(!chili::render_ge_sprites(m,rg,v,0x060002,end,err))throw std::runtime_error("TEXSHADE incorrectly rejected linear texture: "+err);
for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
    if(m.load32(0x04000000+(y*512+x)*4)!=color(x,y))throw std::runtime_error("linear texel layout changed");
// Heights below eight still reserve a complete block. Put it exactly at the
// 32 MiB RAM boundary so the bounds check must use the rounded storage height.
rg[0xC1]=0;rg[0xC2]=1;rg[0xA0]=0xFFFF80;rg[0xA8]=0x090004;
for(unsigned y=0;y<8;++y)for(unsigned x=0;x<4;++x)m.store32(0x09FFFF80+(y*4+x)*4,color(x,y));
for(unsigned exponent=0;exponent<3;++exponent){const unsigned height=1u<<exponent;
    rg[0xB8]=2|(exponent<<8);rg[0xD5]=3|((height-1)<<10);m.store16(v+10,4);m.store16(v+12,height);m.store16(v+14,4);m.store16(v+16,height);
    if(!chili::render_ge_sprites(m,rg,v,0x060002,end,err))throw std::runtime_error("swizzled short-height block rejected: "+err);
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<4;++x)
        if(m.load32(0x04000000+(y*512+x)*4)!=color(x,y))throw std::runtime_error("swizzled short-height sample mismatch");
}
rg[0xB8]=2;rg[0xD5]=3;m.store16(v+12,1);m.store16(v+16,1);rg[0xA0]=0xFFFFF0;m.store32(0x04000000,0xDEADBEEF);
if(chili::render_ge_sprites(m,rg,v,0x060002,end,err) || m.load32(0x04000000)!=0xDEADBEEF)throw std::runtime_error("swizzled truncated block wrote pixels");
rg[0xA0]=0xFFFF80;rg[0xA8]=0x090005;
if(chili::render_ge_sprites(m,rg,v,0x060002,end,err) || m.load32(0x04000000)!=0xDEADBEEF)throw std::runtime_error("swizzled misaligned block stride wrote pixels");
}
}catch(const std::exception &e){std::cerr<<e.what()<<"\n";return 1;}}
