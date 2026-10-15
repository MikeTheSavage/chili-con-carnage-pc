#include "../host/atrac_stream.hpp"
#include "vcs_media_decoder.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
constexpr unsigned buffer=0x08810000,pcm=0x08900000,counts=0x08802000,info=0x08802100;
void require(bool good,const char *what){if(!good)throw std::runtime_error(what);}
std::vector<std::uint8_t> read(const std::filesystem::path &path){std::ifstream f(path,std::ios::binary);require(bool(f),"Cannot open owned ATRAC file");return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};}
std::vector<std::uint8_t> decode_all(chili::AtracStream &stream,psprecomp::GuestMemory &memory,const std::vector<std::uint8_t> &source,bool refill) {
    std::vector<std::uint8_t> result;unsigned read_rounds{},last_position{};
    for(unsigned iteration=0;iteration<50000;++iteration) {
        require(stream.position(counts+16)==0,"Position failed before stream end");last_position=memory.load32(counts+16);
        const auto status=stream.decode(pcm,counts,counts+4,counts+8);
        if(status==0x80630023 && refill) {
            require(stream.stream_info(info,info+4,info+8)==0,"Stream info failed");
            const auto where=memory.load32(info),available=memory.load32(info+4),offset=memory.load32(info+8);
            require(available && std::uint64_t(offset)+available<=source.size(),"Refill made no bounded progress");
            const auto bytes=std::min(available,(++read_rounds%3==0)?37u:available);
            memory.copy_in(where,std::span<const std::uint8_t>(source).subspan(offset,bytes));
            require(stream.add_data(available+1)==0x80630018,"Overlarge AddStreamData was accepted");
            require(stream.add_data(bytes)==0,"Refill AddStreamData failed");continue;
        }
        require(status==0,"Actual ATRAC decode failed");
        const unsigned samples=memory.load32(counts);require(samples && samples<=2048,"Invalid real decode sample count");
        // Two channels whatever the stream holds: the decode call's output format.
        const auto old=result.size();result.resize(old+samples*4);
        memory.copy_out(pcm,std::span<std::uint8_t>(result).subspan(old));
        require(stream.remaining(info+16)==0 && memory.load32(info+16)==memory.load32(counts+8),"RemainFrame disagrees with decode output");
        if(memory.load32(counts+4)) {
            require(last_position+samples==stream.total_samples(),"Final sample position does not match fact length");
            require(stream.position(counts+16)==0x80630024,"Ended position did not report all decoded");
            require(stream.decode(pcm,counts,counts+4,counts+8)==0x80630024 && memory.load32(counts)==0 && memory.load32(counts+4)==1,"Repeated EOF decode fabricated samples");
            std::cout<<"decoded samples="<<result.size()/4<<" refill rounds="<<read_rounds<<'\n';return result;
        }
        require(stream.position(counts+16)==0 && memory.load32(counts+16)==last_position+samples,"Decode position lost consumed samples");
    }
    throw std::runtime_error("ATRAC fixture exceeded progress bound");
}
void check_reset(chili::AtracStream &stream,psprecomp::GuestMemory &memory,const std::vector<std::uint8_t> &source,const std::vector<std::uint8_t> &reference,unsigned sample,bool resident) {
    require(stream.reset_info(sample,info)==0,"Reset info failed");
    for(unsigned p=16;p<32;p+=4)require(memory.load32(info+p)==0,"Nonloop reset requested a synthetic second buffer");
    const auto where=memory.load32(info),available=memory.load32(info+4),minimum=memory.load32(info+8),offset=memory.load32(info+12);
    require(where && minimum && minimum<=available && std::uint64_t(offset)+minimum<=source.size(),"Reset first-buffer bounds invalid");
    require(stream.reset(sample,resident?0:minimum,4)==0x80630017,"Unconfigured second buffer write accepted");
    if(!resident){memory.copy_in(where,std::span<const std::uint8_t>(source).subspan(offset,minimum));require(stream.reset(sample,minimum-1,0)==0x80630016,"Too little reset data accepted");}
    require(stream.reset(sample,resident?0:minimum,0)==0,"Valid reset failed");
    require(stream.position(counts+16)==0 && memory.load32(counts+16)==sample,"Reset position incorrect");
    require(stream.decode(pcm,counts,counts+4,counts+8)==0,"Real reset/preroll decode failed");
    std::vector<std::uint8_t> actual(memory.load32(counts)*4);memory.copy_out(pcm,actual);
    require(std::uint64_t(sample)*4+actual.size()<=reference.size(),"Reset output exceeded reference");
    require(std::equal(actual.begin(),actual.end(),reference.begin()+sample*4),"Reset/preroll PCM differs from original decoded samples");
}
}
int main(int argc,char **argv){try {
    require(argc==2 || argc==3,"Usage: chili_atrac_stream_test <owned SND0.AT3> [<owned one-channel stream>]");
    const auto source=read(argv[1]);psprecomp::GuestMemory memory;memory.copy_in(buffer,source);
    chili::AtracStream full(memory,0x1000);
    memory.store32(counts,0xDEADBEEF);require(full.remaining(counts)==0x80630010 && memory.load32(counts)==0xDEADBEEF,"Unset stream query changed output");
    require(full.attach(buffer,static_cast<unsigned>(source.size()),static_cast<unsigned>(source.size()))==0,"Owned ATRAC RIFF attach failed");
    require(full.channels()==2 && full.total_samples()==1366238 && full.loop_start()==0 && full.loop_end()==1366237,"Actual fact/encoder skip/smpl loop metadata incorrect");
    const auto baseline=decode_all(full,memory,source,false);
    require(full.add_data(0)==0x80630009,"Resident stream add did not return all-data-in-memory");
    vcs::AudioStreamDecoder decoder;require(decoder.open(argv[1],44100,2,0),"Independent WAV decoder open failed");
    std::vector<std::uint8_t> reference;std::vector<std::uint8_t> chunk(8192);
    for(;;){const auto bytes=decoder.read(chunk);reference.insert(reference.end(),chunk.begin(),chunk.begin()+bytes);if(bytes<chunk.size())break;}
    std::cout<<"reference samples="<<reference.size()/4<<" native trimmed samples="<<baseline.size()/4<<'\n';
    constexpr unsigned trim=3506+368;
    require(reference.size()>=baseline.size()+trim*4,"Independent raw reference is shorter than RIFF trim range");
    require(std::equal(baseline.begin(),baseline.end(),reference.begin()+trim*4),"Native PCM differs from independent WAV decode after actual fact/codec priming trim");
    check_reset(full,memory,source,baseline,123456,true);check_reset(full,memory,source,baseline,0,true);
    require(full.reset_info(full.total_samples(),info)==0x80630015,"Out-of-range sample reset accepted");
    memory.copy_in(buffer,std::span<const std::uint8_t>(source).first(500));chili::AtracStream streamed(memory,0x1000);
    require(streamed.attach(buffer,500,1540)==0,"Halfway streamed ATRAC attach failed");
    const auto streamed_pcm=decode_all(streamed,memory,source,true);
    require(streamed_pcm==baseline,"Tiny guest ring / partial writes changed real PCM");
    check_reset(streamed,memory,source,baseline,123456,false);check_reset(streamed,memory,source,baseline,0,false);
    memory.copy_in(buffer,source);chili::AtracStream invalid(memory,0x1000);require(invalid.attach(buffer,static_cast<unsigned>(source.size()),static_cast<unsigned>(source.size()))==0,"Error fixture setup failed");
    require(invalid.reset_info(0,info)==0,"Error fixture reset metadata missing");
    const auto first=memory.load32(info+12);std::vector<std::uint8_t> corrupt(376,0xFF),sentinel(8192,0xA5);memory.copy_in(buffer+first,corrupt);memory.copy_in(pcm,sentinel);
    require(invalid.decode(pcm,counts,counts+4,counts+8)==0x80630002,"Corrupt guest ATRAC was reported as successfully decoded");
    std::vector<std::uint8_t> actual(8192);memory.copy_out(pcm,actual);require(actual==sentinel,"Corrupt decode changed PCM or inserted silence");
    require(invalid.error_info(info)==0 && static_cast<std::int32_t>(memory.load32(info))<0,"Native decoder error was not recorded");
    if(argc==3) {
        // A voice line: one channel in the file, two in the decode call's output with the same sample in
        // both, and nothing written past samples * 4 bytes. tools/sound_assets.py extract gives the file.
        const auto voice=read(argv[2]);memory.copy_in(buffer,voice);
        chili::AtracStream first(memory,0x1000),mono(memory,0x1000);
        require(first.attach(buffer,static_cast<unsigned>(voice.size()),static_cast<unsigned>(voice.size()))==0,"One-channel ATRAC RIFF attach failed");
        require(first.channels()==1,"The second file is not a one-channel stream");
        unsigned lead=368;
        for(std::size_t p=12;p+16<=voice.size();++p)if(std::equal(voice.begin()+p,voice.begin()+p+4,"fact")){lead+=voice[p+12]|voice[p+13]<<8|voice[p+14]<<16|unsigned(voice[p+15])<<24;break;}
        std::vector<std::uint8_t> guard(8192+64,0xA5);memory.copy_in(pcm,guard);
        require(first.decode(pcm,counts,counts+4,counts+8)==0,"One-channel first decode failed");
        const auto first_samples=memory.load32(counts);memory.copy_out(pcm,guard);
        require(first_samples && first_samples<=2048 && std::all_of(guard.begin()+first_samples*4,guard.end(),[](std::uint8_t b){return b==0xA5;}),"One-channel decode wrote past samples * 4 bytes");
        // A second stream for the whole file: the decoder keeps its overlap history across a reset.
        require(mono.attach(buffer,static_cast<unsigned>(voice.size()),static_cast<unsigned>(voice.size()))==0,"One-channel ATRAC RIFF attach failed");
        const auto stereo=decode_all(mono,memory,voice,false);
        require(stereo.size()==std::size_t(mono.total_samples())*4,"One-channel stream did not decode to four bytes per sample");
        vcs::AudioStreamDecoder single;require(single.open(argv[2],44100,1,0),"Independent one-channel decode unavailable");
        std::vector<std::uint8_t> expected;for(;;){const auto bytes=single.read(chunk);expected.insert(expected.end(),chunk.begin(),chunk.begin()+bytes);if(bytes<chunk.size())break;}
        require(expected.size()>=(std::size_t(lead)+mono.total_samples())*2,"Independent one-channel reference is shorter than the stream");
        for(std::size_t i=0;i<mono.total_samples();++i) {
            const auto *want=expected.data()+(lead+i)*2;const auto *have=stereo.data()+i*4;
            require(have[0]==want[0] && have[1]==want[1] && have[2]==want[0] && have[3]==want[1],"One-channel stream: left and right are not the independent decode's sample");
        }
        // The way the game feeds a voice line: the header and three frames first, a ring of six frames.
        const unsigned frame_bytes=voice[32]|unsigned(voice[33])<<8;
        memory.copy_in(buffer,std::span<const std::uint8_t>(voice).first(96+3*frame_bytes));chili::AtracStream fed(memory,0x1000);
        require(fed.attach(buffer,96+3*frame_bytes,6*frame_bytes)==0,"One-channel halfway attach failed");
        require(decode_all(fed,memory,voice,true)==stereo,"One-channel stream through the game's ring differs from the whole-file decode");
        std::cout<<"one-channel stream: "<<mono.total_samples()<<" samples, two channels out, equal to the independent decode\n";
    }
    std::cout<<"PASS: genuine full/halfway ATRAC PCM equality, 1366238 consumed samples/EOF, partial guest ring writes, sample-accurate reset/preroll, loop metadata, second-buffer ABI/errors, strict corrupt-data failure\n";
}catch(const std::exception &e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
