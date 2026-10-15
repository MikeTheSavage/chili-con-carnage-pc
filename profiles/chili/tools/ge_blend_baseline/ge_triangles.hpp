#pragma once
#include "psprecomp/guest_memory.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <string>
#include <vector>

namespace chili {
// Geometry bring-up for the game's first submitted 3D format. Matrix data is
// column-major affine 3x4 (world/view) and column-major 4x4 (projection).
// Format/layout references: PSPSDK pspgu.h and sceGuDrawArray documentation.
// This path proves zero coverage for coincident projected vertices. Draws with
// any other coverage remain strict until rasterization is implemented.
inline bool render_ge_triangles(psprecomp::GuestMemory &memory,
    const std::array<unsigned,256> &regs, const std::array<float,12> &world,
    const std::array<float,12> &view, const std::array<float,16> &projection,
    unsigned vertices, unsigned data, unsigned &advanced, std::string &error) {
    auto fail=[&](const char *message){error=message;return false;};
    const unsigned count=data&0xFFFFu, stride=20u;
    if(((data>>16)&7u)!=3u || (regs[0x12]&0xFFFFFFu)!=0x19Du ||
       (regs[0xD3]&1u) || count%3u)
        return fail("triangle primitive/vertex format unsupported");
    if(!memory.contains(vertices,count*stride))return fail("triangle vertices outside guest memory");
    for(float f:world)if(!std::isfinite(f))return fail("nonfinite world matrix");
    for(float f:view)if(!std::isfinite(f))return fail("nonfinite view matrix");
    for(float f:projection)if(!std::isfinite(f))return fail("nonfinite projection matrix");
    auto affine=[](const std::array<float,12> &matrix,const std::array<float,3> &point){
        std::array<float,3> out{};
        for(unsigned row=0;row<3;++row)
            out[row]=matrix[row]*point[0]+matrix[row+3]*point[1]+matrix[row+6]*point[2]+matrix[row+9];
        return out;
    };
    std::vector<std::array<float,4>> clip;clip.reserve(count);
    for(unsigned i=0;i<count;++i){
        const unsigned address=vertices+i*stride;
        // UV8 at 0, aligned RGBA8888 at 4, aligned float XYZ at 8.
        std::array<float,3> position{};
        for(unsigned component=0;component<3;++component){
            position[component]=std::bit_cast<float>(memory.load32(address+8+component*4));
            if(!std::isfinite(position[component]))return fail("nonfinite triangle position");
        }
        const auto camera=affine(view,affine(world,position));
        std::array<float,4> projected{};
        for(unsigned row=0;row<4;++row){
            projected[row]=projection[row]*camera[0]+projection[row+4]*camera[1]+
                           projection[row+8]*camera[2]+projection[row+12];
            if(!std::isfinite(projected[row]))return fail("nonfinite transformed triangle position");
        }
        if(projected[3]==0.0f)return fail("triangle on projection plane requires clipping");
        clip.push_back(projected);
    }
    // Preflight the entire batch: no partial progress can hide a later visible
    // primitive. Equal clip X/Y/W guarantees equal raster X/Y independently of
    // viewport rounding, culling, lighting, texture or fragment state.
    for(unsigned i=0;i<count;i+=3){
        for(unsigned vertex=i+1;vertex<i+3;++vertex)
            for(unsigned component:{0u,1u,3u})
                if(clip[i][component]!=clip[vertex][component])
                    return fail("native triangle rasterization requires implementation");
    }
    advanced=vertices+count*stride;
    return true;
}
}
