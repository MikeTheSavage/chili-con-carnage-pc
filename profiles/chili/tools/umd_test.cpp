#include "../host/umd_runtime.hpp"
#include <iostream>
#include <stdexcept>
int main(int argc,char **argv) {
    try {
        if(argc!=3) throw std::runtime_error("usage: chili_umd_test disc0 memory-stick-fixture");
        psprecomp::Runtime rt;rt.set_game_root(argv[1]);chili::FileRuntime files(rt,argv[2]);chili::ThreadRuntime threads(rt);threads.initialize_bootstrap();chili::UmdRuntime umd(rt,files,threads);
        auto require=[](bool value,const char *text){if(!value)throw std::runtime_error(text);};
        auto &c=rt.cpu();const auto name=0x08802000u;
        const std::string path="disc0:/PSP_GAME/USRDIR/assets00.naz";
        for(std::size_t i=0;i<=path.size();++i)rt.memory().store8(name+static_cast<std::uint32_t>(i),i==path.size()?0:static_cast<std::uint8_t>(path[i]));
        c.gpr[4]=name;c.gpr[5]=1u;rt.invoke_import("IoFileMgrForUser",0x109F50BCu,c);require(c.gpr[2]==0x80010013u,"disc accessible before mount");
        require(umd.state()==0x12u,"initial state mismatch");
        rt.memory().store32(name,0x63736964u);rt.memory().store32(name+4u,0x00003A30u);
        c.gpr[4]=0;c.gpr[5]=name;rt.invoke_import("sceUmdUser",0xC6183D47u,c);require(c.gpr[2]==0x80010016u,"invalid mode accepted");
        c.gpr[4]=1;c.gpr[5]=name;rt.invoke_import("sceUmdUser",0xC6183D47u,c);require(c.gpr[2]==0 && umd.state()==0x32u,"mount failed");
        c.gpr[4]=0x20u;rt.invoke_import("sceUmdUser",0x8EF08FCEu,c);require(c.gpr[2]==0,"ready wait failed");
        c.gpr[4]=0;rt.invoke_import("sceUmdUser",0x8EF08FCEu,c);require(c.gpr[2]==0x80010016u,"empty state mask accepted");
        for(std::size_t i=0;i<=path.size();++i)rt.memory().store8(name+static_cast<std::uint32_t>(i),i==path.size()?0:static_cast<std::uint8_t>(path[i]));
        c.gpr[4]=name;c.gpr[5]=1;rt.invoke_import("IoFileMgrForUser",0x109F50BCu,c);require(static_cast<std::int32_t>(c.gpr[2])>0,"mounted archive inaccessible");
        c.gpr[4]=c.gpr[2];rt.invoke_import("IoFileMgrForUser",0x810C4BC3u,c);
        rt.invoke_import("sceUmdUser",0x20628E6Fu,c);require(c.gpr[2]==0,"successful mount retains error");
        std::cout<<"PASS: disc mount gate, states, validation, ready wait and actual archive access\n";
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
