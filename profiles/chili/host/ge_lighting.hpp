#pragma once
// Independent model of measured PSP lighting. No guest writes or latched defaults.
#include "ge_vertex_decoder.hpp"
#include <string>
#include <utility>
namespace chili::ge {
struct LightingResult {
    Color primary{};
    std::array<std::uint8_t,3> secondary{}; // Interpolate independently; add AFTER texture function.
    bool separate_specular{};
};
namespace lighting_detail {
inline float checked(float v){if(!std::isfinite(v))throw std::runtime_error("Nonfinite GE lighting state/result is not prepared");return v;}
inline float dot(const Vec3 &a,const Vec3 &b){return checked(checked(a[0]*b[0])+checked(a[1]*b[1])+checked(a[2]*b[2]));}
inline Vec3 normalized(const Vec3 &v){
    for(float x:v)checked(x);float length=std::sqrt(dot(v,v));
    if(!(length>0))throw std::runtime_error("Zero GE lighting vector needs a verified or retained normal/direction");
    Vec3 out{};for(unsigned n=0;n<3;++n)out[n]=checked(v[n]/length);return out;
}
inline std::uint8_t byte(unsigned color,unsigned channel){return std::uint8_t(color>>(channel*8));}
inline Color rgba(unsigned color){return {byte(color,0),byte(color,1),byte(color,2),byte(color,3)};}
inline bool rgb_present(const Color &c){return c[0]||c[1]||c[2];}
inline unsigned half(unsigned c){return c*2+1;}
inline unsigned term(unsigned material,unsigned light,float multiplier){
    checked(multiplier);if(!(multiplier>0))return 0;
    // Bound before conversion. The scale is a nine-bit half-offset factor.
    unsigned scale=multiplier>=1?512u:unsigned(std::ceil(512.0f*multiplier+1.0f));
    return (half(material)*half(light)*std::min(scale,512u))>>19;
}
} // namespace lighting_detail
// Quantization applies to MATERIAL_SPECULAR_COEF, not the spotlight exponent.
inline float ge_specular_degree(float exponent){
    lighting_detail::checked(exponent);
    return std::bit_cast<float>(std::bit_cast<std::uint32_t>(exponent)&0xFFF80000u);
}
// Independent piecewise-linear log2/exp2 construction (Mitchell approximation).
// Positive finite normal numbers and bounded results only; no ideal std::pow fallback.
inline float ge_lighting_power(float x,float exponent){
    using lighting_detail::checked;checked(x);checked(exponent);
    if(exponent<=0)return 1;if(x<=0)return 0;
    if(std::fpclassify(x)==FP_SUBNORMAL)throw std::runtime_error("Subnormal GE lighting power requires hardware validation");
    int binary_exponent{};float mantissa=std::frexp(x,&binary_exponent);
    float linear_log=float(binary_exponent-1)+(mantissa*2-1);
    float logarithm=checked(exponent*linear_log);
    if(logarithm<-126 || logarithm>=127)throw std::runtime_error("GE lighting power overflow/underflow stages require hardware validation");
    int whole=int(std::floor(logarithm));
    return checked(std::ldexp(1.0f+(logarithm-float(whole)),whole));
}
inline unsigned add_ge_separate_specular(unsigned textured_color,const std::array<std::uint8_t,3> &secondary){
    unsigned output=textured_color&0xFF000000u;
    for(unsigned n=0;n<3;++n)output|=std::min(255u,unsigned(lighting_detail::byte(textured_color,n))+secondary[n])<<(n*8);
    return output;
}
// Inputs are known world position/normal BEFORE NORMAL_REVERSE. Normals missing
// from the source may be supplied only when a caller has actually retained their state.
// This helper never invents a normal, applies bones/world matrices, or generates UVs.
inline LightingResult prepare_ge_lighting(const std::array<unsigned,256> &commands,
    const std::array<float,12> &view,const Vec3 &world_position,const std::optional<Vec3> &known_world_normal,
    const std::optional<Color> &source_color,bool through=false){
    using namespace lighting_detail;
    auto reg=[&](unsigned i){return commands[i]&0xFFFFFFu;};
    auto number=[&](unsigned i){return checked(std::bit_cast<float>(reg(i)<<8));};
    auto vector=[&](unsigned i){return Vec3{number(i),number(i+1),number(i+2)};};
    const Color material_ambient=rgba(reg(0x55)|((reg(0x58)&255)<<24));
    LightingResult result{};
    if(through || !(reg(0x17)&1)){result.primary=source_color.value_or(material_ambient);return result;}
    if(reg(0x53)&~7u || reg(0x5E)>1 || reg(0x51)>1)throw std::runtime_error("Unknown active GE lighting/material mode");
    const unsigned flags=source_color?reg(0x53)&7u:0u;
    const Color ambient=(flags&1)?*source_color:material_ambient;
    const Color diffuse=(flags&2)?*source_color:rgba(reg(0x56));
    const Color specular=(flags&4)?*source_color:rgba(reg(0x57));
    const Color global=rgba(reg(0x5C)|((reg(0x5D)&255)<<24));
    const Color emission=rgba(reg(0x54));
    std::array<unsigned,4> primary{};std::array<unsigned,3> secondary{};
    for(unsigned channel=0;channel<4;++channel)
        primary[channel]=(half(ambient[channel])*half(global[channel]))>>10;
    for(unsigned channel=0;channel<3;++channel)primary[channel]+=emission[channel];
    std::optional<Vec3> normal,viewer;std::optional<float> degree;
    auto required_normal=[&]() -> const Vec3&{
        if(!normal){
            if(!known_world_normal)throw std::runtime_error("GE diffuse/specular lighting requires a known source or retained world normal");
            normal=normalized(*known_world_normal);
            if(reg(0x51)&1)for(float &value:*normal)value=-value;
        }return *normal;
    };
    auto required_view=[&]() -> const Vec3&{
        if(!viewer)viewer=normalized(Vec3{view[2],view[5],view[8]});return *viewer;
    };
    auto required_degree=[&](){if(!degree)degree=ge_specular_degree(number(0x5B));return *degree;};
    for(unsigned light=0;light<4;++light){
        if(!(reg(0x18+light)&1))continue;
        unsigned mode=reg(0x5F+light),kind=mode&3,type=(mode>>8)&3;
        if((mode&~0x303u)||kind>2||type>2)throw std::runtime_error("Unknown enabled GE light type/component");
        const Color light_ambient=rgba(reg(0x8F+light*3)),light_diffuse=rgba(reg(0x90+light*3)),light_specular=rgba(reg(0x91+light*3));
        const bool use_ambient=rgb_present(ambient)&&rgb_present(light_ambient);
        const bool use_diffuse=rgb_present(diffuse)&&rgb_present(light_diffuse);
        const bool use_specular=kind==1&&rgb_present(specular)&&rgb_present(light_specular);
        if(!use_ambient&&!use_diffuse&&!use_specular)continue;
        Vec3 displacement=vector(0x63+light*3);float multiplier=1;
        if(type){
            for(unsigned n=0;n<3;++n)displacement[n]=checked(displacement[n]-checked(world_position[n]));
            float distance=std::sqrt(dot(displacement,displacement));
            if(!(distance>0))throw std::runtime_error("Coincident GE point/spot light behavior requires hardware validation");
            auto attenuation=vector(0x7B+light*3);
            float denominator=checked(attenuation[0]+checked(attenuation[1]*distance)+checked(attenuation[2]*checked(distance*distance)));
            if(!denominator)throw std::runtime_error("Zero GE light attenuation denominator requires hardware validation");
            multiplier=std::clamp(checked(1.0f/denominator),0.0f,1.0f);
        }
        Vec3 direction=normalized(displacement);
        if(type==2){
            auto spot_direction=normalized(vector(0x6F+light*3));
            float cutoff=number(0x8B+light),exponent=number(0x87+light);
            float cosine=dot(spot_direction,direction);
            multiplier=cosine>=cutoff?checked(multiplier*ge_lighting_power(cosine,exponent)):0;
        }
        if(!(multiplier>0))continue;
        if(use_ambient)for(unsigned n=0;n<3;++n)primary[n]+=term(ambient[n],light_ambient[n],multiplier);
        float diffuse_dot=0;
        if(use_diffuse||use_specular)diffuse_dot=dot(direction,required_normal());
        float diffuse_factor=kind==2&&use_diffuse?ge_lighting_power(diffuse_dot,required_degree()):diffuse_dot;
        if(use_diffuse&&diffuse_factor>0)for(unsigned n=0;n<3;++n)primary[n]+=term(diffuse[n],light_diffuse[n],checked(multiplier*diffuse_factor));
        if(use_specular&&diffuse_dot>=0){
            Vec3 half_vector{};const auto &v=required_view();for(unsigned n=0;n<3;++n)half_vector[n]=checked(direction[n]+v[n]);
            float specular_factor=ge_lighting_power(dot(normalized(half_vector),required_normal()),required_degree());
            if(specular_factor>0)for(unsigned n=0;n<3;++n)secondary[n]+=term(specular[n],light_specular[n],checked(multiplier*specular_factor));
        }
    }
    result.separate_specular=reg(0x5E)&1;
    for(unsigned n=0;n<3;++n){
        if(!result.separate_specular)primary[n]+=secondary[n];
        result.secondary[n]=result.separate_specular?std::uint8_t(std::min(255u,secondary[n])):0;
    }
    for(unsigned n=0;n<4;++n)result.primary[n]=std::uint8_t(std::min(255u,primary[n]));
    return result;
}
inline bool try_prepare_ge_lighting(const std::array<unsigned,256> &commands,const std::array<float,12> &view,
    const Vec3 &position,const std::optional<Vec3> &normal,const std::optional<Color> &color,
    LightingResult &result,std::string &error,bool through=false){
    try{auto prepared=prepare_ge_lighting(commands,view,position,normal,color,through);result=prepared;return true;}
    catch(const std::exception &failure){error=failure.what();return false;}
}
} // namespace chili::ge
