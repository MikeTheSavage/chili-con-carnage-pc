#include "../host/ge_triangle_raster.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
int main(){try{
    psprecomp::GuestMemory m;std::array<unsigned,256> r{};
    std::array<float,12> matrix;matrix.fill(std::numeric_limits<float>::quiet_NaN());
    std::array<float,16> projection;projection.fill(std::numeric_limits<float>::quiet_NaN());
    const unsigned v=0x08808000,tex=0x08820000,pal=0x08810000,fb=0x040D8000;
    chili::GeClutCache cache;std::string error;for(unsigned i=0;i<16;++i)m.store32(pal+i*4,0xFFFFFFFF);
    if(!cache.load(m,pal&0xFFFFFF,(pal>>8)&0xF0000,2,error))throw std::runtime_error(error);
    m.zero(tex,128);r[0x12]=0x80011E;r[0x1E]=r[0x17]=r[0x18]=r[0x21]=1;
    r[0x9C]=fb&0x1FFFF0;r[0x9D]=512;r[0x9E]=0x88000;r[0x9F]=512;
    r[0xA0]=tex&0xFFFFFF;r[0xA8]=((tex>>8)&0xF0000)|32;r[0xB8]=0x303;
    r[0xC2]=1;r[0xC3]=4;r[0xC5]=0xFF03;r[0xC6]=0x101;r[0xC8]=0xE80000;
    r[0xC0]=1;r[0xC9]=0;r[0xD2]=3;r[0xD7]=65535;r[0xDF]=0x32;r[0x50]=1;
    r[0x16]=r[0xD5]=511|(319<<10);
    for(unsigned i=0x42;i<=0x4B;++i)r[i]=0x7FC000;
    for(unsigned y=94;y<=101;++y)for(unsigned x=245;x<=252;++x)m.store32(fb+(y*512+x)*4,0xAB000000);
    const unsigned u[]={0,0,8,8},tv[]={0,8,8,0},x[]={246,246,252,252},y[]={95,101,101,95};
    for(unsigned i=0;i<4;++i){const unsigned a=v+i*16;m.store16(a,u[i]);m.store16(a+2,tv[i]);
        m.store32(a+4,0x72FFFFFF);m.store16(a+8,x[i]);m.store16(a+10,y[i]);m.store16(a+12,65535);}
    unsigned end=0xDEADBEEF,written=0xDEADBEEF;
    if(!chili::render_ge_triangle_raster(m,r,matrix,matrix,projection,v,0x050004,end,error,cache,&written) || end!=v+64 || written!=36)
        throw std::runtime_error("captured through fan failed: "+error);
    for(unsigned sy=94;sy<=101;++sy)for(unsigned sx=245;sx<=252;++sx){
        const bool inside=sx>=246&&sx<252&&sy>=95&&sy<101;
        if(m.load32(fb+(sy*512+sx)*4)!=(inside?0xAB727272u:0xAB000000u))
            throw std::runtime_error("through fan coverage/blend/alpha preservation mismatch");
    }
    std::cout<<"PASS captured through fan:36 exact blended pixels,16-byte stride/unsignedZ, texture UV units; unused matrices/lights/UV generation ignored\n";
    return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
