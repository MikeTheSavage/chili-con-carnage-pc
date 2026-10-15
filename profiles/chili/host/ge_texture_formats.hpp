#pragma once
#include "psprecomp/guest_memory.hpp"
#include <cstdint>
#include <string>

namespace chili {
// Independent decoding from PSP format encodings and recorded hardware tests:
// https://github.com/hrydgard/pspautotests/tree/master/tests/gpu/texcolors
// PSPSDK pspgu.h assigns direct formats 0..3 and DXT formats 8..10.
// PSP DXT blocks store color indices BEFORE endpoints, then alpha indices
// BEFORE alpha endpoints. DXT RGB has left-shifted 5/6-bit precision; direct
// 16-bit RGB instead replicates high bits. DXT3 alpha is nibble <<4 (max 240).
// DXT3/5 also retain the endpoint-order-dependent three-color/black mode.
class GeTextureFormatView {
    unsigned address_{},stride_{},width_{},height_{},format_{},byte_pitch_{},storage_bytes_{},block_bytes_{};
    bool swizzled_{},configured_{};
    static unsigned expand(unsigned value,unsigned bits){
        return bits==5?((value<<3)|(value>>2)):bits==6?((value<<2)|(value>>4)):value*17u;
    }
    static unsigned direct16(unsigned format,unsigned raw){
        unsigned red,green,blue,alpha;
        if(format==0){red=expand(raw&31,5);green=expand((raw>>5)&63,6);blue=expand((raw>>11)&31,5);alpha=255;}
        else if(format==1){red=expand(raw&31,5);green=expand((raw>>5)&31,5);blue=expand((raw>>10)&31,5);alpha=(raw>>15)?255:0;}
        else{red=expand(raw&15,4);green=expand((raw>>4)&15,4);blue=expand((raw>>8)&15,4);alpha=expand((raw>>12)&15,4);}
        return red|(green<<8)|(blue<<16)|(alpha<<24);
    }
    static unsigned dxt_color(unsigned first,unsigned second,unsigned index){
        if(first<=second && index==3)return 0;
        unsigned color=0;
        for(unsigned channel=0;channel<3;++channel){
            const unsigned shift=channel==0?11:channel==1?5:0,bits=channel==1?6:5;
            const unsigned mask=(1u<<bits)-1,a=((first>>shift)&mask)<<(8-bits),b=((second>>shift)&mask)<<(8-bits);
            unsigned value;
            if(index<2)value=index?b:a;
            else if(first>second)value=((index==2?2u:1u)*a+(index==2?1u:2u)*b)/3u;
            else value=(a+b)/2u;
            if(index==0)value=a;else if(index==1)value=b;
            color|=value<<(channel*8);
        }
        return color;
    }
    static unsigned dxt5_alpha(unsigned first,unsigned second,unsigned index){
        if(index<2)return index?second:first;
        if(first<=second && index>=6)return index==6?0u:255u;
        const unsigned denominator=first>second?7u:5u,weight=index-1u;
        // Two terms are quantized separately at 8.8 precision; the observed
        // hardware interpolation has a 31/256 offset before dropping fraction.
        const unsigned left=(first*(denominator-weight)*256u)/denominator;
        const unsigned right=(second*weight*256u)/denominator;
        return (left+right+31u)/256u;
    }
public:
    bool configure(const psprecomp::GuestMemory &memory,unsigned address,unsigned stride,
                   unsigned width,unsigned height,unsigned format,bool swizzled,std::string &error){
        auto fail=[&](const char *message){error=message;return false;};
        const bool compressed=format>=8 && format<=10;
        if(format>3 && !compressed)return fail("direct/compressed texture format unsupported");
        if(!width || !height || width>512 || height>512 || (width&(width-1)) || (height&(height-1)) ||
           !stride || stride>2047 || stride<width)return fail("invalid direct/compressed texture dimensions");
        unsigned pitch,rows,block=0;
        if(compressed){
            // The real-PSP fixtures use block-linear compressed data. The
            // swizzle bit's compressed behavior is not established here.
            if(swizzled)return fail("swizzled compressed texture requires hardware verification");
            if(stride<4 || (stride&3))return fail("compressed texture stride must hold complete four-texel blocks");
            block=format==8?8u:16u;pitch=(stride/4)*block;rows=(height+3)/4;
        }else{
            pitch=stride*(format==3?4u:2u);rows=swizzled?((height+7u)&~7u):height;
            if(swizzled && (pitch<16 || (pitch&15)))return fail("swizzled direct texture stride unsupported");
        }
        const unsigned bytes=pitch*rows;
        if(!memory.contains(address,bytes))return fail("direct/compressed texture storage outside guest memory");
        address_=address;stride_=stride;width_=width;height_=height;format_=format;byte_pitch_=pitch;
        storage_bytes_=bytes;block_bytes_=block;swizzled_=swizzled;configured_=true;return true;
    }
    // Only callers with a successfully prepared view and bounded coordinates
    // use this fast fetch. The checked sample below retains atomic outputs.
    unsigned fetch_prepared(const psprecomp::GuestMemory &memory,unsigned u,unsigned v) const {
        if(!block_bytes_){
            const unsigned bx=u*(format_==3?4u:2u);
            const unsigned offset=swizzled_?(v/8u)*byte_pitch_*8u+(bx/16u)*128u+(v%8u)*16u+bx%16u:v*byte_pitch_+bx;
            return format_==3?memory.aot_load32(address_+offset):direct16(format_,memory.aot_load16(address_+offset));
        }
        const unsigned block=address_+(v/4u)*byte_pitch_+(u/4u)*block_bytes_,x=u&3u,y=v&3u;
        const unsigned first=memory.aot_load16(block+4),second=memory.aot_load16(block+6);
        const unsigned index=(memory.aot_load8(block+y)>>(x*2u))&3u;
        unsigned color=dxt_color(first,second,index),alpha;
        if(format_==8)alpha=first<=second && index==3?0u:255u;
        else if(format_==9)alpha=((memory.aot_load16(block+8+y*2u)>>(x*4u))&15u)<<4;
        else{
            const std::uint64_t indices=std::uint64_t(memory.aot_load32(block+8))|(std::uint64_t(memory.aot_load16(block+12))<<32);
            const unsigned selected=unsigned((indices>>(3u*(y*4u+x)))&7u);
            alpha=dxt5_alpha(memory.aot_load8(block+14),memory.aot_load8(block+15),selected);
        }
        return color|(alpha<<24);
    }
    bool sample(const psprecomp::GuestMemory &memory,unsigned u,unsigned v,unsigned &color,std::string &error) const {
        if(!configured_ || u>=width_ || v>=height_){error="direct/compressed texture sample outside prepared view";return false;}
        color=fetch_prepared(memory,u,v);return true;
    }
    unsigned address() const{return address_;}
    unsigned byte_pitch() const{return byte_pitch_;}
    unsigned storage_bytes() const{return storage_bytes_;}
};
}
