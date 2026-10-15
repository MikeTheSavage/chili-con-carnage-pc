// Decodes an ATRAC3plus RIFF file to WAV with FFmpeg's own demuxer, apart from the game's stream
// path (host/atrac_stream.cpp, which frames the packets and trims the lead-in itself): the reference a
// captured stream is compared with (tools/speech_compare.py).
//
//   chili_at3_reference <stream.at3> <output.wav>
//
// The output keeps the file's channel count and holds only the samples the file's fact chunk declares:
// the encoder's lead-in (the fact chunk's last word plus the decoder's own 368 samples) is cut off the
// front and the padding off the end, which is what a PSP plays.
#include "vcs_media_decoder.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
unsigned le16(const std::vector<std::uint8_t> &b,std::size_t p){return b[p]|unsigned(b[p+1])<<8;}
unsigned le32(const std::vector<std::uint8_t> &b,std::size_t p){return le16(b,p)|le16(b,p+2)<<16;}
void put32(std::ofstream &out,std::uint32_t v){const char b[4]{char(v),char(v>>8),char(v>>16),char(v>>24)};out.write(b,4);}
void put16(std::ofstream &out,std::uint16_t v){const char b[2]{char(v),char(v>>8)};out.write(b,2);}
constexpr unsigned decoder_delay=368;
}
int main(int argc,char **argv){try {
    if(argc!=3)throw std::runtime_error("Usage: chili_at3_reference <stream.at3> <output.wav>");
    std::ifstream file(argv[1],std::ios::binary);if(!file)throw std::runtime_error("Cannot open the stream");
    const std::vector<std::uint8_t> source{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    if(source.size()<44 || std::memcmp(source.data(),"RIFF",4) || std::memcmp(source.data()+8,"WAVE",4))throw std::runtime_error("Not a RIFF/WAVE file");
    unsigned channels{},rate{},samples{},skip{};bool fact{};
    for(std::size_t p=12;p+8<=source.size();){
        const auto length=le32(source,p+4);
        if(!std::memcmp(source.data()+p,"fmt ",4) && length>=16){channels=le16(source,p+10);rate=le32(source,p+12);}
        else if(!std::memcmp(source.data()+p,"fact",4) && length>=4){samples=le32(source,p+8);skip=length>=8?le32(source,p+8+(length>=12?8:4)):0;fact=true;}
        else if(!std::memcmp(source.data()+p,"data",4))break;
        p+=8+((std::size_t(length)+1)&~std::size_t(1));
    }
    if(channels<1 || channels>2 || !rate)throw std::runtime_error("No usable fmt chunk");
    vcs::AudioStreamDecoder decoder;
    if(!decoder.open(argv[1],rate,channels,0))throw std::runtime_error("FFmpeg cannot open the stream");
    std::vector<std::uint8_t> pcm,chunk(16384);
    for(;;){const auto bytes=decoder.read(chunk);pcm.insert(pcm.end(),chunk.begin(),chunk.begin()+bytes);if(bytes<chunk.size())break;}
    const std::size_t frame=channels*2u,decoded=pcm.size()/frame,lead=std::size_t(skip)+decoder_delay;
    if(decoded<=lead)throw std::runtime_error("The stream decodes to less than its lead-in");
    const std::size_t kept=fact?std::min<std::size_t>(samples,decoded-lead):decoded-lead;
    std::ofstream out(argv[2],std::ios::binary);if(!out)throw std::runtime_error("Cannot create the output");
    const auto bytes=static_cast<std::uint32_t>(kept*frame);
    out.write("RIFF",4);put32(out,36u+bytes);out.write("WAVEfmt ",8);put32(out,16);put16(out,1);put16(out,static_cast<std::uint16_t>(channels));
    put32(out,rate);put32(out,rate*static_cast<std::uint32_t>(frame));put16(out,static_cast<std::uint16_t>(frame));put16(out,16);
    out.write("data",4);put32(out,bytes);out.write(reinterpret_cast<const char *>(pcm.data()+lead*frame),bytes);
    std::cout<<argv[2]<<": "<<channels<<" channel(s), "<<rate<<" Hz, "<<kept<<" samples ("<<decoded<<" decoded, "<<lead<<" lead-in cut, fact "<<samples<<")\n";
}catch(const std::exception &e){std::cerr<<"chili_at3_reference: "<<e.what()<<'\n';return 1;}}
