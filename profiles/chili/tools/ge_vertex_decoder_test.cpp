#include "ge_vertex_decoder.hpp"
#include <fstream>
#include <iostream>
#include <string>
using namespace chili::ge;
void require(bool value,const char *why){if(!value)throw std::runtime_error(why);}
template<class F> void reject(F &&fn,const char *why){bool rejected=false;try{fn();}catch(const std::runtime_error&){rejected=true;}require(rejected,why);}
void write(std::span<std::uint8_t> bytes,unsigned at,unsigned n,std::uint32_t value){require(at+n<=bytes.size(),"Fixture writer overflow");for(unsigned i=0;i<n;++i)bytes[at+i]=std::uint8_t(value>>(8*i));}
void floating(std::span<std::uint8_t> bytes,unsigned at,float value){write(bytes,at,4,std::bit_cast<std::uint32_t>(value));}
struct Memory {
    std::uint32_t base=0x08F00000u;
    std::vector<std::uint8_t> bytes;
    unsigned reads=0;
    bool operator()(std::uint32_t address,std::span<std::uint8_t> destination){
        ++reads;if(address<base)return false;auto offset=std::uint64_t(address)-base;
        if(offset>bytes.size()||destination.size()>bytes.size()-offset)return false;
        std::copy_n(bytes.begin()+std::size_t(offset),destination.size(),destination.begin());return true;
    }
};
void layouts(){
    auto l=make_layout(0x19D);require(l.uv_offset==0&&l.color_offset==4&&l.position_offset==8&&l.vertex_stride==20&&l.alignment==4,"Real decal layout mismatch");
    l=make_layout(0x80011E);require(l.uv_offset==0&&l.color_offset==4&&l.position_offset==8&&l.vertex_stride==16&&l.alignment==4,"Real through fan layout mismatch");
    l=make_layout(1u|(4u<<2)|(1u<<5)|(2u<<7));
    require(l.uv_offset==0&&l.color_offset==2&&l.normal_offset==4&&l.position_offset==8&&l.sample_stride==14&&l.alignment==2,"Mixed component natural alignment mismatch");
    // Weights are first, not appended after the position; whole samples repeat.
    l=make_layout(1u|(4u<<2)|(1u<<5)|(2u<<7)|(1u<<9)|(2u<<14)|(7u<<18));
    require(l.weight_offset==0&&l.weight_count==3&&l.uv_offset==3&&l.color_offset==6&&l.normal_offset==8&&l.position_offset==12&&l.sample_stride==18&&l.vertex_stride==144,"Weight/morph storage alignment mismatch");
    for(unsigned bit : {13u,17u,21u,22u,24u,31u})reject([&]{make_layout(0x180u|(1u<<bit));},"Reserved VTYPE bit accepted");
    for(unsigned col=1;col<=3;++col)reject([&]{make_layout(0x180u|(col<<2));},"Undocumented color accepted");
    reject([]{make_layout(0);},"Absent position accepted");reject([]{make_layout(0x180u|(3u<<11));},"Unverified 32-bit index accepted");
    std::cout<<"PASS: exact real 19D/80011E layouts, mixed member padding, whole-record morph storage, invalid encoding gates\n";
}
void numeric(){
    constexpr unsigned widths[]={0,1,2,4};
    for(unsigned type=1;type<=3;++type){
        auto l=make_layout(type|(type<<5)|(type<<7)|(type<<9));std::vector<std::uint8_t>b(l.vertex_stride,0xCC);
        unsigned width=widths[type];
        if(type==3){floating(b,l.uv_offset,1.75f);floating(b,l.uv_offset+width,0.25f);floating(b,l.weight_offset,1.5f);
          for(unsigned i=0;i<3;++i){floating(b,l.normal_offset+i*width,i==0?-1.0f:i==1?0.0f:0.75f);floating(b,l.position_offset+i*width,i==0?-1.0f:i==1?0.0f:0.75f);}}
        else{auto max=type==1?255u:65535u;auto signbit=type==1?128u:32768u;auto signedmax=signbit-1;
          write(b,l.uv_offset,width,max);write(b,l.uv_offset+width,width,signbit);write(b,l.weight_offset,width,max);
          for(unsigned at : {l.normal_offset,l.position_offset}){write(b,at,width,signbit);write(b,at+width,width,0);write(b,at+2*width,width,signedmax);}}
        auto s=decode_sample(l,b);require(s.uv&&s.normal&&!s.color,"Absent optional source attributes invented");
        if(type==3){require((*s.uv)[0]==1.75f&&s.weights[0]==1.5f&&s.position[2]==0.75f,"Float source attributes were renormalized");}
        else{float limit=type==1?255.0f/128.0f:65535.0f/32768.0f;float high=type==1?127.0f/128.0f:32767.0f/32768.0f;
          require((*s.uv)[0]==limit&&(*s.uv)[1]==1.0f&&s.weights[0]==limit,"Unsigned UV/weight interpretation or scale mismatch");
          require(s.position==Vec3{-1,0,high}&&*s.normal==Vec3{-1,0,high},"Signed XYZ/normal endpoint or scale mismatch");}
    }
    // Explicit partial channels test bit replication, ordering, and alpha modes.
    struct ColorCase{unsigned type,packed;Color expected;};
    for(auto c : {ColorCase{4,(17u)|(33u<<5)|(9u<<11),{140,134,74,255}},ColorCase{5,17u|(9u<<5)|(3u<<10),{140,74,24,0}},ColorCase{5,0xFFFF,{255,255,255,255}},ColorCase{6,0xA531,{17,51,85,170}},ColorCase{7,0x72423110,{16,49,66,114}}}){
        std::array<std::uint8_t,4>b{};write(b,0,c.type==7?4:2,c.packed);require(decode_color(b,0,c.type)==c.expected,"Packed color expansion mismatch");}
    auto l=make_layout(0x180);std::vector<std::uint8_t>b(l.vertex_stride);auto s=decode_sample(l,b);
    require(!s.uv&&!s.normal&&!s.color&&l.weight_count==0,"Persistent default attributes were invented");
    for(unsigned at=0;at<12;at+=4){floating(b,at,std::numeric_limits<float>::quiet_NaN());reject([&]{decode_sample(l,b);},"Unverified nonfinite source accepted");floating(b,at,0);}
    std::cout<<"PASS: UV/weights unsigned 8/16/float; XYZ/normals signed fixed-point endpoints; all four packed colors; absent attributes remain absent\n";
}
void through(){
    auto l=make_layout(0x80011E);Memory m;m.bytes.resize(64,0xCC);
    constexpr unsigned uv[4][2]={{0,0},{0,8},{8,8},{8,0}};constexpr unsigned xy[4][2]={{246,95},{246,101},{252,101},{252,95}};
    for(unsigned i=0;i<4;++i){auto at=i*16;write(m.bytes,at,2,uv[i][0]);write(m.bytes,at+2,2,uv[i][1]);write(m.bytes,at+4,4,0x72FFFFFFu);write(m.bytes,at+8,2,xy[i][0]);write(m.bytes,at+10,2,xy[i][1]);write(m.bytes,at+12,2,0xFFFF);}
    auto draw=prepare_draw(m,0x80011E,m.base,0,4);
    for(unsigned i=0;i<4;++i){auto&s=draw.vertices[i].samples[0];require(*s.uv==Vec2{float(uv[i][0]),float(uv[i][1])}&&*s.color==Color{255,255,255,114}&&s.position==Vec3{float(xy[i][0]),float(xy[i][1]),65535},"Real captured through fan mismatch");}
    require(draw.next_vertex_address==m.base+64&&draw.source_attributes_ready(),"Through fan cursor/readiness mismatch");
    // Re-read root's captured 64 bytes when available; literal reconstruction above remains portable.
    std::ifstream f("captured_80011e_vertices.bin",std::ios::binary);
    if(f){Memory captured;captured.bytes.resize(64);f.read(reinterpret_cast<char*>(captured.bytes.data()),64);require(f.gcount()==64&&f.peek()==std::char_traits<char>::eof(),"Captured fixture size differs");auto actual=prepare_draw(captured,0x80011E,captured.base,0,4);for(unsigned i=0;i<4;++i){auto&a=actual.vertices[i].samples[0];auto&e=draw.vertices[i].samples[0];require(a.uv==e.uv&&a.color==e.color&&a.position==e.position,"Captured source fields differ from literal golden records");}std::cout<<"PASS: actual captured ge_parallel_geometry_25 fan fields match portable golden records; arbitrary padding is ignored\n";}
    l=make_layout(0x800102);std::vector<std::uint8_t>b(l.vertex_stride);write(b,0,2,0xFFFF);write(b,2,2,255);write(b,l.position_offset,2,0x8000);write(b,l.position_offset+2,2,0xFFFF);write(b,l.position_offset+4,2,0xFFFF);auto s=decode_sample(l,b);
    require(*s.uv==Vec2{65535,255}&&s.position==Vec3{-32768,-1,65535},"Through16 signedXY/unsignedZ or UV units mismatch");
    l=make_layout(0x800101);b.resize(l.vertex_stride);write(b,0,1,255);write(b,1,1,128);write(b,l.position_offset,2,7);write(b,l.position_offset+2,2,9);write(b,l.position_offset+4,2,65535);s=decode_sample(l,b);require(*s.uv==Vec2{255,128}&&s.position==Vec3{7,9,65535},"Through8-bit UV was signed or normalized");
    l=make_layout(0x800183);b.resize(l.vertex_stride);floating(b,l.uv_offset,2.25f);floating(b,l.uv_offset+4,4.5f);floating(b,l.position_offset,-3.25f);floating(b,l.position_offset+4,11.75f);
    for(auto pair : {std::pair{-1.0f,0.0f},std::pair{4.9f,4.0f},std::pair{65536.0f,65535.0f}}){floating(b,l.position_offset+8,pair.first);s=decode_sample(l,b);require(s.position==Vec3{-3.25f,11.75f,pair.second}&&*s.uv==Vec2{2.25f,4.5f},"Throughfloat rawXY/UV or depth clamp/truncation mismatch");}
    l=make_layout(0x800081);b.resize(l.vertex_stride);reject([&]{decode_sample(l,b);},"Through8 unverified conversion silently accepted");
    std::cout<<"PASS: captured through fan source data; unsigned16 depth65535; raw through UV/XY; finite float depth truncation; through8 gate\n";
}
void storage(){
    constexpr unsigned widths[]={0,1,2,4};
    for(unsigned type=1;type<=3;++type)for(unsigned n=1;n<=8;++n)for(unsigned morph=1;morph<=8;++morph){
        auto vt=0x180u|(type<<9)|((n-1)<<14)|((morph-1)<<18);auto l=make_layout(vt);Memory m;m.bytes.resize(l.vertex_stride,0xAA);
        for(unsigned sample=0;sample<morph;++sample){auto at=sample*l.sample_stride;
            for(unsigned w=0;w<n;++w){if(type==3)floating(m.bytes,at+w*4,float(sample+w+1));else write(m.bytes,at+w*widths[type],widths[type],(sample+w+1)*(type==1?8:2048));}
            floating(m.bytes,at+l.position_offset,float(100+sample));floating(m.bytes,at+l.position_offset+4,float(200+sample));floating(m.bytes,at+l.position_offset+8,float(300+sample));}
        auto d=prepare_draw(m,vt,m.base,0,1);require(d.next_vertex_address==m.base+l.vertex_stride&&!d.source_attributes_ready(),"Skin/morph source incorrectly declared complete");
        for(unsigned sample=0;sample<morph;++sample){auto&s=d.vertices[0].samples[sample];require(s.position==Vec3{float(100+sample),float(200+sample),float(300+sample)},"Morph samples do not repeat complete records");
            for(unsigned w=0;w<n;++w)require(s.weights[w]==float(sample+w+1)*(type==3?1.0f:1.0f/16.0f),"Stored weight count/type or morph sample mismatch");}
    }
    std::cout<<"PASS: all 192 weight type/count/morph count combinations retain complete source records and require transform integration\n";
}
void indexed_and_bounds(){
    for(unsigned idx : {1u,2u}){
        auto vt=0x180u|(idx<<11);Memory m;m.bytes.resize(64);for(unsigned i=0;i<3;++i)floating(m.bytes,i*12,float(10+i));unsigned width=idx;
        write(m.bytes,48,width,2);write(m.bytes,48+width,width,0);write(m.bytes,48+2*width,width,2);auto d=prepare_draw(m,vt,m.base,m.base+48,3);
        require(d.next_vertex_address==m.base&&d.next_index_address==m.base+48+3*width,"Indexed cursor advanced VADDR or wrong IADDR");
        require(d.vertices[0].source_index==2&&d.vertices[1].source_index==0&&d.vertices[2].source_index==2&&d.vertices[0].samples[0].position[0]==12&&d.vertices[1].samples[0].position[0]==10,"Index stream ordering/duplicates lost");
    }
    Memory big;big.bytes.resize(65536*12+4);floating(big.bytes,65535*12,99);write(big.bytes,65536*12,2,65535);auto d=prepare_draw(big,0x1180,big.base,big.base+65536*12,1);require(d.vertices[0].samples[0].position[0]==99,"Maximum16-bit index was signed/truncated");
    Memory m;m.bytes.resize(24);floating(m.bytes,0,42);d=prepare_draw(m,0x180,m.base,0,2);require(d.next_vertex_address==m.base+24&&d.next_index_address==0,"Nonindexed cursor accounting mismatch");
    unsigned reads=m.reads;auto zero=prepare_draw(m,0x180,1,1,0);require(zero.vertices.empty()&&zero.next_vertex_address==1&&zero.next_index_address==1&&m.reads==reads,"Zero count touched/advanced invalid memory");
    auto saved=d.next_vertex_address;reject([&]{d=prepare_draw(m,0x180,m.base,0,3);},"Truncated last vertex accepted");require(d.next_vertex_address==saved&&d.vertices.size()==2,"Failure published partial draw/cursors");
    reject([&]{prepare_draw(m,0x180,m.base+1,0,1);},"Misaligned VADDR accepted");reject([&]{prepare_draw(m,0x1180,m.base,m.base+1,1);},"Misaligned16-bit IADDR accepted");
    reject([&]{prepare_draw(m,0x1180,m.base,m.base+24,1);},"Truncated index stream accepted");write(m.bytes,20,2,65535);reject([&]{prepare_draw(m,0x1180,m.base,m.base+20,1);},"Out-of-range index vertex accepted");
    reject([&]{prepare_draw(m,0x180,0xFFFFFFF0u,0,2);},"Wrapping nonindexed cursor accepted");reject([&]{prepare_draw(m,0x1180,m.base,0xFFFFFFFEu,1);},"Wrapping index cursor accepted");
    reject([&]{prepare_draw(m,0x180,m.base,0,65536);},"Oversized PRIM count accepted");
    std::cout<<"PASS: index8/16 ordering/max value, exact VADDR/IADDR advancement, zero count, truncated/misaligned/wrapping bounds, atomic failures\n";
}
int main(){try{layouts();numeric();through();storage();indexed_and_bounds();}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
