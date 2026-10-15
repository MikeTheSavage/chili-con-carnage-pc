// Reads captures made from your own copy of the game at run time. No copyrighted encoded media is embedded.
#include "mpeg_runtime.hpp"
#include "av_runtime.hpp"
#include "thread_runtime.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr unsigned ring_address=0x08801000, mpeg_address=0x08801100, header_address=0x08802000;
constexpr unsigned work_address=0x08810000, ring_data=0x08830000, audio_au=0x08804000, video_au=0x08804040;
constexpr unsigned status_address=0x08804100, output_pointer=0x08804104, es_address=0x08805000;
constexpr unsigned pcm_address=0x08806000, image_address=0x08A00000;
constexpr unsigned callback_address=0x08000100, done_address=0x08000104;
constexpr unsigned no_data=0x80618001, decode_error=0x80628002;
void require(bool good,const std::string &what) { if(!good)throw std::runtime_error(what); }
Bytes read_file(const std::filesystem::path &path) {
    std::ifstream file(path,std::ios::binary);require(bool(file),"Cannot read owned capture: "+path.string());
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
unsigned be32(std::span<const std::uint8_t> v,unsigned p) {
    require(p+4<=v.size(),"BE32 outside capture");
    return unsigned(v[p])<<24|unsigned(v[p+1])<<16|unsigned(v[p+2])<<8|v[p+3];
}
std::uint64_t pts(std::span<const std::uint8_t> v) {
    require(v.size()>=5 && (v[0]&1) && (v[2]&1) && (v[4]&1),"Invalid captured PTS markers");
    return std::uint64_t((v[0]>>1)&7)<<30|std::uint64_t(v[1])<<22|
        std::uint64_t(v[2]>>1)<<15|std::uint64_t(v[3])<<7|(v[4]>>1);
}
struct Capture {
    Bytes header,program,ring;
    std::array<Bytes,2> frames;
    std::uint64_t first_pts{};
    explicit Capture(const std::filesystem::path &directory) {
        header=read_file(directory/"mpeg_startup_failure_header.bin");
        program=read_file(directory/"mpeg_startup_failure_program.bin");
        ring=read_file(directory/"mpeg_startup_failure_ring.bin");
        require(header.size()==2048 && program.size()==2048 && ring.size()==524288,"Capture sizes changed");
        require(std::equal(header.begin(),header.begin()+8,"PSMF0014") && be32(header,8)==2048 &&
            be32(header,12)==3106816,"Unexpected actual PSMF header");
        require(std::equal(program.begin(),program.end(),ring.begin()+2048),"Failure packet differs from ring packet 1");
        const std::array<std::uint8_t,8> sync{0x0F,0xD0,0x28,0x5C,0,0,0,0};
        std::vector<std::size_t> starts;
        for(std::size_t i=0;i+sync.size()<=program.size();++i)
            if(std::equal(sync.begin(),sync.end(),program.begin()+i))starts.push_back(i);
        require(starts==std::vector<std::size_t>{35,787,1539},"Actual complete-header boundaries changed");
        // Boundaries are measured in captured bytes, independently of the size-field formula.
        for(unsigned i=0;i<2;++i)frames[i].assign(program.begin()+starts[i]+8,program.begin()+starts[i+1]);
        require(frames[0].size()==744 && frames[1].size()==744,"Measured encoded payload is not 744 bytes");
        const std::array<std::uint8_t,8> old_landing{0xCB,0x35,0x0A,0xD9,0x54,0x41,0x89,0x80};
        require(std::equal(old_landing.begin(),old_landing.end(),program.begin()+779),"Old formula landing changed");
        require(program[14]==0 && program[15]==0 && program[16]==1 && program[17]==0xBD &&
            (unsigned(program[18])<<8|program[19])==2028 && program[20]==0x81 &&
            program[21]==0x81 && program[22]==8 && program[31]==0 &&
            program[32]==0 && program[33]==0 && program[34]==0,"Unexpected actual private PES framing");
        first_pts=pts(std::span<const std::uint8_t>(program).subspan(23,5));
        std::cout<<"PASS capture boundaries: BD PES length 2028, valid stream 0, ATRAC headers 35/787/1539, "
            <<"encoded bytes 744, old landing 779 is compressed data, first audio PTS "<<first_pts<<'\n';
    }
};

// Independently extract measured frame payloads from the captured PES, then use
// FFmpeg directly. The bundled build omits OMA demuxing; its primary source still
// provides an independent size-field oracle documented in the report.
std::array<Bytes,2> independent_decode(const Capture &capture) {
    AVCodecContext *codec=avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_ATRAC3P));
    require(codec,"Independent ATRAC decoder allocation failed");
    codec->block_align=static_cast<int>(capture.frames[0].size());
    codec->sample_rate=44100;av_channel_layout_default(&codec->ch_layout,2);
    require(avcodec_open2(codec,codec->codec,nullptr)>=0,"Independent ATRAC decoder open failed");
    AVFrame *frame=av_frame_alloc();AVPacket *packet=av_packet_alloc();require(frame && packet,"Reference frame allocation failed");
    SwrContext *resampler{};AVChannelLayout stereo{};av_channel_layout_default(&stereo,2);
    std::array<Bytes,2> pcm;
    for(unsigned i=0;i<2;++i) {
        require(av_new_packet(packet,static_cast<int>(capture.frames[i].size()))>=0,"Reference packet allocation failed");
        std::copy(capture.frames[i].begin(),capture.frames[i].end(),packet->data);
        require(avcodec_send_packet(codec,packet)>=0 && avcodec_receive_frame(codec,frame)>=0 &&
            frame->nb_samples==2048 && frame->sample_rate==44100 && frame->ch_layout.nb_channels==2,
            "Independent captured ATRAC decode failed");
        if(!resampler)require(swr_alloc_set_opts2(&resampler,&stereo,AV_SAMPLE_FMT_S16,44100,&frame->ch_layout,
            static_cast<AVSampleFormat>(frame->format),44100,0,nullptr)>=0 && swr_init(resampler)>=0,"PCM converter open failed");
        pcm[i].resize(8192);auto *destination=pcm[i].data();
        require(swr_convert(resampler,&destination,2048,const_cast<const std::uint8_t**>(frame->extended_data),2048)==2048,
            "Reference PCM conversion failed");
        require(std::any_of(pcm[i].begin(),pcm[i].end(),[](auto value){return value!=0;}),"Actual ATRAC reference frame is silent");
        av_frame_unref(frame);av_packet_unref(packet);
    }
    swr_free(&resampler);av_channel_layout_uninit(&stereo);av_packet_free(&packet);av_frame_free(&frame);
    avcodec_free_context(&codec);
    std::cout<<"PASS independent extraction/FFmpeg decode: 2 measured 744-byte captured frames decode to nonzero 2048-sample stereo PCM; no size-formula-generated source\n";
    return pcm;
}
struct Fixture {
    psprecomp::Runtime runtime;chili::AvRuntime av{runtime};chili::ThreadRuntime threads{runtime};
    chili::MpegRuntime mpeg{runtime,av,&threads};Bytes source;std::size_t source_cursor{};
    static Fixture *active;
    Fixture(const Capture &capture,Bytes packets,unsigned count) : source(std::move(packets)) {
        active=this;runtime.cpu().gpr[29]=0x09FFF000;threads.initialize_bootstrap();
        runtime.register_function(callback_address,&callback,"captured_ring_callback");
        runtime.register_function(done_address,[](auto &rt,auto &){rt.stop("fixture-return");},"captured_fixture_done");
        for(unsigned module:{0u,3u}) { runtime.cpu().gpr[4]=module;runtime.invoke_import("sceUtility",0xC629AF26,runtime.cpu());
            require(runtime.cpu().gpr[2]==0,"AV module load failed"); }
        require(call(0x682A619B)==0,"MPEG Init failed");
        runtime.memory().copy_in(header_address,capture.header);
        require(call(0x37295ED8,{ring_address,count,ring_data,count*2152,callback_address,0x1234})==0,"Ring construct failed");
        require(call(0xD8C5F121,{mpeg_address,work_address,0x10000,ring_address,480,0,0})==0,"MPEG Create failed");
        require(call(0x21FF80E4,{mpeg_address,header_address,status_address})==0 && runtime.memory().load32(status_address)==2048,
            "Actual PSMF query/analyze failed");
        runtime.memory().store32(output_pointer,image_address);
        require(call(0xB240A59E,{ring_address,count,count})==count,"Captured source refill failed");
    }
    ~Fixture() { active=nullptr; }
    static void callback(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c) {
        auto &self=*active;require(c.gpr[6]==0x1234,"Ring callback parameter mismatch");
        const auto accepted=std::min<std::size_t>(c.gpr[5],(self.source.size()-self.source_cursor)/2048);
        rt.memory().copy_in(c.gpr[4],std::span<const std::uint8_t>(self.source).subspan(self.source_cursor,accepted*2048));
        self.source_cursor+=accepted*2048;c.gpr[2]=static_cast<unsigned>(accepted);c.pc=c.gpr[31];
    }
    unsigned call(unsigned nid,std::initializer_list<unsigned> arguments={}) {
        auto &c=runtime.cpu();c.pc=0;c.gpr[31]=done_address;
        for(unsigned i=4;i<=10;++i)c.gpr[i]=0;unsigned i=4;for(auto value:arguments)c.gpr[i++]=value;
        auto expected=c;expected.pc=done_address;runtime.invoke_import("sceMpeg",nid,c);
        if(c.pc) {
            runtime.run(c.pc,100);require(runtime.stop_reason()=="fixture-return","Unexpected native dispatch stop: "+runtime.stop_reason());
            expected.gpr[2]=c.gpr[2];
            require(c.gpr==expected.gpr && c.fpr==expected.fpr && c.vfpu==expected.vfpu && c.vfpu_ctrl==expected.vfpu_ctrl &&
                c.hi==expected.hi && c.lo==expected.lo && c.fcr31==expected.fcr31,"Deferred service changed caller context");
        }
        return c.gpr[2];
    }
    unsigned init_audio() {
        const auto stream=call(0x42560F23,{mpeg_address,1,0});require(stream<0x80000000,"Audio stream register failed");
        require(call(0x167AFD9E,{mpeg_address,es_address,audio_au})==0,"Audio AU init failed");return stream;
    }
    unsigned init_video() {
        const auto stream=call(0x42560F23,{mpeg_address,0,0});const auto es=call(0xA780CF7E,{mpeg_address});
        require(es==1 && call(0x167AFD9E,{mpeg_address,es,video_au})==0,"Video AU init failed");return stream;
    }
    void cleanup() {
        require(call(0x606A4649,{mpeg_address})==0 && call(0x13407F13,{ring_address})==0 && call(0x874624D6)==0,
            "MPEG cleanup failed");
    }
};
Fixture *Fixture::active{};

void valid(const Capture &capture) {
    const auto reference=independent_decode(capture);
    {
    Fixture f(capture,capture.program,1);const auto stream=f.init_audio();auto &m=f.runtime.memory();
    for(unsigned i=0;i<2;++i) {
        require(f.call(0xE1CE83A7,{mpeg_address,stream,audio_au,status_address})==0,"Captured GetAtracAu failed");
        require(m.load32(audio_au+20)==744,"Guest AU has wrong encoded size");
        Bytes encoded(744);m.copy_out(es_address,encoded);require(encoded==capture.frames[i],"Guest AU truncates or crosses actual frame boundary");
        const auto guest_pts=std::uint64_t(m.load32(audio_au))<<32|m.load32(audio_au+4);
        require(guest_pts==capture.first_pts+i*2048ull*90000ull/44100,"Captured audio timestamp drift");
        require(f.call(0x800C44DF,{mpeg_address,audio_au,pcm_address,i?0u:1u})==0,"Captured native AtracDecode failed");
        Bytes actual(8192);m.copy_out(pcm_address,actual);require(actual==reference[i],"Guest PCM differs from independently demuxed captured reference");
    }
    require(f.call(0xE1CE83A7,{mpeg_address,stream,audio_au,status_address})==no_data,"Partial third frame fabricated an AU");
    f.cleanup();
    std::cout<<"PASS live sceMpeg ABI: actual header + actual PES; 2 exact 744-byte AUs, anchored PTS, PCM byte-equal to independent extraction/decoder; partial frame remains pending\n";
    }
    Fixture movie(capture,capture.ring,256);const auto video=movie.init_video();unsigned presented{};
    for(unsigned i=0;i<16;++i) {
        require(movie.call(0xFE246728,{mpeg_address,video,video_au,status_address})==0,"Captured multiplexed GetAvcAu failed");
        require(movie.call(0x0E3C2E9D,{mpeg_address,video_au,480,output_pointer,status_address})==0,"Captured multiplexed AvcDecode failed");
        presented+=movie.runtime.memory().load32(status_address)!=0;
    }
    require(presented>0,"Captured multiplexed movie produced no native video frames");movie.cleanup();
    std::cout<<"PASS live multiplexed stream: 16 H264 AU/decode calls coexist with actual private audio PES; "<<presented<<" video outputs\n";
}

void audit(const Capture &capture,bool expect_latched=false) {
    // This is a bounded characterization of the current robustness gap, not an
    // acceptance rule for the desired parser. Do not turn it into a passing-fidelity claim.
    auto broken=capture.program;broken[787]=0xEE;
    Bytes packets;for(unsigned i=0;i<3;++i)packets.insert(packets.end(),broken.begin(),broken.end());
    Fixture f(capture,std::move(packets),3);const auto audio=f.init_audio();const auto video=f.init_video();auto &m=f.runtime.memory();
    const auto failed=f.call(0xFE246728,{mpeg_address,video,video_au,status_address});
    require(failed==decode_error,"Mutated private frame header did not produce strict failure");
    const auto remaining1=m.load32(ring_address+12);
    const auto leaked=f.call(0xE1CE83A7,{mpeg_address,audio,audio_au,status_address});
    const bool leaked_exact=leaked==0 && m.load32(audio_au+20)==744;
    if(leaked_exact) { Bytes bytes(744);m.copy_out(es_address,bytes);require(bytes==capture.frames[0],"Failure leaked unexpected audio bytes"); }
    if(leaked==0)require(f.call(0x800C44DF,{mpeg_address,audio_au,pcm_address,1})==0,"Leaked valid frame failed decode");
    const auto retry=f.call(0xFE246728,{mpeg_address,video,video_au,status_address});
    const auto remaining2=m.load32(ring_address+12);
    require(retry==decode_error,"Mutated parse failure did not recur");
    std::cout<<"AUDIT current malformed-input behavior: first GetAvcAu="<<std::hex<<failed<<", following GetAtracAu="<<leaked
        <<", retry GetAvcAu="<<retry<<std::dec<<", ring remaining "<<remaining1<<" -> "<<remaining2
        <<", prior complete AU visible after failed packet="<<leaked_exact<<'\n';
    if(expect_latched) {
        require(leaked==decode_error && !leaked_exact && remaining1==2 && remaining2==2,
            "Proposed failure latch leaked an AU or consumed another packet");
        f.cleanup();
        std::cout<<"PASS proposed strict failure latch: both stream types retain fatal status, no new AU escapes, ring cursor remains unchanged on retry; clean teardown succeeds\n";
    } else {
        require(leaked_exact && remaining1==2 && remaining2==1,"Known failure behavior changed; review characterization and fix it deliberately");
        f.cleanup();std::cout<<"CHARACTERIZED robustness gap: failed PES retains program/audio prefixes while completed AUs escape; retry consumes another ring packet. Hardware recovery semantics are not established.\n";
    }
}
}
int main(int argc,char **argv) { try {
    require(argc==3,"Usage: chili_mpeg_capture_test <owned capture directory> <valid|audit|audit_latched>");
    Capture capture(argv[1]);const std::string mode=argv[2];
    if(mode=="valid")valid(capture);else if(mode=="audit")audit(capture);else if(mode=="audit_latched")audit(capture,true);
    else throw std::runtime_error("Unknown test mode");
} catch(const std::exception &error) { std::cerr<<"FAIL: "<<error.what()<<'\n';return 1; } }
