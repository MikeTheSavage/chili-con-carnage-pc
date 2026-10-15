#pragma once
#include "psprecomp/guest_memory.hpp"
#include "ge_indexed_texture.hpp"
#include "ge_feedback.hpp"
#include <stdexcept>
#include <array>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
namespace chili {
inline bool ge_compare(unsigned f,unsigned a,unsigned b){
    switch(f&7){case 0:return false;case 1:return true;case 2:return a==b;
    case 3:return a!=b;case 4:return a<b;case 5:return a<=b;case 6:return a>b;default:return a>=b;}
}
// PSPSDK sceGuClear emits COLOR_8888 | VERTEX_16BIT | TRANSFORM_2D
// sprites with 12-byte vertices. CLEAR_MODE bits 8/9/10 select RGB/stencil/Z.
// https://github.com/pspdev/pspsdk/blob/master/src/gu/sceGuClear.c
// Alpha is the framebuffer stencil channel. Clear still honors PMSKC/PMSKA,
// but bypasses texturing, fragment tests, blending, logic ops and ZMSK.
// Protocol behavior: PPSSPP GPU/Software/DrawPixel.cpp and FuncId.cpp.
inline bool render_ge_clear_sprites(psprecomp::GuestMemory &m,const std::array<unsigned,256> &regs,
                                   unsigned vertices,unsigned data,unsigned &advanced,std::string &error){
    auto r=[&](unsigned i){return regs[i]&0xFFFFFFu;};
    auto fail=[&](const char *s){error=s;return false;};
    const unsigned count=data&0xFFFFu,vertex_stride=12;
    if(((data>>16)&7)!=6 || r(0x12)!=0x80011C || (count&1))
        return fail("clear sprite vertex format/primitive unsupported");
    if((r(0xD2)&3)!=3 || (r(0x20)&1) || r(0x15))
        return fail("clear sprite framebuffer/dither/region-rate state unsupported");
    if(!m.contains(vertices,count*vertex_stride))return fail("clear sprite vertices outside guest memory");
    const unsigned flags=(r(0xD3)>>8)&7;
    const unsigned preserve=r(0xE8)|((r(0xE9)&255)<<24);
    const unsigned selected=((flags&1)?0x00FFFFFFu:0u)|((flags&2)?0xFF000000u:0u);
    const unsigned write_bits=selected&~preserve;
    const bool depth_clear=flags&4;
    // REGION1 scales drawing coordinates rather than providing a top-left clip.
    // REGION2 limits the inclusive scissor bottom-right even in through mode.
    const int sx0=r(0xD4)&1023,sy0=(r(0xD4)>>10)&1023;
    const int sx1=std::min(r(0xD5)&1023,r(0x16)&1023);
    const int sy1=std::min((r(0xD5)>>10)&1023,(r(0x16)>>10)&1023);
    const unsigned stride=r(0x9D)&0x7FC,ds=r(0x9F)&0x7FC;
    const unsigned fb=0x04000000|(r(0x9C)&0x1FFFF0),db=0x04000000|(r(0x9E)&0x1FFFF0);
    const bool visible=sx0<=sx1 && sy0<=sy1;
    const unsigned color_bytes=visible?((sy1*stride+sx1+1)*4u):0u;
    const unsigned depth_bytes=visible?((sy1*ds+sx1+1)*2u):0u;
    // Validate every participating target before the first write. Unselected
    // targets and stale texture state must not require valid guest storage.
    if(visible && ((write_bits && (!stride || sx1>=int(stride))) ||
                   (depth_clear && (!ds || sx1>=int(ds)))))return fail("invalid clear sprite buffer dimensions");
    if(visible && ((write_bits && !m.contains(fb,color_bytes)) ||
                   (depth_clear && !m.contains(db,depth_bytes))))return fail("clear sprite buffer outside guest memory");
    // GuestMemory exposes mirrored EDRAM, but overlapping clear targets require
    // physical-range reasoning. Keep wrapped target layouts strict until that
    // ordering is implemented; the game's 512x320 targets are contiguous.
    if(visible && ((write_bits && fb+color_bytes>0x04000000u+m.vram_size()) ||
                   (depth_clear && db+depth_bytes>0x04000000u+m.vram_size())))
        return fail("clear sprite wrapping buffers unsupported");
    if(visible && write_bits && depth_clear && fb<db+depth_bytes && db<fb+color_bytes)
        return fail("clear sprite overlapping color/depth buffers unsupported");
    struct V{unsigned color,z;int x,y;};std::vector<V> vs;vs.reserve(count);
    for(unsigned i=0;i<count;++i){const unsigned a=vertices+i*vertex_stride;
        vs.push_back({m.load32(a),m.load16(a+8),static_cast<short>(m.load16(a+4)),
                      static_cast<short>(m.load16(a+6))});}
    for(unsigned i=0;i<count;i+=2){const V &a=vs[i],&b=vs[i+1];
        const int x0=std::max(std::min(a.x,b.x),sx0),x1=std::min(std::max(a.x,b.x)-1,sx1);
        const int y0=std::max(std::min(a.y,b.y),sy0),y1=std::min(std::max(a.y,b.y)-1,sy1);
        // Sprite attributes are flat from the second vertex, regardless of
        // endpoint direction; upper/right edges are excluded from coverage.
        for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x){
            if(depth_clear)m.store16(db+(y*ds+x)*2u,b.z);
            if(write_bits){const unsigned address=fb+(y*stride+x)*4u;
                m.store32(address,(m.load32(address)&~write_bits)|(b.color&write_bits));}
        }
    }
    advanced=vertices+count*vertex_stride;return true;
}
// Initial native software path; unsupported state fails before any pixel write.
inline bool render_ge_sprites(psprecomp::GuestMemory &m,const std::array<unsigned,256> &regs,
                             unsigned vertices,unsigned data,unsigned &advanced,std::string &error,const GeClutCache *palette=nullptr){
    auto r=[&](unsigned i){return regs[i]&0xFFFFFFu;};
    auto fail=[&](const char *s){error=s;return false;};
    if(r(0xD3)&1)return render_ge_clear_sprites(m,regs,vertices,data,advanced,error);
    const unsigned count=data&0xFFFFu;
    const bool vertex_colors=r(0x12)==0x80011E;
    const unsigned vertex_stride=vertex_colors?16u:10u,position_offset=vertex_colors?8u:4u;
    if(((data>>16)&7)!=6 || (r(0x12)!=0x800102 && !vertex_colors) || (count&1))return fail("sprite vertex format/primitive unsupported");
    const unsigned texture_format=r(0xC3);
    const bool indexed=texture_format==4 || texture_format==5;
    if(!(r(0x1E)&1) || (texture_format!=3 && !indexed) || ((r(0xC6)&7)>1) || (((r(0xC6)>>8)&7)>1) ||
       ((r(0xC9)&7)!=3 && (r(0xC9)&7)!=0) || (r(0xC9)&0x10000) || (r(0xD2)&3)!=3)return fail("sprite texture/framebuffer state unsupported");
    if((r(0x24)|r(0x25)|r(0x28))&1)return fail("sprite stencil/fog/color test unsupported");
    if((r(0x21)&1) && r(0xDF)!=0x32)return fail("sprite blend equation unsupported");
    const unsigned stride=r(0x9D)&0x7FC,ds=r(0x9F)&0x7FC,ts=r(0xA8)&0x7FF;
    const unsigned fb=0x04000000|(r(0x9C)&0x1FFFF0),db=0x04000000|(r(0x9E)&0x1FFFF0);
    const unsigned tex=(r(0xA0)&0xFFFFF0)|((r(0xA8)<<8)&0x0F000000);
    const unsigned tw=1u<<(r(0xB8)&15),th=1u<<((r(0xB8)>>8)&15);
    // TEXMODE (0xC2), not TEXSHADE (0xC1), carries the swizzle bit.
    // PSP texture blocks are 16 bytes wide and 8 rows high. RGBA8888 therefore
    // stores four adjacent texels per block row. See PSPSDK sceGuTexMode.c and
    // https://www.ppsspp.org/docs/development/psp-internals/ge-performance/ .
    const bool swizzled=r(0xC2)&1;
    const unsigned texture_pitch=indexed?(texture_format==4?ts/2u:ts):ts*4u;
    const unsigned texture_height=swizzled?((th+7u)&~7u):th;
    if(!indexed && swizzled && (texture_pitch<16u || (texture_pitch&15u)))return fail("swizzled RGBA8888 texture stride unsupported");
    GeIndexedTexture indexed_texture;
    if(indexed){
        if(!palette)return fail("indexed sprite palette cache unavailable");
        if(!indexed_texture.configure(m,tex,ts,tw,th,texture_format,swizzled,error) ||
           !palette->ready(texture_format,r(0xC5),error))return false;
    }
    const unsigned texture_bytes=indexed?indexed_texture.storage_bytes():texture_pitch*texture_height;
    const int sx0=r(0xD4)&1023,sy0=(r(0xD4)>>10)&1023,sx1=r(0xD5)&1023,sy1=(r(0xD5)>>10)&1023;
    if(!stride || !ts || tw>1024 || th>1024 || ts<tw || sx1>=int(stride) || (ds && sx1>=int(ds)))return fail("invalid sprite buffer dimensions");
    if(!m.contains(vertices,count*vertex_stride) || !m.contains(fb,(sy1+1)*stride*4u) ||
       !m.contains(tex,texture_bytes) || (ds && !m.contains(db,(sy1+1)*ds*2u)))return fail("sprite buffer outside guest memory");
    const bool feedback=GeFeedbackGuard::overlaps(tex,texture_bytes,fb,(sy1+1)*stride*4u) ||
        (ds && !(r(0xE7)&1) && GeFeedbackGuard::overlaps(tex,texture_bytes,db,(sy1+1)*ds*2u));
    struct V{unsigned u,v,z,color;int x,y;};std::vector<V> vs;vs.reserve(count);
    for(unsigned i=0;i<count;++i){unsigned a=vertices+i*vertex_stride;
        vs.push_back({m.load16(a),m.load16(a+2),m.load16(a+position_offset+4),
                      vertex_colors?m.load32(a+4):r(0x55)|((r(0x58)&255)<<24),
                      static_cast<short>(m.load16(a+position_offset)),static_cast<short>(m.load16(a+position_offset+2))});}
    const unsigned mask=r(0xE8)|((r(0xE9)&255)<<24),alpha=r(0xDB);
    auto texture_address=[&](int su,int sv){
        su=(r(0xC7)&1)?std::clamp(su,0,int(tw)-1):(su&int(tw-1));
        sv=(r(0xC7)&0x100)?std::clamp(sv,0,int(th)-1):(sv&int(th-1));
        const unsigned byte_x=indexed?(texture_format==4?unsigned(su)/2u:unsigned(su)):unsigned(su)*4u;
        const unsigned row=unsigned(sv);
        const unsigned offset=swizzled?(row/8u)*texture_pitch*8u+(byte_x/16u)*128u+(row%8u)*16u+byte_x%16u:
                                         row*texture_pitch+byte_x;
        return tex+offset;
    };
    if(feedback){
        GeFeedbackGuard guard;
        const unsigned sample_bytes=indexed?1u:4u;
        // Trace the exact native traversal before any framebuffer/depth write.
        // Conservative marks include fragments that a later test may reject.
        for(unsigned i=0;i<count;i+=2){const V &a=vs[i],&b=vs[i+1];
            const int x0=std::max(std::min(a.x,b.x),sx0),x1=std::min(std::max(a.x,b.x)-1,sx1);
            const int y0=std::max(std::min(a.y,b.y),sy0),y1=std::min(std::max(a.y,b.y)-1,sy1);
            if(a.x==b.x || a.y==b.y)continue;
            const bool minify=std::abs(double(int(b.u)-int(a.u))/(b.x-a.x))>1 ||
                              std::abs(double(int(b.v)-int(a.v))/(b.y-a.y))>1;
            const bool linear=((r(0xC6)>>(minify?0:8))&7)==1;
            for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x){
                const double u=a.u+(x+0.5-a.x)*(int(b.u)-int(a.u))/double(b.x-a.x);
                const double v=a.v+(y+0.5-a.y)*(int(b.v)-int(a.v))/double(b.y-a.y);
                const int iu=int(std::floor(u-(linear?0.5:0.0))),iv=int(std::floor(v-(linear?0.5:0.0)));
                if(!guard.readable(texture_address(iu,iv),sample_bytes) ||
                   (linear && (!guard.readable(texture_address(iu+1,iv),sample_bytes) ||
                               !guard.readable(texture_address(iu,iv+1),sample_bytes) ||
                               !guard.readable(texture_address(iu+1,iv+1),sample_bytes))))
                    return fail("sprite feedback has a read-after-write dependency");
                if(mask!=0xFFFFFFFFu)guard.written(fb+(y*stride+x)*4u,4u);
                if(ds && !(r(0xE7)&1))guard.written(db+(y*ds+x)*2u,2u);
            }
        }
    }
    for(unsigned i=0;i<count;i+=2){const V &a=vs[i],&b=vs[i+1];
        int x0=std::max(std::min(a.x,b.x),sx0),x1=std::min(std::max(a.x,b.x)-1,sx1);
        int y0=std::max(std::min(a.y,b.y),sy0),y1=std::min(std::max(a.y,b.y)-1,sy1);
        if(a.x==b.x || a.y==b.y)continue;
        for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x){
            const double u=a.u+(x+0.5-a.x)*(int(b.u)-int(a.u))/double(b.x-a.x);
            const double v=a.v+(y+0.5-a.y)*(int(b.v)-int(a.v))/double(b.y-a.y);
            auto sample=[&](int su,int sv){
                su=(r(0xC7)&1)?std::clamp(su,0,int(tw)-1):(su&int(tw-1));
                sv=(r(0xC7)&0x100)?std::clamp(sv,0,int(th)-1):(sv&int(th-1));
                if(indexed){unsigned color{};
                    // configure + ready preflight all storage and reachable palette
                    // entries before pixel writes; clamp/wrap bounds each sample.
                    if(!indexed_texture.sample(m,*palette,r(0xC5),su,sv,color,error))
                        throw std::logic_error("indexed sprite preflight invariant: "+error);
                    return color;
                }
                return m.load32(texture_address(su,sv));
            };
            const bool minify=std::abs(double(int(b.u)-int(a.u))/(b.x-a.x))>1 ||
                              std::abs(double(int(b.v)-int(a.v))/(b.y-a.y))>1;
            const bool linear=((r(0xC6)>>(minify?0:8))&7)==1;
            unsigned color{};
            if(linear){
                const double pu=u-0.5,pv=v-0.5;
                const int iu=int(std::floor(pu)),iv=int(std::floor(pv));
                const double fu=pu-iu,fv=pv-iv;
                const unsigned samples[]={sample(iu,iv),sample(iu+1,iv),sample(iu,iv+1),sample(iu+1,iv+1)};
                for(unsigned channel=0;channel<4;++channel){const unsigned shift=channel*8;
                    const double top=((samples[0]>>shift)&255)*(1-fu)+((samples[1]>>shift)&255)*fu;
                    const double bottom=((samples[2]>>shift)&255)*(1-fu)+((samples[3]>>shift)&255)*fu;
                    color|=unsigned(std::clamp(std::lround(top*(1-fv)+bottom*fv),0l,255l))<<shift;
                }
            }else color=sample(int(std::floor(u)),int(std::floor(v)));
            const unsigned ambient=b.color;
            if((r(0xC9)&7)==0){
                unsigned modulated=0;
                for(unsigned ch=0;ch<4;++ch){const unsigned shift=ch*8;
                    modulated|=((((color>>shift)&255)*((ambient>>shift)&255)+127)/255)<<shift;
                }
                color=modulated;
            }
            if(!(r(0xC9)&0x100))color=(color&0xFFFFFF)|(ambient&0xFF000000);
            if((r(0x22)&1) && !ge_compare(alpha,color>>24&alpha>>16,alpha>>8&alpha>>16&255))continue;
            const unsigned depth=db+(y*ds+x)*2u;
            if(ds && (r(0x23)&1) && !ge_compare(r(0xDE),a.z,m.load16(depth)))continue;
            if(ds && !(r(0xE7)&1))m.store16(depth,a.z);
            const unsigned dest=fb+(y*stride+x)*4u;
            const unsigned destination=m.load32(dest);
            if(r(0x21)&1){
                // PSP GU_ADD / SRC_ALPHA / ONE_MINUS_SRC_ALPHA (BLEND_MODE=0x32).
                // Alpha belongs to the stencil channel and is not color-blended.
                const unsigned sa=color>>24;unsigned blended=color&0xFF000000;
                for(unsigned ch=0;ch<3;++ch){const unsigned shift=ch*8;
                    blended|=((((color>>shift)&255)*sa+((destination>>shift)&255)*(255-sa)+127)/255)<<shift;
                }
                color=blended;
            }
            m.store32(dest,(color&~mask)|(destination&mask));
        }
    }
    advanced=vertices+count*vertex_stride;return true;
}
}
