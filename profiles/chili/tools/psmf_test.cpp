#include "../host/psmf_header.hpp"
#include <iostream>
#include <array>
int main(){try{
std::array<std::uint8_t,2048> b{};b[0]='P';b[1]='S';b[2]='M';b[3]='F';b[4]='0';b[5]='0';b[6]='1';b[7]='4';b[10]=8;b[14]=0x20;b[0x81]=3;b[0x82]=0xE0;b[0x92]=0xBD;b[0xA2]=0xBD;b[0xA3]=0x10;
auto h=chili::parse_psmf_header(b);
if(h.stream_offset!=2048 || h.stream_size!=8192 || h.count(0)!=1 || h.count(1)!=1 || h.count(2)!=1 || h.count(15)!=2 || h.count(99)!=0)throw std::runtime_error("PSMF metadata mismatch");
auto rejected=[&](auto bytes){try{chili::parse_psmf_header(bytes);}catch(const std::runtime_error &){return true;}return false;};
if(!rejected(std::span<const std::uint8_t>(b.data(),0x90)))throw std::runtime_error("truncated descriptors accepted");
b[0]='X';if(!rejected(std::span<const std::uint8_t>(b)))throw std::runtime_error("invalid magic accepted");
std::cout<<"PASS: PSMF big-endian offset/size, AVC/ATRAC/PCM stream counts, audio aggregate and truncated header rejection\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
