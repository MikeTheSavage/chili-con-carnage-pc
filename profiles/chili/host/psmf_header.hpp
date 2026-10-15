#pragma once
#include <span>
#include <cstdint>
#include <vector>
#include <stdexcept>
namespace chili {
struct PsmfHeader {
    unsigned version{},stream_offset{},stream_size{};
    std::vector<unsigned> stream_types;
    unsigned count(unsigned type) const {unsigned n=0;for(auto t:stream_types)if(t==type || (type==15 && (t==1 || t==2)))++n;return n;}
};
inline PsmfHeader parse_psmf_header(std::span<const std::uint8_t> bytes){
    if(bytes.size()<0x82)throw std::runtime_error("Truncated PSMF header");
    if(bytes[0]!='P' || bytes[1]!='S' || bytes[2]!='M' || bytes[3]!='F')throw std::runtime_error("Invalid PSMF magic");
    PsmfHeader h;for(unsigned i=0;i<4;++i)h.version|=unsigned(bytes[4+i])<<(i*8);
    if(!h.version)throw std::runtime_error("Invalid PSMF version");
    auto be=[&](unsigned off){unsigned v=0;for(unsigned i=0;i<4;++i)v=(v<<8)|bytes[off+i];return v;};
    h.stream_offset=be(8);h.stream_size=be(12);
    if(!h.stream_offset)throw std::runtime_error("Invalid PSMF stream offset");
    const unsigned streams=(unsigned(bytes[0x80])<<8)|bytes[0x81];
    if(streams>(bytes.size()-0x82)/16)throw std::runtime_error("Truncated PSMF stream descriptors");
    for(unsigned i=0;i<streams;++i){const auto id=bytes[0x82+i*16],sub=bytes[0x83+i*16];
        if((id&0xE0)==0xE0)h.stream_types.push_back(0);
        else if((id&0xBD)==0xBD)h.stream_types.push_back((sub&0xF0)?2:1);
    }
    return h;
}
}
