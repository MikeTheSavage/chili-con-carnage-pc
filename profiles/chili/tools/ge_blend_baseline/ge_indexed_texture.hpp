#pragma once
#include "psprecomp/guest_memory.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <span>

namespace chili {
// Native implementation from the GE format rules for game-driven indexed draws. Register encoding: PSPSDK sceGuClutLoad.c / sceGuClutMode.c.
// Cache and block layout reference: PPSSPP's documented GE/software behavior.
// Invalid guest reads and sampling bytes never loaded into the cache fail explicitly;
// hardware's undefined/invalid-memory behavior is not replaced with a guessed color.
class GeClutCache {
    std::array<std::uint8_t,1024> bytes_{};
    std::array<bool,1024> loaded_{};

    static unsigned entry(unsigned index,unsigned mode){
        const unsigned shifted=index>>((mode>>2)&31u);
        const unsigned selected=shifted&((mode>>8)&255u);
        const unsigned start=((mode>>16)&31u)*16u;
        // Raw CLUT memory holds 256 32-bit or 512 16-bit entries.
        return (selected|start)%((mode&3u)==3u?256u:512u);
    }
    bool present(unsigned index,unsigned mode,std::string &error) const {
        const unsigned size=(mode&3u)==3u?4u:2u,offset=entry(index,mode)*size;
        for(unsigned i=0;i<size;++i)if(!loaded_[offset+i]){
            error="CLUT sample refers to palette bytes not loaded";return false;
        }
        return true;
    }
    static unsigned expand(unsigned value,unsigned bits){
        return bits==5u?((value<<3)|(value>>2)):bits==6u?((value<<2)|(value>>4)):((value<<4)|value);
    }
public:
    bool load(const psprecomp::GuestMemory &memory,unsigned address_low,unsigned address_upper,
              unsigned command,std::string &error){
        // C4 uses six block-count bits, each block has 32 bytes, cache is 1 KiB.
        // Upper count bits are ignored; count 0 leaves every previous byte intact.
        const unsigned count=command&63u,length=std::min(count*32u,1024u);
        if(!length)return true;
        const unsigned address=(address_low&0x00FFFFF0u)|((address_upper<<8)&0x0F000000u);
        if(!memory.contains(address,length)){error="CLUT load outside guest memory";return false;}
        std::array<std::uint8_t,1024> staged{};
        memory.copy_out(address,std::span<std::uint8_t>(staged.data(),length));
        std::copy_n(staged.begin(),length,bytes_.begin());
        std::fill_n(loaded_.begin(),length,true);return true;
    }
    bool color(unsigned index,unsigned mode,unsigned &result,std::string &error) const {
        if(!present(index,mode,error))return false;
        const unsigned format=mode&3u,size=format==3u?4u:2u,offset=entry(index,mode)*size;
        unsigned raw=bytes_[offset]|(unsigned(bytes_[offset+1])<<8);
        if(format==3u){result=raw|(unsigned(bytes_[offset+2])<<16)|(unsigned(bytes_[offset+3])<<24);return true;}
        unsigned r,g,b,a;
        if(format==0u){r=expand(raw&31u,5);g=expand((raw>>5)&63u,6);b=expand((raw>>11)&31u,5);a=255;}
        else if(format==1u){r=expand(raw&31u,5);g=expand((raw>>5)&31u,5);b=expand((raw>>10)&31u,5);a=(raw&0x8000u)?255u:0u;}
        else{r=expand(raw&15u,4);g=expand((raw>>4)&15u,4);b=expand((raw>>8)&15u,4);a=expand((raw>>12)&15u,4);}
        result=r|(g<<8)|(b<<16)|(a<<24);return true;
    }
    bool ready(unsigned texture_format,unsigned mode,std::string &error) const {
        if(texture_format!=4u && texture_format!=5u){error="indexed texture format unsupported";return false;}
        // This is deliberately done before rasterization: no partially drawn result
        // can hide an unloaded palette entry behind a later sample failure.
        const unsigned count=texture_format==4u?16u:256u;
        for(unsigned i=0;i<count;++i)if(!present(i,mode,error))return false;
        return true;
    }
};

class GeIndexedTexture {
    unsigned address_{},byte_pitch_{},width_{},height_{},format_{},storage_bytes_{};
    bool swizzled_{},configured_{};
public:
    bool configure(const psprecomp::GuestMemory &memory,unsigned address,unsigned stride,
                   unsigned width,unsigned height,unsigned format,bool swizzled,std::string &error){
        auto fail=[&](const char *message){error=message;return false;};
        if(format!=4u && format!=5u)return fail("indexed texture format unsupported");
        if(!width || !height || width>1024u || height>1024u || (width&(width-1u)) || (height&(height-1u)) ||
           !stride || stride>2047u || stride<width)return fail("invalid indexed texture dimensions");
        if(format==4u && (stride&1u))return fail("odd CLUT4 stride unsupported");
        const unsigned pitch=format==4u?stride/2u:stride;
        if(swizzled && (pitch<16u || (pitch&15u)))return fail("swizzled indexed texture stride unsupported");
        const unsigned rows=swizzled?((height+7u)&~7u):height,bytes=pitch*rows;
        if(!memory.contains(address,bytes))return fail("indexed texture outside guest memory");
        address_=address;byte_pitch_=pitch;width_=width;height_=height;format_=format;
        swizzled_=swizzled;storage_bytes_=bytes;configured_=true;return true;
    }
    bool sample(const psprecomp::GuestMemory &memory,const GeClutCache &palette,unsigned mode,
                unsigned u,unsigned v,unsigned &result,std::string &error) const {
        if(!configured_ || u>=width_ || v>=height_){error="indexed texture coordinate outside prepared view";return false;}
        const unsigned byte_x=format_==4u?u/2u:u;
        const unsigned offset=swizzled_?(v/8u)*byte_pitch_*8u+(byte_x/16u)*128u+(v%8u)*16u+byte_x%16u:
                                          v*byte_pitch_+byte_x;
        const unsigned packed=memory.load8(address_+offset);
        const unsigned index=format_==4u?((packed>>((u&1u)*4u))&15u):packed;
        return palette.color(index,mode,result,error);
    }
    unsigned address() const{return address_;}
    unsigned storage_bytes() const{return storage_bytes_;}
};
}
