#include "psprecomp/runtime.hpp"
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

using Words=std::array<std::uint32_t,4>;
constexpr std::uint32_t untouched=0xDEADBEEFu;
constexpr std::uint32_t ret=0x08808000u;
struct Form { unsigned index,source,destination,length,output_length; };
constexpr Form forms[]{
    {0,2,2,4,1},{1,3,3,4,1},{2,32,8,4,1},{3,0,4,4,1},
    {4,1,8,4,2},{5,1,8,4,2},{6,1,8,2,1},{7,1,8,2,1},
    {8,0,0,4,1},{9,0,0,4,1},{11,3,4,4,1},{12,1,44,4,2},
};
void require(bool ok,const char*message){if(!ok)throw std::runtime_error(message);}
void set_source(psprecomp::AllegrexContext &c,const Form &form,const Words &words){
    float inputs[4]{};
    for(unsigned i=0;i<4;i++)inputs[i]=std::bit_cast<float>(words[i]);
    c.write_vfpu_vector(inputs,form.source,form.length);
}
std::uint32_t result(const psprecomp::AllegrexContext &c,const Form &f,unsigned lane){
    return std::bit_cast<std::uint32_t>(c.vfpu[psprecomp::AllegrexContext::vfpu_vector_lane_index(f.destination,f.output_length,lane)]);
}
psprecomp::AllegrexContext run(const Form &f,const Words &input,unsigned pfxs=0xE4,unsigned pfxd=0){
    psprecomp::Runtime rt;
    psprecomp::register_generated_functions(rt);
    auto &c=rt.cpu();c.vfpu.fill(std::bit_cast<float>(untouched));
    set_source(c,f,input);c.vfpu_ctrl[0]=pfxs;c.vfpu_ctrl[1]=0xFFFFFu;c.vfpu_ctrl[2]=pfxd;
    c.vfpu_ctrl[3]=0xBADC0FFEu;c.gpr[31]=ret;
    const auto before=c.vfpu;
    rt.run(0x08804000u+f.index*0x20u,1);
    require(c.pc==ret,"Instruction did not return normally");
    require(c.vfpu_ctrl[0]==0xE4 && c.vfpu_ctrl[1]==0xE4 && c.vfpu_ctrl[2]==0,"Packing did not consume S/T/D prefixes");
    require(c.vfpu_ctrl[3]==0xBADC0FFE,"Packing changed condition flags");
    // Check every physical lane outside the reduced destination register.
    for(unsigned i=0;i<128;i++){
        bool destination=false;
        for(unsigned lane=0;lane<f.output_length;lane++)
            destination|=i==psprecomp::AllegrexContext::vfpu_vector_lane_index(f.destination,f.output_length,lane);
        if(!destination)require(std::bit_cast<unsigned>(before[i])==std::bit_cast<unsigned>(c.vfpu[i]),"Packing corrupted neighboring VFPU lanes");
    }
    return c;
}
void check(const Form &f,const Words &in,Words expected,unsigned pfxs=0xE4,unsigned pfxd=0){
    auto c=run(f,in,pfxs,pfxd);
    for(unsigned lane=0;lane<f.output_length;lane++)
        if(result(c,f,lane)!=expected[lane])throw std::runtime_error("Wrong packed output in fixture "+std::to_string(f.index)+" lane "+std::to_string(lane));
}
void strict_stop(unsigned index,unsigned pfxs=0xE4,unsigned pfxd=0){
    psprecomp::Runtime rt;psprecomp::register_generated_functions(rt);
    auto &c=rt.cpu();c.vfpu.fill(std::bit_cast<float>(untouched));
    c.gpr[31]=ret;c.vfpu_ctrl[0]=pfxs;c.vfpu_ctrl[1]=0xABCDE;c.vfpu_ctrl[2]=pfxd;
    const auto before=c.vfpu;const auto controls=c.vfpu_ctrl;
    rt.run(0x08804000u+index*0x20u,1);
    require(rt.stop_reason().find("VI2X")!=std::string::npos,"Unverified form did not fail visibly");
    require(c.pc!=ret,"Unverified form pretended to complete");
    require(c.vfpu_ctrl==controls,"Unverified form consumed pending prefixes");
    for(unsigned i=0;i<128;i++)require(std::bit_cast<unsigned>(before[i])==std::bit_cast<unsigned>(c.vfpu[i]),"Unverified form changed VFPU state");
}
int main(){try{
    // One vector from PSP hardware's cpu/vfpu/convert oracle: its signed
    // inputs also distinguish integer-bit interpretation from float conversion.
    const Words hardware{0xFCCF00FFu,0x1100FF11u,0xFFFFFFFFu,0x7F7F7F7Fu};
    check(forms[0],hardware,{0x7FFF11FCu});
    check(forms[1],hardware,{0xFE002200u});
    check(forms[4],hardware,{0x1100FCCFu,0x7F7FFFFFu});
    check(forms[5],hardware,{0x22010000u,0xFEFE0000u});
    check(forms[6],hardware,{0x1100FCCFu});
    check(forms[7],hardware,{0x22010000u});

    // Thresholds prove truncation/bit extraction, negative clipping and lane order.
    const Words boundaries{0x007FFFFFu,0x00800000u,0x7FFFFFFFu,0x80000000u};
    check(forms[0],boundaries,{0x807F0000u});
    check(forms[1],boundaries,{0x00FF0100u});
    check(forms[2],boundaries,{0x807F0000u}); // row register input
    check(forms[3],boundaries,{0x00FF0100u});
    check(forms[8],boundaries,{0x807F0000u}); // overlapping source/destination
    check(forms[9],boundaries,{0x00FF0100u});
    const Words short_bounds{0x00007FFFu,0x00008000u,0x7FFFFFFFu,0x80000000u};
    check(forms[4],short_bounds,{0x00000000u,0x80007FFFu});
    check(forms[5],short_bounds,{0x00010000u,0x0000FFFFu});
    check(forms[6],short_bounds,{0x00000000u});
    check(forms[7],short_bounds,{0x00010000u});
    check(forms[11],short_bounds,{0x00000000u,0x80007FFFu}); // row output

    // Full reversed swizzle, then repeated lane, with input snapshot before writes.
    const Words bytes{0x01000000u,0x02000000u,0x03000000u,0x04000000u};
    check(forms[8],bytes,{0x01020304u},0x1B);
    check(forms[8],bytes,{0x02020202u},0x55);
    check(forms[6],hardware,{0xFCCF1100u},0xE1); // pair swap, remaining prefix lanes idle

    // Only output mask lane0/1 applies; unused D mask/sat-selector bits do not.
    check(forms[3],bytes,{untouched},0xE4,0x100);
    check(forms[4],hardware,{untouched,0x7F7FFFFFu},0xE4,0x100);
    check(forms[4],hardware,{0x1100FCCFu,untouched},0xE4,0x200);
    check(forms[3],bytes,{0x08060402u},0xE4,0xEAA); // sat-code2 disables saturation

    // Rejection covers undocumented sizes and disputed prefix modes; no bypass.
    strict_stop(10);strict_stop(13);
    strict_stop(6,0xE2); // nonconstant z swizzle in a pair
    strict_stop(0,0x100E4); // S negate
    strict_stop(0,0x1E4); // S absolute
    strict_stop(0,0x10E4); // S constant
    strict_stop(0,0xE4,1); // D [0,1] saturation
    strict_stop(0,0xE4,3); // D [-1,1] saturation
    std::cout<<"PASS: generated VI2C/UC/US/S hardware golden vector, edge thresholds, raw-bit values, endian lanes, scalar/pair outputs, row/column views, overlap, swizzles, masks, prefix consumption, strict unverified-mode stops\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
