#include "../host/display_runtime.hpp"
#include <stdexcept>
#include <iostream>
int main(){try{psprecomp::Runtime rt;chili::ThreadRuntime threads(rt);threads.initialize_bootstrap();chili::DisplayRuntime display(rt,threads);auto &c=rt.cpu();
if(chili::DisplayRuntime::next_vblank(0)!=16683 || chili::DisplayRuntime::next_vblank(16683)!=33366 || chili::DisplayRuntime::next_vblank(33366)!=50050)throw std::runtime_error("vblank boundaries mismatch");
rt.register_function(0x08804000,[](auto &,auto &ctx){if(ctx.gpr[2]!=0 || ctx.gpr[4]!=123 || ctx.gpr[16]!=0xABCDEF || ctx.gpr[28]!=0x1234 || ctx.gpr[31]!=0x08804000)throw std::runtime_error("vblank context mismatch");ctx.pc=0;},"display_done");
c.gpr[4]=123;c.gpr[16]=0xABCDEF;c.gpr[28]=0x1234;c.gpr[31]=0x08804000;
rt.invoke_import("sceDisplay",0x46F186C3,c);if(threads.now_us()!=16683)throw std::runtime_error("vblank did not wait");rt.run(c.pc,100);if(c.pc!=0)throw std::runtime_error("vblank did not resume");
c.gpr[4]=1;c.gpr[5]=0;c.gpr[6]=0;rt.invoke_import("sceDisplay",0x0E20F177,c);if(c.gpr[2]!=0x80000107)throw std::runtime_error("invalid mode precedence failed");
c.gpr[4]=0;c.gpr[5]=479;c.gpr[6]=272;rt.invoke_import("sceDisplay",0x0E20F177,c);if(c.gpr[2]!=0x80000104)throw std::runtime_error("invalid display size accepted");
rt.register_function(0x08804004,[](auto &,auto &ctx){if(ctx.gpr[2]!=0 || ctx.gpr[4]!=0 || ctx.gpr[5]!=480 || ctx.gpr[6]!=272 || ctx.gpr[16]!=0xABCDEF)throw std::runtime_error("display mode continuation changed");ctx.pc=0;},"mode_done");
c.gpr[4]=0;c.gpr[5]=480;c.gpr[6]=272;c.gpr[31]=0x08804004;rt.invoke_import("sceDisplay",0x0E20F177,c);if(threads.now_us()!=33366)throw std::runtime_error("mode did not wait next vblank");rt.run(c.pc,100);if(c.pc!=0)throw std::runtime_error("mode did not resume");
c.gpr[4]=0x04000000;c.gpr[5]=512;c.gpr[6]=3;c.gpr[7]=1;rt.invoke_import("sceDisplay",0x289D82FE,c);if(c.gpr[2]!=0 || display.framebuffer_address()!=0)throw std::runtime_error("framebuffer latched too early");
rt.register_function(0x08804008,[](auto &,auto &ctx){ctx.pc=0;},"latch_done");c.gpr[31]=0x08804008;rt.invoke_import("sceDisplay",0x46F186C3,c);rt.run(c.pc,100);if(display.framebuffer_address()!=0x04000000)throw std::runtime_error("framebuffer not latched at vblank");
c.gpr[4]=0x04080000;c.gpr[5]=512;c.gpr[6]=3;c.gpr[7]=0;rt.invoke_import("sceDisplay",0x289D82FE,c);if(c.gpr[2]!=0 || display.framebuffer_address()!=0x04080000)throw std::runtime_error("immediate framebuffer failed");
c.gpr[4]=0x04000001;rt.invoke_import("sceDisplay",0x289D82FE,c);if(c.gpr[2]!=0x80000103 || display.framebuffer_address()!=0x04080000)throw std::runtime_error("invalid framebuffer changed display");
std::cout<<"PASS: rational vblank boundaries, scheduler wait and saved-context return; display rendering/callback parity not verified\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
