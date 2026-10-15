#pragma once
#include "ge_mip_texture.hpp"
#include "ge_vertex_decoder.hpp"
#include "ge_vertex_deform.hpp"
#include "ge_lighting.hpp"
#include "ge_fog.hpp"
#include "ge_blend.hpp"
#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
namespace chili_quad_trace_candidate {
using namespace chili;
namespace triangle_native {
struct Vertex {std::array<float,4> clip{},color{};std::array<float,3> secondary{};float u{},v{},fog{1.0f};int x{},y{};unsigned z{};};
struct Fragment {unsigned x,y,z,color;};
inline bool compare(unsigned f,unsigned a,unsigned b){switch(f&7){case 0:return false;case 1:return true;case 2:return a==b;case 3:return a!=b;case 4:return a<b;case 5:return a<=b;case 6:return a>b;default:return a>=b;}}
inline std::int64_t edge(const Vertex &a,const Vertex &b,int x,int y){return std::int64_t(b.x-a.x)*(y-a.y)-std::int64_t(b.y-a.y)*(x-a.x);}
inline bool inclusive(const Vertex &a,const Vertex &b){return b.y<a.y || (b.y==a.y && b.x>a.x);}
inline Vertex lerp(const Vertex &a,const Vertex &b,float t){Vertex out;
    for(unsigned i=0;i<4;++i){out.clip[i]=a.clip[i]+(b.clip[i]-a.clip[i])*t;out.color[i]=a.color[i]+(b.color[i]-a.color[i])*t;}
    for(unsigned i=0;i<3;++i)out.secondary[i]=a.secondary[i]+(b.secondary[i]-a.secondary[i])*t;
    out.u=a.u+(b.u-a.u)*t;out.v=a.v+(b.v-a.v)*t;out.fog=a.fog+(b.fog-a.fog)*t;return out;}
inline unsigned packed(const std::array<float,4> &color){unsigned out=0;for(unsigned c=0;c<4;++c)out|=unsigned(std::clamp(color[c],0.0f,255.0f))<<(c*8);return out;}
}
// Native source records: documented member formats and index8/16, with
// optional morph-only or skin-only state. See ge_vertex_deform.hpp for gates.
// Protocol references: PSPSDK GU API; primary PPSSPP software transform,
// clipper, lighting and pixel pipeline; PSP homogeneous/mipmap hardware tests.
// Depth transform currently uses ordinary float division, not the PSP's
// float24 reciprocal/table arithmetic. Exact depth precision and subpixel
// hardware parity remain unverified; fixtures cover exact dyadic/W=1 cases.
inline bool render_ge_triangle_raster(psprecomp::GuestMemory &m,const std::array<unsigned,256> &regs,
    const std::array<float,12> &world,const std::array<float,12> &view,const std::array<float,16> &projection,
    unsigned vertices,unsigned data,unsigned &advanced,std::string &error,const GeClutCache &clut,unsigned *written_pixels=nullptr,unsigned indices=0,unsigned *advanced_indices=nullptr,const ge::DeformState *deformation_state=nullptr){
    using namespace triangle_native;
    auto r=[&](unsigned i){return regs[i]&0xFFFFFFu;};
    auto f=[&](unsigned i){return std::bit_cast<float>(r(i)<<8);};
    auto fail=[&](const char *message){error=message;return false;};
    const unsigned count=data&0xFFFFu,primitive=(data>>16)&7u;
    if((primitive!=3 && primitive!=4 && primitive!=5) || (r(0xD3)&1) || (primitive==3 && count%3))
        return fail("native triangle primitive unsupported");
    ge::Draw decoded;
    ge::DeformedDraw deformed;bool deformation_needed=false;
    try {
        const auto layout=ge::make_layout(r(0x12));
        deformation_needed=layout.needs_skinning() || layout.needs_morphing();
        if(deformation_needed && !deformation_state)return fail("native triangle skinning/morphing requires explicit deformation state");
        if(layout.index_bytes && !advanced_indices)return fail("indexed native triangle requires an IADDR output cursor");
        if((r(0x1E)&1) && !layout.uv_type)return fail("textured native triangle requires explicit source UV");
        auto reader=[&](unsigned address,std::span<std::uint8_t> dst){
            if(!m.contains(address,dst.size()))return false;
            for(std::size_t n=0;n<dst.size();++n)dst[n]=m.load8(address+static_cast<unsigned>(n));return true;
        };
        decoded=ge::prepare_draw(reader,r(0x12),vertices,indices,count);
        if(deformation_needed)deformed=ge::prepare_deformed_draw(decoded,*deformation_state);
    } catch(const std::runtime_error &exception){error=exception.what();return false;}
    const bool through=decoded.layout.through;
    if((r(0xD2)&3)!=3 || r(0x15) || ((r(0x20)|r(0x24)|r(0x25)|r(0x27)|r(0x28))&1))
        return fail("native triangle framebuffer/dither/stencil/AA/color-test/logic state unsupported");
    ge::FogState fog;
    if(!ge::try_prepare_ge_fog(regs,through,fog,error))return false;
    auto fixed_blend=ge::blend::prepare(false,0,0,0);
    if((r(0x21)&1) && r(0xDF)!=0x32){
        try{fixed_blend=ge::blend::prepare(true,r(0xDF),r(0xE0),r(0xE1));}
        catch(const std::runtime_error &exception){error=exception.what();return false;}
    }
    const bool textured=r(0x1E)&1,depth_test=r(0x23)&1,depth_write=depth_test && !(r(0xE7)&1),depth_clamp=r(0x1C)&1;
    if(textured && ((!through && (r(0xC0)&3)) || ((r(0xC9)&7)!=0 && (r(0xC9)&7)!=3) || (r(0xC9)&0x10000)))
        return fail("native triangle texture generation/function unsupported");

    if(!through){
    for(float v:world)if(!std::isfinite(v))return fail("nonfinite triangle world matrix");
    for(float v:view)if(!std::isfinite(v))return fail("nonfinite triangle view matrix");
    for(float v:projection)if(!std::isfinite(v))return fail("nonfinite triangle projection matrix");
    for(unsigned i=0x42;i<=0x47;++i)if(!std::isfinite(f(i)))return fail("nonfinite triangle viewport");
    if(textured)for(unsigned i=0x48;i<=0x4B;++i)if(!std::isfinite(f(i)))return fail("nonfinite triangle UV scale/offset");
    }
    const unsigned fb=0x04000000|(r(0x9C)&0x1FFFF0),db=0x04000000|(r(0x9E)&0x1FFFF0);
    const unsigned stride=r(0x9D)&0x7FC,ds=r(0x9F)&0x7FC;
    const int sx0=r(0xD4)&1023,sy0=(r(0xD4)>>10)&1023;
    const int sx1=std::min(r(0xD5)&1023,r(0x16)&1023),sy1=std::min((r(0xD5)>>10)&1023,(r(0x16)>>10)&1023);
    const unsigned fb_bytes=(sy1*stride+sx1+1)*4u,db_bytes=(sy1*ds+sx1+1)*2u;
    if(!stride || sx1>=int(stride) || (depth_test && (!ds || sx1>=int(ds))))return fail("invalid native triangle buffer dimensions");
    if(!m.contains(fb,fb_bytes) || fb+fb_bytes>0x04000000u+m.vram_size() ||
       (depth_test && (!m.contains(db,db_bytes) || db+db_bytes>0x04000000u+m.vram_size())))
        return fail("native triangle target storage/wrapping unsupported");
    if(depth_test && fb<db+db_bytes && db<fb+fb_bytes)return fail("native triangle overlapping color/depth targets unsupported");
    GeMipTexture texture;
    if(textured){if(!texture.configure(m,regs,&clut,error))return false;
        for(unsigned level=0;level<=texture.max_level();++level){const auto &v=texture.level(level);unsigned physical=psprecomp::GuestMemory::canonical(v.address);
            if(physical>=0x04000000 && physical<0x04800000){physical=0x04000000|((physical-0x04000000)&0x1FFFFF);
                if(physical+v.storage_bytes>0x04200000)return fail("native triangle wrapping texture feedback check unsupported");
                if((physical<fb+fb_bytes && fb<physical+v.storage_bytes) ||
                   (depth_write && physical<db+db_bytes && db<physical+v.storage_bytes))return fail("native triangle texture target feedback unsupported");}
        }
    }
    auto affine=[](const std::array<float,12> &matrix,const std::array<float,3> &p){std::array<float,3> out{};
        for(unsigned row=0;row<3;++row)out[row]=matrix[row]*p[0]+matrix[row+3]*p[1]+matrix[row+6]*p[2]+matrix[row+9];return out;};
    std::vector<Vertex> input;input.reserve(count);
    for(unsigned i=0;i<count;++i){const auto &source=deformation_needed?deformed.vertices[i].attributes:decoded.vertices[i].samples[0];Vertex v;const auto position=source.position;
        unsigned color=r(0x55)|((r(0x58)&255)<<24);
        if(source.color){color=0;for(unsigned ch=0;ch<4;++ch)color|=unsigned((*source.color)[ch])<<(ch*8);}
        if(through){
            // PSP through coordinates bypass viewport/offset; finite float XY
            // truncates after scaling to 1/16 pixel. Bound conversion and edge
            // arithmetic to the verified signed16 drawing-coordinate range.
            const float x=position[0],y=position[1];
            if(x<-32768.0f || x>=32768.0f || y<-32768.0f || y>=32768.0f)return fail("native through float XY outside verified drawing range");
            v.x=int(x*16.0f);v.y=int(y*16.0f);v.z=unsigned(position[2]);
            v.clip={x,y,float(v.z),1.0f};
            for(unsigned channel=0;channel<4;++channel)v.color[channel]=float((color>>(channel*8))&255);
            if(textured){v.u=(*source.uv)[0]/texture.level(0).width;v.v=(*source.uv)[1]/texture.level(0).height;}
            input.push_back(v);continue;
        }
        const auto world_position=affine(world,position);
        const auto camera=affine(view,world_position);
        if(!ge::try_prepare_ge_fog_factor(fog,camera[2],v.fog,error))return false;
        for(unsigned row=0;row<4;++row){v.clip[row]=projection[row]*camera[0]+projection[row+4]*camera[1]+projection[row+8]*camera[2]+projection[row+12];
            if(!std::isfinite(v.clip[row]))return fail("nonfinite native triangle transform");}
        // Preserve absence: a world normal exists only when the decoded (or
        // deformed) source contains one. Translation never affects normals.
        std::optional<ge::Vec3> world_normal;
        if((r(0x17)&1) && source.normal){
            world_normal=ge::Vec3{};
            for(unsigned row=0;row<3;++row)(*world_normal)[row]=world[row]*(*source.normal)[0]+world[row+3]*(*source.normal)[1]+world[row+6]*(*source.normal)[2];
        }
        ge::LightingResult lighting;
        if(!ge::try_prepare_ge_lighting(regs,view,world_position,world_normal,source.color,lighting,error))return false;
        for(unsigned c=0;c<4;++c)v.color[c]=lighting.primary[c];
        for(unsigned c=0;c<3;++c)v.secondary[c]=lighting.secondary[c];
        if(textured){v.u=(*source.uv)[0]*f(0x48)+f(0x4A);v.v=(*source.uv)[1]*f(0x49)+f(0x4B);
            if(!std::isfinite(v.u) || !std::isfinite(v.v))return fail("nonfinite native triangle UV");}
        input.push_back(v);
    }
    struct Triangle{std::array<Vertex,3> v;unsigned flat;std::array<std::uint8_t,3> flat_secondary;};std::vector<Triangle> triangles;
    auto to_screen=[&](Vertex &v,bool &outside,bool original=false){
        if(v.clip[3]<=0)return false;
        const float x=v.clip[0]*f(0x42)/v.clip[3]+f(0x45),y=v.clip[1]*f(0x43)/v.clip[3]+f(0x46);
        float z=v.clip[2]*f(0x44)/v.clip[3]+f(0x47);
        if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z))return false;
        constexpr float guard=4095.0f+15.5f/16.0f;
        const bool outside_xy=x<0 || y<0 || x>=guard || y>=guard;
        // A vertex behind the near plane is removed/intersected before its
        // guardband check when depth clamp is enabled. Recheck intersections.
        outside=(outside_xy && !(original && depth_clamp && v.clip[2]<=-v.clip[3])) ||
                (!depth_clamp && (z<0 || z>=65536));
        if(outside_xy && !outside)return true;
        if(outside)return true;
        if(depth_clamp)z=std::clamp(z,0.0f,65535.0f);
        v.x=int(x*16.0f+0.375f-float(r(0x4C)&65535));v.y=int(y*16.0f+0.375f-float(r(0x4D)&65535));v.z=unsigned(std::max(0.0f,z));return true;
    };
    auto prepare=[&](unsigned ia,unsigned ib,unsigned ic)->bool{
        const unsigned flat=packed(input[ic].color);
        std::array<std::uint8_t,3> flat_secondary{};
        for(unsigned ch=0;ch<3;++ch)flat_secondary[ch]=std::uint8_t(std::clamp(input[ic].secondary[ch],0.0f,255.0f));
        if(through){
            Triangle t{{input[ia],input[ib],input[ic]},flat,flat_secondary};
            const auto area=edge(t.v[0],t.v[1],t.v[2].x,t.v[2].y);
            if(!area || ((r(0x1D)&1) && ((area>0)!=bool(r(0x9B)&1))))return true;
            if(area<0)std::swap(t.v[0],t.v[1]);
            triangles.push_back(t);return true;
        }
        std::vector<Vertex> poly{input[ia],input[ib],input[ic]};
        if(poly[0].clip[3]<0 && poly[1].clip[3]<0 && poly[2].clip[3]<0)return true;
        unsigned far_count=0,near_count=0;
        for(auto &v:poly){
            // With depth clamp, original vertices strictly behind Z+W=0
            // are removed/intersected in homogeneous space. Projecting W<=0
            // here would divide before the near clip has made W positive.
            // Do not invent a W epsilon or add X/Y/far clipping to PSP rules.
            if(depth_clamp && v.clip[3]<=0 && v.clip[2]<-v.clip[3]){
                if(v.clip[3]==0){++near_count;continue;}
                const float ndc=v.clip[2]/v.clip[3];
                far_count+=ndc>=1.000030517578125f;near_count+=ndc<=-1.000030517578125f;
                continue;
            }
            bool outside=false;if(!to_screen(v,outside,true)){error="native triangle nonpositive W not removed by near clip requires validated projection";return false;}
            if(outside)return true;const float ndc=v.clip[2]/v.clip[3];far_count+=ndc>=1.000030517578125f;near_count+=ndc<=-1.000030517578125f;}
        if((!depth_clamp && (far_count||near_count)) || far_count==3 || near_count==3)return true;
        // PSP clips only the near homogeneous plane Z+W>=0. X/Y use a
        // guardband and scissor; far Z is clamped rather than clipped here.
        std::vector<Vertex> clipped;Vertex a=poly.back();float da=a.clip[2]+a.clip[3];
        for(const Vertex &b:poly){const float db=b.clip[2]+b.clip[3];
            if((da>=0)!=(db>=0))clipped.push_back(lerp(a,b,da/(da-db)));
            if(db>=0)clipped.push_back(b);a=b;da=db;}
        if(clipped.size()<3)return true;
        for(auto &v:clipped){bool outside=false;if(!to_screen(v,outside)){error="native clipped triangle projection plane unsupported";return false;}if(outside)return true;}
        for(unsigned i=1;i+1<clipped.size();++i){Triangle t{{clipped[0],clipped[i],clipped[i+1]},flat,flat_secondary};
            auto area=edge(t.v[0],t.v[1],t.v[2].x,t.v[2].y);if(!area)continue;
            // CULL mode chooses front winding in PSP projected coordinates.
            if((r(0x1D)&1) && ((area>0)!=bool(r(0x9B)&1)))continue;
            if(area<0)std::swap(t.v[0],t.v[1]);triangles.push_back(t);}
        return true;
    };
    if(primitive==3){for(unsigned i=0;i<count;i+=3)if(!prepare(i,i+1,i+2))return false;}
    else if(primitive==4){for(unsigned i=2;i<count;++i)if(!prepare((i&1)?i-1:i-2,(i&1)?i-2:i-1,i))return false;}
    else for(unsigned i=2;i<count;++i)if(!prepare(0,i-1,i))return false;
    // Stage shading before writes so a later unsupported/invalid interpolation
    // cannot leave a partially effected GE draw. Pixel tests/blends commit in
    // submission order, preserving dependencies between overlapping triangles.
    std::vector<Fragment> fragments;
    const bool memoize_t4_auto=textured && r(0xC3)==4 && (r(0xC8)&3)==0;
    for(const auto &t:triangles){const auto &a=t.v[0],&b=t.v[1],&c=t.v[2];const auto area=edge(a,b,c.x,c.y);
        const int x0=std::max(sx0,int(std::floor(std::min({a.x,b.x,c.x})/16.0))),x1=std::min(sx1,int(std::floor(std::max({a.x,b.x,c.x})/16.0)));
        const int y0=std::max(sy0,int(std::floor(std::min({a.y,b.y,c.y})/16.0))),y1=std::min(sy1,int(std::floor(std::max({a.y,b.y,c.y})/16.0)));
        auto weights=[&](int x,int y){return std::array<double,3>{double(edge(b,c,x,y))/area,double(edge(c,a,x,y))/area,double(edge(a,b,x,y))/area};};
        auto uv=[&](int x,int y,float &u,float &v,float &w,bool helper){quad_cache_fixture::attempt(x,y,helper);const auto l=weights(x,y);
            const double q=l[0]/a.clip[3]+l[1]/b.clip[3]+l[2]/c.clip[3];
            // Uncovered quad helpers extrapolate the rational UV plane. Their
            // finite denominator may be negative outside the triangle. Only
            // actual covered fragments require positive reciprocal W; a zero
            // helper denominator remains unsupported rather than inventing LOD.
            if(!std::isfinite(q)||q==0||(!helper&&q<0))return false;
            u=float((l[0]*a.u/a.clip[3]+l[1]*b.u/b.clip[3]+l[2]*c.u/c.clip[3])/q);
            v=float((l[0]*a.v/a.clip[3]+l[1]*b.v/b.clip[3]+l[2]*c.v/c.clip[3])/q);
            w=float(l[0]*a.clip[3]+l[1]*b.clip[3]+l[2]*c.clip[3]);quad_cache_fixture::uv(x,y,helper,u,v,w);return std::isfinite(u)&&std::isfinite(v)&&std::isfinite(w);};
        struct QuadMemo{bool valid{};int x{},y{};float footprint{};GeMipSelection selection{};} memo;
        for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x){const int px=x*16+7,py=y*16+7;
            const auto e0=edge(b,c,px,py),e1=edge(c,a,px,py),e2=edge(a,b,px,py);
            if(e0<0||e1<0||e2<0 || (!e0&&!inclusive(b,c)) || (!e1&&!inclusive(c,a)) || (!e2&&!inclusive(a,b)))continue;
            const auto l=weights(px,py);const unsigned z=(a.z==b.z && a.z==c.z)?a.z:unsigned(l[0]*a.z+l[1]*b.z+l[2]*c.z);
            if(z<(r(0xD6)&65535) || z>(r(0xD7)&65535))continue;
            unsigned color=t.flat;
            auto secondary=t.flat_secondary;
            if(r(0x50)&1){std::array<float,4> interpolated{};for(unsigned ch=0;ch<4;++ch)interpolated[ch]=float(l[0]*a.color[ch]+l[1]*b.color[ch]+l[2]*c.color[ch]);color=packed(interpolated);}
            // PSP colors use affine screen weights, including secondary RGB;
            // UV uses the separate perspective-correct path below.
            if(r(0x50)&1)for(unsigned ch=0;ch<3;++ch)secondary[ch]=std::uint8_t(std::clamp(float(l[0]*a.secondary[ch]+l[1]*b.secondary[ch]+l[2]*c.secondary[ch]),0.0f,255.0f));
            if(textured){float u,v,w,u0,v0,w0,ux,vx,wx,uy,vy,wy;
                const int qx=(x&~1)*16+7,qy=(y&~1)*16+7;
                if(!uv(px,py,u,v,w,false))return fail("native triangle mip interpolation footprint unsupported");
                GeMipSelection selection;unsigned sampled;float traced_footprint{};
                if(memoize_t4_auto && memo.valid && memo.x==qx && memo.y==qy){selection=memo.selection;traced_footprint=memo.footprint;}
                else{
                    if(!uv(qx,qy,u0,v0,w0,true)||!uv(qx+16,qy,ux,vx,wx,true)||!uv(qx,qy+16,uy,vy,wy,true))return fail("native triangle mip interpolation footprint unsupported");
                    const float footprint=std::max(std::abs(ux-u0)*texture.level(0).width,std::abs(vy-v0)*texture.level(0).height);traced_footprint=footprint;
                    if(!texture.select(footprint,w,selection,error))return false;
                    if(memoize_t4_auto)memo={true,qx,qy,footprint,selection};
                }
                quad_cache_fixture::selected(x,y,traced_footprint,selection);
                if(!texture.sample(m,u,v,selection,sampled,error))return false;
                const unsigned ambient=color;
                if((r(0xC9)&7)==0){color=0;for(unsigned ch=0;ch<4;++ch){const unsigned shift=ch*8;color|=((((ambient>>shift)&255)+1)*((sampled>>shift)&255)/256)<<shift;}}
                else color=sampled;
                if(!(r(0xC9)&0x100))color=(color&0xFFFFFF)|(ambient&0xFF000000);
            }
            color=ge::add_ge_separate_specular(color,secondary);
            const unsigned alpha=r(0xDB),mask=(alpha>>16)&255;
            if((r(0x22)&1) && !compare(alpha,(color>>24)&mask,((alpha>>8)&255)&mask))continue;
            // Fog is affine in screen space, independent of shade model and
            // projected depth. Clamp the raw interpolated camera factor here.
            // It follows texturing/secondary color and leaves alpha untouched.
            if(fog.enabled){const float factor=float(l[0]*a.fog+l[1]*b.fog+l[2]*c.fog);
                if(!std::isfinite(factor))return fail("nonfinite native triangle fog interpolation unsupported");
                color=ge::apply_ge_fog(color,fog.rgb,ge::quantize_ge_fog_factor(factor));}
            if(fragments.size()>=4u*1024u*1024u)return fail("native triangle fragment staging budget exceeded");
            fragments.push_back({unsigned(x),unsigned(y),z,color});
        }
    }
    const unsigned preserve=r(0xE8)|0xFF000000u; // Stencil disabled: preserve alpha independently of PMSKA.
    unsigned committed=0;
    for(const auto &p:fragments){const unsigned depth=db+(p.y*ds+p.x)*2u;
        if(depth_test && !compare(r(0xDE),p.z,m.aot_load16(depth)))continue;
        if(depth_write)m.aot_store16(depth,p.z);
        const unsigned address=fb+(p.y*stride+p.x)*4u,old=m.aot_load32(address);unsigned color=p.color;
        if(fixed_blend.enabled())color=ge::blend::rgb(fixed_blend,color,old);
        else if(r(0x21)&1){const unsigned sa=color>>24;unsigned blended=0;
            for(unsigned ch=0;ch<3;++ch){const unsigned shift=ch*8;
                const unsigned source=((((color>>shift)&255)*2+1)*(sa*2+1))/1024;
                const unsigned destination=((((old>>shift)&255)*2+1)*((255-sa)*2+1))/1024;
                blended|=std::min(255u,source+destination)<<shift;}color=blended;}
        m.aot_store32(address,(color&~preserve)|(old&preserve));++committed;
    }
    advanced=decoded.next_vertex_address;if(advanced_indices)*advanced_indices=decoded.next_index_address;if(written_pixels)*written_pixels=committed;return true;
}
}
