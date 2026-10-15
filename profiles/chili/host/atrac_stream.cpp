// PSPRecomp contributors, MIT license. Public buffer ABI: PSPSDK pspatrac3.h.
// This implementation decodes current guest bytes with the native FFmpeg
// backend. It does not cache a substitute copy of the encoded file or PCM.
#include "atrac_stream.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}
namespace chili {
namespace {
constexpr unsigned param_error=0x80630001u,api_error=0x80630002u,bad_codec=0x80630004u;
constexpr unsigned unknown_format=0x80630006u,format_mismatch=0x80630007u,bad_data=0x80630008u;
constexpr unsigned all_in_memory=0x80630009u,unset_data=0x80630010u,small_read=0x80630011u;
constexpr unsigned read_overflow=0x80630013u,bad_alignment=0x80630014u,bad_sample=0x80630015u;
constexpr unsigned first_write=0x80630016u,second_write=0x80630017u,add_overflow=0x80630018u;
constexpr unsigned no_buffer=0x80630023u,all_decoded=0x80630024u,unknown_loop=0xFFFFFFFFu;
unsigned le16(std::span<const std::uint8_t> bytes,std::size_t p){return bytes[p]|unsigned(bytes[p+1])<<8;}
unsigned le32(std::span<const std::uint8_t> bytes,std::size_t p){return le16(bytes,p)|le16(bytes,p+2)<<16;}
bool fourcc(std::span<const std::uint8_t> bytes,std::size_t p,const char *name){return std::equal(bytes.begin()+p,bytes.begin()+p+4,name);}
bool pointer(const psprecomp::GuestMemory &m,unsigned address,std::size_t length,unsigned alignment=4){return address && !(address&(alignment-1)) && m.contains(address,length);}
}
struct AtracStream::Impl {
    psprecomp::GuestMemory &memory;
    unsigned type{},base{},capacity{},file_size{},data_offset{},data_size{},block{},rate{},channel_count{};
    unsigned sample_count{},encoder_skip{},initial_skip{},loop_begin=unknown_loop,loop_finish=unknown_loop;
    unsigned head{},tail{},buffered{},next_file_offset{},decode_file_offset{},sample_position{},skip_samples{};
    bool ready{},resident{},ended{};
    // What sceAtracDecodeData writes. A PSP hands every block over as 16-bit stereo whatever the stream
    // holds (only sceAtracSetMOut* streams decode to one channel, and the game does not use them): the
    // game takes samples * 4 bytes from its buffer. Written as one channel, a one-channel stream (every
    // voice line) came out as half a block at double speed and half a block of leftover data.
    static constexpr unsigned output_channels=2u;
    std::vector<std::uint8_t> block_pcm,stereo_pcm;
    int internal_error{};
    AVCodecContext *codec{};AVFrame *frame{};SwrContext *resampler{};
    Impl(psprecomp::GuestMemory &m,unsigned codec_type):memory(m),type(codec_type){}
    ~Impl(){release_codec();}
    void release_codec(){if(resampler)swr_free(&resampler);if(frame)av_frame_free(&frame);if(codec)avcodec_free_context(&codec);}
    unsigned writable()const {
        if(!ready || resident || next_file_offset>=file_size)return 0;
        return std::min({capacity-buffered,capacity-tail,file_size-next_file_offset});
    }
    int remaining()const {
        if(resident)return -1;
        if(next_file_offset>=file_size)return -2;
        return static_cast<int>(std::min(buffered,data_offset+data_size-decode_file_offset)/block);
    }
    unsigned open_codec() {
        const auto *decoder=avcodec_find_decoder(type==0x1000?AV_CODEC_ID_ATRAC3P:AV_CODEC_ID_ATRAC3);
        if(!decoder)return bad_codec;
        codec=avcodec_alloc_context3(decoder);frame=av_frame_alloc();
        if(!codec || !frame){release_codec();return api_error;}
        codec->sample_rate=static_cast<int>(rate);codec->block_align=static_cast<int>(block);
        av_channel_layout_default(&codec->ch_layout,static_cast<int>(channel_count));
        internal_error=avcodec_open2(codec,decoder,nullptr);
        if(internal_error<0){release_codec();return api_error;}return 0;
    }
    unsigned attach(unsigned address,unsigned read,unsigned size) {
        if(type!=0x1000 && type!=0x1001)return bad_codec;
        if(address&3)return bad_alignment;
        if(read>size)return read_overflow;
        if(read<12)return small_read;
        if(!pointer(memory,address,size))return param_error;
        // RIFF headers must be wholly present through the data chunk descriptor.
        // Bound metadata reads even when the guest supplies a large buffer.
        std::vector<std::uint8_t> header(std::min(read,65536u));memory.copy_out(address,header);
        if(!fourcc(header,0,"RIFF") || !fourcc(header,8,"WAVE"))return unknown_format;
        const auto length=std::uint64_t(le32(header,4))+8;
        if(length>0x7FFFFFFF || length<12 || read>length)return bad_data;
        unsigned local_channels{},local_rate{},local_block{},local_samples{},local_skip{},local_data{},local_data_size{};
        unsigned local_loop_begin=unknown_loop,local_loop_end=unknown_loop;
        bool format_seen{},fact_seen{};
        for(std::size_t p=12;p+8<=header.size();) {
            const unsigned chunk=le32(header,p+4);
            if(std::uint64_t(p)+8+chunk>length)return bad_data;
            if(fourcc(header,p,"data")){local_data=static_cast<unsigned>(p+8);local_data_size=chunk;break;}
            if(std::uint64_t(p)+8+chunk>header.size())return small_read;
            const auto payload=std::span<const std::uint8_t>(header).subspan(p+8,chunk);
            if(fourcc(header,p,"fmt ")) {
                if(format_seen || chunk<18)return bad_data;
                const unsigned tag=le16(payload,0);local_channels=le16(payload,2);local_rate=le32(payload,4);local_block=le16(payload,12);
                if((type==0x1000 && tag!=0xFFFE) || (type==0x1001 && tag!=0x270))return format_mismatch;
                if(type==0x1001)return bad_codec; // Installed minimal backend has no ATRAC3 decoder/extradata path.
                constexpr std::array<std::uint8_t,16> atrac3p_guid{0xBF,0xAA,0x23,0xE9,0x58,0xCB,0x71,0x44,0xA1,0x19,0xFF,0xFA,0x01,0xE4,0xCE,0x62};
                if(chunk<40 || !std::equal(atrac3p_guid.begin(),atrac3p_guid.end(),payload.begin()+24))return unknown_format;
                if(local_channels<1 || local_channels>2 || local_rate!=44100 || !local_block || local_block>2112)return bad_data;
                format_seen=true;
            } else if(fourcc(header,p,"fact")) {
                if(fact_seen || (chunk!=4 && chunk!=8 && chunk!=12))return bad_data;
                local_samples=le32(payload,0);local_skip=chunk>=8?le32(payload,chunk==12?8:4):0;fact_seen=true;
            } else if(fourcc(header,p,"smpl")) {
                if(chunk<36)return bad_data;
                const unsigned loops=le32(payload,28);
                if(loops>1 || (loops && (chunk<60 || le32(payload,40)!=0)))return bad_data;
                if(loops){local_loop_begin=le32(payload,44);local_loop_end=le32(payload,48);}
            }
            p+=8ull+((std::uint64_t(chunk)+1)&~1ull);
        }
        if(!format_seen || !local_data || !local_data_size || local_data_size%local_block)return bad_data;
        if(read<local_data || size<local_block*3u)return small_read;
        const auto raw_samples=std::uint64_t(local_data_size/local_block)*2048;
        const auto trim=std::uint64_t(local_skip)+368; // ATRAC3+ decoder priming delay, plus RIFF encoder padding.
        if(trim>=raw_samples || (fact_seen && (!local_samples || std::uint64_t(local_samples)+trim>raw_samples)))return bad_data;
        if(!fact_seen){if(raw_samples-trim>0x7FFFFFFF)return bad_data;local_samples=static_cast<unsigned>(raw_samples-trim);}
        if(local_loop_begin!=unknown_loop) {
            if(local_loop_begin<local_skip || local_loop_end<local_loop_begin || local_loop_end-local_skip>=local_samples)return bad_data;
            local_loop_begin-=local_skip;local_loop_end-=local_skip;
        }
        release_codec();ready=false;
        base=address;capacity=size;file_size=static_cast<unsigned>(length);data_offset=local_data;data_size=local_data_size;
        block=local_block;rate=local_rate;channel_count=local_channels;sample_count=local_samples;encoder_skip=local_skip;initial_skip=static_cast<unsigned>(trim);
        loop_begin=local_loop_begin;loop_finish=local_loop_end;head=data_offset;tail=read%capacity;buffered=read-data_offset;
        next_file_offset=read;decode_file_offset=data_offset;sample_position=0;skip_samples=initial_skip;
        resident=read==file_size && capacity>=file_size;ended=false;
        const auto error=open_codec();if(error)return error;ready=true;internal_error=0;return 0;
    }
    unsigned stream_info(unsigned write_pointer,unsigned available,unsigned file_offset) {
        if(!ready)return unset_data;
        if(!pointer(memory,write_pointer,4) || !pointer(memory,available,4) || !pointer(memory,file_offset,4))return param_error;
        const auto count=writable();memory.store32(write_pointer,count?base+tail:0);memory.store32(available,count);memory.store32(file_offset,next_file_offset);return 0;
    }
    unsigned add(unsigned bytes) {
        if(!ready)return unset_data;
        if(resident)return all_in_memory;
        if(bytes>writable())return add_overflow;
        tail=(tail+bytes)%capacity;buffered+=bytes;next_file_offset+=bytes;return 0;
    }
    unsigned decode_packet(std::vector<std::uint8_t> &pcm) {
        if(buffered<block || decode_file_offset+block>data_offset+data_size)return no_buffer;
        AVPacket *packet=av_packet_alloc();if(!packet)return api_error;
        int status=av_new_packet(packet,static_cast<int>(block));
        if(status>=0) {
            const auto first=std::min(block,capacity-head);
            memory.copy_out(base+head,std::span<std::uint8_t>(packet->data,first));
            if(first<block)memory.copy_out(base,std::span<std::uint8_t>(packet->data+first,block-first));
            status=avcodec_send_packet(codec,packet);
        }
        av_packet_free(&packet);
        if(status<0){internal_error=status;return api_error;}
        status=avcodec_receive_frame(codec,frame);
        if(status<0){internal_error=status;return api_error;}
        if(frame->nb_samples!=2048 || frame->sample_rate!=static_cast<int>(rate) || frame->ch_layout.nb_channels!=static_cast<int>(channel_count))return bad_data;
        if(!resampler) {
            AVChannelLayout layout{};av_channel_layout_default(&layout,static_cast<int>(channel_count));
            status=swr_alloc_set_opts2(&resampler,&layout,AV_SAMPLE_FMT_S16,static_cast<int>(rate),&frame->ch_layout,
                static_cast<AVSampleFormat>(frame->format),static_cast<int>(rate),0,nullptr);av_channel_layout_uninit(&layout);
            if(status>=0)status=swr_init(resampler);if(status<0){internal_error=status;return api_error;}
        }
        pcm.resize(2048u*channel_count*2u);auto *destination=pcm.data();
        status=swr_convert(resampler,&destination,2048,const_cast<const std::uint8_t **>(frame->extended_data),2048);
        av_frame_unref(frame);if(status!=2048){internal_error=status;return api_error;}
        head=(head+block)%capacity;buffered-=block;decode_file_offset+=block;return 0;
    }
    unsigned decode(unsigned pcm_address,unsigned count_address,unsigned end_address,unsigned remain_address) {
        if(!ready)return unset_data;
        if((pcm_address&1) || !pointer(memory,count_address,4) || !pointer(memory,end_address,4) ||
            !pointer(memory,remain_address,4) || (pcm_address && !pointer(memory,pcm_address,2048u*output_channels*2u,2)))return param_error;
        if(ended || sample_position>=sample_count){memory.store32(count_address,0);memory.store32(end_address,1);return all_decoded;}
        auto &pcm=block_pcm;auto &stereo=stereo_pcm;
        for(;;) {
            const auto error=decode_packet(pcm);
            if(error){memory.store32(count_address,0);memory.store32(end_address,0);return error;}
            const unsigned skip=std::min(skip_samples,2048u);skip_samples-=skip;
            if(skip==2048)continue;
            const unsigned samples=std::min(2048u-skip,sample_count-sample_position);
            if(pcm_address && channel_count==output_channels)memory.copy_in(pcm_address,std::span<const std::uint8_t>(pcm).subspan(skip*4u,samples*4u));
            else if(pcm_address) {
                // One channel in the stream: the same sample goes to the left and the right.
                stereo.resize(samples*4u);
                for(unsigned i=0;i<samples;++i){const auto *from=pcm.data()+(skip+i)*2u;auto *to=stereo.data()+i*4u;to[0]=to[2]=from[0];to[1]=to[3]=from[1];}
                memory.copy_in(pcm_address,stereo);
            }
            sample_position+=samples;ended=sample_position==sample_count;
            memory.store32(count_address,samples);memory.store32(end_address,ended?1u:0u);memory.store32(remain_address,static_cast<unsigned>(remaining()));return 0;
        }
    }
    unsigned remaining_info(unsigned out){if(!ready)return unset_data;if(!pointer(memory,out,4))return param_error;memory.store32(out,static_cast<unsigned>(remaining()));return 0;}
    unsigned position(unsigned out){if(!ready)return unset_data;if(!pointer(memory,out,4))return param_error;if(ended)return all_decoded;memory.store32(out,sample_position);return 0;}
    struct Reset {unsigned offset{},skip{},minimum{};};
    Reset reset_plan(unsigned sample)const {
        const std::uint64_t raw=std::uint64_t(sample)+initial_skip;const auto target=static_cast<unsigned>(raw/2048);
        const auto preceding=target>2?target-2:0;
        return {data_offset+preceding*block,static_cast<unsigned>(raw-std::uint64_t(preceding)*2048),(target-preceding+1)*block};
    }
    unsigned reset_info(unsigned sample,unsigned out) {
        if(!ready)return unset_data;if(sample>=sample_count)return bad_sample;if(!pointer(memory,out,32))return param_error;
        const auto plan=reset_plan(sample);
        const unsigned where=resident?base+plan.offset:base;
        const unsigned available=resident?file_size-plan.offset:std::min(capacity,file_size-plan.offset);
        const std::array<unsigned,8> values{where,available,plan.minimum,plan.offset,0,0,0,0};
        for(unsigned i=0;i<values.size();++i)memory.store32(out+i*4,values[i]);return 0;
    }
    unsigned reset(unsigned sample,unsigned first,unsigned second) {
        if(!ready)return unset_data;if(sample>=sample_count)return bad_sample;if(second)return second_write;
        const auto plan=reset_plan(sample);
        if((resident && first>file_size-plan.offset) || (!resident && (first<plan.minimum || first>std::min(capacity,file_size-plan.offset))))return first_write;
        avcodec_flush_buffers(codec);if(resampler)swr_free(&resampler);av_frame_unref(frame);
        if(resident){head=plan.offset;buffered=data_offset+data_size-plan.offset;next_file_offset=file_size;tail=file_size%capacity;}
        else{head=0;buffered=first;tail=first%capacity;next_file_offset=plan.offset+first;}
        decode_file_offset=plan.offset;sample_position=sample;skip_samples=plan.skip;ended=false;internal_error=0;return 0;
    }
    unsigned error_info(unsigned out){if(!pointer(memory,out,4))return param_error;memory.store32(out,static_cast<unsigned>(internal_error));return 0;}
};
AtracStream::AtracStream(psprecomp::GuestMemory &m,unsigned type):impl_(std::make_unique<Impl>(m,type)){}
AtracStream::~AtracStream()=default;
std::uint32_t AtracStream::attach(unsigned b,unsigned r,unsigned s){return impl_->attach(b,r,s);}
std::uint32_t AtracStream::stream_info(unsigned p,unsigned a,unsigned o){return impl_->stream_info(p,a,o);}
std::uint32_t AtracStream::add_data(unsigned b){return impl_->add(b);}
std::uint32_t AtracStream::decode(unsigned p,unsigned n,unsigned e,unsigned r){return impl_->decode(p,n,e,r);}
std::uint32_t AtracStream::remaining(unsigned o){return impl_->remaining_info(o);}
std::uint32_t AtracStream::position(unsigned o){return impl_->position(o);}
std::uint32_t AtracStream::reset_info(unsigned s,unsigned o){return impl_->reset_info(s,o);}
std::uint32_t AtracStream::reset(unsigned s,unsigned a,unsigned b){return impl_->reset(s,a,b);}
std::uint32_t AtracStream::error_info(unsigned o){return impl_->error_info(o);}
bool AtracStream::attached()const noexcept{return impl_->ready;}
unsigned AtracStream::channels()const noexcept{return impl_->channel_count;}
unsigned AtracStream::total_samples()const noexcept{return impl_->sample_count;}
unsigned AtracStream::loop_start()const noexcept{return impl_->loop_begin;}
unsigned AtracStream::loop_end()const noexcept{return impl_->loop_finish;}
AtracStream::Info AtracStream::info()const noexcept{return {impl_->base,impl_->capacity,impl_->file_size,impl_->data_offset,impl_->data_size,impl_->block,impl_->channel_count,impl_->sample_count,impl_->resident};}
}
