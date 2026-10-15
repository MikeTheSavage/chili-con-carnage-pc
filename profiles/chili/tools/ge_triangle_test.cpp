#include "../host/ge_triangles.hpp"
#include <iostream>
#include <stdexcept>
#include <limits>
int main(){
    try {
        psprecomp::GuestMemory memory;std::array<unsigned,256> regs{};
        std::array<float,12> world{1,0,0,0,1,0,0,0,1,-0.15f,-3.11304f,5.43298f};
        std::array<float,12> view{1,-1.31153e-7f,1.97045e-6f,-1.31775e-7f,0.987564f,0.156456f,
                                1.9744e-6f,0.156456f,-0.987564f,-3.40498e-9f,0.0255175f,0.00404263f};
        std::array<float,16> projection{1.88071f,0,0,0,0,3.31891f,0,0,0,0,-1.00024f,-1,0,0,-0.250031f,0};
        const unsigned v=0x08808000;regs[0x12]=0x19D;
        // Captured game batch: 48 zero-filled vertices. Deliberately inaccessible
        // texture state must not create writes for zero-coverage geometry.
        memory.zero(v,960);regs[0xA0]=0xFFFFFF;memory.store32(0x04000000,0x79685746);
        memory.store16(0x04088000,0x2468);unsigned end=0xDEADBEEF;std::string error;
        auto draw=[&](unsigned count){return chili::render_ge_triangles(memory,regs,world,view,projection,v,0x30000|count,end,error);};
        if(!draw(48) || end!=v+960 || memory.load32(0x04000000)!=0x79685746 || memory.load16(0x04088000)!=0x2468)
            throw std::runtime_error("captured coincident geometry/cursor changed buffers: "+error);
        // A later visible primitive must reject the whole batch and preserve the
        // caller's cursor as well as both destination sentinels.
        memory.store32(v+47*20+8,std::bit_cast<unsigned>(1.0f));end=0xDEADBEEF;
        if(draw(48) || end!=0xDEADBEEF || memory.load32(0x04000000)!=0x79685746)
            throw std::runtime_error("visible primitive silently discarded");
        memory.zero(v,960);memory.store32(v+8,std::bit_cast<unsigned>(std::numeric_limits<float>::infinity()));
        if(draw(48) || end!=0xDEADBEEF)throw std::runtime_error("nonfinite input accepted");
        memory.zero(v,960);auto saved=projection;projection.fill(0);
        if(draw(48) || end!=0xDEADBEEF)throw std::runtime_error("projection-plane input accepted");
        projection=saved;regs[0x12]=0x119D;
        if(draw(48) || end!=0xDEADBEEF)throw std::runtime_error("indexed format silently accepted");
        regs[0x12]=0x19D;
        if(draw(47) || end!=0xDEADBEEF)throw std::runtime_error("unimplemented primitive continuation accepted");
        std::cout<<"Native coincident triangle transformation fixture passed; visible rasterization remains strict\n";
        return 0;
    } catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
