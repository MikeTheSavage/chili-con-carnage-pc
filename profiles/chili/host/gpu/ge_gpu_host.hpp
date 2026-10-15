#pragma once
// Glue between Chili's GE list executor and the lifted hardware renderer.
// Everything here runs on the thread that executes GE lists.
#include "../ge_runtime.hpp"
#include "ge_gpu_backend.hpp"
#include "ge_gpu_config.hpp"
#include "ge_gpu_log.hpp"
#include "ge_renderer.hpp"
namespace chili {
// Sends every PRIM of `ge` to vcs::render_ge_primitive. With the D3D12 backend
// active the draw is queued for the GPU and guest VRAM is left alone.
// A draw the renderer cannot decode stops the list when `strict`; otherwise it
// is reported once and skipped, with the vertex and index cursors moved past it
// the way the PSP moves them.
inline void attach_gpu_draw_backend(GeRuntime &ge,bool strict=true){
    ge.draw_backend=[strict](psprecomp::GuestMemory &memory,const GeRuntime &state,unsigned prim,
                             unsigned &vertices,unsigned &indices,std::string &error){
        vcs::GeTransformState transform;
        transform.bones=state.bones;transform.world=state.world;transform.view=state.view;
        transform.projection=state.projection;transform.texture=state.texture;transform.morph_weights=state.morph;
        vcs::GeRenderStats stats;
        if(vcs::render_ge_primitive(memory,state.commands,transform,vertices,indices,prim,stats,error,1u,
                                    state.draw_revision,0u,state.lighting_revision,false)){
            vertices=stats.next_vertex_address;indices=stats.next_index_address;
            return true;
        }
        if(strict)return false;
        try{
            const auto layout=ge::make_layout(state.commands[0x12]&0xFFFFFFu);
            const unsigned count=prim&0xFFFFu;
            (void)vcs::runtime_log_once("draw skipped: "+error);
            if(layout.index_bytes)indices+=count*layout.index_bytes;else vertices+=count*layout.vertex_stride;
            error.clear();
            return true;
        }catch(const std::exception &){return false;}
    };
}
}
