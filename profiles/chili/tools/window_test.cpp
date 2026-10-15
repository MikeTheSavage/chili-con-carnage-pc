#include "../host/presentation.hpp"
#include <iostream>
int main(){try{psprecomp::GuestMemory memory;std::vector<unsigned> frame;
const unsigned fb=0x04000000;
for(unsigned y=0;y<272;++y)for(unsigned x=0;x<512;++x)memory.store32(fb+(y*512+x)*4,0xFFFF0000);
memory.store32(fb,0xFF0000FF);memory.store32(fb+(271*512+479)*4,0xFF00FF00);
if(!chili::decode_display_frame(memory,fb,512,3,frame) || frame[0]!=0xFFFF0000 || frame.back()!=0xFF00FF00 || frame[480]!=0xFF0000FF)throw std::runtime_error("RGBA display channels/stride/top-down conversion failed");
for(unsigned fmt=0;fmt<3;++fmt){memory.store16(fb,fmt==0?0xF800:fmt==1?0x7C00:0x0F00);
if(!chili::decode_display_frame(memory,fb,512,fmt,frame) || frame[0]!=0xFF0000FF)throw std::runtime_error("16-bit framebuffer format conversion failed");}
if(chili::decode_display_frame(memory,fb,64,3,frame) || chili::decode_display_frame(memory,0x03000000,512,3,frame))throw std::runtime_error("invalid display memory accepted");
chili::PcWindow window(false);if(!window.handle() || !window.present(memory,fb,512,3) || !window.pump())throw std::runtime_error("native window/presentation smoke test failed");

const auto capture=std::filesystem::temp_directory_path()/("chili-frame-test-"+std::to_string(GetCurrentProcessId())+".bmp");
if(!window.save_frame(capture) || std::filesystem::file_size(capture)!=54+480u*272u*4u)throw std::runtime_error("frame capture failed");
std::ifstream bmp(capture,std::ios::binary);std::array<unsigned char,58> head{};bmp.read(reinterpret_cast<char*>(head.data()),head.size());bmp.close();std::filesystem::remove(capture);
chili::decode_display_frame(memory,fb,512,3,frame);
if(head[0]!='B' || head[1]!='M' || head[10]!=54 || head[18]!=0xE0 || head[19]!=1 || head[22]!=0xF0 || head[23]!=0xFE || head[28]!=32)throw std::runtime_error("frame capture BMP layout failed");
for(unsigned ch=0;ch<4;++ch)if(head[54+ch]!=((frame[0]>>(8*ch))&255))throw std::runtime_error("capture differs from presented pixels");
SendMessageW(window.handle(),WM_CLOSE,0,0);if(window.pump() || window.handle())throw std::runtime_error("native close lifecycle failed");

{
psprecomp::Runtime rt;chili::ThreadRuntime threads(rt);threads.initialize_bootstrap();chili::DisplayRuntime display(rt,threads);chili::Presentation presenter(rt,display,threads,false);
presenter.tick(true);if(presenter.window_handle())throw std::runtime_error("presentation opened window before display configuration");
rt.register_function(0x08805000,[](auto &r,auto &){r.stop("display fixture resumed");},"fixture_return");
auto &c=rt.cpu();c.gpr[4]=0;c.gpr[5]=480;c.gpr[6]=272;c.gpr[31]=0x08805000;
rt.invoke_import("sceDisplay",0x0E20F177,c);rt.run(c.pc,20);
c.gpr[4]=fb;c.gpr[5]=512;c.gpr[6]=3;c.gpr[7]=0;rt.invoke_import("sceDisplay",0x289D82FE,c);
presenter.tick(true);if(!presenter.window_handle() || presenter.presented_frames()!=1)throw std::runtime_error("configured guest framebuffer did not present");
const auto snap=display.snapshot();if(snap.address!=fb || snap.stride!=512 || snap.format!=3 || !snap.configured)throw std::runtime_error("display snapshot mismatch");
rt.memory().store32(0x08806000,0x00626975);c.gpr[4]=0x08806000;c.gpr[5]=0x08807000;c.gpr[6]=0;
rt.invoke_import("ThreadManForUser",0xE81CAF8Fu,c);const unsigned exit_cb=c.gpr[2];
c.gpr[4]=exit_cb;rt.invoke_import("LoadExecForUser",0x4AC57943u,c);
SendMessageW(presenter.window_handle(),WM_CLOSE,0,0);presenter.tick(true);
c.gpr[4]=exit_cb;rt.invoke_import("ThreadManForUser",0x2A3D44FFu,c);
if(c.gpr[2]!=1 || presenter.window_handle())throw std::runtime_error("window close did not notify registered game exit callback");
presenter.tick(true);c.gpr[4]=exit_cb;rt.invoke_import("ThreadManForUser",0x2A3D44FFu,c);
if(c.gpr[2]!=1)throw std::runtime_error("window close repeatedly notified exit callback");

}
std::cout<<"PASS: framebuffer channels/stride/565/5551/4444/bounds and hidden native window present/close\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
