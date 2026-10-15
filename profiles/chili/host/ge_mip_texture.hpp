#pragma once
#include "ge_indexed_texture.hpp"
#include "ge_texture_formats.hpp"
#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>

namespace chili {
struct GeMipSelection {
    unsigned lower{},upper{},fraction16{};
    bool linear{};
    int detail16{};
};
struct GeMipLevel {
    unsigned address{},storage_bytes{},width{},height{},stride{},byte_pitch{};
};
// Independent native sampler from PSP register behavior. Encoding references:
// PSPSDK sceGuTexMode, sceGuTexFilter, sceGuTexLevelMode and sceGuTexSlope.
// Hardware reference: pspautotests tests/gpu/textures/mipmap and
// tests/gpu/filtering/mipmaplinear (source plus real-PSP expected outputs).
// LOD uses exponent plus four mantissa bits, rather than ideal mathematical
// log2. Filtering uses four-bit fractions and floors color channel divisions.
// The caller supplies its derivative footprint in level-zero texels/pixel;
// exact hardware cross-derivative selection is not claimed by this helper.
class GeMipTexture {
public:
    // Explicit draw-local immutable-source opt-in. Standalone samplers remain
    // uncached. Logical texels are decoded lazily by the original integer path;
    // no unselected level or previously unrequested texel is read eagerly.
    class ImmutableSmallDxtCache {
        friend class GeMipTexture;
        std::array<std::array<unsigned,256>,8> rgba_{};
        std::array<std::array<std::uint64_t,4>,8> valid_{};
        void reset(){for(auto &level:valid_)level.fill(0);}
    };
private:
    ImmutableSmallDxtCache *immutable_small_dxt_cache_{};
    std::array<GeMipLevel,8> levels_{};
    std::array<GeTextureFormatView,8> direct_{};
    std::array<std::array<unsigned,256>,8> colors_{};
    unsigned prepared_levels_{},format_{},max_level_{},min_filter_{},mag_filter_{},lod_mode_{};
    int bias16_{};
    float slope_{};
    bool swizzled_{},clamp_s_{},clamp_t_{},configured_{};

    static int lod16(float value){
        if(value==0)return -127*16;
        int exponent{};
        const float mantissa=std::frexp(value,&exponent)*2.0f;
        const int fraction=static_cast<int>((mantissa-1.0f)*16.0f);
        return (exponent-1)*16+fraction;
    }
    static bool palette_color(const GeClutCache &cache,unsigned raw,unsigned mode,unsigned offset,
                              unsigned &color,std::string &error){
        if(!offset)return cache.color(raw,mode,color,error);
        const unsigned capacity=(mode&3)==3?256u:512u;
        const unsigned mapped=(((raw>>((mode>>2)&31))&((mode>>8)&255))|(((mode>>16)&31)*16))%capacity;
        // Separate T4 palettes add 16 entries per level AFTER C5's index map.
        // Wrap/overflow of this additional offset is not established by the
        // cited hardware fixtures; reject that layout instead of guessing.
        const unsigned entry=mapped+offset;
        if(entry>=capacity){error="multiclut palette cache overflow unsupported";return false;}
        const unsigned absolute_mode=(mode&3)|0xFF00u|((entry&256)?(16u<<16):0u);
        return cache.color(entry&255,absolute_mode,color,error);
    }
    unsigned texel(const psprecomp::GuestMemory &m,unsigned level,int x,int y) const {
        const auto &v=levels_[level];
        const unsigned u=clamp_s_?static_cast<unsigned>(std::clamp(x,0,int(v.width)-1)):
                                    static_cast<unsigned>(x)&(v.width-1);
        const unsigned t=clamp_t_?static_cast<unsigned>(std::clamp(y,0,int(v.height)-1)):
                                    static_cast<unsigned>(y)&(v.height-1);
        if(format_!=4 && format_!=5){
            if(immutable_small_dxt_cache_ && format_>=8 && format_<=10 && v.width*v.height<=256){
                const unsigned key=t*v.width+u;
                const std::uint64_t bit=std::uint64_t(1)<<(key&63u);
                auto &valid=immutable_small_dxt_cache_->valid_[level][key/64u];
                auto &color=immutable_small_dxt_cache_->rgba_[level][key];
                if(!(valid&bit)){color=direct_[level].fetch_prepared(m,u,t);valid|=bit;}
                return color;
            }
            return direct_[level].fetch_prepared(m,u,t);
        }
        const unsigned byte_x=format_==4?u/2u:u;
        const unsigned offset=swizzled_?(t/8u)*v.byte_pitch*8u+(byte_x/16u)*128u+(t%8u)*16u+byte_x%16u:
                                         t*v.byte_pitch+byte_x;
        const unsigned packed=m.aot_load8(v.address+offset);
        const unsigned index=format_==4?(packed>>((u&1u)*4u))&15u:packed;
        return colors_[level][index];
    }
    static int whole_texel(int fixed){return fixed>=0?fixed/256:-((-fixed+255)/256);}
    unsigned sample_level(const psprecomp::GuestMemory &m,unsigned level,float s,float t,bool linear) const {
        const auto &v=levels_[level];
        // Normalize extreme finite coordinates before conversion to avoid host
        // integer overflow. Repeat has period 1; clamped extremes sample edges.
        s=clamp_s_?std::clamp(s,-1.0f,2.0f):std::fmod(s,1.0f);
        t=clamp_t_?std::clamp(t,-1.0f,2.0f):std::fmod(t,1.0f);
        const int sx=static_cast<int>(s*float(v.width)*256.0f)-(linear?128:0);
        const int sy=static_cast<int>(t*float(v.height)*256.0f)-(linear?128:0);
        const int x=whole_texel(sx),y=whole_texel(sy);
        if(!linear)return texel(m,level,x,y);
        const unsigned fx=(sx-x*256)/16,fy=(sy-y*256)/16;
        const unsigned samples[]={texel(m,level,x,y),texel(m,level,x+1,y),texel(m,level,x,y+1),texel(m,level,x+1,y+1)};
        unsigned color=0;
        for(unsigned ch=0;ch<4;++ch){const unsigned shift=ch*8;
            const unsigned sum=((samples[0]>>shift)&255)*(16-fx)*(16-fy)+
                               ((samples[1]>>shift)&255)*fx*(16-fy)+
                               ((samples[2]>>shift)&255)*(16-fx)*fy+
                               ((samples[3]>>shift)&255)*fx*fy;
            color|=(sum/256)<<shift;
        }
        return color;
    }
public:
    // The sentinel retains full validation for existing callers. An explicit
    // mask prepares only contributing levels; zero validates global metadata.
    static constexpr unsigned kAllLevels=0xFFFFFFFFu;
    bool configure(const psprecomp::GuestMemory &m,const std::array<unsigned,256> &regs,
                   const GeClutCache *cache,std::string &error,unsigned required_levels=kAllLevels){
        // Every configure attempt detaches any immutable draw cache, including
        // failed transactional reconfiguration. Metadata/error behavior is
        // unchanged; a failed attempt can never leave stale cached source data.
        immutable_small_dxt_cache_=nullptr;
        auto r=[&](unsigned i){return regs[i]&0xFFFFFFu;};
        auto fail=[&](const char *message){error=message;return false;};
        GeMipTexture staged;
        staged.format_=r(0xC3);
        const bool indexed=staged.format_==4 || staged.format_==5;
        if(staged.format_>3 && !indexed && (staged.format_<8 || staged.format_>10))return fail("mip texture format unsupported");
        staged.min_filter_=r(0xC6)&7;staged.mag_filter_=(r(0xC6)>>8)&7;
        if(staged.min_filter_==2 || staged.min_filter_==3 || staged.mag_filter_>1)return fail("mip filter encoding unsupported");
        staged.max_level_=(staged.min_filter_&4)?((r(0xC2)>>16)&7):0;
        staged.swizzled_=r(0xC2)&1;staged.clamp_s_=r(0xC7)&1;staged.clamp_t_=r(0xC7)&0x100;
        staged.lod_mode_=r(0xC8)&3;
        const unsigned bias=(r(0xC8)>>16)&255;
        staged.bias16_=(bias&128)?int(bias)-256:int(bias);
        staged.slope_=std::bit_cast<float>(r(0xD0)<<8);
        const bool multiclut=staged.format_==4 && (r(0xC2)&0x100);
        const unsigned declared=(1u<<(staged.max_level_+1u))-1u;
        const unsigned required=required_levels==kAllLevels?declared:required_levels;
        if(required&~declared)return fail("required mip levels outside declared range");
        if(indexed && required && !cache)return fail("mip palette cache unavailable");
        for(unsigned level=0;level<=staged.max_level_;++level){auto &v=staged.levels_[level];
            const unsigned stride=r(0xA8+level)&0x7FF;
            const unsigned width_exp=r(0xB8+level)&15,height_exp=(r(0xB8+level)>>8)&15;
            // PSPSDK's documented texture dimensions are powers of two <=512.
            // Larger raw-register sizes need verified 9-bit address behavior.
            const bool prepare=(required&(1u<<level))!=0;
            if((level==0 || prepare) && (width_exp>9 || height_exp>9))return fail("mip texture dimensions unsupported");
            v.width=1u<<width_exp;v.height=1u<<height_exp;v.stride=stride;
            v.address=(r(0xA0+level)&0xFFFFF0)|((r(0xA8+level)<<8)&0x0F000000);
            if(!prepare)continue;
            // Uncompressed GE buffer widths address16-byte row quanta. Low
            // pitch bits are ignored; an aligned zero uses one row quantum.
            // Raw registers remain untouched, including address upper bits.
            // This reference-supported rule is not a new PSP hardware golden.
            if(staged.format_<=5){const unsigned quantum=staged.format_==3?4u:staged.format_==4?32u:staged.format_==5?16u:8u;
                v.stride=std::max(quantum,stride&~(quantum-1u));}
            if(!v.stride || v.stride<v.width || (staged.format_==4 && (v.stride&1)))return fail("invalid mip texture stride");
            if(!indexed){
                auto &prepared=staged.direct_[level];
                if(!prepared.configure(m,v.address,v.stride,v.width,v.height,staged.format_,staged.swizzled_,error)){
                    error+=" at mip level "+std::to_string(level);return false;
                }
                v.byte_pitch=prepared.byte_pitch();v.storage_bytes=prepared.storage_bytes();
            }else{
                v.byte_pitch=staged.format_==4?v.stride/2u:v.stride;
                if(staged.swizzled_ && (v.byte_pitch<16 || (v.byte_pitch&15)))return fail("swizzled mip texture stride unsupported");
                const unsigned rows=staged.swizzled_?((v.height+7)&~7u):v.height;
                v.storage_bytes=v.byte_pitch*rows;
                if(!m.contains(v.address,v.storage_bytes)){error="mip texture storage outside guest memory at level "+std::to_string(level);return false;}const unsigned entries=staged.format_==4?16u:256u;
                for(unsigned i=0;i<entries;++i)
                    if(!palette_color(*cache,i,r(0xC5),multiclut?level*16u:0u,staged.colors_[level][i],error))return false;
            }
        }
        staged.prepared_levels_=required;staged.configured_=true;*this=staged;return true;
    }
    // Caller must finish required-level and target-feedback validation first,
    // retain cache lifetime, and keep sources immutable throughout staging.
    // Sharing one cache between active samplers is outside this contract.
    // A job sampler never borrows another active sampler's mutable cache.
    void disable_immutable_small_dxt_cache() noexcept {immutable_small_dxt_cache_=nullptr;}
    void enable_immutable_small_dxt_cache(ImmutableSmallDxtCache &cache){
        immutable_small_dxt_cache_=nullptr;
        if(configured_ && format_>=8 && format_<=10){cache.reset();immutable_small_dxt_cache_=&cache;}
    }
    bool select(float footprint_texels,float clip_w,GeMipSelection &selection,std::string &error) const {
        if(!configured_){error="mip sampler not configured";return false;}
        int detail=0;
        if(lod_mode_==0){
            if(!std::isfinite(footprint_texels) || footprint_texels<0){error="invalid mip derivative footprint";return false;}
            detail=lod16(footprint_texels);
        }else if(lod_mode_==2){
            const float scale=2.0f*clip_w*slope_;
            if(!std::isfinite(clip_w) || clip_w<0 || !std::isfinite(slope_) || slope_<0 || !std::isfinite(scale)){
                error="invalid mip slope/clip-W state";return false;}
            detail=lod16(scale);
        }
        // Hardware fixture mode 3 has the same behavior as CONST mode 1.
        detail+=bias16_;
        GeMipSelection result;result.detail16=detail;result.linear=(detail>0?min_filter_:mag_filter_)&1;
        if(detail>0 && max_level_){const unsigned clamped=std::min(unsigned(detail),max_level_*16u);
            if(min_filter_&2){result.lower=clamped/16;result.fraction16=clamped%16;
                result.upper=result.lower+(result.fraction16?1u:0u);
            }else result.lower=result.upper=(clamped+8)/16;
        }
        selection=result;return true;
    }
    bool sample(const psprecomp::GuestMemory &m,float normalized_s,float normalized_t,
                const GeMipSelection &selection,unsigned &color,std::string &error) const {
        if(!configured_ || selection.lower>max_level_ || selection.upper>max_level_ || selection.fraction16>15 ||
           selection.upper<selection.lower || (selection.fraction16 && selection.upper!=selection.lower+1)){
            error="invalid prepared mip selection";return false;}
        const unsigned required=(1u<<selection.lower)|(selection.fraction16?(1u<<selection.upper):0u);
        if((prepared_levels_&required)!=required){error="mip selection level not prepared";return false;}
        if(!std::isfinite(normalized_s) || !std::isfinite(normalized_t)){error="nonfinite mip texture coordinate";return false;}
        const unsigned lower=sample_level(m,selection.lower,normalized_s,normalized_t,selection.linear);
        if(!selection.fraction16){color=lower;return true;}
        const unsigned upper=sample_level(m,selection.upper,normalized_s,normalized_t,selection.linear);
        unsigned blended=0;
        for(unsigned ch=0;ch<4;++ch){const unsigned shift=ch*8;
            blended|=((((lower>>shift)&255)*(16-selection.fraction16)+((upper>>shift)&255)*selection.fraction16)/16)<<shift;}
        color=blended;return true;
    }
    unsigned max_level() const{return max_level_;}
    unsigned prepared_levels() const{return prepared_levels_;}
    const GeMipLevel &level(unsigned i) const{return levels_.at(i);}
};
}
