#include "../host/ge_indexed_texture.hpp"
#include <iostream>
#include <stdexcept>
using chili::GeClutCache;
using chili::GeIndexedTexture;
static void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
static unsigned color(unsigned i){return 0x80000000u|i|(i*3u<<8)|(i*5u<<16);}
int main(){try{
    psprecomp::GuestMemory m;GeClutCache cache;std::string error;unsigned actual=0;
    const unsigned pal=0x08810000;
    for(unsigned i=0;i<256;++i)m.store32(pal+i*4,color(i));
    require(!cache.color(0,0xFF03,actual,error),"unloaded CLUT accepted");
    require(cache.load(m,pal&0xFFFFFF,(pal>>8)&0xF0000,32,error),"full CLUT load failed");
    require(cache.ready(5,0xFF03,error),"full CLUT not ready");
    for(unsigned i=0;i<256;++i)require(cache.color(i,0xFF03,actual,error)&&actual==color(i),"full CLUT color mismatch");
    m.store32(pal+17*4,0x12345678);
    require(cache.color(17,0xFF03,actual,error)&&actual==color(17),"CLUT reread guest memory after load");
    require(cache.load(m,pal&0xFFFFFF,(pal>>8)&0xF0000,2,error),"short reload failed");
    require(cache.color(17,0xFF03,actual,error)&&actual==color(17),"short reload destroyed tail");
    require(cache.load(m,pal&0xFFFFFF,(pal>>8)&0xF0000,32,error),"second full load failed");
    require(cache.color(17,0xFF03,actual,error)&&actual==0x12345678,"reload failed to observe memory change");
    require(cache.load(m,0,0,64,error),"masked zero-count load failed");
    require(cache.color(17,0xFF03,actual,error)&&actual==0x12345678,"zero-count load changed cache");
    require(!cache.load(m,0xFFFFF0,0x090000,32,error),"invalid CLUT range accepted");
    require(cache.color(17,0xFF03,actual,error)&&actual==0x12345678,"invalid load changed cache");
    // Index transformations must OR the 16-entry start field, not add it.
    require(cache.color(0x3F,3|(1<<2)|(0x3F<<8)|(1<<16),actual,error)&&actual==color(31),"CLUT shift/mask/start selection mismatch");
    require(cache.color(0,3|(0xFF<<8)|(16<<16),actual,error)&&actual==color(0),"32-bit CLUT wrap mismatch");
    require(cache.color(0x80000000u,3|(31u<<2)|(0xFFu<<8),actual,error)&&actual==color(1),"CLUT maximum shift mismatch");
    GeClutCache narrow;require(narrow.load(m,pal&0xFFFFFF,(pal>>8)&0xF0000,2,error),"16-entry palette load failed");
    require(narrow.ready(4,0xFF03,error)&&!narrow.ready(5,0xFF03,error),"palette preflight capacity mismatch");
    require(narrow.ready(5,0x0F03,error),"masked indexed preflight rejected loaded subset");
    // Reinterpret raw cached bytes using each C5 palette color format.
    const unsigned small=0x08812000;
    for(unsigned i=0;i<512;++i)m.store16(small+i*2,0);
    m.store16(small,0x001F);m.store16(small+2,0x07E0);m.store16(small+4,0xF800);m.store16(small+6,(12u<<11)|(27u<<5)|17u);
    require(cache.load(m,small&0xFFFFFF,(small>>8)&0xF0000,32,error),"16-bit raw cache load failed");
    require(cache.color(0,0xFF00,actual,error)&&actual==0xFF0000FF,"565 red decode failed");
    require(cache.color(1,0xFF00,actual,error)&&actual==0xFF00FF00,"565 green decode failed");
    require(cache.color(2,0xFF00,actual,error)&&actual==0xFFFF0000,"565 blue decode failed");
    require(cache.color(3,0xFF00,actual,error)&&actual==0xFF636D8C,"565 fractional channel expansion failed");
    m.store16(small,0xFC00);m.store16(small+2,0x001F);m.store16(small+4,0x8000|(9u<<10)|(5u<<5)|18u);m.store16(small+0x200,0x801F);
    require(cache.load(m,small&0xFFFFFF,(small>>8)&0xF0000,32,error),"5551 cache load failed");
    require(cache.color(0,0xFF01,actual,error)&&actual==0xFFFF0000,"5551 opaque blue decode failed");
    require(cache.color(1,0xFF01,actual,error)&&actual==0x000000FF,"5551 transparent red decode failed");
    require(cache.color(2,0xFF01,actual,error)&&actual==0xFF4A2994,"5551 fractional channel expansion failed");
    require(cache.color(0,((16u<<16)|0xFF01u),actual,error)&&actual==0xFF0000FF,"16-bit palette start high bit lost");
    m.store16(small,0xABCD);require(cache.load(m,small&0xFFFFFF,(small>>8)&0xF0000,1,error),"4444 load failed");
    require(cache.color(0,0xFF02,actual,error)&&actual==0xAABBCCDD,"4444 decode failed");
    // Restore 32-bit palette; later sampler fixtures use actual logical indices.
    for(unsigned i=0;i<256;++i)m.store32(pal+i*4,color(i));
    require(cache.load(m,pal&0xFFFFFF,(pal>>8)&0xF0000,63,error),"clamped CLUT load failed");
    const unsigned tex=0x08820000;
    for(unsigned format=4;format<=5;++format){
        const unsigned width=format==4?64u:32u,height=16u,bytes_per_row=32u;
        // Physically construct 2x2 tiles in sequential block order. T4 packs
        // adjacent logical indices into low/high nibbles independently.
        unsigned write=tex;
        for(unsigned by=0;by<2;++by)for(unsigned bx=0;bx<2;++bx)for(unsigned y=0;y<8;++y)for(unsigned x=0;x<16;++x){
            unsigned gx=bx*16+x,gy=by*8+y;
            const unsigned packed=format==4?((2*gx+gy)&15u)|(((2*gx+1+gy)&15u)<<4):((gx+gy*32u)&255u);
            m.store8(write++,packed);
        }
        GeIndexedTexture view;require(view.configure(m,tex,width,width,height,format,true,error),"swizzled indexed view failed");
        require(view.storage_bytes()==bytes_per_row*height,"indexed storage size mismatch");
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){const unsigned index=format==4?(x+y)&15u:(x+y*32u)&255u;
            require(view.sample(m,cache,0xFF03,x,y,actual,error)&&actual==color(index),"swizzled indexed tile/nibble mismatch");}
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<bytes_per_row;++x){
            const unsigned packed=format==4?((2*x+y)&15u)|(((2*x+1+y)&15u)<<4):((x+y*32u)&255u);m.store8(tex+y*bytes_per_row+x,packed);}
        require(view.configure(m,tex,width,width,height,format,false,error),"linear indexed view failed");
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){const unsigned index=format==4?(x+y)&15u:(x+y*32u)&255u;
            require(view.sample(m,cache,0xFF03,x,y,actual,error)&&actual==color(index),"linear indexed pitch/nibble mismatch");}
        require(!view.sample(m,cache,0xFF03,width,0,actual,error),"out-of-range indexed sample accepted");
    }
    GeIndexedTexture edge;
    for(unsigned format=4;format<=5;++format){const unsigned width=format==4?32u:16u;
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<16;++x)m.store8(0x09FFFF80+y*16+x,format==4?0x10:x);
        require(edge.configure(m,0x09FFFF80,width,width,1,format,true,error)&&edge.storage_bytes()==128,"short indexed block padding failed");
        require(edge.sample(m,cache,0xFF03,1,0,actual,error)&&actual==color(1),"short indexed block sample failed");
        require(!edge.configure(m,0x09FFFFF0,width,width,1,format,true,error),"truncated indexed block accepted");
    }
    require(!edge.configure(m,tex,34,32,8,4,true,error),"misaligned T4 swizzle stride accepted");
    require(!edge.configure(m,tex,17,16,8,5,true,error),"misaligned T8 swizzle stride accepted");
    require(!edge.configure(m,tex,33,32,8,4,false,error),"odd T4 stride accepted");
    GeClutCache isolated;require(!isolated.color(0,0xFF03,actual,error),"CLUT cache leaked across instances");
    std::cout<<"Native CLUT cache and indexed texture fixture tests passed\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
