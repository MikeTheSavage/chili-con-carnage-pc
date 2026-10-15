#include "../host/system_settings.hpp"
#include <chrono>
int main(int argc,char **argv){try{
    if(argc!=2)throw std::runtime_error("usage: chili_settings_test fixture-directory");
    const auto path=std::filesystem::path(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())/"settings.txt";
    auto require=[](bool ok,const char *s){if(!ok)throw std::runtime_error(s);};
    {
        psprecomp::Runtime rt;chili::SystemSettings settings(rt,path);auto &c=rt.cpu();c.gpr[4]=0x08802000u;c.gpr[5]=0x08802004u;
        rt.invoke_import("sceImpose",0x24FD7BCFu,c);require(c.gpr[2]==0 && rt.memory().load32(c.gpr[4])==1 && rt.memory().load32(c.gpr[5])==1,"default language/button mismatch");
        rt.memory().store32(c.gpr[4],0xDEADBEEFu);c.gpr[5]=0x00000001u;rt.invoke_import("sceImpose",0x24FD7BCFu,c);require(c.gpr[2]==0x800200D3u && rt.memory().load32(c.gpr[4])==0xDEADBEEFu,"invalid second output modified first output");
        c.gpr[4]=0;c.gpr[5]=0;rt.invoke_import("sceImpose",0x24FD7BCFu,c);require(c.gpr[2]==0,"null optional outputs failed");
    }
    {std::ofstream file(path);file<<"3 0\n";}
    {
        psprecomp::Runtime rt;chili::SystemSettings settings(rt,path);auto &c=rt.cpu();c.gpr[4]=0x08802000u;c.gpr[5]=0x08802004u;rt.invoke_import("sceImpose",0x24FD7BCFu,c);
        require(rt.memory().load32(c.gpr[4])==3 && rt.memory().load32(c.gpr[5])==0,"saved settings were not restored");
    }
    std::cout<<"PASS: default settings, optional outputs, invalid pointer atomicity and saved language/button restoration\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
