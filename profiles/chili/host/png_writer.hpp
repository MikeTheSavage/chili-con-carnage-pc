#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>
namespace chili {
// Smallest useful PNG encoder: 8-bit RGB in stored (uncompressed) deflate
// blocks. Pixels are the 0xAARRGGBB words decode_display_frame produces.
inline bool write_png(const std::filesystem::path &path,unsigned width,unsigned height,std::span<const unsigned> pixels){
    if(!width || !height || pixels.size()!=static_cast<std::size_t>(width)*height)return false;
    static const auto table=[]{
        std::array<std::uint32_t,256> t{};
        for(std::uint32_t n=0;n<256u;++n){auto c=n;for(unsigned k=0;k<8u;++k)c=(c&1u)?0xEDB88320u^(c>>1u):c>>1u;t[n]=c;}
        return t;
    }();
    const std::size_t row=1u+static_cast<std::size_t>(width)*3u,raw_size=row*height;
    std::vector<std::uint8_t> raw(raw_size);
    for(unsigned y=0;y<height;++y){
        auto *out=raw.data()+row*y;*out++=0u;
        const auto *in=pixels.data()+static_cast<std::size_t>(width)*y;
        for(unsigned x=0;x<width;++x){const auto p=in[x];*out++=static_cast<std::uint8_t>(p>>16u);*out++=static_cast<std::uint8_t>(p>>8u);*out++=static_cast<std::uint8_t>(p);}
    }
    std::vector<std::uint8_t> file{0x89u,'P','N','G','\r','\n',0x1Au,'\n'};
    file.reserve(raw_size+raw_size/65535u*5u+128u);
    auto be32=[&file](std::uint32_t v){for(int shift=24;shift>=0;shift-=8)file.push_back(static_cast<std::uint8_t>(v>>shift));};
    auto chunk=[&](const char *type,std::span<const std::uint8_t> data){
        be32(static_cast<std::uint32_t>(data.size()));const auto start=file.size();
        file.insert(file.end(),type,type+4);file.insert(file.end(),data.begin(),data.end());
        std::uint32_t crc=0xFFFFFFFFu;
        for(auto i=start;i<file.size();++i)crc=table[(crc^file[i])&255u]^(crc>>8u);
        be32(~crc);
    };
    std::array<std::uint8_t,13> header{};
    for(unsigned i=0;i<4u;++i){header[i]=static_cast<std::uint8_t>(width>>(24u-8u*i));header[4u+i]=static_cast<std::uint8_t>(height>>(24u-8u*i));}
    header[8]=8u;header[9]=2u;
    chunk("IHDR",header);
    std::vector<std::uint8_t> zlib{0x78u,0x01u};
    zlib.reserve(raw_size+raw_size/65535u*5u+16u);
    std::uint32_t a=1u,b=0u;
    for(std::size_t offset=0;offset<raw_size;){
        const auto count=std::min<std::size_t>(65535u,raw_size-offset);
        zlib.push_back(offset+count==raw_size?1u:0u);
        zlib.push_back(static_cast<std::uint8_t>(count));zlib.push_back(static_cast<std::uint8_t>(count>>8u));
        zlib.push_back(static_cast<std::uint8_t>(~count));zlib.push_back(static_cast<std::uint8_t>(~count>>8u));
        zlib.insert(zlib.end(),raw.begin()+static_cast<std::ptrdiff_t>(offset),raw.begin()+static_cast<std::ptrdiff_t>(offset+count));
        // Reducing every 4096 bytes stays under the 5552-byte bound of 32-bit Adler sums.
        for(std::size_t i=offset;i<offset+count;++i){a+=raw[i];b+=a;if((i&0xFFFu)==0xFFFu){a%=65521u;b%=65521u;}}
        a%=65521u;b%=65521u;offset+=count;
    }
    const auto adler=(b<<16u)|a;
    for(int shift=24;shift>=0;shift-=8)zlib.push_back(static_cast<std::uint8_t>(adler>>shift));
    chunk("IDAT",zlib);
    chunk("IEND",{});
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out.write(reinterpret_cast<const char*>(file.data()),static_cast<std::streamsize>(file.size()));
    out.close();return !out.fail();
}
}
