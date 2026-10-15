#include "../host/ge_sprites.hpp"
#include <iostream>
#include <stdexcept>
int main(){try{
    psprecomp::GuestMemory m;std::array<unsigned,256> r{};
    const unsigned tex=0x040E8000,fb=0x040D8000,vertices=0x08808000;
    std::vector<unsigned> original(512*512);
    for(unsigned y=0;y<512;++y)for(unsigned x=0;x<512;++x){
        const unsigned color=0xFF000000u|((x*9)&255)|(((y*7)&255)<<8)|(((x+y*3)&255)<<16);
        original[y*512+x]=color;m.store32(tex+(y*512+x)*4,color);
    }
    r[0x12]=0x800102;r[0x1E]=1;r[0xC3]=r[0xC9]=r[0xD2]=3;r[0xC6]=0x101;
    r[0x9C]=fb&0x1FFFF0;r[0x9D]=512;r[0x9E]=0x88000;r[0x9F]=512;
    r[0xA0]=tex&0xFFFFFF;r[0xA8]=0x040200;r[0xB8]=0x909;r[0xD5]=511|(319<<10);
    r[0x55]=0xFFFFFF;r[0x58]=255;
    for(unsigned i=0;i<30;++i){const unsigned pair=i/2,x=(pair+(i&1))*32,a=vertices+i*10;
        m.store16(a,x);m.store16(a+2,(i&1)?272:0);m.store16(a+4,x);m.store16(a+6,(i&1)?272:0);m.store16(a+8,0);
    }
    unsigned end=0xDEADBEEF;std::string error;
    if(!chili::render_ge_sprites(m,r,vertices,0x06001E,end,error) || end!=vertices+300)
        throw std::runtime_error("forward overlapping copy rejected: "+error);
    for(unsigned y=0;y<272;++y)for(unsigned x=0;x<480;++x)
        if(m.load32(fb+(y*512+x)*4)!=original[y*512+x])throw std::runtime_error("overlapping strip copy changed source pixel");
    // Reverse overlap reads texture rows already overwritten by earlier pixels.
    // Reject the complete batch before either framebuffer or depth changes.
    r[0x9C]=tex&0x1FFFF0;r[0xA0]=fb&0xFFFFFF;end=0xDEADBEEF;
    std::vector<unsigned char> before(m.vram_size()),after(m.vram_size());m.copy_out(0x04000000,before);
    if(chili::render_ge_sprites(m,r,vertices,0x06001E,end,error) || end!=0xDEADBEEF)
        throw std::runtime_error("dependent feedback silently rendered");
    m.copy_out(0x04000000,after);if(before!=after)throw std::runtime_error("dependent feedback partially changed VRAM");
    // Physical mirrors must participate in the same dependency map.
    chili::GeFeedbackGuard guard;guard.written(0x04000010,4);
    if(guard.readable(0x04200010,4) || !guard.readable(0x04000014,4))
        throw std::runtime_error("mirrored feedback byte alias mismatch");
    std::cout<<"PASS overlapping framebuffer copy: all130560 pixels match original; dependent feedback leaves VRAM/cursor unchanged\n";
    return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
