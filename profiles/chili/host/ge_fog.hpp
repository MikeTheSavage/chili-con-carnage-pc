#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace chili::ge {
// Independent finite-state implementation of the GU fog protocol. CD/CE are
// float24 end/slope. Factor is signed camera Z, before projection/depth mapping.
// Keep the raw factor through clipping and affine screen interpolation; clamp
// only at the fragment. Exact GPU float24 intermediate precision is unverified.
struct FogState {
    bool enabled{};
    float end{},slope{};
    std::uint32_t rgb{};
};
inline FogState prepare_ge_fog(const std::array<unsigned,256> &regs,bool through=false) {
    FogState state;
    state.enabled=(regs[0x1F]&1u)!=0 && !through;
    if(!state.enabled)return state; // Through bypasses fog, including stale values.
    state.end=std::bit_cast<float>((regs[0xCD]&0xFFFFFFu)<<8);
    state.slope=std::bit_cast<float>((regs[0xCE]&0xFFFFFFu)<<8);
    state.rgb=regs[0xCF]&0xFFFFFFu;
    if(!std::isfinite(state.end)||!std::isfinite(state.slope))
        throw std::runtime_error("nonfinite GE fog coefficients unsupported");
    return state;
}
inline float prepare_ge_fog_factor(const FogState &state,float camera_z) {
    if(!state.enabled)return 1.0f;
    if(!std::isfinite(camera_z))throw std::runtime_error("nonfinite GE fog camera Z");
    const float factor=(camera_z+state.end)*state.slope;
    if(!std::isfinite(factor))throw std::runtime_error("GE fog factor overflow unsupported");
    return factor;
}
inline std::uint8_t quantize_ge_fog_factor(float factor) {
    if(!std::isfinite(factor))throw std::runtime_error("nonfinite GE fog interpolation unsupported");
    if(factor<=0.0f)return 0;
    if(factor>=1.0f)return 255;
    return static_cast<std::uint8_t>(factor*256.0f);
}
inline std::uint32_t apply_ge_fog(std::uint32_t rgba,std::uint32_t fog_rgb,std::uint8_t factor) {
    std::uint32_t out=rgba&0xFF000000u;
    // Hardware command fixture verifies every factor byte. RGB uses the two
    // complementary weights summing to255, a +255 bias, and division by256.
    for(unsigned channel=0;channel<3;++channel){
        const unsigned shift=channel*8u;
        const unsigned source=(rgba>>shift)&255u,fog=(fog_rgb>>shift)&255u;
        out|=((source*factor+fog*(255u-factor)+255u)/256u)<<shift;
    }
    return out;
}
inline bool try_prepare_ge_fog(const std::array<unsigned,256> &regs,bool through,FogState &out,std::string &error) {
    try{const auto prepared=prepare_ge_fog(regs,through);out=prepared;return true;}
    catch(const std::runtime_error &failure){error=failure.what();return false;}
}
inline bool try_prepare_ge_fog_factor(const FogState &state,float camera_z,float &out,std::string &error) {
    try{const float prepared=prepare_ge_fog_factor(state,camera_z);out=prepared;return true;}
    catch(const std::runtime_error &failure){error=failure.what();return false;}
}
}
