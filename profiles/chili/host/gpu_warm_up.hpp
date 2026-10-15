#pragma once
// The renderer's first-use work, kept between runs. Direct3D 12 builds a pipeline state the first time a
// draw needs it, which takes tens of milliseconds, and creates a surface the first time the game selects
// it; at the start of a mission that was a row of long frames and a hole in the sound. The pipeline keys
// and surface addresses a run met are written to a small text file beside the executable when it ends
// and created at the next start, before the game runs. A built-in list covers a first start: the states
// of the menus, the movies and the first mission. Runtime thread only.
//
//   <exe folder>\ChiliNative.gpucache    one "pipeline <hex key>", "color <hex>", "depth <hex>" or
//                                        "copy <hex>" per line under a "chili-gpu-cache <version>" line
//
// A file of another version, or one that cannot be read, is ignored and replaced.
#include "gpu/ge_gpu_backend.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
namespace chili {
class GpuWarmUp {
public:
    // Bumped when the renderer's pipeline key layout changes (pipeline_key in ge_gpu_backend_dx12.cpp).
    static constexpr unsigned version=1u;
    explicit GpuWarmUp(std::filesystem::path file):file_(std::move(file)){
        known_=built_in();
        std::ifstream stream(file_,std::ios::binary);
        std::string line;
        if(stream && std::getline(stream,line) && trimmed(line)=="chili-gpu-cache "+std::to_string(version)){
            while(std::getline(stream,line)){
                std::istringstream words(trimmed(line));
                std::string kind,value;
                if(!(words>>kind>>value))continue;
                unsigned long long number{};
                try{std::size_t used{};number=std::stoull(value,&used,16);if(used!=value.size())continue;}catch(const std::exception &){continue;}
                if(kind=="pipeline")known_.pipelines.push_back(number);
                else if(number<=0x001FFFF0ull){
                    if(kind=="color")known_.color_surfaces.push_back(static_cast<std::uint32_t>(number));
                    else if(kind=="depth")known_.depth_surfaces.push_back(static_cast<std::uint32_t>(number));
                    else if(kind=="copy")known_.feedback_copies.push_back(static_cast<std::uint32_t>(number));
                }
                ++read_;
            }
        }
        normalize(known_);
    }
    // Creates everything known. Returns one line for the log.
    std::string apply(){
        const auto start=std::chrono::steady_clock::now();
        vcs::ge_gpu_backend_warm_up(known_);
        const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        const auto made=vcs::ge_gpu_backend_created();
        char text[256];
        std::snprintf(text,sizeof(text),"warm-up: %zu pipeline states, %zu surfaces, %zu depth buffers, %zu surface copies created in %.0f ms (%zu entries from %s)",
            made.pipelines.size(),made.color_surfaces.size(),made.depth_surfaces.size(),made.feedback_copies.size(),ms,read_,file_.filename().string().c_str());
        return text;
    }
    // Writes the file when this run created something the file did not have. Returns one line for the log.
    std::string save(){
        auto now=vcs::ge_gpu_backend_created();
        const auto fresh=count_missing(now.pipelines,known_.pipelines)+count_missing(now.color_surfaces,known_.color_surfaces)+
            count_missing(now.depth_surfaces,known_.depth_surfaces)+count_missing(now.feedback_copies,known_.feedback_copies);
        // Keep what earlier runs met as well: this run may not have reached those places.
        merge(now.pipelines,known_.pipelines);merge(now.color_surfaces,known_.color_surfaces);
        merge(now.depth_surfaces,known_.depth_surfaces);merge(now.feedback_copies,known_.feedback_copies);
        char text[200];
        std::error_code status;
        if(!fresh && std::filesystem::is_regular_file(file_,status)){
            std::snprintf(text,sizeof(text),"warm-up: nothing new (%zu pipeline states known)",now.pipelines.size());
            return text;
        }
        std::ostringstream out;
        out<<"chili-gpu-cache "<<version<<'\n'<<std::hex;
        for(const auto key:now.pipelines)out<<"pipeline "<<key<<'\n';
        for(const auto address:now.color_surfaces)out<<"color "<<address<<'\n';
        for(const auto address:now.depth_surfaces)out<<"depth "<<address<<'\n';
        for(const auto address:now.feedback_copies)out<<"copy "<<address<<'\n';
        // Written beside and renamed over, so a reader never sees half a file.
        auto stage=file_;stage+=".new";
        bool written=false;
        {
            std::ofstream stream(stage,std::ios::binary|std::ios::trunc);
            stream<<out.str();stream.close();written=!stream.fail();
        }
        if(written){std::filesystem::rename(stage,file_,status);written=!status;}
        if(!written)std::filesystem::remove(stage,status);
        std::snprintf(text,sizeof(text),"warm-up: %zu new, %zu pipeline states %s %s",fresh,now.pipelines.size(),written?"written to":"NOT written to",
            file_.filename().string().c_str());
        known_=std::move(now);
        return text;
    }
private:
    // What boot, the menus, the movies and Story 1-1 create (ChiliNative.gpucache of a replay run of
    // mission1_baseline_fl1.replay).
    static vcs::GeGpuWarmUp built_in(){
        vcs::GeGpuWarmUp items;
        items.pipelines={0x84403Eull,0x84471Cull,0x84471Eull,0x84471Full,0x844F3Eull,0x844F40ull,0xC0571Cull,0xC0571Dull,0xC0571Full,
            0xC1371Cull,0x140571Cull,0x1C0571Cull,0x4000000000C0571Dull,0x4000000000C0571Full,0x6000000000C0571Dull,
            0x6000000000C0571Full,
            // The glow drawn at the PSP's size ([Rendering] Glow, gpu/ge_gpu_glow_dx12.hpp): the
            // sprite that enlarges it and, with multisampling, the three states of the sprites on the small sheet.
            // The renderer leaves out the ones a run cannot need.
            0x0800000000C0571Cull,0x100000000084471Cull,0x1000000000C0571Cull,0x100000000140571Cull};
        // The LCD, the bloom and depth sheet, the composite and the scene (RENDERING.md).
        items.color_surfaces={0x000000u,0x088000u,0x0D8000u,0x0E8000u};
        items.depth_surfaces={0x088000u};
        items.feedback_copies={0x088000u};
        return items;
    }
    static std::string trimmed(std::string text){
        while(!text.empty() && (text.back()=='\r' || text.back()==' ' || text.back()=='\t'))text.pop_back();
        return text;
    }
    template<class T> static void sort_unique(std::vector<T> &values){
        std::sort(values.begin(),values.end());values.erase(std::unique(values.begin(),values.end()),values.end());
    }
    static void normalize(vcs::GeGpuWarmUp &items){
        sort_unique(items.pipelines);sort_unique(items.color_surfaces);sort_unique(items.depth_surfaces);sort_unique(items.feedback_copies);
    }
    template<class T> static std::size_t count_missing(const std::vector<T> &now,const std::vector<T> &known){
        std::size_t missing{};
        for(const auto &value:now)if(!std::binary_search(known.begin(),known.end(),value))++missing;
        return missing;
    }
    template<class T> static void merge(std::vector<T> &now,const std::vector<T> &known){
        now.insert(now.end(),known.begin(),known.end());sort_unique(now);
    }
    std::filesystem::path file_;
    vcs::GeGpuWarmUp known_;
    std::size_t read_{};
};
}
