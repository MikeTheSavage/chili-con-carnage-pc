#pragma once
#include "ge_mip_texture.hpp"
#include "ge_feedback.hpp"
#include "ge_blend.hpp"
#include "ge_vertex_decoder.hpp"
#include "performance_telemetry.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace chili {
// Through sprites have affine UV in level-zero texels and clip-W=1.  AUTO
// detail uses max(abs(du/dx),abs(dv/dy)); the shared sampler retains its tested
// signed bias, mip rounding and four-bit filtering.  Legacy filters0/1 stay in
// the original renderer. No world matrices/UV scale or lighting are applied.
inline bool render_ge_mip_sprites(psprecomp::GuestMemory &m,const std::array<unsigned,256> &regs,
    unsigned vertices,unsigned data,unsigned &advanced,std::string &error,
    const GeClutCache *palette=nullptr,PerformanceTelemetry *telemetry=nullptr){
    PerformanceTelemetry::SpriteScope scope(telemetry,PerformanceTelemetry::SpritePath::Textured);
    auto r=[&](unsigned i){return regs[i]&0xFFFFFFu;};
    auto fail=[&](const char *message){error=message;return false;};
    auto compare=[](unsigned f,unsigned a,unsigned b){switch(f&7){case 0:return false;case 1:return true;
        case 2:return a==b;case 3:return a!=b;case 4:return a<b;case 5:return a<=b;case 6:return a>b;default:return a>=b;}};
    const unsigned count=data&0xFFFFu;
    const bool vertex_colors=r(0x12)==0x80011Eu;
    if(((data>>16)&7)!=6 || (count&1) || (r(0x12)!=0x800102u && !vertex_colors))
        return fail("mip sprite vertex format/primitive unsupported");
    const unsigned format=r(0xC3),filter=r(0xC6)&7;
    const bool compressed=format>=8 && format<=10;
    if(!(r(0x1E)&1) || filter<4 || ((r(0xC6)>>8)&7)>1 ||
       (format>5 && !compressed) || ((r(0xC9)&7)!=0 && (r(0xC9)&7)!=3) || (r(0xC9)&0x10000))
        return fail("mip sprite texture state unsupported");
    if((r(0xD2)&3)!=3 || r(0x15) || ((r(0x20)|r(0x24)|r(0x25)|r(0x27)|r(0x28))&1))
        return fail("mip sprite framebuffer/region/dither/stencil/AA/color-test/logic state unsupported");
    auto blend=ge::blend::prepare(false,0,0,0);
    if((r(0x21)&1) && r(0xDF)!=0x32){
        try{blend=ge::blend::prepare(true,r(0xDF),r(0xE0),r(0xE1));}
        catch(const std::runtime_error &exception){error=exception.what();return false;}
    }
    const unsigned source_stride=vertex_colors?16u:10u,position=vertex_colors?8u:4u;
    unsigned next{};
    try{next=ge::add_address(vertices,std::uint64_t(count)*source_stride);}
    catch(const std::runtime_error &exception){error=exception.what();return false;}
    if(vertices%(vertex_colors?4u:2u) || (count && !m.contains(vertices,count*source_stride)))
        return fail("mip sprite source alignment/storage unsupported");
    struct Vertex{unsigned u,v,z,color;int x,y;};
    std::vector<Vertex> source;source.reserve(count);
    const unsigned ambient=r(0x55)|((r(0x58)&255)<<24);
    for(unsigned n=0;n<count;++n){const unsigned address=vertices+n*source_stride;
        source.push_back({m.aot_load16(address),m.aot_load16(address+2),m.aot_load16(address+position+4),
            vertex_colors?m.aot_load32(address+4):ambient,
            std::bit_cast<std::int16_t>(m.aot_load16(address+position)),
            std::bit_cast<std::int16_t>(m.aot_load16(address+position+2))});
    }
    const int sx0=r(0xD4)&1023,sy0=(r(0xD4)>>10)&1023;
    const int sx1=std::min(r(0xD5)&1023,r(0x16)&1023),sy1=std::min((r(0xD5)>>10)&1023,(r(0x16)>>10)&1023);
    struct Rectangle{unsigned first;int x0,x1,y0,y1;GeMipSelection selection;};
    std::vector<Rectangle> rectangles;rectangles.reserve(count/2u);
    if(sx0<=sx1 && sy0<=sy1)for(unsigned n=0;n<count;n+=2){const auto &a=source[n],&b=source[n+1];
        const int x0=std::max(std::min(a.x,b.x),sx0),x1=std::min(std::max(a.x,b.x)-1,sx1);
        const int y0=std::max(std::min(a.y,b.y),sy0),y1=std::min(std::max(a.y,b.y)-1,sy1);
        if(x0<=x1 && y0<=y1)rectangles.push_back({n,x0,x1,y0,y1,{}});
    }
    // Full source/cursor, known pipeline and region/scissor intersection were
    // validated. No covered fragments can sample texture/cache/target storage.
    if(rectangles.empty()){advanced=next;return true;}

    const bool depth_test=r(0x23)&1,depth_write=depth_test && !(r(0xE7)&1);
    const unsigned fb=0x04000000|(r(0x9C)&0x1FFFF0),db=0x04000000|(r(0x9E)&0x1FFFF0);
    const unsigned stride=r(0x9D)&0x7FC,ds=r(0x9F)&0x7FC;
    if(!stride || sx1>=int(stride) || (depth_test && (!ds || sx1>=int(ds))))
        return fail("invalid mip sprite target dimensions");
    const unsigned color_bytes=(sy1*stride+sx1+1)*4u,depth_bytes=(sy1*ds+sx1+1)*2u;
    if(!m.contains(fb,color_bytes) || fb+color_bytes>0x04000000u+m.vram_size() ||
       (depth_test && (!m.contains(db,depth_bytes) || db+depth_bytes>0x04000000u+m.vram_size())))
        return fail("mip sprite target storage/wrapping unsupported");
    if(depth_test && fb<db+depth_bytes && db<fb+color_bytes)
        return fail("mip sprite overlapping color/depth targets unsupported");
    GeMipTexture texture;
    if(!texture.configure(m,regs,palette,error,0))return false;
    unsigned required=0;
    for(auto &rectangle:rectangles){const auto &a=source[rectangle.first],&b=source[rectangle.first+1];
        const double du=double(int(b.u)-int(a.u))/double(b.x-a.x);
        const double dv=double(int(b.v)-int(a.v))/double(b.y-a.y);
        const float footprint=float(std::max(std::abs(du),std::abs(dv)));
        if(!std::isfinite(footprint) || !texture.select(footprint,1.0f,rectangle.selection,error))return false;
        required|=(1u<<rectangle.selection.lower)|(rectangle.selection.fraction16?(1u<<rectangle.selection.upper):0u);
    }
    if(!texture.configure(m,regs,palette,error,required))return false;
    bool feedback=false;
    for(unsigned level=0;level<=texture.max_level();++level){
        if(!(required&(1u<<level)))continue;
        const auto &v=texture.level(level);
        feedback|=GeFeedbackGuard::overlaps(v.address,v.storage_bytes,fb,color_bytes) ||
            (depth_write && GeFeedbackGuard::overlaps(v.address,v.storage_bytes,db,depth_bytes));
    }
    // Packed block feedback needs a proven bit-level dependency contract.
    // Nonoverlapping compressed textures use the already prepared decoder.
    if(compressed && feedback)return fail("mip sprite compressed texture target feedback unsupported");
    auto uv=[&](const Rectangle &rectangle,int x,int y){const auto &a=source[rectangle.first],&b=source[rectangle.first+1];
        const double u=a.u+(x+0.5-a.x)*double(int(b.u)-int(a.u))/double(b.x-a.x);
        const double v=a.v+(y+0.5-a.y)*double(int(b.v)-int(a.v))/double(b.y-a.y);
        return std::array<float,2>{float(u)/texture.level(0).width,float(v)/texture.level(0).height};
    };
    if(feedback){
        scope.feedback();GeFeedbackGuard guard;
        auto level_readable=[&](unsigned level,float s,float t,bool linear){const auto &v=texture.level(level);
            s=(r(0xC7)&1)?std::clamp(s,-1.0f,2.0f):std::fmod(s,1.0f);
            t=(r(0xC7)&0x100)?std::clamp(t,-1.0f,2.0f):std::fmod(t,1.0f);
            const int sx=int(s*float(v.width)*256.0f)-(linear?128:0),sy=int(t*float(v.height)*256.0f)-(linear?128:0);
            auto whole=[](int f){return f>=0?f/256:-((-f+255)/256);};
            const int ix=whole(sx),iy=whole(sy);const unsigned fx=(sx-ix*256)/16,fy=(sy-iy*256)/16;
            auto readable=[&](int x,int y){
                const unsigned tx=(r(0xC7)&1)?unsigned(std::clamp(x,0,int(v.width)-1)):unsigned(x)&(v.width-1);
                const unsigned ty=(r(0xC7)&0x100)?unsigned(std::clamp(y,0,int(v.height)-1)):unsigned(y)&(v.height-1);
                const unsigned bx=format==4?tx/2u:format==5?tx:tx*(format==3?4u:2u);
                const unsigned offset=(r(0xC2)&1)?(ty/8u)*v.byte_pitch*8u+(bx/16u)*128u+(ty%8u)*16u+bx%16u:
                    ty*v.byte_pitch+bx;
                return guard.readable(v.address+offset,format==3?4u:format<3?2u:1u);
            };
            return readable(ix,iy) && (!linear || ((!fx || readable(ix+1,iy)) &&
                (!fy || readable(ix,iy+1)) && (!fx || !fy || readable(ix+1,iy+1))));
        };
        const unsigned preserve=r(0xE8)|0xFF000000u;
        for(const auto &rectangle:rectangles)for(int y=rectangle.y0;y<=rectangle.y1;++y)for(int x=rectangle.x0;x<=rectangle.x1;++x){
            const auto coordinate=uv(rectangle,x,y);const auto &selection=rectangle.selection;
            if(!level_readable(selection.lower,coordinate[0],coordinate[1],selection.linear) ||
               (selection.fraction16 && !level_readable(selection.upper,coordinate[0],coordinate[1],selection.linear)))
                return fail("mip sprite feedback has a read-after-write dependency");
            if(preserve!=0xFFFFFFFFu)guard.written(fb+(y*stride+x)*4u,4u);
            if(depth_write)guard.written(db+(y*ds+x)*2u,2u);
        }
    }
    // Shade every potentially surviving fragment before the first write. An
    // invalid mip/cache/sample in a later rectangle leaves both targets intact.
    struct Fragment{unsigned x,y,z,color;};std::vector<Fragment> fragments;
    const unsigned alpha=r(0xDB),alpha_mask=(alpha>>16)&255;
    scope.raster();
    for(const auto &rectangle:rectangles){const auto &b=source[rectangle.first+1];
        for(int y=rectangle.y0;y<=rectangle.y1;++y)for(int x=rectangle.x0;x<=rectangle.x1;++x){
            const auto coordinate=uv(rectangle,x,y);unsigned color{};
            if(!texture.sample(m,coordinate[0],coordinate[1],rectangle.selection,color,error))return false;
            if((r(0xC9)&7)==0){unsigned modulated=0;
                for(unsigned ch=0;ch<4;++ch){const unsigned shift=ch*8;
                    modulated|=(((((b.color>>shift)&255)+1)*((color>>shift)&255))/256)<<shift;}
                color=modulated;
            }
            if(!(r(0xC9)&0x100))color=(color&0xFFFFFF)|(b.color&0xFF000000u);
            if((r(0x22)&1) && !compare(alpha,(color>>24)&alpha_mask,((alpha>>8)&255)&alpha_mask))continue;
            if(fragments.size()>=4u*1024u*1024u)return fail("mip sprite fragment staging budget exceeded");
            fragments.push_back({unsigned(x),unsigned(y),b.z,color});
        }
    }
    const unsigned preserve=r(0xE8)|0xFF000000u;
    for(const auto &fragment:fragments){const unsigned depth=db+(fragment.y*ds+fragment.x)*2u;
        if(depth_test && !compare(r(0xDE),fragment.z,m.aot_load16(depth)))continue;
        const unsigned address=fb+(fragment.y*stride+fragment.x)*4u,destination=m.aot_load32(address);
        unsigned color=fragment.color;
        if(blend.enabled())color=ge::blend::rgb(blend,color,destination);
        else if(r(0x21)&1){const unsigned sa=color>>24;unsigned rgb=0;
            for(unsigned ch=0;ch<3;++ch){const unsigned shift=ch*8;
                const unsigned a=((((color>>shift)&255)*2+1)*(sa*2+1))/1024;
                const unsigned b=((((destination>>shift)&255)*2+1)*((255-sa)*2+1))/1024;
                rgb|=std::min(255u,a+b)<<shift;}
            color=rgb;
        }
        if(depth_write)m.aot_store16(depth,fragment.z);
        m.aot_store32(address,(color&~preserve)|(destination&preserve));
    }
    advanced=next;return true;
}
} // namespace chili
