#include "ge_blend.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks{};
void check(bool value, const char *why) { ++checks; if (!value) throw std::runtime_error(why); }
unsigned gray(unsigned rgb, unsigned alpha) { return (alpha << 24) | rgb * 0x010101u; }
double reference_factor(unsigned f, unsigned other, unsigned sa, unsigned da, unsigned fixed) {
    // Independent scalar definition; double represents all byte and half-byte
    // inputs/products exactly. This fixture does not call the production factor.
    const double factors[]{double(other), 255.0-other, double(sa), 255.0-sa,
        double(da), 255.0-da, 2.0*sa, std::max(255.0-2.0*sa,0.0),
        2.0*da, std::max(255.0-2.0*da,0.0), double(fixed)};
    return factors[f];
}
unsigned reference(unsigned mode,unsigned s,unsigned d,unsigned fa,unsigned fb) {
    unsigned result{};
    for(unsigned c=0;c<3;++c) {
        const unsigned shift=c*8, sc=(s>>shift)&255, dc=(d>>shift)&255;
        const auto fs=reference_factor(mode&15,dc,s>>24,d>>24,(fa>>shift)&255);
        const auto fd=reference_factor((mode>>4)&15,sc,s>>24,d>>24,(fb>>shift)&255);
        const double a=std::floor((sc+0.5)*(fs+0.5)/256.0);
        const double b=std::floor((dc+0.5)*(fd+0.5)/256.0);
        const unsigned eq=(mode>>8)&7;
        const double v=eq==1?a-b:eq==2?b-a:a+b;
        result|=unsigned(std::clamp(v,0.0,255.0))<<shift;
    }
    return result;
}
void observed_factors() {
    // Independently transcribed RGB numeric observations from the primary PSP
    // blend autotest's Factors section. Its AA stencil replacement is excluded.
    constexpr unsigned source[]{0x20,0x60,0x48,0x37,0x28,0x58,0x90,0x00,0x50,0x2F,0x40};
    constexpr unsigned destination[]{0x20,0x20,0x24,0x1C,0x14,0x2C,0x48,0x00,0x28,0x18};
    for(unsigned f=0;f<11;++f) {
        auto state=chili::ge::blend::prepare(true,0xA0|f,0x808080,0);
        check(chili::ge::blend::rgb(state,0x90808080,0x50404040)==source[f]*0x010101,
              "recorded source factor RGB observation");
        if(f<10) {
            state=chili::ge::blend::prepare(true,(f<<4)|10,0,0);
            check(chili::ge::blend::rgb(state,0x90808080,0x50404040)==destination[f]*0x010101,
                  "recorded destination factor RGB observation");
        }
    }
    // Common Alpha + Inverse alpha is kept in caller-owned legacy32, but this
    // pure factor formula must explain its recorded RGB without changing it.
    check(reference(0x32,0xEEDDCCBB,0x44444444,0,0)==0xD2C2B2,"recorded legacy32 arithmetic provenance");
    std::cout<<"PASS21 recorded source/destination factor RGB vectors and common alpha reference\n";
}
void observed_doubling() {
    struct V{unsigned mode,s,d,rgb;};
    constexpr V values[]{
      {0xA6,0x80808080,0x40404040,0x80},{0xA6,0x80909090,0x40404040,0x90},
      {0xA6,0x90808080,0x40404040,0x90},{0xA6,0xFF707070,0x40404040,0xE0},
      {0xA6,0xFF909090,0x40404040,0xFF},{0xA6,0x7FFFFFFF,0x40404040,0xFE},
      {0xA6,0xFF7F7F7F,0x40404040,0xFE},
      {0xA7,0x80808080,0x40404040,0},{0xA7,0x80909090,0x40404040,0},
      {0xA7,0x90808080,0x40404040,0},{0xA7,0x40808080,0x40404040,0x3F},
      {0xA7,0xFF1188FF,0x40404040,0},{0xA7,0x7FFFFFFF,0x40404040,1},
      {0xA7,0xFF7F7F7F,0x40404040,0},
      {0xA8,0x80808080,0x40404040,0x40},{0xA8,0x80808080,0x90404040,0x90},
      {0xA8,0x80909090,0x40404040,0x48},{0xA8,0x70707070,0xFF404040,0xE0},
      {0xA8,0x90909090,0xFF404040,0xFF},{0xA8,0xFFFFFFFF,0x7F404040,0xFE},
      {0xA8,0x7F7F7F7F,0xFF404040,0xFE},
      {0xA9,0x80808080,0x40404040,0x3F},{0xA9,0x80909090,0x90404040,0},
      {0xA9,0x80808080,0x40404040,0x3F},{0xA9,0x80808080,0xFF404040,0},
      {0xA9,0xFF1188FF,0xFF404040,0},{0xA9,0xFFFFFFFF,0x7F404040,1},
      {0xA9,0x7F7F7F7F,0xFF404040,0},
    };
    for(const auto &v:values) {
        const auto state=chili::ge::blend::prepare(true,v.mode,0,0);
        check(chili::ge::blend::rgb(state,v.s,v.d)==v.rgb*0x010101,"recorded doubled-alpha boundary RGB");
    }
    // Truncate each >255 term before subtraction, then clamp the final equation.
    check(chili::ge::blend::rgb(chili::ge::blend::prepare(true,0x186,0,0),gray(200,255),gray(190,255))==0x141414,
          "unclamped doubled terms subtract before final saturation");
    check(chili::ge::blend::rgb(chili::ge::blend::prepare(true,0x286,0,0),gray(190,255),gray(200,255))==0x141414,
          "unclamped doubled terms reverse subtract before final saturation");
    std::cout<<"PASS28 recorded doubled/inverse doubled alpha vectors and unclamped subtract terms\n";
}
void exhaustive_terms() {
    for(unsigned f=0;f<11;++f)for(unsigned c=0;c<256;++c)for(unsigned a=0;a<256;++a) {
        const unsigned source=gray(c,a), destination=gray(a,a), fixed=gray(a,0);
        const unsigned mode=0xA0|f;
        check(chili::ge::blend::rgb(chili::ge::blend::prepare(true,mode,fixed,0),source,destination)
              ==reference(mode,source,destination,fixed,0),"all65536 source color/factor dyadics");
        const unsigned dst_mode=(f<<4)|10, s=gray(a,a), d=gray(c,a);
        check(chili::ge::blend::rgb(chili::ge::blend::prepare(true,dst_mode,0,fixed),s,d)
              ==reference(dst_mode,s,d,0,fixed),"all65536 destination color/factor dyadics");
    }
    for(unsigned eq=0;eq<3;++eq)for(unsigned sf=0;sf<11;++sf)for(unsigned df=0;df<11;++df) {
        const unsigned mode=(eq<<8)|(df<<4)|sf;
        if(mode==0x32)continue;
        for(unsigned a:{0u,1u,64u,127u,128u,254u,255u}) {
            const unsigned s=(a<<24)|0x19C7FF,d=((255-a)<<24)|0xD07F01, fa=0x20FF80,fb=0x7F01FE;
            check(chili::ge::blend::rgb(chili::ge::blend::prepare(true,mode,fa,fb),s,d)==reference(mode,s,d,fa,fb),
                  "all documented factor pairs/equations asymmetric channels");
        }
    }
    std::cout<<"PASS all65536 color/factor dyadics for each source/destination factor and all362 admitted mode pairs\n";
}
void strict_gates() {
    for(unsigned mode:{0x32u,0xB0u,0xABu,0x3AAu,0x4AAu,0x5AAu,0x800u,0x1000AAu,0xDF0000A0u}) {
        bool rejected=false;try{(void)chili::ge::blend::prepare(true,mode,0,0);}catch(const std::runtime_error&){rejected=true;}
        check(rejected,"strict unknown/reserved/equation and caller-owned32 gate");
    }
    check(chili::ge::blend::rgb(chili::ge::blend::prepare(false,0xFFFFFFFF,0,0),0xAB123456,0)==0x123456,
          "disabled unused mode untouched");
}
}
int main(){try{observed_factors();observed_doubling();exhaustive_terms();strict_gates();
    std::cout<<"PASS "<<checks<<" general blend assertions; recorded RGB observations plus derived arithmetic, no new PSP hardware run\n";
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
