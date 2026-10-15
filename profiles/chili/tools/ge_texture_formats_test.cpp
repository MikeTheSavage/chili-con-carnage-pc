#include "../host/ge_texture_formats.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include "ge_texture_formats_dxt5_vectors.inc"
static void require(bool r,const std::string &what){if(!r)throw std::runtime_error(what);}
struct Fixture{
    psprecomp::GuestMemory m;chili::GeTextureFormatView texture;std::string error;unsigned address=0x08810000;
    void color_block(unsigned at,unsigned first,unsigned second,unsigned selector){
        for(unsigned row=0;row<4;++row)m.store8(at+row,selector*0x55u);m.store16(at+4,first);m.store16(at+6,second);}
    void alpha3(unsigned at,unsigned nibble){for(unsigned row=0;row<4;++row)m.store16(at+8+row*2,nibble*0x1111u);}
    void alpha5(unsigned at,unsigned first,unsigned second,unsigned selector){
        std::uint64_t packed=0;for(unsigned pixel=0;pixel<16;++pixel)packed|=std::uint64_t(selector)<<(pixel*3);
        m.store32(at+8,unsigned(packed));m.store16(at+12,unsigned(packed>>32));m.store8(at+14,first);m.store8(at+15,second);}
    void prepare(unsigned format,unsigned stride=8,unsigned width=8,unsigned height=8,bool swizzle=false){
        require(texture.configure(m,address,stride,width,height,format,swizzle,error),error);}
    unsigned sample(unsigned x=0,unsigned y=0){unsigned color=0xDEADBEEF;require(texture.sample(m,x,y,color,error),error);return color;}
};
struct ColorVector{unsigned first,second,selector,color;};
static constexpr ColorVector dxt_colors[]={
    {0xFFFF,0xFFFF,0,0xF8FCF8},{0xFFFF,0xFFFF,1,0xF8FCF8},{0xFFFF,0xFFFF,2,0xF8FCF8},{0xFFFF,0xFFFF,3,0},
    {0xFFE3,0x8410,0,0x18FCF8},{0xFFE3,0x8410,1,0x808080},{0xFFE3,0x8410,2,0x3AD2D0},{0xFFE3,0x8410,3,0x5DA9A8},
    {0x8410,0xF85F,0,0x808080},{0x8410,0xF85F,1,0xF808F8},{0x8410,0xF85F,2,0xBC44BC},{0x8410,0xF85F,3,0},
    {0x7890,0x1234,2,0x8A2155},{0x7890,0x1234,3,0x953232},{0x7777,0x1356,2,0xB5C050},
};
static void recorded_hardware_colors(){
    Fixture f;
    for(unsigned format:{8u,9u,10u}){f.prepare(format);
        for(const auto &v:dxt_colors){f.color_block(f.address,v.first,v.second,v.selector);
            if(format==9)f.alpha3(f.address,5);else if(format==10)f.alpha5(f.address,0x55,0xFF,0);
            const unsigned alpha=format==8?(v.first<=v.second&&v.selector==3?0:255):format==9?80:85;
            require(f.sample()==(v.color|(alpha<<24)),"recorded PSP DXT color/alpha mismatch format="+std::to_string(format));}}
    f.prepare(9);f.color_block(f.address,0x7890,0x1234,2);f.alpha3(f.address,0);
    require(f.sample()==0x008A2155,"recorded DXT3 alpha zero mismatch");
    f.alpha3(f.address,15);require(f.sample()==0xF08A2155,"DXT3 incorrectly replicated alpha F to FF");
    f.prepare(10);f.color_block(f.address,0x7777,0x1356,2);
    const unsigned tl_alpha[]={225,89,205,186,166,147,127,108};
    for(unsigned selector=0;selector<8;++selector){f.alpha5(f.address,225,89,selector);
        require(f.sample()==(0xB5C050|(tl_alpha[selector]<<24)),"recorded DXT5 top-left alpha interpolation mismatch");}
    const unsigned lesser_alpha[]={85,255,119,153,187,221,0,255};
    for(unsigned selector=0;selector<8;++selector){f.alpha5(f.address,85,255,selector);
        require((f.sample()>>24)==lesser_alpha[selector],"recorded DXT5 lesser-alpha branch mismatch");}
    // Every observation from the 256 endpoint x 8 selector real-PSP sweep.
    f.color_block(f.address,0x7890,0x1234,2);
    for(unsigned endpoint=0;endpoint<256;++endpoint)for(unsigned selector=0;selector<8;++selector){
        f.alpha5(f.address,255,endpoint,selector);
        require(f.sample()==(0x8A2155|(dxt5_hardware_alpha[endpoint][selector]<<24)),
            "hardware DXT5 alpha sweep mismatch endpoint="+std::to_string(endpoint)+" selector="+std::to_string(selector));}
}
static void recorded_direct_formats(){
    Fixture f;const unsigned words[3][4]={{0xFFFF,0x000E,0x8410,0x2825},{0xFFFF,0x000E,0x4210,0x9425},{0xFFFF,0x000F,0x8888,0x0525}};
    const unsigned expected[3][4]={{0xFFFFFFFF,0xFF000073,0xFF848284,0xFF290429},{0xFFFFFFFF,0x00000073,0x00848484,0xFF290829},{0xFFFFFFFF,0x000000FF,0x88888888,0x00552255}};
    for(unsigned format=0;format<3;++format){f.prepare(format);
        for(unsigned i=0;i<4;++i){f.m.store16(f.address,words[format][i]);require(f.sample()==expected[format][i],"recorded direct16 PSP expansion mismatch");}}
}
static void block_pitch_and_indices(){
    Fixture f;
    const unsigned endpoints[]={0xF800,0x07E0,0x001F,0xFFFF},expected[]={0x0000F8,0x00FC00,0xF80000,0xF8FCF8};
    for(unsigned format:{8u,9u,10u}){const unsigned bytes=format==8?8:16;
        f.prepare(format,16,8,8);require(f.texture.storage_bytes()==2*4*bytes,"compressed storage used texel rows instead of block rows");
        for(unsigned by=0;by<2;++by)for(unsigned bx=0;bx<4;++bx){const unsigned id=bx<2?by*2+bx:0;
            const unsigned at=f.address+(by*4+bx)*bytes;f.color_block(at,endpoints[id],0,0);
            if(format==9)f.alpha3(at,id+1);if(format==10)f.alpha5(at,192+id,0,0);}
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){const unsigned id=(y/4)*2+x/4;
            const unsigned alpha=format==8?255:format==9?(id+1)*16:192+id;
            require(f.sample(x,y)==(expected[id]|(alpha<<24)),"compressed block X/Y axis or padded pitch addressing mismatch");}
        // Exercise every packed selector, both color and alpha, on all rows.
        f.color_block(f.address,0xF800,0x001F,0);std::uint64_t indices=0;
        for(unsigned y=0;y<4;++y){unsigned colors=0,alphas=0;for(unsigned x=0;x<4;++x){colors|=((x+y)&3)<<(x*2);alphas|=(y*4+x)<<(x*4);indices|=std::uint64_t((x+y*4)&7)<<(3*(x+y*4));}
            f.m.store8(f.address+y,colors);if(format==9)f.m.store16(f.address+8+y*2,alphas);}
        if(format==10){f.m.store32(f.address+8,unsigned(indices));f.m.store16(f.address+12,unsigned(indices>>32));f.m.store8(f.address+14,255);f.m.store8(f.address+15,0);}
        const unsigned palette[]={0x0000F8,0xF80000,0x5200A5,0xA50052};
        for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){const unsigned alpha=format==8?255:format==9?(y*4+x)*16:dxt5_hardware_alpha[0][(x+y*4)&7];
            require(f.sample(x,y)==(palette[(x+y)&3]|(alpha<<24)),"packed block row/color/alpha selector orientation mismatch");}
    }
    f.address=psprecomp::GuestMemory::kPhysicalBase+f.m.size()-16;f.prepare(9,4,1,1);
    f.color_block(f.address,0xF800,0,0);f.alpha3(f.address,15);
    require(f.texture.storage_bytes()==16&&f.sample()==0xF00000F8,"tiny compressed mip did not retain a full block");
}
static void swizzle_and_atomic_invalid(){
    Fixture f;const unsigned linear=0x08810000,swizzled=0x08820000;
    for(unsigned format=0;format<4;++format){const unsigned pixel_bytes=format==3?4:2,pitch=32*pixel_bytes;
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){const unsigned raw=0xA480u^(x+y*16),bx=x*pixel_bytes;
            const unsigned offset=(y/8)*pitch*8+(bx/16)*128+(y%8)*16+bx%16;
            if(format==3){f.m.store32(linear+y*pitch+bx,0xFF123000|raw);f.m.store32(swizzled+offset,0xFF123000|raw);}
            else{f.m.store16(linear+y*pitch+bx,raw);f.m.store16(swizzled+offset,raw);}}
        chili::GeTextureFormatView a,b;require(a.configure(f.m,linear,32,16,16,format,false,f.error)&&b.configure(f.m,swizzled,32,16,16,format,true,f.error),f.error);
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){unsigned av,bv;require(a.sample(f.m,x,y,av,f.error)&&b.sample(f.m,x,y,bv,f.error)&&av==bv,"direct swizzle across both tile axes mismatch");}}
    f.address=linear;f.prepare(9);f.color_block(linear,0xF800,0,0);f.alpha3(linear,15);const unsigned valid=f.sample();
    const auto before=f.m.vram_bytes();
    for(unsigned format:{4u,5u,6u,7u,11u})require(!f.texture.configure(f.m,linear,8,8,8,format,false,f.error)&&f.sample()==valid,"unsupported format changed prepared view");
    require(!f.texture.configure(f.m,linear,8,8,8,9,true,f.error)&&f.sample()==valid,"unverified compressed swizzle silently accepted");
    require(!f.texture.configure(f.m,linear,10,8,8,9,false,f.error)&&f.sample()==valid,"partial block stride accepted");
    require(!f.texture.configure(f.m,psprecomp::GuestMemory::kPhysicalBase+f.m.size()-16,4,4,8,9,false,f.error)&&f.sample()==valid,"missing compressed row changed prepared view");
    require(f.texture.configure(f.m,linear,8,8,8,0,true,f.error),"valid direct swizzle rejected"); // 16-byte pitch.
    require(!f.texture.configure(f.m,linear,4,4,4,0,true,f.error),"short direct swizzle tile accepted");
    require(f.m.vram_bytes()==before,"texture preflight mutated VRAM");
    unsigned sentinel=0xDEADBEEF;require(!f.texture.sample(f.m,99,0,sentinel,f.error)&&sentinel==0xDEADBEEF,"invalid sample changed output");
    chili::GeTextureFormatView cold;require(!cold.sample(f.m,0,0,sentinel,f.error)&&sentinel==0xDEADBEEF,"unprepared sample changed output");
}
int main(){try{recorded_hardware_colors();recorded_direct_formats();block_pitch_and_indices();swizzle_and_atomic_invalid();
    std::cout<<"PSP direct16/DXT recorded colors, 2048 alpha observations, block pitch/swizzle and atomic fixtures passed\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
