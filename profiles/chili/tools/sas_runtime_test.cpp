#include "../host/sas_runtime.hpp"
#include <iostream>
#include <stdexcept>
int main(){try{
 auto require=[](bool ok,const char *s){if(!ok)throw std::runtime_error(s);};
 psprecomp::Runtime rt, other; chili::SasRuntime sas(rt), isolated(other);
 require(!sas.loaded() && !isolated.loaded(),"mixer starts loaded");
 require(sas.load() && isolated.load(),"mixer load failed");
 require(!sas.load(),"duplicate mixer load succeeded");
 constexpr unsigned core=0x08804000,data=0x08805000,out=0x08806000;
 auto call=[](psprecomp::Runtime &r,unsigned nid,std::initializer_list<unsigned> args){auto &c=r.cpu();unsigned i=4;for(auto a:args)c.gpr[i++]=a;r.invoke_import("sceSasCore",nid,c);return c.gpr[2];};
 require(call(rt,0x42778A9F,{core,64,32,0,44100})==0,"init failed");
 require(call(other,0x42778A9F,{core,64,32,0,44100})==0,"second init failed");
 rt.memory().zero(data,32);for(unsigned i=2;i<16;++i)rt.memory().store8(data+i,0x11);rt.memory().store8(data+17,7);
 require(call(rt,0x99944089,{core,0,data,32,0})==0,"VAG voice failed");
 require(call(rt,0x440CA7D8,{core,0,4096,4096,0,0})==0,"volume failed");
 require(call(rt,0x019B25EB,{core,0,15,0x40000000,0,0,0x40000000})==0,"sample ADSR failed");
 require(call(rt,0x5F9529F6,{core,0,0x40000000})==0,"sample sustain failed");
 require(call(rt,0x76F01ACA,{core,0})==0,"key on failed");
 require(call(rt,0xA3589D81,{core,out})==0,"mix failed");
 require(rt.memory().load16(out)==0 && rt.memory().load16(out+41*4)==4096,"known ADPCM samples mismatch");
 require(rt.memory().load16(out+61*4)==0,"terminal marker did not end voice");
 require(call(rt,0x68A46B95,{core})==0xFFFFFFFFu,"end flag mismatch");
 require(call(other,0xA3589D81,{core,out})==0,"isolated mix failed");
 for(unsigned i=0;i<256;i+=2)require(other.memory().load16(out+i)==0,"voice leaked across runtimes");
 require(call(rt,0x42778A9F,{core,63,32,0,44100})==0x80420001u,"invalid grain accepted");

 // Zero-rate default envelopes must not invent audible attack/release ramps.
 require(call(other,0x99944089,{core,0,data,32,1})==0,"default voice failed");
 other.memory().zero(data,32);for(unsigned i=2;i<16;++i)other.memory().store8(data+i,0x11);
 call(other,0x440CA7D8,{core,0,4096,4096,0,0});call(other,0x76F01ACA,{core,0});
 for(unsigned grain=0;grain<4;++grain){call(other,0xA3589D81,{core,out});for(unsigned i=0;i<256;i+=2)require(other.memory().load16(out+i)==0,"default envelope invented sound");}
 require(call(other,0x74AE582A,{core,0})==0,"default envelope height rose");
 // A finite sound with the default silent envelope must still end.
 call(other,0xA0CF2FA4,{core,0});call(other,0xA3589D81,{core,out});
 other.memory().store8(data+17,7);
 require(call(other,0x99944089,{core,0,data,32,0})==0,"muted finite voice failed");
 require(call(other,0x76F01ACA,{core,0})==0,"muted finite key on failed");
 call(other,0xA3589D81,{core,out});
 require((call(other,0x68A46B95,{core})&1)==1,"zero envelope froze finite decoder");
 for(unsigned i=0;i<256;i+=2)require(other.memory().load16(out+i)==0,"muted finite voice produced sound");
 // A two-block looping stream crosses multiple grains. Configure explicit
 // envelopes so this test does not depend on the inherited fallback ramp.
 require(call(rt,0x42778A9F,{core,64,32,0,44100})==0,"loop init failed");
 rt.memory().zero(data,32);
 for(unsigned b=0;b<2;++b)for(unsigned i=2;i<16;++i)rt.memory().store8(data+b*16+i,0x11);
 rt.memory().store8(data+1,6);rt.memory().store8(data+17,3);
 require(call(rt,0x99944089,{core,0,data,32,1})==0,"loop voice failed");
 require(call(rt,0x440CA7D8,{core,0,4096,static_cast<unsigned>(-4096),0,0})==0,"signed volume failed");
 require(call(rt,0x019B25EB,{core,0,15,0x40000000,0,0,0x40000000})==0,"explicit ADSR failed");
 require(call(rt,0x5F9529F6,{core,0,0x40000000})==0,"sustain failed");
 require(call(rt,0x76F01ACA,{core,0})==0,"loop key on failed");
 call(rt,0xA3589D81,{core,out});
 for(unsigned grain=0;grain<4;++grain){
   require(call(rt,0xA3589D81,{core,out})==0,"loop grain failed");
   for(unsigned frame=0;frame<64;++frame){
     require(rt.memory().load16(out+frame*4)==4096,"loop continuity failed");
     require(static_cast<short>(rt.memory().load16(out+frame*4+2))==-4096,"signed right volume mismatch");
   }
 }
 require((call(rt,0x68A46B95,{core})&1)==0,"loop ended unexpectedly");
 require(call(rt,0x787D04D5,{core,1,1})==0,"pause failed");
 require(call(rt,0x2C8E6AB3,{core})==1,"pause flags failed");
 call(rt,0xA3589D81,{core,out});
 for(unsigned i=0;i<256;i+=2)require(rt.memory().load16(out+i)==0,"paused voice produced samples");
 require(call(rt,0x76F01ACA,{core,0})==0x80420016u,"paused key on accepted");
 call(rt,0x787D04D5,{core,1,0});call(rt,0xA3589D81,{core,out});
 require(rt.memory().load16(out)==4096,"unpause failed to resume");
 require(call(rt,0xA0CF2FA4,{core,0})==0,"key off failed");
 call(rt,0xA3589D81,{core,out});
 require(call(rt,0x74AE582A,{core,0})==0,"release envelope did not terminate");
 require((call(rt,0x68A46B95,{core})&1)==1,"key off left loop active");
 for(unsigned i=0;i<256;i+=2)require(rt.memory().load16(out+i)==0,"terminated release produced samples");
 // CoreWithMix must preserve caller PCM with no active voices, including sign.
 for(unsigned f=0;f<64;++f){rt.memory().store16(out+f*4,1234);rt.memory().store16(out+f*4+2,static_cast<unsigned short>(-2345));}
 require(call(rt,0x50A14DFC,{core,out,4096,4096})==0,"caller PCM mix failed");
 for(unsigned f=0;f<64;++f){require(rt.memory().load16(out+f*4)==1234,"caller left PCM changed");require(static_cast<short>(rt.memory().load16(out+f*4+2))==-2345,"caller right PCM changed");}

 require(call(rt,0x42778A9F,{core,64,32,1,44100})==0,"raw output init failed");
 call(rt,0x99944089,{core,0,data,32,1});
 call(rt,0x440CA7D8,{core,0,4096,2048,1024,static_cast<unsigned>(-1024)});
 call(rt,0x019B25EB,{core,0,15,0x40000000,0,0,0x40000000});
 call(rt,0x5F9529F6,{core,0,0x40000000});call(rt,0x76F01ACA,{core,0});
 rt.memory().store32(out+512,0x12345678);
 call(rt,0xA3589D81,{core,out});call(rt,0xA3589D81,{core,out});
 for(unsigned f=0;f<64;++f){
   require(rt.memory().load16(out+f*2)==4096,"raw dry L plane mismatch");
   require(rt.memory().load16(out+128+f*2)==2048,"raw dry R plane mismatch");
   require(rt.memory().load16(out+256+f*2)==1024,"raw effect L plane mismatch");
   require(static_cast<short>(rt.memory().load16(out+384+f*2))==-1024,"raw effect R plane mismatch");
 }
 require(rt.memory().load32(out+512)==0x12345678,"raw mix overwrote following memory");
 require(call(rt,0x50A14DFC,{core,out,4096,4096})==0x800001FFu,"raw caller mix accepted");

 // Boundary results match PSP autotest pitch.expected, including legal zero.
 for(unsigned pitch: {0u,1u,0x800u,0x3FFFu,0x4000u})
   require(call(rt,0xAD84D37F,{core,0,pitch})==0,"legal pitch rejected");
 for(unsigned pitch: {0xFFFFFFFFu,0x4001u,0x8000u,0x80000001u})
   require(call(rt,0xAD84D37F,{core,0,pitch})==0x80420012u,"invalid pitch accepted");
 require(call(rt,0xAD84D37F,{core,32,4096})==0x80420010u,"invalid pitch voice accepted");
 // Zero pitch holds the source sample while envelope/mixing continue.
 call(rt,0x42778A9F,{core,64,32,0,44100});
 call(rt,0x99944089,{core,0,data,32,0});call(rt,0xAD84D37F,{core,0,0});
 call(rt,0x440CA7D8,{core,0,4096,4096,0,0});
 call(rt,0x019B25EB,{core,0,15,0x40000000,0,0,0x40000000});
 call(rt,0x5F9529F6,{core,0,0x40000000});call(rt,0x76F01ACA,{core,0});
 for(unsigned grain=0;grain<4;++grain)call(rt,0xA3589D81,{core,out});
 for(unsigned f=0;f<64;++f)require(rt.memory().load16(out+f*4)==4096,"zero pitch did not hold first sample");
 require((call(rt,0x68A46B95,{core})&1)==0,"zero pitch prematurely ended");
 // Changing pitch on a live voice advances it to the finite stream end.
 require(call(rt,0xAD84D37F,{core,0,0x4000})==0,"live pitch change failed");
 call(rt,0xA3589D81,{core,out});
 require((call(rt,0x68A46B95,{core})&1)==1,"4x pitch failed to reach end");
 // Init must restore default voice volumes independently of prior settings.
 call(rt,0x42778A9F,{core,64,32,1,44100});
 call(rt,0x99944089,{core,0,data,32,1});
 call(rt,0x019B25EB,{core,0,15,0x40000000,0,0,0x40000000});
 call(rt,0x5F9529F6,{core,0,0x40000000});call(rt,0x76F01ACA,{core,0});
 call(rt,0xA3589D81,{core,out});call(rt,0xA3589D81,{core,out});
 for(unsigned plane=0;plane<4;++plane)for(unsigned f=0;f<64;++f)
   require(rt.memory().load16(out+plane*128+f*2)==4096,"default voice volume mismatch");
 require(sas.unload() && !sas.loaded(),"mixer unload failed");
 require(!sas.unload(),"duplicate mixer unload succeeded");
 require(sas.load(),"mixer reload failed");
 require(call(rt,0x68A46B95,{core})==0x80420100u,"reload retained initialized core");
 require(call(rt,0x42778A9F,{core,64,32,0,44100})==0,"reloaded init failed");
 require(call(rt,0x68A46B95,{core})==0xFFFFFFFFu,"reload retained voices");
 sas.unload();rt.cpu().gpr[4]=core;
 rt.invoke_import("sceSasCore",0x68A46B95u,rt.cpu());
 require(rt.stopped() && rt.stop_reason().find("loaded native mixer")!=std::string::npos,"unloaded mixer accepted call");
 std::cout<<"PASS: VAG samples, end markers, isolation, loops, signed volumes, ADSR, pause/release, caller PCM, raw planes/bounds, pitch boundaries/zero/live change\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
