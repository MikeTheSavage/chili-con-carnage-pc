#include "ge_fog.hpp"
#include "ge_fog_fixture_data.hpp"
#include <iostream>
#include <limits>
#include <vector>
using namespace chili::ge;
static unsigned assertions{};
static void require(bool result,const std::string &why){++assertions;if(!result)throw std::runtime_error(why);}
static void hardware_vectors(const std::array<unsigned,256> &expected){
    for(unsigned factor=0;factor<256;++factor){
        const auto value=apply_ge_fog(0x11881100,0xFFFF33FF,static_cast<std::uint8_t>(factor));
        require((value&0xFFFFFF)==(expected[factor]&0xFFFFFF),"recorded fog byte "+std::to_string(factor));
        require((value>>24)==0x11,"fog must preserve source alpha");
        require(quantize_ge_fog_factor(float(factor)/256.0f)==factor,"exact byte factor quantization");
    }
    const struct Common {float near_value,far_value;unsigned rgb;} common[]={
        {1,1,0xFFFFFF},{-1,1,0xC3C3C3},{1,-1,0xC3C3C3},{-1,-1,0xFFFFFF},{0,1,0x888888},{1,0,0xFFFFFF}};
    for(const auto &c:common){float distance=c.far_value-c.near_value;FogState fog{true,c.far_value,distance?1.0f/distance:0.0f,0xFFFFFF};
        require((apply_ge_fog(0x88888888,fog.rgb,quantize_ge_fog_factor(prepare_ge_fog_factor(fog,0)))&0xFFFFFF)==c.rgb,"recorded near/far common case");}
    require((apply_ge_fog(0x88888888,0xFFFFFF,0)&0xFFFFFF)==0xFFFFFF,"recorded direct00");
    require((apply_ge_fog(0x88888888,0xFFFFFF,128)&0xFFFFFF)==0xC3C3C3,"recorded direct80");
    require((apply_ge_fog(0x88888888,0xFFFFFF,255)&0xFFFFFF)==0x888888,"recorded directFF");
    std::cout<<"PASS all256 recorded hardware fog bytes and9 common cases; alpha preserved\n";
}
static void factors_and_registers(){
    std::array<unsigned,256> r{};r[0x1F]=1;r[0xCD]=std::bit_cast<unsigned>(20.f)>>8;r[0xCE]=std::bit_cast<unsigned>(0.1f)>>8;r[0xCF]=0xF0123456;
    const auto fog=prepare_ge_fog(r);require(fog.enabled&&fog.end==20&&fog.slope==std::bit_cast<float>((r[0xCE]&0xFFFFFF)<<8)&&fog.rgb==0x123456,"float24 and RGB register provenance");
    require(prepare_ge_fog_factor(fog,-20)==0,"signed camera far");require(prepare_ge_fog_factor(fog,0)>1,"raw factor remains unclamped");
    FogState dyadic{true,4,0.25f,0};require(prepare_ge_fog_factor(dyadic,-3)==0.25f,"signed negative camera depth");
    require(prepare_ge_fog_factor(dyadic,3)==1.75f,"positive camera not abs-depth");
    require(quantize_ge_fog_factor(-1)==0&&quantize_ge_fog_factor(2)==255,"fragment clamp");
    for(unsigned q=1;q<256;++q){const float boundary=float(q)/256;
        require(quantize_ge_fog_factor(std::nextafter(boundary,0.f))==q-1,"quantization below threshold");
        require(quantize_ge_fog_factor(boundary)==q,"quantization threshold");}
    require(quantize_ge_fog_factor(std::numeric_limits<float>::denorm_min())==0,"finite tiny factor");
    r[0xCD]=0x7FC000;r[0xCE]=0x7F8000;
    require(!prepare_ge_fog(r,true).enabled,"through bypasses stale NaN/Inf");r[0x1F]=0;
    require(!prepare_ge_fog(r).enabled,"disabled fog ignores unused coefficients");
    std::string error;FogState out{true,17,29,0x123};r[0x1F]=1;
    require(!try_prepare_ge_fog(r,false,out,error)&&out.end==17&&out.slope==29&&out.rgb==0x123,"failed state prepare leaves output untouched");
    float factor_out=19;
    require(!try_prepare_ge_fog_factor(FogState{true,0,2,0},std::numeric_limits<float>::max(),factor_out,error)&&factor_out==19,"overflow leaves factor output untouched");
    require(!try_prepare_ge_fog_factor(dyadic,std::numeric_limits<float>::infinity(),factor_out,error)&&factor_out==19,"nonfinite camera leaves output untouched");
    bool rejected=false;try{quantize_ge_fog_factor(std::numeric_limits<float>::quiet_NaN());}catch(const std::runtime_error&){rejected=true;}require(rejected,"nonfinite fragment strict");
    std::cout<<"PASS signed preprojection camera factor, float24 state, thresholds, through/disabled bypass and strict output atomicity\n";
}
int main(){try{hardware_vectors(chili::fog_fixture::recorded);factors_and_registers();
    std::cout<<"PASS "<<assertions<<" fog assertions; intermediate float24 hardware precision remains unverified\n";
}catch(const std::exception &failure){std::cerr<<"FAIL "<<failure.what()<<'\n';return 1;}}
