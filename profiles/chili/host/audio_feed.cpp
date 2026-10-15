#include "audio_feed.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
namespace chili {
namespace {
template<class T> struct Com {
    T *p{};
    Com()=default;
    Com(const Com &)=delete;
    Com &operator=(const Com &)=delete;
    ~Com(){if(p)p->Release();}
    T *operator->() const noexcept {return p;}
    T **put() noexcept {if(p){p->Release();p=nullptr;}return &p;}
    explicit operator bool() const noexcept {return p!=nullptr;}
};
std::mutex report_mutex;
std::string failure(const char *step,HRESULT result){
    char text[96];std::snprintf(text,sizeof(text),"no audio device (%s failed, 0x%08lX)",step,static_cast<unsigned long>(result));return text;
}
}
// The default output device, opened in shared mode. Windows converts 44.1 kHz
// 16 bit to whatever the device mixes at.
struct HostPcmFeed::Device {
    Com<IMMDeviceEnumerator> enumerator;
    Com<IMMDevice> endpoint;
    Com<IAudioClient> client;
    Com<IAudioRenderClient> render;
    HANDLE event{};
    UINT32 buffer_frames{};
    bool running{};
    std::wstring id;
    // Whether Windows now names another device as the default output.
    bool superseded() const {
        Com<IMMDevice> now;
        LPWSTR text{};
        if(FAILED(enumerator->GetDefaultAudioEndpoint(eRender,eConsole,now.put())) || FAILED(now->GetId(&text)) || !text)return false;
        const bool changed=id!=text;
        CoTaskMemFree(text);
        return changed;
    }
    ~Device(){if(running)client->Stop();if(event)CloseHandle(event);}
};
double AudioStats::performance_frequency(){LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);return static_cast<double>(frequency.QuadPart);}
HostPcmFeed::HostPcmFeed(unsigned resume_frames,AudioDeviceCounters &counters,WavWriter *capture)
    :ring_(capacity*2u),resume_frames_(std::max(resume_frames,fade_frames*2u)),counters_(counters),capture_(capture),scratch_(rate/50u*2u){
    wake_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    thread_=std::thread([this]{run();});
    // The first open has finished, one way or the other, before the game hears of the device.
    for(unsigned i=0;i<3000u && !opened_.load(std::memory_order_acquire);++i)Sleep(1);
}
HostPcmFeed::~HostPcmFeed(){
    stop_.store(true,std::memory_order_release);
    if(wake_)SetEvent(static_cast<HANDLE>(wake_));
    if(thread_.joinable())thread_.join();
    if(wake_)CloseHandle(static_cast<HANDLE>(wake_));
}
std::string HostPcmFeed::device_report() const {const std::lock_guard lock(report_mutex);return report_;}
bool HostPcmFeed::write(std::span<const short> interleaved) noexcept {
    const auto frames=static_cast<std::uint64_t>(interleaved.size()/2u);
    const auto written=write_.load(std::memory_order_relaxed);
    if(written-read_.load(std::memory_order_acquire)+frames>capacity)return false;
    const auto at=written&(capacity-1u),first=std::min(frames,capacity-at);
    std::memcpy(ring_.data()+at*2u,interleaved.data(),first*4u);
    std::memcpy(ring_.data(),interleaved.data()+first*2u,(frames-first)*4u);
    write_.store(written+frames,std::memory_order_release);
    return true;
}
void HostPcmFeed::copy_out(short *out,std::uint64_t from,unsigned frames) const noexcept {
    const auto at=from&(capacity-1u),first=std::min<std::uint64_t>(frames,capacity-at);
    std::memcpy(out,ring_.data()+at*2u,first*4u);
    std::memcpy(out+first*2u,ring_.data(),(frames-first)*4u);
}
// One device request. Playing: the next frames of the ring, as long as a fade's
// worth stays behind them. Otherwise what is left, faded out, then silence
// until the ring is back to resume_frames_. After restart() the frames written
// before it go the same way: a fade's worth is played, the rest skipped.
void HostPcmFeed::fill(short *const request,const unsigned requested) noexcept {
    auto read=read_.load(std::memory_order_relaxed);
    unsigned faded{};
    if(const auto drop=drop_to_.load(std::memory_order_acquire);drop>read){
        if(playing_){
            faded=static_cast<unsigned>(std::min<std::uint64_t>({requested,fade_frames,drop-read}));
            copy_out(request,read,faded);
            for(unsigned i=0;i<faded;++i){
                const auto gain=static_cast<int>(faded-1u-i);
                request[i*2u]=static_cast<short>(request[i*2u]*gain/static_cast<int>(faded));
                request[i*2u+1u]=static_cast<short>(request[i*2u+1u]*gain/static_cast<int>(faded));
            }
            playing_=false;fade_in_left_=0u;
        }
        counters_.dropped_frames.fetch_add(drop-read-faded,std::memory_order_relaxed);
        counters_.restarts.fetch_add(1u,std::memory_order_relaxed);
        read=drop;
        read_.store(read,std::memory_order_release);
    }
    short *const out=request+faded*2u;
    const unsigned frames=requested-faded;
    const auto available=static_cast<unsigned>(std::min<std::uint64_t>(write_.load(std::memory_order_acquire)-read,capacity));
    unsigned take{};
    if(!playing_ && available>=resume_frames_){playing_=true;started_=true;fade_in_left_=fade_frames;}
    if(playing_){
        const bool dry=available<frames+fade_frames;
        take=std::min(available,frames);
        copy_out(out,read,take);
        if(fade_in_left_){
            const auto count=std::min(take,fade_in_left_);
            for(unsigned i=0;i<count;++i){
                const auto gain=static_cast<int>(fade_frames-fade_in_left_+i+1u);
                out[i*2u]=static_cast<short>(out[i*2u]*gain/static_cast<int>(fade_frames));
                out[i*2u+1u]=static_cast<short>(out[i*2u+1u]*gain/static_cast<int>(fade_frames));
            }
            fade_in_left_-=count;
        }
        if(dry){
            const auto count=std::min(take,fade_frames);
            for(unsigned i=0;i<count;++i){
                const auto frame=take-count+i;const auto gain=static_cast<int>(count-1u-i);
                out[frame*2u]=static_cast<short>(out[frame*2u]*gain/static_cast<int>(count));
                out[frame*2u+1u]=static_cast<short>(out[frame*2u+1u]*gain/static_cast<int>(count));
            }
            playing_=false;fade_in_left_=0u;
            counters_.underruns.fetch_add(1u,std::memory_order_relaxed);
        }
        read_.store(read+take,std::memory_order_release);
    }
    if(take<frames){
        std::memset(out+take*2u,0,(frames-take)*4u);
        if(started_)counters_.dry_frames.fetch_add(frames-take,std::memory_order_relaxed);
    }
    counters_.played_frames.fetch_add(requested,std::memory_order_relaxed);
    // The device's rate as it asks: frames after the first request over the time since.
    LARGE_INTEGER now{};QueryPerformanceCounter(&now);
    if(!counters_.first_request.load(std::memory_order_relaxed))counters_.first_request.store(now.QuadPart,std::memory_order_relaxed);
    else counters_.frames_since_first.fetch_add(requested,std::memory_order_relaxed);
    counters_.last_request.store(now.QuadPart,std::memory_order_relaxed);
    if(capture_)capture_->write({request,requested*2u});
}
bool HostPcmFeed::open(Device &device){
    const auto fail=[this](const char *step,HRESULT result){const std::lock_guard lock(report_mutex);report_=failure(step,result);return false;};
    HRESULT result=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void **>(device.enumerator.put()));
    if(FAILED(result))return fail("device enumerator",result);
    result=device.enumerator->GetDefaultAudioEndpoint(eRender,eConsole,device.endpoint.put());
    if(FAILED(result))return fail("default output device",result);
    if(LPWSTR text{};SUCCEEDED(device.endpoint->GetId(&text)) && text){device.id=text;CoTaskMemFree(text);}
    result=device.endpoint->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void **>(device.client.put()));
    if(FAILED(result))return fail("device activation",result);
    WAVEFORMATEX format{};
    format.wFormatTag=WAVE_FORMAT_PCM;format.nChannels=2;format.nSamplesPerSec=rate;format.wBitsPerSample=16;
    format.nBlockAlign=4;format.nAvgBytesPerSec=rate*4u;
    // Two device periods in the device's own buffer: it asks once a period, and
    // one spare period covers a late feeder thread.
    REFERENCE_TIME period{},shortest{};
    if(FAILED(device.client->GetDevicePeriod(&period,&shortest)) || period<=0)period=100000;
    const auto duration=std::max<REFERENCE_TIME>(period*2,200000);
    result=device.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK|AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,duration,0,&format,nullptr);
    if(FAILED(result))return fail("stream setup",result);
    if(FAILED(result=device.client->GetBufferSize(&device.buffer_frames)))return fail("buffer size",result);
    device.event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!device.event)return fail("event",HRESULT_FROM_WIN32(GetLastError()));
    if(FAILED(result=device.client->SetEventHandle(device.event)))return fail("event handle",result);
    if(FAILED(result=device.client->GetService(__uuidof(IAudioRenderClient),reinterpret_cast<void **>(device.render.put()))))return fail("render service",result);
    std::string name="default output";
    Com<IPropertyStore> properties;
    if(SUCCEEDED(device.endpoint->OpenPropertyStore(STGM_READ,properties.put()))){
        PROPVARIANT value;PropVariantInit(&value);
        if(SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName,&value)) && value.vt==VT_LPWSTR && value.pwszVal){
            name.clear();
            for(const wchar_t *c=value.pwszVal;*c;++c)name.push_back(*c>=32 && *c<127?static_cast<char>(*c):'?');
        }
        PropVariantClear(&value);
    }
    REFERENCE_TIME latency{};
    (void)device.client->GetStreamLatency(&latency);
    if(FAILED(result=device.client->Start()))return fail("stream start",result);
    device.running=true;
    char text[256];
    std::snprintf(text,sizeof(text),"%s, shared mode, device buffer %.1f ms, period %.1f ms, stream latency %.1f ms",name.c_str(),
        device.buffer_frames*1000.0/rate,period/10000.0,latency/10000.0);
    {const std::lock_guard lock(report_mutex);report_=text;}
    return true;
}
// True when it stopped because the default output device changed.
bool HostPcmFeed::play(Device &device){
    const HANDLE handles[2]{device.event,static_cast<HANDLE>(wake_)};
    unsigned silent{},requests{};
    while(!stop_.load(std::memory_order_relaxed)){
        const auto waited=WaitForMultipleObjects(2u,handles,FALSE,200u);
        if(waited==WAIT_OBJECT_0+1u)return false;
        if(waited!=WAIT_OBJECT_0){
            // The device stopped asking. The game still has to be relieved of its blocks.
            for(unsigned i=0;i<10u;++i)fill(scratch_.data(),static_cast<unsigned>(scratch_.size()/2u));
            if(++silent>=5u)return false;
            continue;
        }
        silent=0u;
        UINT32 padding{};
        if(FAILED(device.client->GetCurrentPadding(&padding)))return false;
        const UINT32 frames=device.buffer_frames>padding?device.buffer_frames-padding:0u;
        if(frames){
            BYTE *data{};
            if(FAILED(device.render->GetBuffer(frames,&data)))return false;
            fill(reinterpret_cast<short *>(data),frames);
            if(FAILED(device.render->ReleaseBuffer(frames,0u)))return false;
        }
        // Every few seconds, right after a request so the device has a full buffer.
        if(++requests>=300u){requests=0u;if(device.superseded())return true;}
    }
    return false;
}
// No device: the ring is still emptied at the device's rate, so the game keeps its timing.
void HostPcmFeed::play_without_device(unsigned milliseconds){
    LARGE_INTEGER frequency{},start{},now{};
    QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);
    std::uint64_t done{};
    const auto chunk=static_cast<unsigned>(scratch_.size()/2u);
    while(!stop_.load(std::memory_order_relaxed)){
        QueryPerformanceCounter(&now);
        const auto elapsed=static_cast<std::uint64_t>(now.QuadPart-start.QuadPart);
        if(elapsed*1000u/static_cast<std::uint64_t>(frequency.QuadPart)>=milliseconds)return;
        const auto due=elapsed*rate/static_cast<std::uint64_t>(frequency.QuadPart);
        while(done+chunk<=due){fill(scratch_.data(),chunk);done+=chunk;}
        if(WaitForSingleObject(static_cast<HANDLE>(wake_),5u)==WAIT_OBJECT_0)return;
    }
}
void HostPcmFeed::run(){
    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    DWORD task{};
    const auto priority=AvSetMmThreadCharacteristicsW(L"Pro Audio",&task);
    while(!stop_.load(std::memory_order_relaxed)){
        {
            Device device;
            const bool ready=open(device);
            opened_.store(true,std::memory_order_release);
            if(ready && play(device))continue;
        }
        // Lost or missing device: carry on without one, and look again a second later.
        play_without_device(1000u);
    }
    if(priority)AvRevertMmThreadCharacteristics(priority);
    if(SUCCEEDED(com))CoUninitialize();
}
}
