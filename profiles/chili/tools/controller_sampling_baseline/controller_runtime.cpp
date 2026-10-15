#include "controller_runtime.hpp"
#include "psprecomp/common.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>
#include <algorithm>
#include <iostream>
namespace chili {
ControllerRuntime *ControllerRuntime::active_{};
ControllerInput poll_host_controller(){
    ControllerInput input;DWORD owner{};const auto window=GetForegroundWindow();
    if(!window || !GetWindowThreadProcessId(window,&owner) || owner!=GetCurrentProcessId())return input;
    auto held=[](int key){return (GetAsyncKeyState(key)&0x8000)!=0;};
    const std::pair<int,std::uint32_t> keys[]{{VK_TAB,1u},{VK_RETURN,8u},{VK_UP,0x10u},{VK_RIGHT,0x20u},{VK_DOWN,0x40u},{VK_LEFT,0x80u},{'Q',0x100u},{'E',0x200u},{'R',0x1000u},{'C',0x2000u},{VK_SPACE,0x4000u},{VK_CONTROL,0x8000u}};
    for(auto [key,bit]:keys)if(held(key))input.buttons|=bit;
    XINPUT_STATE state{};
    if(XInputGetState(0,&state)==ERROR_SUCCESS){
        const std::pair<WORD,std::uint32_t> buttons[]{{XINPUT_GAMEPAD_BACK,1u},{XINPUT_GAMEPAD_START,8u},{XINPUT_GAMEPAD_DPAD_UP,0x10u},{XINPUT_GAMEPAD_DPAD_RIGHT,0x20u},{XINPUT_GAMEPAD_DPAD_DOWN,0x40u},{XINPUT_GAMEPAD_DPAD_LEFT,0x80u},{XINPUT_GAMEPAD_LEFT_SHOULDER,0x100u},{XINPUT_GAMEPAD_RIGHT_SHOULDER,0x200u},{XINPUT_GAMEPAD_Y,0x1000u},{XINPUT_GAMEPAD_B,0x2000u},{XINPUT_GAMEPAD_A,0x4000u},{XINPUT_GAMEPAD_X,0x8000u}};
        for(auto [button,bit]:buttons)if(state.Gamepad.wButtons&button)input.buttons|=bit;
        auto axis=[](int value,bool invert){if(value>=-XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE && value<=XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)return std::uint8_t{128};if(invert)value=-value;return static_cast<std::uint8_t>(std::clamp(128+value*127/32767,0,255));};
        input.x=axis(state.Gamepad.sThumbLX,false);input.y=axis(state.Gamepad.sThumbLY,true);
    }
    if(held('A') || held('D'))input.x=held('A')==held('D')?128u:held('A')?0u:255u;
    if(held('W') || held('S'))input.y=held('W')==held('S')?128u:held('W')?0u:255u;
    return input;
}
ControllerRuntime::ControllerRuntime(psprecomp::Runtime &rt,ThreadRuntime &threads,std::function<ControllerInput()> input):runtime_(rt),threads_(threads),input_(std::move(input)){
    if(active_)throw psprecomp::Error("Only one Chili controller runtime may be active");active_=this;
    rt.register_hle("sceCtrl",0x1F4011E6u,[this](auto &,auto &c){if(c.gpr[4]>1u){c.set_gpr(2,0x80000107u);return;}const auto old=mode_;mode_=c.gpr[4];c.set_gpr(2,old);std::cout<<"[ctrl] mode="<<mode_<<'\n';});
    rt.register_hle("sceCtrl",0x1F803938u,[this](auto &,auto &c){read(c);});
    rt.register_function(ThreadRuntime::return_trampoline+8u,&resumed,"chili_controller_resume");
}
ControllerRuntime::~ControllerRuntime(){active_=nullptr;}
void ControllerRuntime::read(psprecomp::AllegrexContext &c){
    const auto output=c.gpr[4],count=c.gpr[5];
    if(!count || count>64u){c.set_gpr(2,0x80000104u);return;}
    if(count!=1u){runtime_.stop("Controller multi-sample ring reads require buffered sampling support");return;}
    if(!output || (output&3u) || !runtime_.memory().contains(output,16u)){c.set_gpr(2,0x800200D3u);return;}
    if(!threads_.can_wait()){c.set_gpr(2,0x800201A7u);return;}
    const auto now=threads_.now_us();
    if(now<next_sample_){
        pending_[threads_.current_uid()]=c;c.gpr[4]=static_cast<std::uint32_t>(next_sample_-now);c.gpr[31]=ThreadRuntime::return_trampoline+8u;
        runtime_.invoke_import("ThreadManForUser",0xCEADEB47u,c);return;
    }
    const auto input=input_();std::array<std::uint8_t,16> sample{};
    auto word=[&](unsigned offset,std::uint32_t value){for(unsigned i=0;i<4;++i)sample[offset+i]=static_cast<std::uint8_t>(value>>(8*i));};
    word(0,static_cast<std::uint32_t>(now));word(4,input.buttons&0xF3F9u);sample[8]=mode_?input.x:128u;sample[9]=mode_?input.y:128u;
    runtime_.memory().copy_in(output,sample);next_sample_=now+16667u;c.set_gpr(2,1u);
}
void ControllerRuntime::resumed(psprecomp::Runtime &,psprecomp::AllegrexContext &c){
    auto &self=*active_;auto found=self.pending_.find(self.threads_.current_uid());
    if(found==self.pending_.end()){self.runtime_.stop("Controller return without pending read");return;}
    c=found->second;self.pending_.erase(found);c.pc=c.gpr[31];self.read(c);
}
}
