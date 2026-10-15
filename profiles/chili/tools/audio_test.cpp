#include "../host/audio_runtime.hpp"
#include "../host/pcm_queue.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdexcept>
#include <iostream>
int main(){try{psprecomp::Runtime rt;chili::AudioRuntime audio(rt);auto &c=rt.cpu();
auto call=[&](unsigned nid,unsigned a){c.gpr[4]=a;rt.invoke_import("sceAudio",nid,c);if(rt.stopped())throw std::runtime_error(rt.stop_reason());return c.gpr[2];};
auto check=[](bool ok,const char *s){if(!ok)throw std::runtime_error(s);};
check(call(0x63F2889C,16)==0x80260006,"length validation precedence failed");
check(call(0x63F2889C,512)==0x80260008,"unreserved length accepted");
check(call(0x01562BA3,512)==0,"native reserve failed");
check(call(0x01562BA3,512)==0x80268002,"duplicate reserve accepted");
for(unsigned n:{17u,512u,4111u})check(call(0x63F2889C,n)==0,"valid length rejected");
for(unsigned n:{0u,16u,4112u,0x80000200u,0xFFFFFFFFu})check(call(0x63F2889C,n)==0x80260006,"invalid length accepted");
check(call(0x43196845,0)==0,"release failed");
check(call(0x01562BA3,17)==0,"re-reserve failed");check(call(0x43196845,0)==0,"second release failed");
{
 // The host ring and its feeder thread: nothing is played below the resume level.
 chili::AudioDeviceCounters counters;chili::HostPcmFeed feed(4096,counters,nullptr);std::vector<short> silence(1024,0);
 check(feed.write(silence) && feed.write(silence),"ring write failed");
 Sleep(100);check(feed.lead_frames()==1024,"ring played below its resume level");
 // The game takes its output again: what was queued before is given up at once and skipped by the feeder.
 feed.restart();check(feed.lead_frames()==0,"a restart must empty the ring for the game");
 {const auto end=GetTickCount64()+2000;while(!counters.restarts.load() && GetTickCount64()<end)Sleep(1);}
 check(counters.restarts.load()==1 && counters.dropped_frames.load()==1024,"the feeder did not skip the frames from before the restart");
 check(feed.write(silence) && feed.write(silence) && feed.lead_frames()==1024,"ring write after a restart failed");
 // The game's queue over the ring: two blocks at most while the ring is above its lead.
 unsigned long long now=0;chili::LivePcmSink sink([&]{return now;},feed,chili::AudioPolicy{512u,1024u,true});
 check(sink.submit(silence,0x8000) && sink.submit(silence,0x8000),"PCM submission failed");
 check(sink.pending()==2 && !sink.submit(silence,0x8000),"queue backpressure failed");
 // Above the resume level the feeder plays everything, with or without a device.
 for(unsigned i=0;i<6;++i)check(feed.write(silence),"ring refill failed");
 const auto end=GetTickCount64()+2000;
 while(feed.lead_frames() && GetTickCount64()<end)Sleep(1);
 check(feed.lead_frames()==0 && counters.played_frames.load()>=5120,"feeder did not play the ring");
 check(sink.pending()==0,"an empty ring must release the game's queue");
}
std::cout<<"PASS: reserve/release/re-reserve, duplicate handling, length boundaries and validation precedence; host ring resume level, restart, playback and queue backpressure verified\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
