#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>

namespace chili {
// Independent stages supported by real-PSP transformprecision inputs/outputs:
// https://github.com/hrydgard/pspautotests/blob/master/tests/gpu/depth/transformprecision.cpp
// https://github.com/hrydgard/pspautotests/blob/master/tests/gpu/depth/transformprecision.expected
// Baseline/range-center vectors additionally use tests/gpu/depth/precision.
// A GE register float has 16 significant bits: IEEE binary32 bits 31..8.
// Matrix-product translation and viewport-center addition discard the low
// significance of BOTH operands at the larger operand's exponent before adding.
// The general GE dot-product accumulation and reciprocal-table interpolation
// are NOT established by those fixtures. This helper does not implement them.
namespace ge_float24_detail {
inline bool ordinary(float v) { return v==0.0f || std::isnormal(v); }
inline bool reject(std::string &error,const char *message) { error=message;return false; }
inline float discard_low_bits(float v) { return std::bit_cast<float>(std::bit_cast<std::uint32_t>(v)&0xFFFFFF00u); }
inline bool supported_result(float v,std::string &error) {
    return ordinary(v) || reject(error,"GPU float24 overflow/denormal result unsupported");
}
}
inline float ge_float24_register(unsigned command) {
    return std::bit_cast<float>((command&0xFFFFFFu)<<8);
}
inline bool ge_float24_truncate(float value,float &result,std::string &error) {
    if(!ge_float24_detail::ordinary(value))return ge_float24_detail::reject(error,"GPU float24 nonfinite/denormal input unsupported");
    result=ge_float24_detail::discard_low_bits(value);return true;
}
inline bool ge_float24_multiply(float a,float b,float &result,std::string &error) {
    using namespace ge_float24_detail;
    if(!ordinary(a)||!ordinary(b))return reject(error,"GPU float24 multiply nonfinite/denormal input unsupported");
    // binary32 operands have at most 24 significant bits; their exact product
    // fits binary64. Quantize directly to 16 bits so host binary32 rounding
    // cannot round across a truncation boundary before the bits are dropped.
    const double exact=double(a)*double(b);
    if(exact==0.0){result=float(exact);return true;}
    int exponent;
    const double mantissa=std::frexp(std::abs(exact),&exponent);
    const double magnitude=std::ldexp(std::floor(mantissa*65536.0),exponent-16);
    const float staged=float(std::copysign(magnitude,exact));
    if(!supported_result(staged,error))return false;
    result=staged;return true;
}
inline bool ge_float24_add(float a,float b,float &result,std::string &error) {
    using namespace ge_float24_detail;
    if(!ordinary(a)||!ordinary(b))return reject(error,"GPU float24 add nonfinite/denormal input unsupported");
    if(a==0.0f && b==0.0f){result=a+b;return true;}
    const unsigned ea=(std::bit_cast<unsigned>(a)>>23)&255u,eb=(std::bit_cast<unsigned>(b)>>23)&255u;
    const int exponent=int(std::max(ea,eb))-127;
    // Each signed significand is independently truncated toward zero after
    // exponent alignment. Consequently 65536 + (-65535) yields 2, not 1.
    const int ma=int(std::trunc(std::ldexp(double(a),15-exponent)));
    const int mb=int(std::trunc(std::ldexp(double(b),15-exponent)));
    const float sum=float(std::ldexp(double(ma+mb),exponent-15));
    const float staged=discard_low_bits(sum); // Carry can require one more bit.
    if(!supported_result(staged,error))return false;
    result=staged;return true;
}
inline bool ge_float24_projection_z(float source_z,float coefficient,float translation,
                                    float &clip_z,std::string &error) {
    if((std::bit_cast<unsigned>(coefficient)|std::bit_cast<unsigned>(translation))&255u)
        return ge_float24_detail::reject(error,"GPU projection coefficients must be decoded register floats");
    float product,staged;
    if(!ge_float24_multiply(source_z,coefficient,product,error)||!ge_float24_add(product,translation,staged,error))return false;
    clip_z=staged;return true;
}
inline bool ge_verified_reciprocal(float clip_w,float &inverse,std::string &error) {
    // W=3's endpoint is constrained by all eight real-PSP depth vectors to
    // 0x3EAAAA00 among every float24 number in [0.25,0.5). It is below ideal
    // 1/3. W=1 is exact. No ideal division/table interpolation fallback exists.
    if(clip_w==1.0f){inverse=1.0f;return true;}
    if(clip_w==3.0f){inverse=std::bit_cast<float>(0x3EAAAA00u);return true;}
    return ge_float24_detail::reject(error,"GPU reciprocal outside verified W=1/W=3 endpoints");
}
struct GeDepthSample {
    float normalized_z{},scaled_z{},screen_z{};
    unsigned depth{};
    bool range_pass{},clamped{};
    bool operator==(const GeDepthSample &) const = default;
};
inline bool ge_depth_with_reciprocal(float clip_z,float inverse_w,float z_scale,float z_center,
                                    unsigned min_depth,unsigned max_depth,bool clamp,
                                    GeDepthSample &sample,std::string &error) {
    if((std::bit_cast<unsigned>(inverse_w)|std::bit_cast<unsigned>(z_scale)|std::bit_cast<unsigned>(z_center))&255u)
        return ge_float24_detail::reject(error,"GPU depth reciprocal/viewport must be decoded float24 values");
    if(inverse_w<=0.0f || min_depth>65535 || max_depth>65535)
        return ge_float24_detail::reject(error,"GPU depth reciprocal/range unsupported");
    float z,normalized,scaled,screen;
    if(!ge_float24_truncate(clip_z,z,error)||!ge_float24_multiply(z,inverse_w,normalized,error)||
       !ge_float24_multiply(normalized,z_scale,scaled,error)||!ge_float24_add(scaled,z_center,screen,error))return false;
    GeDepthSample staged;staged.normalized_z=normalized;staged.scaled_z=scaled;staged.screen_z=screen;
    if(clamp){staged.screen_z=std::clamp(screen,0.0f,65535.0f);staged.clamped=staged.screen_z!=screen;}
    if(staged.screen_z>=0.0f && staged.screen_z<65536.0f){
        staged.depth=unsigned(std::floor(staged.screen_z));
        staged.range_pass=staged.depth>=min_depth && staged.depth<=max_depth;
    }
    sample=staged;return true;
}
inline bool ge_depth_verified(float clip_z,float clip_w,float z_scale,float z_center,
                             unsigned min_depth,unsigned max_depth,bool clamp,
                             GeDepthSample &sample,std::string &error) {
    float inverse;
    if(!ge_verified_reciprocal(clip_w,inverse,error))return false;
    return ge_depth_with_reciprocal(clip_z,inverse,z_scale,z_center,min_depth,max_depth,clamp,sample,error);
}
}
