#include "ge_triangle_raster.hpp"
#include "original_reference.hpp"
#include "psprecomp/runtime.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks=0;
void require(bool value,const char *what){++checks;if(!value)throw std::runtime_error(what);}
struct Fixture{
    psprecomp::Runtime rt;chili::GeClutCache clut;std::array<unsigned,256> regs{};
    std::array<float,12> matrix{1,0,0,0,1,0,0,0,1,0,0,0};std::array<float,16> projection{};
    unsigned cursor=0xDEADBEEF,index=0xBAADF00D,commits=0xACEDFACE;std::string error;
    Fixture(){
        regs[0x12]=0x80019D;regs[0xD2]=3;regs[0x9D]=128;regs[0x9E]=0x80000;regs[0x9F]=128;
        regs[0xD5]=regs[0x16]=95|(95<<10);regs[0xD7]=65535;regs[0x58]=255;
        for(unsigned n=0;n<6;++n)vertex(n,n%3==1?96.0f:0.0f,n%3==2?96.0f:0.0f,20.0f,
            n<3?0xC0875321u:0x91D56319u,n%3==1?15:0,n%3==2?15:0);
        for(unsigned n=0;n<128*96;++n){rt.memory().store32(0x04000000+n*4,0xB1996655);rt.memory().store16(0x04080000+n*2,100);}
        auto &cpu=rt.cpu();cpu.pc=0x08801234;for(unsigned n=1;n<32;++n)cpu.gpr[n]=0x90000000+n;
        for(unsigned n=0;n<cpu.fpr.size();++n)cpu.fpr[n]=std::bit_cast<float>(0x7FC10000+n);
        for(unsigned n=0;n<cpu.vfpu.size();++n)cpu.vfpu[n]=std::bit_cast<float>(0xFFC10000+n);
    }
    void vertex(unsigned n,float x,float y,float z,unsigned color,unsigned u,unsigned v){
        const auto a=0x08804000+n*20;rt.memory().store8(a,std::uint8_t(u));rt.memory().store8(a+1,std::uint8_t(v));
        rt.memory().store16(a+2,0);rt.memory().store32(a+4,color);rt.memory().store32(a+8,std::bit_cast<unsigned>(x));
        rt.memory().store32(a+12,std::bit_cast<unsigned>(y));rt.memory().store32(a+16,std::bit_cast<unsigned>(z));
    }
    void texture(unsigned format,unsigned filter){
        regs[0x1E]=1;regs[0xC3]=format;regs[0xC9]=0x100;regs[0xC2]=0x10000;regs[0xC6]=filter|0x100;
        regs[0xA0]=0x810000;regs[0xA8]=0x080020;regs[0xB8]=4|(4<<8);
        regs[0xA1]=0x820000;regs[0xA9]=0x080020;regs[0xB9]=3|(3<<8);
        regs[0xB0]=0x830000;regs[0xB1]=0x080000;regs[0xC4]=32;regs[0xC5]=0xFF03;
        for(unsigned n=0;n<1024;++n)rt.memory().store8(0x08810000+n,std::uint8_t(n*43+11));
        for(unsigned n=0;n<1024;++n)rt.memory().store8(0x08820000+n,std::uint8_t(n*37+23));
        for(unsigned n=0;n<256;++n)rt.memory().store32(0x08830000+n*4,0xA5000000|n*0x010103);
        require(clut.load(rt.memory(),regs[0xB0],regs[0xB1],regs[0xC4],error),"load synthetic CLUT");
    }
};
bool same_cpu(const psprecomp::AllegrexContext&a,const psprecomp::AllegrexContext&b){return a.gpr==b.gpr&&a.pc==b.pc&&a.hi==b.hi&&a.lo==b.lo&&a.fcr31==b.fcr31&&a.vfpu_ctrl==b.vfpu_ctrl&&std::memcmp(a.fpr.data(),b.fpr.data(),sizeof(a.fpr))==0&&std::memcmp(a.vfpu.data(),b.vfpu.data(),sizeof(a.vfpu))==0;}
void pair(Fixture &f,bool expected_success,bool parallel,bool fallback,unsigned csr_seed=0,int round=FE_TONEAREST,bool ftz=false,bool worker_denormal=false){
    const auto image=f.rt.memory().vram_bytes(),ram=f.rt.memory().bytes();const auto cpu=f.rt.cpu();
    std::fenv_t saved{};require(std::fegetenv(&saved)==0,"capture fenv");const auto saved_csr=_mm_getcsr();
    auto set_env=[&]{require(std::fesetenv(FE_DFL_ENV)==0&&std::fesetround(round)==0,"set test environment");_mm_setcsr((_mm_getcsr()&~0x3Fu)|csr_seed|(ftz?0x8000u:0));};
    set_env();
    const bool a=chili_triangle_reference::render_ge_triangle_raster(f.rt.memory(),f.regs,f.matrix,f.matrix,f.projection,0x08804000,0x30006,f.cursor,f.error,f.clut,&f.commits,0,&f.index);
    std::fenv_t expected_env{};require(std::fegetenv(&expected_env)==0,"reference fenv");const auto expected_csr=_mm_getcsr();
    const auto expected=f.rt.memory().vram_bytes();const auto cursor=f.cursor,index=f.index,commits=f.commits;const auto error=f.error;
    require(a==expected_success,"reference success premise");require(f.rt.memory().bytes()==ram&&same_cpu(f.rt.cpu(),cpu),"reference immutable RAM/CPU");
    f.rt.memory().copy_in(0x04000000,image);f.cursor=0xDEADBEEF;f.index=0xBAADF00D;f.commits=0xACEDFACE;f.error.clear();
    set_env();
    const bool b=chili::render_ge_triangle_raster(f.rt.memory(),f.regs,f.matrix,f.matrix,f.projection,0x08804000,0x30006,f.cursor,f.error,f.clut,&f.commits,0,&f.index);
    std::fenv_t actual_env{};require(std::fegetenv(&actual_env)==0,"candidate fenv");const auto actual_csr=_mm_getcsr();
    require(a==b&&f.rt.memory().vram_bytes()==expected&&f.cursor==cursor&&f.index==index&&f.commits==commits&&f.error==error,"complete image/error/cursors/count parity");
    require(f.rt.memory().bytes()==ram&&same_cpu(f.rt.cpu(),cpu),"candidate immutable RAM/CPU");
    require(actual_env._Fe_ctl==expected_env._Fe_ctl&&actual_env._Fe_stat==expected_env._Fe_stat&&actual_csr==expected_csr,"exact host FP control/status/denormal flag parity");
    require(chili::last_parallel_shade.succeeded==parallel&&chili::last_parallel_shade.fallback==fallback,"intended parallel or fallback route");
    if(worker_denormal)require(!(chili::last_parallel_shade.initial_csr_flags&2u)&&(chili::last_parallel_shade.job_csr_flags&2u)&&(actual_csr&2u),"new worker-only denormal-operand status retained");
    if(!a)require(expected==image&&cursor==0xDEADBEEF&&index==0xBAADF00D&&commits==0xACEDFACE,"failing draw atomicity");
    require(std::fesetenv(&saved)==0,"restore saved fenv");_mm_setcsr(saved_csr);
}
}
int main(){try{
    for(unsigned format:{0u,1u,2u,3u,4u,5u,8u,9u,10u})for(unsigned filter:{0u,1u,4u,5u,6u,7u}){
        Fixture f;f.texture(format,filter);f.regs[0x50]=1;f.regs[0x23]=1;f.regs[0xDE]=6;f.regs[0x21]=1;f.regs[0xDF]=0x2AA;f.regs[0xE0]=0x8F6A43;f.regs[0xE1]=0x192D51;
        pair(f,true,true,false);
    }
    for(unsigned seed:{0u,0x10u,0x02u,0x3Fu}){Fixture f;pair(f,true,true,false,seed);}
    {Fixture f;pair(f,true,false,false,0,FE_DOWNWARD);}
    {Fixture f;pair(f,true,false,false,0,FE_TONEAREST,true);}
    {Fixture f;f.texture(3,1);f.regs[0x12]=0x80019F;
        // Minimum-normal input divided by the power-of-two width yields an
        // exact subnormal UV without a denormal operand in vertex preparation.
        // Worker interpolation then converts that subnormal operand to double.
        for(unsigned n=0;n<6;++n){const auto a=0x08804000+n*24;
            f.rt.memory().store32(a,0x00800000);f.rt.memory().store32(a+4,0);
            f.rt.memory().store32(a+8,0xFFFFFFFF);f.rt.memory().store32(a+12,std::bit_cast<unsigned>(n%3==1?96.0f:0.0f));
            f.rt.memory().store32(a+16,std::bit_cast<unsigned>(n%3==2?96.0f:0.0f));f.rt.memory().store32(a+20,std::bit_cast<unsigned>(20.0f));}
        pair(f,true,true,false,0,FE_TONEAREST,false,true);
    }
    // First triangle stages valid level0. The second needs a deliberately bad
    // level1, so every parallel task retires and original sequential failure
    // is recovered without writing the earlier fragments.
    {Fixture f;f.texture(8,6);f.regs[0xB8]=2|(2<<8);f.regs[0xB9]=1|(1<<8);f.regs[0xA9]=0x080000;
        for(unsigned n=0;n<3;++n)f.vertex(n,n%3==1?96.0f:0.0f,n%3==2?96.0f:0.0f,20,0xFFFFFFFF,0,0);
        for(unsigned n=3;n<6;++n)f.vertex(n,n%3==1?96.0f:0.0f,n%3==2?96.0f:0.0f,20,0xFFFFFFFF,n%3==1?255:0,n%3==2?255:0);
        pair(f,false,false,true);
    }
    // Feedback validation is still performed on selected storage, before any
    // commit; a private task failure uses the untouched original shader path.
    {Fixture f;f.texture(8,0);f.regs[0xA0]=0;f.regs[0xA8]=0x040020;pair(f,false,false,true);}
    std::cout<<"PASS "<<checks<<" parallel shading formats/filter/order/atomicity/FP checks\n";
    return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
