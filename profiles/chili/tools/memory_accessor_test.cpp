#include "psprecomp/guest_memory.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
unsigned checks{};
void require(bool ok,const char *why){++checks;if(!ok)throw std::runtime_error(why);}
struct Outcome{bool ok{};std::uint32_t value{};std::string error,watch;};
struct CaptureCerr{
 std::ostringstream output;std::streambuf *old;std::ios::fmtflags flags;
 CaptureCerr():old(std::cerr.rdbuf(output.rdbuf())),flags(std::cerr.flags()){}
 ~CaptureCerr(){std::cerr.rdbuf(old);std::cerr.flags(flags);}
};
Outcome access(psprecomp::GuestMemory &m,unsigned address,unsigned width,bool store,bool fast){
 CaptureCerr captured;Outcome out;
 try{
  if(store){if(width==2){if(fast)m.aot_store16(address,0x8E31);else m.store16(address,0x8E31);}
   else{if(fast)m.aot_store32(address,0xF09D713B);else m.store32(address,0xF09D713B);}}
  else out.value=width==2?(fast?m.aot_load16(address):m.load16(address)):(fast?m.aot_load32(address):m.load32(address));
  out.ok=true;
 }catch(const std::exception &e){out.error=e.what();}
 out.watch=captured.output.str();return out;
}
void seed(psprecomp::GuestMemory &m){
 CaptureCerr discard;std::vector<std::uint8_t> pixels(psprecomp::GuestMemory::kVramSize);
 for(std::size_t n=0;n<pixels.size();++n)pixels[n]=std::uint8_t(n*13+37);
 m.copy_in(0x04000000,pixels);
 std::array<std::uint8_t,64> bytes{};for(unsigned n=0;n<bytes.size();++n)bytes[n]=std::uint8_t(n*29+5);
 for(unsigned address:{0x08000000u,0x08800000u,0x09FFFFC0u,0x00010000u,0x00013FC0u})m.copy_in(address,bytes);
}
bool same_memory(const psprecomp::GuestMemory &a,const psprecomp::GuestMemory &b){
 if(a.bytes()!=b.bytes()||a.vram_bytes()!=b.vram_bytes())return false;
 std::array<std::uint8_t,psprecomp::GuestMemory::kScratchpadSize> as{},bs{};a.copy_out(0x10000,as);b.copy_out(0x10000,bs);return as==bs;
}
void boundaries(bool watching){
 psprecomp::GuestMemory reference,candidate;seed(reference);seed(candidate);
 std::vector<unsigned> addresses{0u,0x03FFFFFFu,0x047FFFFCu,0x047FFFFDu,0x047FFFFEu,0x047FFFFFu,0x04800000u,
  0x07FFFFFFu,0x09FFFFFCu,0x09FFFFFDu,0x09FFFFFEu,0x09FFFFFFu,0x0A000000u,
  0x0000FFFFu,0x00013FFCu,0x00013FFDu,0x00013FFEu,0x00013FFFu,0x00014000u,0xFFFFFFFFu};
 for(unsigned base:{0x04000000u,0x04200000u,0x04400000u,0x04600000u,0x08000000u,0x08800000u,0x00010000u})
  for(unsigned n=0;n<8;++n)addresses.push_back(base+n);
 for(unsigned base:{0x041FFFFCu,0x043FFFFCu,0x045FFFFCu})for(unsigned n=0;n<4;++n)addresses.push_back(base+n);
 const auto canonical=addresses;
 for(unsigned address:canonical)if(address>=0x04000000u&&address<0x0A000000u)
  for(unsigned alias:{0x40000000u,0x80000000u,0xA0000000u})addresses.push_back(address|alias);
 unsigned observed{},rejected{},wrapped{};
 for(unsigned address:addresses)for(unsigned width:{2u,4u}){
  const auto old_load=access(reference,address,width,false,false),new_load=access(candidate,address,width,false,true);
  require(old_load.ok==new_load.ok&&old_load.value==new_load.value&&old_load.error==new_load.error,"load alias/unaligned/wrap/bounds/error parity");
  if(!old_load.ok)++rejected;
  if((psprecomp::GuestMemory::canonical(address)&0x1FFFFF)>psprecomp::GuestMemory::kVramSize-width&&old_load.ok)++wrapped;
  const auto old_store=access(reference,address,width,true,false),new_store=access(candidate,address,width,true,true);
  require(old_store.ok==new_store.ok&&old_store.error==new_store.error,"store alias/unaligned/wrap/bounds/error parity");
  require(old_store.watch==new_store.watch,"watched store operation/address/old/new/UID/PC log parity");
  observed+=old_store.watch.find("[watch-write]")!=std::string::npos;
  require(same_memory(reference,candidate),"successful or failed scalar store changed full RAM/VRAM/scratch parity");
 }
 require(rejected>0&&wrapped>0,"boundary fixture did not reach strict error and mirror wrap cases");
 require((observed>0)==watching,"watch fixture lacks actual events or disabled path emitted watch");
 std::cout<<"PASS Scalar16/32 "<<addresses.size()<<" address cases, "<<rejected<<" rejected loads, "<<wrapped
          <<" wrapped loads, "<<observed<<" watched stores: exact values/errors/logs/full memory parity\n";
}
}
int main(int argc,char **argv){try{
 require(argc<=2,"Usage: memory_accessor_test [--watch]");const bool watching=argc==2;require(!watching||std::string(argv[1])=="--watch","unknown argument");
 require(watching?std::getenv("PSPRECOMP_WATCH_WRITE")!=nullptr:std::getenv("PSPRECOMP_WATCH_WRITE")==nullptr,"configure watch before constructing memory in isolated process");
 boundaries(watching);std::cout<<"PASS "<<checks<<" existing public scalar AOT parity assertions; no core source change\n";
}catch(const std::exception &e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
