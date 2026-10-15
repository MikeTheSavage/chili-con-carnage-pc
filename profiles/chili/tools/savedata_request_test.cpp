#include "savedata_request.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>
namespace {
void require(bool b,const char*m){if(!b)throw std::runtime_error(m);}
std::vector<std::uint8_t> read(const std::filesystem::path&p){std::ifstream f(p,std::ios::binary);require(bool(f),"Fixture source unavailable");return {std::istreambuf_iterator<char>(f),{}};}
constexpr unsigned params=0x08DF68C0u,ram_base=0x08DF0000u;
struct Fixture {
    psprecomp::GuestMemory m;
    std::vector<std::uint8_t> ram;
    Fixture(const std::filesystem::path&base){
        ram=read(base/"listsave_pointer_actual_request_ram.bin");require(ram.size()==65536u,"Actual RAM capture must be64KiB");m.copy_in(ram_base,ram);
        const auto payload=read(base/"listsave_pointer_actual_payload.bin");require(payload.size()==1066u,"Captured payload must be1066bytes");m.copy_in(m.load32(params+116u),payload);
        constexpr const char*names[]{"icon0","pic1","snd0"};constexpr unsigned offsets[]{1412u,1444u,1460u};
        for(unsigned i=0;i<3;++i){auto data=read(base/(std::string("listsave_pointer_actual_")+names[i]+".bin"));require(data.size()==m.load32(params+offsets[i]+8u),"Captured media size mismatch");m.copy_in(m.load32(params+offsets[i]),data);}
        // New-data display title is a separately verified original static string;
        // all request bytes/resources above are actual pointer-followed captures.
        const auto new_data=m.load32(params+1476u),title=m.load32(new_data+16u);const std::string label="New data";
        for(unsigned i=0;i<=label.size();++i)m.store8(title+i,i==label.size()?0u:std::uint8_t(label[i]));
    }
    std::vector<std::uint8_t> snapshot(){std::vector<std::uint8_t> out(ram.size());m.copy_out(ram_base,out);return out;}
};
}
int main(int argc,char**argv){try{
    require(argc==2,"Expected actual capture directory");const std::filesystem::path base=argv[1];
    Fixture f(base);auto before=f.snapshot();const auto r=chili::save_request::parse(f.m,params);
    require(r.mode==5u&&r.game=="ULUS10216"&&r.file=="SECURE.BIN"&&r.title=="RAM"&&r.savedata_title=="RAM"&&r.detail=="0%"&&r.parental==9u,"Actual captured save metadata mismatch");
    require(r.slot.empty()&&r.slots.size()==20u&&r.slots.front()=="DATA00"&&r.slots.back()=="DATA19"&&r.focus==3u&&r.overwrite_word==1u&&r.secure_version==0u&&r.new_title=="New data","Actual list/new-data shape mismatch");
    require(chili::save_request::bytes(f.m,r.data)==read(base/"listsave_pointer_actual_payload.bin"),"Captured1066-byte payload mismatch");
    require(chili::save_request::bytes(f.m,r.media[0])==read(base/"listsave_pointer_actual_icon0.bin")&&chili::save_request::bytes(f.m,r.media[2])==read(base/"listsave_pointer_actual_pic1.bin")&&chili::save_request::bytes(f.m,r.media[3])==read(base/"listsave_pointer_actual_snd0.bin"),"Captured media snapshots mismatch");
    require(before==f.snapshot()&&chili::save_request::header_unchanged(f.m,r),"Reader changed guest memory");
    f.m.store32(params+28u,123u);require(!chili::save_request::header_unchanged(f.m,r),"Pending header mutation not detected");
    // Mode 6 stands for the modes the reader does not take (7, the delete with a
    // list, is read now). The word at offset 56 left this table: it used to be
    // refused above 1 and read as the permission to write over a save. The
    // firmware takes any value there and it permits nothing, so the reader only
    // hands it on (checked below).
    constexpr unsigned fields[]{0u,48u,52u,4u,8u,1480u,96u,116u,120u,124u,1412u,1416u,1420u,1424u};
    constexpr unsigned values[]{1480u,6u,1u,12u,2u,9u,params,params,0xFFFFFFFFu,1067u,0xDEADBEEFu,0xFFFFFFFFu,20906u,1u};
    for(unsigned i=0;i<std::size(fields);++i){Fixture bad(base);bad.m.store32(params+fields[i],values[i]);auto original=bad.snapshot();bool failed=false;try{(void)chili::save_request::parse(bad.m,params);}catch(const chili::save_request::Error&){failed=true;}require(failed&&bad.snapshot()==original,"Invalid request accepted or memory changed");}
    {Fixture bad(base);auto list=bad.m.load32(params+96u);for(unsigned i=0;i<20u;++i)bad.m.store8(list+20u+i,bad.m.load8(list+i));bool failed=false;try{(void)chili::save_request::parse(bad.m,params);}catch(const chili::save_request::Error&){failed=true;}require(failed,"Duplicate slots accepted");}
    {Fixture bad(base);for(unsigned i=0;i<13u;++i)bad.m.store8(params+100u+i,'A');bool failed=false;try{(void)chili::save_request::parse(bad.m,params);}catch(const chili::save_request::Error&){failed=true;}require(failed,"Unterminated file accepted");}
    for(unsigned address:{0x04000000u,0x04200000u,0x00010000u,0x08400000u}){Fixture bad(base);bad.m.store32(params+116u,address);bool failed=false;try{(void)chili::save_request::parse(bad.m,params);}catch(const chili::save_request::Error&){failed=true;}require(failed,"Unadopted memory bank accepted");}
    for(bool alias:{false,true}){Fixture bad(base);bad.m.store32(params+48u,4u);const auto address=alias?bad.m.load32(params+116u)|0x40000000u:bad.m.load32(params+96u);bad.m.store32(params+1412u,address);bool failed=false;try{(void)chili::save_request::parse(bad.m,params);}catch(const chili::save_request::Error&){failed=true;}require(failed,"Load outputs aliasing source list/other buffers accepted");}
    for(unsigned word:{0u,1u,2u,0x10u,0xFFFFFFFFu}){Fixture any(base);any.m.store32(params+56u,word);const auto original=any.snapshot();const auto result=chili::save_request::parse(any.m,params);
        require(result.overwrite_word==word&&result.mode==5u&&result.is_save()&&any.snapshot()==original,"The word at offset 56 was judged or changed the request");}
    {   // Mode 7 as the game builds it: the list, no slot name, no payload, no media.
        Fixture del(base);del.m.store32(params+48u,7u);del.m.store32(params+56u,0u);for(unsigned off:{116u,1412u,1428u,1444u,1460u})for(unsigned i=0;i<12u;i+=4u)del.m.store32(params+off+i,0u);
        const auto original=del.snapshot();const auto result=chili::save_request::parse(del.m,params);
        require(result.mode==7u&&result.is_delete()&&!result.is_save()&&result.interactive()&&result.slots.size()==20u&&result.slots.front()=="DATA00"&&result.data.capacity==0u&&del.snapshot()==original,"The delete request with a list was not read");
        // The payload and media fields of a delete are not looked at, whatever they hold.
        del.m.store32(params+116u,0xDEADBEEFu);del.m.store32(params+120u,0xFFFFFFFFu);del.m.store32(params+1424u,1u);require(chili::save_request::parse(del.m,params).is_delete(),"A delete was judged by payload or media fields");
        del.m.store32(params+96u,0u);bool failed=false;try{(void)chili::save_request::parse(del.m,params);}catch(const chili::save_request::Error&){failed=true;}require(failed,"A delete without a list was accepted");}
    for(unsigned mode:{0u,1u,4u}){Fixture other(base);other.m.store32(params+48u,mode);for(unsigned i=0;i<7u;++i)other.m.store8(params+76u+i,i==6u?0u:std::uint8_t("DATA00"[i]));const auto result=chili::save_request::parse(other.m,params);require(result.mode==mode&&result.slot=="DATA00"&&result.is_save()==(mode==1u)&&result.interactive()==(mode==4u),"Shared AUTOLOAD/AUTOSAVE/LISTLOAD wire interpretation failed");}
    std::cout<<"Actual LISTSAVE metadata/payload/media; source-only reader; malformed spans/capacities/alias/duplicates/termination; operation variants PASS\n";
    return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
