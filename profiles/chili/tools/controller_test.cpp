#include "../host/controller_runtime.hpp"
#include <iostream>
#include <stdexcept>
namespace {
constexpr auto output=0x08802000u;
void check(bool ok,const char *s){if(!ok)throw std::runtime_error(s);}
}
int main(){try{
    psprecomp::Runtime rt;chili::ThreadRuntime threads(rt);threads.initialize_bootstrap();unsigned polls=0;
    chili::ControllerRuntime controller(rt,threads,[&]{++polls;return chili::ControllerInput{0xFFFFFFFFu,17u,240u};});
    auto &c=rt.cpu();auto call=[&](std::uint32_t nid){rt.invoke_import("sceCtrl",nid,c);};
    c.gpr[4]=output;c.gpr[5]=1;call(0x1F803938u);
    check(c.gpr[2]==1 && rt.memory().load32(output)==0u && rt.memory().load32(output+4)==0xF3F9u,"digital sample layout mismatch");
    check(rt.memory().load8(output+8)==128u && rt.memory().load8(output+9)==128u,"digital sample was not centered");
    for(unsigned i=10;i<16;++i)check(rt.memory().load8(output+i)==0u,"reserved bytes uninitialized");
    c.gpr[4]=1;call(0x1F4011E6u);check(c.gpr[2]==0u,"mode setter did not return old mode");c.gpr[4]=2;call(0x1F4011E6u);check(c.gpr[2]==0x80000107u,"invalid mode accepted");
    rt.register_function(0x08804000u,[](auto &runtime,auto &ctx){
        check(ctx.gpr[2]==1 && ctx.gpr[16]==0xABCDEFu && ctx.gpr[28]==0x1234u,"read continuation registers changed");
        check(runtime.memory().load32(output)==16667u && runtime.memory().load32(output+4)==0xF3F9u,"blocked sample layout mismatch");
        check(runtime.memory().load8(output+8)==17 && runtime.memory().load8(output+9)==240,"analog values lost");ctx.pc=0;
    },"controller_done");
    c.gpr[4]=output;c.gpr[5]=1;c.gpr[16]=0xABCDEFu;c.gpr[28]=0x1234u;c.gpr[31]=0x08804000u;call(0x1F803938u);
    check(polls==1 && threads.now_us()==16667u,"read did not wait before polling");rt.run(c.pc,100u);check(polls==2,"blocked read did not poll once on resume");
    c.gpr[4]=output;c.gpr[5]=65;call(0x1F803938u);check(c.gpr[2]==0x80000104u,"oversize sample count accepted");
    std::cout<<"PASS: controller sample layout/mask/reserved bytes, digital/analog modes, count validation and timed read/context preservation\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
