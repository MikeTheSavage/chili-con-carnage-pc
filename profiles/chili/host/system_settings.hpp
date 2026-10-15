#pragma once
#include "psprecomp/runtime.hpp"
#include "psprecomp/common.hpp"
#include <fstream>
#include <iostream>
namespace chili {
class SystemSettings {
public:
    SystemSettings(psprecomp::Runtime &rt,const std::filesystem::path &path) {
        if(std::filesystem::exists(path)) {
            std::ifstream input(path);
            if(!(input>>language_>>confirm_) || language_>11u || confirm_>1u)
                throw psprecomp::Error("Invalid PSP system settings: "+path.string());
        }else{
            std::filesystem::create_directories(path.parent_path());
            std::ofstream output(path);
            if(!(output<<language_<<' '<<confirm_<<'\n'))throw psprecomp::Error("Cannot save PSP system settings: "+path.string());
        }
        rt.register_hle("sceImpose",0x24FD7BCFu,[this](auto &runtime,auto &c){
            const auto language=c.gpr[4],confirm=c.gpr[5];
            if((language && ((language&3u) || !runtime.memory().contains(language,4u))) ||
               (confirm && ((confirm&3u) || !runtime.memory().contains(confirm,4u)))){c.set_gpr(2,0x800200D3u);return;}
            if(language)runtime.memory().store32(language,language_);
            if(confirm)runtime.memory().store32(confirm,confirm_);
            std::cout<<"[system] language="<<language_<<" confirm="<<confirm_<<'\n';
            c.set_gpr(2,0u);
        });
    }
private:
    std::uint32_t language_{1u},confirm_{1u}; // English, Cross for the USA profile.
};
}
