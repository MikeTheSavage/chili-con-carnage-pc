#pragma once
// Whole-list GE capture and per-draw state trace. Both are opt-in, configured once
// at GeRuntime construction, and cost one predictable branch on lists that are not
// selected.
//
//   CHILI_GE_CAPTURE_DIR=<dir>       enables capture; one .gecap plus one .json per list
//   CHILI_GE_CAPTURE_LISTS=<spec>    which lists: "12,40-41", "every:500", "every:500:2"
//                                    (ordinals count executed lists from 1, which is
//                                    also the id sceGeListEnQueue returns)
//   CHILI_GE_CAPTURE_MAX=<n>         stop after n captures (default 16, 0 = no limit;
//                                    a capture is about 36 MB)
//   CHILI_GE_CAPTURE_AFTER=0         skip the post-list state (default: recorded)
//   CHILI_GE_TRACE=<file>            per-draw JSON lines for captured lists
//   CHILI_GE_TRACE_LISTS=<spec>      also trace these lists without capturing them
//   CHILI_GE_LIST_INDEX=<file>       one JSON line per executed list (ordinal, address,
//                                    commands, draws, final framebuffer), to pick ordinals
//
// While capture is enabled, a list that stops on something unimplemented is also
// captured from the failing command (ge_<ordinal>_<pc>_stop.gecap), selected or not;
// CHILI_GE_CAPTURE_DIR alone therefore arms stop captures and nothing else.
// tools/chili_ge_replay.cpp re-executes a capture offline.
//
// Capture file, little-endian:
//   char magic[8]="CHILIGE1"; u32 version; u32 slot_count;
//   slot_count x { char tag[8]; u64 offset; u64 size }     (unused slots are zero)
//   chunk payloads
// State at enqueue: SCALAR (u32[32], see ge_capture::Scalar), REGS (u32[256]),
//   WORLD/VIEW/TEXMTX (f32[12]), PROJ (f32[16]), BONES (f32[96]), MORPH (f32[8]),
//   CLUT (the 1024 latched palette bytes), CLUTOK (1024 loaded flags),
//   RAM (main RAM from 0x08000000), VRAM (2 MiB from 0x04000000), SPAD (scratchpad).
// State once the list returned, appended to the same file: A.SCALAR, A.REGS,
//   A.WORLD, A.VIEW, A.TEXMTX, A.PROJ, A.BONES, A.MORPH, A.CLUT, A.CLUTOK, A.VRAM.
#include "psprecomp/guest_memory.hpp"
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace chili {

// Everything the GE list executor carries from one list to the next.
struct GeExecutorState {
    std::array<unsigned,256> commands{};
    std::array<float,96> bones{};
    std::array<float,12> world{},view{},texture{};
    std::array<float,16> projection{};
    std::array<float,8> morph{};
    unsigned bone_cursor{},world_cursor{},view_cursor{},texture_cursor{},projection_cursor{};
    unsigned offset_address{},vertex_address{},index_address{};
    unsigned next_list{1},sprite_copy_count{},clut_address{};
    std::array<std::uint8_t,1024> clut_bytes{},clut_loaded{};
};

namespace ge_capture {
static_assert(sizeof(unsigned)==4 && sizeof(float)==4 && std::endian::native==std::endian::little);
inline constexpr std::uint32_t kVersion=1,kSlots=32;
inline constexpr char kMagic[8]={'C','H','I','L','I','G','E','1'};
enum Scalar : unsigned {
    Ordinal,ListAddress,StallAddress,CallbackId,OptionsAddress,
    OffsetAddress,VertexAddress,IndexAddress,
    BoneCursor,WorldCursor,ViewCursor,TextureCursor,ProjectionCursor,
    NextList,SpriteCopyCount,ClutAddress,RamBase,RamSize,SdkVersion,
    CallbackFinish,CallbackSignal,HostMxcsr,
    // Meaningful in A.SCALAR only.
    Completed,ListId,Commands,Draws,EndCursor,
    ScalarCount=32
};
using Scalars=std::array<std::uint32_t,ScalarCount>;
struct Header {char magic[8];std::uint32_t version,slots;};
struct Slot {char tag[8];std::uint64_t offset,size;};
static_assert(sizeof(Header)==16 && sizeof(Slot)==24);

inline void state_to_scalars(const GeExecutorState &s,Scalars &out){
    out[OffsetAddress]=s.offset_address;out[VertexAddress]=s.vertex_address;out[IndexAddress]=s.index_address;
    out[BoneCursor]=s.bone_cursor;out[WorldCursor]=s.world_cursor;out[ViewCursor]=s.view_cursor;
    out[TextureCursor]=s.texture_cursor;out[ProjectionCursor]=s.projection_cursor;
    out[NextList]=s.next_list;out[SpriteCopyCount]=s.sprite_copy_count;out[ClutAddress]=s.clut_address;
}
inline void scalars_to_state(const Scalars &in,GeExecutorState &s){
    s.offset_address=in[OffsetAddress];s.vertex_address=in[VertexAddress];s.index_address=in[IndexAddress];
    s.bone_cursor=in[BoneCursor];s.world_cursor=in[WorldCursor];s.view_cursor=in[ViewCursor];
    s.texture_cursor=in[TextureCursor];s.projection_cursor=in[ProjectionCursor];
    s.next_list=in[NextList];s.sprite_copy_count=in[SpriteCopyCount];s.clut_address=in[ClutAddress];
}

class ChunkWriter {
    std::fstream file_;std::array<Slot,kSlots> slots_{};unsigned used_{};std::uint64_t end_{};
public:
    bool create(const std::filesystem::path &path){
        file_.open(path,std::ios::binary|std::ios::out|std::ios::trunc);
        Header header{};std::memcpy(header.magic,kMagic,sizeof kMagic);header.version=kVersion;header.slots=kSlots;
        file_.write(reinterpret_cast<const char*>(&header),sizeof header);
        file_.write(reinterpret_cast<const char*>(slots_.data()),sizeof slots_);
        end_=sizeof header+sizeof slots_;return file_.good();
    }
    // Reopens a finished capture so the post-list state can be appended to it.
    bool reopen(const std::filesystem::path &path){
        file_.open(path,std::ios::binary|std::ios::in|std::ios::out);
        Header header{};file_.read(reinterpret_cast<char*>(&header),sizeof header);
        if(!file_ || std::memcmp(header.magic,kMagic,sizeof kMagic)!=0 || header.version!=kVersion || header.slots!=kSlots)return false;
        file_.read(reinterpret_cast<char*>(slots_.data()),sizeof slots_);
        if(!file_)return false;
        used_=0;while(used_<kSlots && slots_[used_].tag[0])++used_;
        file_.seekp(0,std::ios::end);end_=static_cast<std::uint64_t>(file_.tellp());return file_.good();
    }
    bool add(std::string_view tag,const void *data,std::size_t size){
        if(used_==kSlots || tag.empty() || tag.size()>sizeof(Slot::tag))return false;
        Slot &slot=slots_[used_++];std::memcpy(slot.tag,tag.data(),tag.size());slot.offset=end_;slot.size=size;
        file_.write(static_cast<const char*>(data),static_cast<std::streamsize>(size));end_+=size;return file_.good();
    }
    bool add_state(std::string_view prefix,const Scalars &scalars,const GeExecutorState &s){
        auto chunk=[&](const char *name,const void *data,std::size_t size){return add(std::string(prefix)+name,data,size);};
        return chunk("SCALAR",scalars.data(),sizeof scalars) && chunk("REGS",s.commands.data(),sizeof s.commands) &&
            chunk("WORLD",s.world.data(),sizeof s.world) && chunk("VIEW",s.view.data(),sizeof s.view) &&
            chunk("TEXMTX",s.texture.data(),sizeof s.texture) && chunk("PROJ",s.projection.data(),sizeof s.projection) &&
            chunk("BONES",s.bones.data(),sizeof s.bones) && chunk("MORPH",s.morph.data(),sizeof s.morph) &&
            chunk("CLUT",s.clut_bytes.data(),sizeof s.clut_bytes) && chunk("CLUTOK",s.clut_loaded.data(),sizeof s.clut_loaded);
    }
    bool close(){
        file_.seekp(sizeof(Header));file_.write(reinterpret_cast<const char*>(slots_.data()),sizeof slots_);
        file_.close();return !file_.fail();
    }
    std::span<const Slot> slots() const noexcept {return {slots_.data(),used_};}
};

inline std::string tag_text(const Slot &slot){
    std::size_t length=0;while(length<sizeof slot.tag && slot.tag[length])++length;
    return std::string(slot.tag,length);
}

// A capture read back into memory, for the replay tool.
struct File {
    Scalars scalars{},after_scalars{};
    GeExecutorState state{},after_state{};
    std::vector<std::uint8_t> ram,vram,scratchpad,after_vram;
    std::vector<Slot> slots;
    bool has_after{};

    bool load(const std::filesystem::path &path,std::string &error){
        std::ifstream file(path,std::ios::binary);
        if(!file){error="cannot open "+path.string();return false;}
        Header header{};file.read(reinterpret_cast<char*>(&header),sizeof header);
        if(!file || std::memcmp(header.magic,kMagic,sizeof kMagic)!=0){error="not a Chili GE capture";return false;}
        if(header.version!=kVersion || !header.slots || header.slots>1024u){error="unsupported capture version";return false;}
        slots.assign(header.slots,Slot{});
        file.read(reinterpret_cast<char*>(slots.data()),static_cast<std::streamsize>(slots.size()*sizeof(Slot)));
        if(!file){error="truncated capture slot table";return false;}
        while(!slots.empty() && !slots.back().tag[0])slots.pop_back();
        auto find=[&](const std::string &tag)->const Slot*{
            for(const auto &slot:slots)if(tag_text(slot)==tag)return &slot;
            return nullptr;
        };
        auto fixed=[&](const std::string &tag,void *destination,std::size_t size){
            const Slot *slot=find(tag);
            if(!slot || slot->size!=size){error="capture chunk "+tag+" missing or of unexpected size";return false;}
            file.clear();file.seekg(static_cast<std::streamoff>(slot->offset));
            file.read(static_cast<char*>(destination),static_cast<std::streamsize>(size));
            if(!file){error="capture chunk "+tag+" truncated";return false;}
            return true;
        };
        auto bytes=[&](const std::string &tag,std::vector<std::uint8_t> &destination,std::uint64_t limit){
            const Slot *slot=find(tag);
            if(!slot || !slot->size || slot->size>limit){error="capture chunk "+tag+" missing or of unexpected size";return false;}
            destination.resize(static_cast<std::size_t>(slot->size));
            return fixed(tag,destination.data(),destination.size());
        };
        auto read_state=[&](const std::string &prefix,Scalars &values,GeExecutorState &s){
            if(!fixed(prefix+"SCALAR",values.data(),sizeof values) || !fixed(prefix+"REGS",s.commands.data(),sizeof s.commands) ||
               !fixed(prefix+"WORLD",s.world.data(),sizeof s.world) || !fixed(prefix+"VIEW",s.view.data(),sizeof s.view) ||
               !fixed(prefix+"TEXMTX",s.texture.data(),sizeof s.texture) || !fixed(prefix+"PROJ",s.projection.data(),sizeof s.projection) ||
               !fixed(prefix+"BONES",s.bones.data(),sizeof s.bones) || !fixed(prefix+"MORPH",s.morph.data(),sizeof s.morph) ||
               !fixed(prefix+"CLUT",s.clut_bytes.data(),sizeof s.clut_bytes) || !fixed(prefix+"CLUTOK",s.clut_loaded.data(),sizeof s.clut_loaded))
                return false;
            scalars_to_state(values,s);return true;
        };
        if(!read_state("",scalars,state) || !bytes("RAM",ram,64u*1024u*1024u) ||
           !bytes("VRAM",vram,psprecomp::GuestMemory::kVramSize) || !bytes("SPAD",scratchpad,psprecomp::GuestMemory::kScratchpadSize))
            return false;
        if(ram.size()!=scalars[RamSize] || vram.size()!=psprecomp::GuestMemory::kVramSize ||
           scratchpad.size()!=psprecomp::GuestMemory::kScratchpadSize){error="capture memory image sizes are inconsistent";return false;}
        has_after=find("A.VRAM")!=nullptr;
        if(has_after && (!read_state("A.",after_scalars,after_state) || !bytes("A.VRAM",after_vram,psprecomp::GuestMemory::kVramSize) ||
                         after_vram.size()!=psprecomp::GuestMemory::kVramSize)){
            if(error.empty())error="capture post-list state is inconsistent";
            return false;
        }
        return true;
    }
};
}

// "12,40-41,every:500" or "every:500:2" (each 500th list and the one after it).
struct GeListSelection {
    struct Range {unsigned first{},last{};};
    std::vector<Range> ranges;unsigned every{},span{1};
    bool empty() const noexcept {return !every && ranges.empty();}
    bool contains(unsigned ordinal) const noexcept {
        if(every && ordinal>=every && ordinal%every<span)return true;
        for(const auto &range:ranges)if(ordinal>=range.first && ordinal<=range.last)return true;
        return false;
    }
    static bool parse(std::string_view text,GeListSelection &out,std::string &error){
        auto number=[](std::string_view digits,unsigned &value){
            const auto result=std::from_chars(digits.data(),digits.data()+digits.size(),value);
            return result.ec==std::errc{} && result.ptr==digits.data()+digits.size() && value!=0;
        };
        GeListSelection parsed;
        while(!text.empty()){
            const auto comma=text.find(',');
            std::string_view item=text.substr(0,comma);
            text=comma==std::string_view::npos?std::string_view{}:text.substr(comma+1);
            while(!item.empty() && item.front()==' ')item.remove_prefix(1);
            while(!item.empty() && item.back()==' ')item.remove_suffix(1);
            if(item.empty())continue;
            const std::string_view whole=item;
            bool valid=false;
            if(item.starts_with("every:")){
                item.remove_prefix(6);
                const auto colon=item.find(':');
                valid=number(item.substr(0,colon),parsed.every) &&
                    (colon==std::string_view::npos || number(item.substr(colon+1),parsed.span)) && parsed.span<=parsed.every;
            }else{
                Range range;const auto dash=item.find('-');
                valid=number(item.substr(0,dash),range.first) &&
                    (dash==std::string_view::npos?(range.last=range.first,true):number(item.substr(dash+1),range.last)) &&
                    range.first<=range.last;
                if(valid)parsed.ranges.push_back(range);
            }
            if(!valid){error="invalid list selection item \""+std::string(whole)+"\"";return false;}
        }
        out=std::move(parsed);return true;
    }
};

namespace ge_json {
inline void escape(std::string &out,std::string_view text){
    for(const char ch:text){
        if(ch=='"' || ch=='\\'){out+='\\';out+=ch;}
        else if(static_cast<unsigned char>(ch)<0x20u){char buffer[8];std::snprintf(buffer,sizeof buffer,"\\u%04x",static_cast<unsigned>(ch));out+=buffer;}
        else out+=ch;
    }
}
// One flat JSON object, built left to right. Nest with raw(name,other.finish()).
class Object {
    std::string text_{"{"};bool first_{true};
    void key(std::string_view name){if(!first_)text_+=',';first_=false;text_+='"';text_+=name;text_+="\":";}
public:
    Object &num(std::string_view name,std::uint64_t value){key(name);text_+=std::to_string(value);return *this;}
    Object &hex(std::string_view name,std::uint32_t value,int digits=8){
        key(name);char buffer[20];std::snprintf(buffer,sizeof buffer,"\"0x%0*X\"",digits,value);text_+=buffer;return *this;
    }
    Object &real(std::string_view name,float value){
        key(name);
        if(!std::isfinite(value)){text_+="null";return *this;}
        char buffer[32];std::snprintf(buffer,sizeof buffer,"%.9g",static_cast<double>(value));text_+=buffer;return *this;
    }
    Object &str(std::string_view name,std::string_view value){key(name);text_+='"';escape(text_,value);text_+='"';return *this;}
    Object &raw(std::string_view name,std::string_view json){key(name);text_+=json;return *this;}
    std::string finish(){text_+='}';return std::move(text_);}
};
}

// Decoded state of one PRIM command, as the software rasterizer reads the registers.
struct GeDrawInfo {
    unsigned list{},index{},pc{},word{};
    unsigned primitive{},count{},vtype{},vertex_address{},index_address{};
    unsigned uv_type{},color_type{},normal_type{},position_type{},weight_type{},weight_count{},morph_count{},index_type{};
    bool through{},lighting{};unsigned lights{};
    unsigned framebuffer{},framebuffer_stride{},framebuffer_format{},depthbuffer{},depthbuffer_stride{};
    std::array<unsigned,4> scissor{};std::array<unsigned,2> region{};
    std::array<float,6> viewport{};
    bool textured{},texture_in_vram{},swizzled{},texture_alpha{},color_double{},clamp_s{},clamp_t{};
    unsigned texture_address{},texture_format{},texture_width{},texture_height{},texture_stride{},texture_levels{};
    unsigned texture_function{},min_filter{},mag_filter{},lod_mode{},texture_map_mode{};float lod_bias{};
    unsigned clut_format{},clut_shift{},clut_mask{},clut_start{},clut_address{},clut_blocks{};
    bool blend{};unsigned blend_mode{},blend_equation{},blend_source{},blend_destination{},blend_fix_a{},blend_fix_b{};
    bool alpha_test{};unsigned alpha_function{},alpha_reference{},alpha_mask{};
    bool depth_test{},depth_write{};unsigned depth_function{},depth_min{},depth_max{};
    bool stencil_test{};unsigned stencil_state{},stencil_operation{};
    bool color_test{},cull{},fog{};unsigned cull_face{},fog_color{};float fog_end{},fog_range{};
    bool clear{},clear_color{},clear_alpha{},clear_depth{};
    unsigned color_mask{};
    bool dither{},logic_op{},antialias{},depth_clamp{},gouraud{};unsigned ambient{};
    // Filled in after the draw ran.
    bool rendered{};unsigned written_pixels{};std::string error;

    bool triangles() const noexcept {return primitive>=3u && primitive<=5u;}
    bool indexed_texture() const noexcept {return texture_format>=4u && texture_format<=7u;}
    static const char *primitive_name(unsigned value){
        static constexpr const char *names[]={"points","lines","line_strip","triangles","triangle_strip","triangle_fan","sprites","reserved"};
        return names[value&7u];
    }
    static const char *compare_name(unsigned value){
        static constexpr const char *names[]={"never","always","equal","notequal","less","lequal","greater","gequal"};
        return names[value&7u];
    }
    static const char *texture_format_name(unsigned value){
        static constexpr const char *names[]={"5650","5551","4444","8888","clut4","clut8","clut16","clut32","dxt1","dxt3","dxt5"};
        return value<11u?names[value]:"reserved";
    }
    static const char *pixel_format_name(unsigned value){
        static constexpr const char *names[]={"5650","5551","4444","8888"};
        return names[value&3u];
    }
    static const char *texture_function_name(unsigned value){
        static constexpr const char *names[]={"modulate","decal","blend","replace","add"};
        return value<5u?names[value]:"reserved";
    }
    static const char *blend_equation_name(unsigned value){
        static constexpr const char *names[]={"add","sub","rsub","min","max","abs","reserved","reserved"};
        return names[value&7u];
    }
    // Factor 0/1 is the other operand's colour: destination for the source factor.
    static const char *blend_factor_name(unsigned value,bool source){
        static constexpr const char *shared[]={"src_alpha","inv_src_alpha","dst_alpha","inv_dst_alpha",
            "2x_src_alpha","inv_2x_src_alpha","2x_dst_alpha","inv_2x_dst_alpha"};
        if(value==0u)return source?"dst_color":"src_color";
        if(value==1u)return source?"inv_dst_color":"inv_src_color";
        if(value<10u)return shared[value-2u];
        return value==10u?(source?"fix_a":"fix_b"):"reserved";
    }

    std::string json() const {
        using ge_json::Object;
        Object line;
        line.str("ev","draw").num("list",list).num("n",index).hex("pc",pc).str("prim",primitive_name(primitive)).num("count",count)
            .hex("vtype",vtype,6).num("through",through).num("uv",uv_type).num("color",color_type).num("normal",normal_type)
            .num("position",position_type).num("weights",weight_count).num("weight_type",weight_type).num("morphs",morph_count)
            .num("index",index_type).hex("vaddr",vertex_address).hex("iaddr",index_address).num("lighting",lighting);
        {char buffer[16];std::snprintf(buffer,sizeof buffer,"[%u,%u,%u,%u]",lights&1u,(lights>>1)&1u,(lights>>2)&1u,(lights>>3)&1u);line.raw("lights",buffer);}
        line.hex("fb",framebuffer).num("fb_stride",framebuffer_stride).str("fb_format",pixel_format_name(framebuffer_format))
            .hex("zb",depthbuffer).num("zb_stride",depthbuffer_stride);
        {char buffer[64];std::snprintf(buffer,sizeof buffer,"[%u,%u,%u,%u]",scissor[0],scissor[1],scissor[2],scissor[3]);line.raw("scissor",buffer);
         std::snprintf(buffer,sizeof buffer,"[%u,%u]",region[0],region[1]);line.raw("region",buffer);}
        if(!through){
            std::string values="[";
            for(std::size_t i=0;i<viewport.size();++i){
                char buffer[32];
                if(std::isfinite(viewport[i]))std::snprintf(buffer,sizeof buffer,"%.9g",static_cast<double>(viewport[i]));
                else std::snprintf(buffer,sizeof buffer,"null");
                if(i)values+=',';
                values+=buffer;
            }
            line.raw("viewport",values+"]");
        }
        Object tex;tex.num("on",textured);
        if(textured){
            tex.hex("addr",texture_address).str("format",texture_format_name(texture_format)).num("w",texture_width).num("h",texture_height)
                .num("stride",texture_stride).num("levels",texture_levels).num("vram",texture_in_vram).num("swizzle",swizzled)
                .str("func",texture_function_name(texture_function)).num("alpha",texture_alpha).num("double",color_double)
                .num("min",min_filter).num("mag",mag_filter).num("clamp_s",clamp_s).num("clamp_t",clamp_t)
                .num("lod_mode",lod_mode).real("lod_bias",lod_bias).num("map",texture_map_mode);
        }
        line.raw("tex",tex.finish());
        if(textured && indexed_texture()){
            Object clut;clut.str("format",pixel_format_name(clut_format)).num("shift",clut_shift).num("mask",clut_mask)
                .num("start",clut_start).hex("addr",clut_address).num("blocks",clut_blocks);
            line.raw("clut",clut.finish());
        }
        Object blending;blending.num("on",blend).str("eq",blend_equation_name(blend_equation))
            .str("src",blend_factor_name(blend_source,true)).str("dst",blend_factor_name(blend_destination,false))
            .hex("fix_a",blend_fix_a,6).hex("fix_b",blend_fix_b,6).hex("mode",blend_mode,6);
        line.raw("blend",blending.finish());
        Object alpha;alpha.num("on",alpha_test).str("func",compare_name(alpha_function)).num("ref",alpha_reference).num("mask",alpha_mask);
        line.raw("alpha_test",alpha.finish());
        Object depth;depth.num("test",depth_test).str("func",compare_name(depth_function)).num("write",depth_write).num("min",depth_min).num("max",depth_max);
        line.raw("depth",depth.finish());
        Object stencil;stencil.num("on",stencil_test).hex("test",stencil_state,6).hex("op",stencil_operation,6);
        line.raw("stencil",stencil.finish());
        line.num("color_test",color_test);
        Object culling;culling.num("on",cull).num("face",cull_face);line.raw("cull",culling.finish());
        Object fogging;fogging.num("on",fog).hex("color",fog_color,6).real("end",fog_end).real("range",fog_range);line.raw("fog",fogging.finish());
        Object clearing;clearing.num("on",clear).num("color",clear_color).num("alpha",clear_alpha).num("depth",clear_depth);line.raw("clear",clearing.finish());
        line.hex("mask",color_mask).num("dither",dither).num("logic_op",logic_op).num("aa",antialias).num("clip",depth_clamp)
            .num("gouraud",gouraud).hex("ambient",ambient).num("ok",rendered);
        if(rendered && triangles() && !clear)line.num("pixels",written_pixels);
        if(!rendered)line.str("error",error);
        return line.finish();
    }
};

// Register decode shared by the trace and the replay tool. `vertices`/`indices`
// are the cursors the draw starts from, `clut_address` the last CLUT load source.
inline GeDrawInfo describe_ge_draw(const std::array<unsigned,256> &commands,unsigned word,
                                   unsigned vertices,unsigned indices,unsigned clut_address){
    auto r=[&](unsigned i){return commands[i]&0xFFFFFFu;};
    auto f=[&](unsigned i){return std::bit_cast<float>(r(i)<<8);};
    GeDrawInfo d;
    const unsigned data=word&0xFFFFFFu,vtype=r(0x12);
    d.word=word;d.primitive=(data>>16)&7u;d.count=data&0xFFFFu;d.vtype=vtype;
    d.vertex_address=vertices;d.index_address=indices;
    d.uv_type=vtype&3u;d.color_type=(vtype>>2)&7u;d.normal_type=(vtype>>5)&3u;d.position_type=(vtype>>7)&3u;
    d.weight_type=(vtype>>9)&3u;d.index_type=(vtype>>11)&3u;
    d.weight_count=d.weight_type?((vtype>>14)&7u)+1u:0u;d.morph_count=((vtype>>18)&7u)+1u;d.through=(vtype>>23)&1u;
    d.lighting=r(0x17)&1u;
    for(unsigned light=0;light<4;++light)d.lights|=(r(0x18+light)&1u)<<light;
    d.framebuffer=0x04000000u|(r(0x9C)&0x1FFFF0u);d.framebuffer_stride=r(0x9D)&0x7FCu;d.framebuffer_format=r(0xD2)&3u;
    d.depthbuffer=0x04000000u|(r(0x9E)&0x1FFFF0u);d.depthbuffer_stride=r(0x9F)&0x7FCu;
    d.scissor={r(0xD4)&1023u,(r(0xD4)>>10)&1023u,r(0xD5)&1023u,(r(0xD5)>>10)&1023u};
    d.region={r(0x16)&1023u,(r(0x16)>>10)&1023u};
    for(unsigned i=0;i<6;++i)d.viewport[i]=f(0x42+i);
    d.textured=r(0x1E)&1u;
    d.texture_address=(r(0xA0)&0xFFFFF0u)|((r(0xA8)<<8)&0x0F000000u);
    d.texture_stride=r(0xA8)&0x7FFu;d.texture_width=1u<<(r(0xB8)&15u);d.texture_height=1u<<((r(0xB8)>>8)&15u);
    d.texture_format=r(0xC3)&15u;d.texture_levels=((r(0xC2)>>16)&7u)+1u;d.swizzled=r(0xC2)&1u;
    const unsigned physical=psprecomp::GuestMemory::canonical(d.texture_address);
    d.texture_in_vram=physical>=psprecomp::GuestMemory::kVramPhysicalBase &&
        physical<psprecomp::GuestMemory::kVramPhysicalBase+psprecomp::GuestMemory::kVramAddressSpan;
    d.texture_function=r(0xC9)&7u;d.texture_alpha=(r(0xC9)>>8)&1u;d.color_double=(r(0xC9)>>16)&1u;
    d.min_filter=r(0xC6)&7u;d.mag_filter=(r(0xC6)>>8)&1u;d.clamp_s=r(0xC7)&1u;d.clamp_t=(r(0xC7)>>8)&1u;
    d.lod_mode=r(0xC8)&3u;d.texture_map_mode=r(0xC0)&3u;
    const unsigned bias=(r(0xC8)>>16)&255u;
    d.lod_bias=static_cast<float>((bias&128u)?static_cast<int>(bias)-256:static_cast<int>(bias))/16.0f;
    d.clut_format=r(0xC5)&3u;d.clut_shift=(r(0xC5)>>2)&31u;d.clut_mask=(r(0xC5)>>8)&255u;d.clut_start=(r(0xC5)>>16)&31u;
    d.clut_address=clut_address;d.clut_blocks=r(0xC4)&63u;
    d.blend=r(0x21)&1u;d.blend_mode=r(0xDF);d.blend_source=d.blend_mode&15u;d.blend_destination=(d.blend_mode>>4)&15u;
    d.blend_equation=(d.blend_mode>>8)&7u;d.blend_fix_a=r(0xE0);d.blend_fix_b=r(0xE1);
    d.alpha_test=r(0x22)&1u;d.alpha_function=r(0xDB)&7u;d.alpha_reference=(r(0xDB)>>8)&255u;d.alpha_mask=(r(0xDB)>>16)&255u;
    d.depth_test=r(0x23)&1u;d.depth_function=r(0xDE)&7u;d.depth_write=!(r(0xE7)&1u);d.depth_min=r(0xD6)&0xFFFFu;d.depth_max=r(0xD7)&0xFFFFu;
    d.stencil_test=r(0x24)&1u;d.stencil_state=r(0xDC);d.stencil_operation=r(0xDD);
    d.color_test=r(0x27)&1u;d.cull=r(0x1D)&1u;d.cull_face=r(0x9B)&1u;
    d.fog=r(0x1F)&1u;d.fog_color=r(0xCF);d.fog_end=f(0xCD);d.fog_range=f(0xCE);
    d.clear=r(0xD3)&1u;d.clear_color=(r(0xD3)>>8)&1u;d.clear_alpha=(r(0xD3)>>9)&1u;d.clear_depth=(r(0xD3)>>10)&1u;
    d.color_mask=r(0xE8)|((r(0xE9)&255u)<<24);
    d.dither=r(0x20)&1u;d.logic_op=r(0x28)&1u;d.antialias=r(0x25)&1u;d.depth_clamp=r(0x1C)&1u;d.gouraud=r(0x50)&1u;
    d.ambient=r(0x55)|((r(0x58)&255u)<<24);
    return d;
}

// Per-draw trace sink: JSON lines to a file, and/or a callback for tools.
class GeDrawTrace {
    std::ofstream file_;
    std::function<void(const GeDrawInfo&)> sink_;
    void line(const std::string &text){if(file_.is_open())file_<<text<<'\n';}
public:
    bool open(const std::filesystem::path &path){
        file_.close();file_.clear();
        file_.open(path,std::ios::out|std::ios::trunc);return file_.is_open();
    }
    void set_sink(std::function<void(const GeDrawInfo&)> sink){sink_=std::move(sink);}
    bool available() const noexcept {return file_.is_open() || static_cast<bool>(sink_);}
    void list_begin(unsigned ordinal,unsigned address){
        line(ge_json::Object().str("ev","list").num("list",ordinal).hex("addr",address).finish());
    }
    void draw(const GeDrawInfo &info){
        if(file_.is_open())line(info.json());
        if(sink_)sink_(info);
    }
    void list_end(unsigned ordinal,bool completed,unsigned id,unsigned commands,unsigned draws,std::string_view stop_reason){
        ge_json::Object end;
        end.str("ev","end").num("list",ordinal).num("completed",completed).num("id",id).num("commands",commands).num("draws",draws);
        if(!completed)end.str("stop",stop_reason);
        line(end.finish());file_.flush();
    }
};

// Decides which lists to capture and writes them. Reads its configuration once.
class GeFrameCapture {
public:
    struct Request {unsigned ordinal{},list{},stall{},callback{},options{},sdk_version{},callback_finish{},callback_signal{},host_mxcsr{};bool stop{};};
    struct Outcome {bool completed{};unsigned id{},commands{},draws{},end{};std::string stop_reason;};

    GeFrameCapture(){
        const char *directory=std::getenv("CHILI_GE_CAPTURE_DIR");
        if(!directory || !*directory)return;
        try{
            directory_=directory;std::filesystem::create_directories(directory_);
            std::string error;
            if(const char *lists=std::getenv("CHILI_GE_CAPTURE_LISTS"))
                if(*lists && !GeListSelection::parse(lists,selection_,error))throw std::runtime_error("CHILI_GE_CAPTURE_LISTS: "+error);
            if(const char *limit=std::getenv("CHILI_GE_CAPTURE_MAX"))
                if(*limit){char *tail{};limit_=static_cast<unsigned>(std::strtoul(limit,&tail,10));if(*tail)throw std::runtime_error("CHILI_GE_CAPTURE_MAX must be a number");}
            if(const char *after=std::getenv("CHILI_GE_CAPTURE_AFTER"))after_=!(after[0]=='0' && !after[1]);
            enabled_=true;
        }catch(const std::exception &failure){std::cerr<<"[ge-capture] disabled: "<<failure.what()<<'\n';}
    }
    bool enabled() const noexcept {return enabled_;}
    // Capture the next `lists` enqueued lists regardless of the selection, for a
    // host hotkey or debug channel. Ignored unless CHILI_GE_CAPTURE_DIR is set.
    void capture_next(unsigned lists) noexcept {if(enabled_)pending_+=lists;}
    bool wants(unsigned ordinal) noexcept {
        if(!enabled_ || (limit_ && written_>=limit_))return false;
        if(pending_){--pending_;return true;}
        return selection_.contains(ordinal);
    }
    bool begin(const Request &request,const GeExecutorState &state,const psprecomp::GuestMemory &memory){
        open_=false;
        try{
            char name[48];std::snprintf(name,sizeof name,"ge_%06u_%08X%s",request.ordinal,request.list,request.stop?"_stop":"");
            path_=directory_/(std::string(name)+".gecap");request_=request;
            scalars_={};ge_capture::state_to_scalars(state,scalars_);
            using namespace ge_capture;
            scalars_[Ordinal]=request.ordinal;scalars_[ListAddress]=request.list;scalars_[StallAddress]=request.stall;
            scalars_[CallbackId]=request.callback;scalars_[OptionsAddress]=request.options;
            scalars_[RamBase]=psprecomp::GuestMemory::kPhysicalBase;scalars_[RamSize]=memory.size();
            scalars_[SdkVersion]=request.sdk_version;scalars_[CallbackFinish]=request.callback_finish;
            scalars_[CallbackSignal]=request.callback_signal;scalars_[HostMxcsr]=request.host_mxcsr;
            std::array<std::uint8_t,psprecomp::GuestMemory::kScratchpadSize> scratchpad{};
            memory.copy_out(psprecomp::GuestMemory::kScratchpadBase,scratchpad);
            ChunkWriter writer;
            if(!writer.create(path_) || !writer.add_state("",scalars_,state) ||
               !writer.add("RAM",memory.bytes().data(),memory.bytes().size()) ||
               !writer.add("VRAM",memory.vram_bytes().data(),memory.vram_bytes().size()) ||
               !writer.add("SPAD",scratchpad.data(),scratchpad.size()) || !writer.close())
                throw std::runtime_error("cannot write "+path_.string());
            sidecar(state,writer.slots(),nullptr);
            ++written_;open_=true;
            std::cout<<"[ge-capture] list="<<request.ordinal<<" address="<<std::hex<<request.list<<std::dec<<" file="<<path_.string()<<'\n';
        }catch(const std::exception &failure){
            enabled_=false;std::cerr<<"[ge-capture] stopped: "<<failure.what()<<'\n';
        }
        return open_;
    }
    // A list that hit something unimplemented: capture from the failing command, so
    // the replay tool reproduces the stop directly. `request.list` is that command's
    // address. Skipped while a capture of the whole list is open; that one records it.
    void stop(Request request,const GeExecutorState &state,const psprecomp::GuestMemory &memory){
        if(!enabled_ || open_ || (limit_ && written_>=limit_))return;
        request.stop=true;
        if(begin(request,state,memory))open_=false;
    }
    // Appends the post-list state to the capture begin() wrote.
    void end(const Outcome &outcome,const GeExecutorState &state,const psprecomp::GuestMemory &memory){
        if(!open_)return;
        open_=false;
        try{
            using namespace ge_capture;
            Scalars scalars=scalars_;state_to_scalars(state,scalars);
            scalars[Completed]=outcome.completed;scalars[ListId]=outcome.id;scalars[Commands]=outcome.commands;
            scalars[Draws]=outcome.draws;scalars[EndCursor]=outcome.end;
            ChunkWriter writer;
            if(!writer.reopen(path_))throw std::runtime_error("cannot reopen "+path_.string());
            if(after_ && (!writer.add_state("A.",scalars,state) ||
                          !writer.add("A.VRAM",memory.vram_bytes().data(),memory.vram_bytes().size())))
                throw std::runtime_error("cannot append to "+path_.string());
            if(!writer.close())throw std::runtime_error("cannot finish "+path_.string());
            sidecar(state,writer.slots(),&outcome);
        }catch(const std::exception &failure){
            enabled_=false;std::cerr<<"[ge-capture] stopped: "<<failure.what()<<'\n';
        }
    }
private:
    void sidecar(const GeExecutorState &state,std::span<const ge_capture::Slot> slots,const Outcome *outcome){
        auto r=[&](unsigned i){return state.commands[i]&0xFFFFFFu;};
        ge_json::Object json;
        json.str("format","chili-ge-capture").num("version",ge_capture::kVersion).str("file",path_.filename().string())
            .str("kind",request_.stop?"stop":"list")
            .num("ordinal",request_.ordinal).hex("list",request_.list).hex("callback",request_.callback).hex("stall",request_.stall)
            .hex("vertex_address",state.vertex_address).hex("index_address",state.index_address).hex("offset_address",state.offset_address)
            .hex("ram_base",psprecomp::GuestMemory::kPhysicalBase).num("ram_size",scalars_[ge_capture::RamSize])
            .num("vram_size",psprecomp::GuestMemory::kVramSize)
            .hex("framebuffer",0x04000000u|(r(0x9C)&0x1FFFF0u)).num("framebuffer_stride",r(0x9D)&0x7FCu)
            .hex("depthbuffer",0x04000000u|(r(0x9E)&0x1FFFF0u));
        std::string chunks="[";
        for(const auto &slot:slots){
            if(chunks.size()>1)chunks+=',';
            chunks+=ge_json::Object().str("tag",ge_capture::tag_text(slot)).num("offset",slot.offset).num("size",slot.size).finish();
        }
        json.raw("chunks",chunks+"]");
        if(outcome){
            ge_json::Object after;
            after.num("completed",outcome->completed).num("id",outcome->id).num("commands",outcome->commands)
                .num("draws",outcome->draws).hex("end",outcome->end).num("final_vram",after_);
            if(!outcome->completed)after.str("stop",outcome->stop_reason);
            json.raw("after",after.finish());
        }
        auto target=path_;target.replace_extension(".json");
        std::ofstream file(target,std::ios::out|std::ios::trunc);
        file<<json.finish()<<'\n';
        if(!file)throw std::runtime_error("cannot write "+target.string());
    }
    std::filesystem::path directory_,path_;
    GeListSelection selection_;
    Request request_{};
    ge_capture::Scalars scalars_{};
    unsigned limit_{16},written_{},pending_{};
    bool enabled_{},after_{true},open_{};
};
}
