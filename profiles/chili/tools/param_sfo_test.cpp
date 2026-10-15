#include "param_sfo.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <set>

namespace {
using namespace chili::sfo;
void require(bool condition,const char *message){if(!condition)throw std::runtime_error(message);}
std::uint32_t u32(const Bytes &b,std::size_t p){return std::uint32_t(b.at(p)) | (std::uint32_t(b.at(p+1u))<<8u) | (std::uint32_t(b.at(p+2u))<<16u) | (std::uint32_t(b.at(p+3u))<<24u);}
void put32(Bytes &b,std::size_t p,std::uint32_t value){for(unsigned i=0u;i<4u;++i)b.at(p+i)=static_cast<std::uint8_t>(value>>(i*8u));}
void put16(Bytes &b,std::size_t p,std::uint16_t value){b.at(p)=static_cast<std::uint8_t>(value);b.at(p+1u)=static_cast<std::uint8_t>(value>>8u);}
Bytes load(const char *path){
    std::ifstream in(path,std::ios::binary | std::ios::ate);require(bool(in),"Cannot read owned/primary SFO fixture");
    const auto size=in.tellg();require(size>=0 && size<16384,"Unexpected fixture length");Bytes b(static_cast<std::size_t>(size));
    in.seekg(0);in.read(reinterpret_cast<char *>(b.data()),size);require(bool(in),"Truncated fixture read");return b;
}
template<class F>void rejects(ErrorCode code,F action){try{action();}catch(const Error &e){require(e.code()==code,"Wrong strict format error classification");return;}throw std::runtime_error("Malformed SFO silently succeeded");}
void owned_and_primary(const Bytes &owned,const Bytes &old_sdk,const Bytes &derived){
    const auto doc=parse(owned);require(doc.entries.size()==10u,"Owned SFO entry count mismatch");
    require(doc.find("DISC_ID") && doc.find("DISC_ID")->text_value()=="ULUS10216","Owned DISC_ID mismatch");
    require(doc.find("TITLE") && doc.find("TITLE")->text_value()=="CHILI CON CARNAGE" && doc.find("TITLE")->used_length==18u && doc.find("TITLE")->storage.size()==128u,"Owned TITLE/reserved capacity mismatch");
    require(doc.find("CATEGORY")->text_value()=="UG" && doc.find("DISC_VERSION")->text_value()=="1.02" && doc.find("PSP_SYSTEM_VER")->text_value()=="2.82","Owned release metadata mismatch");
    require(doc.find("BOOTABLE")->integer_value()==1u && doc.find("DISC_NUMBER")->integer_value()==1u && doc.find("DISC_TOTAL")->integer_value()==1u &&
        doc.find("PARENTAL_LEVEL")->integer_value()==9u && doc.find("REGION")->integer_value()==0x8000u,"Owned integer metadata mismatch");
    require(!doc.find("SAVEDATA_PARAMS") && serialize(doc)==owned && parse(serialize(doc))==doc,"Owned SFO byte-exact/semantic round trip changed metadata");
    rejects(ErrorCode::InvalidText,[&]{parse(old_sdk);});
    const auto sdk=parse(derived);require(sdk.entries.size()==8u && sdk.find("TITLE")->text_value()=="Fixture title" && sdk.find("DISC_ID")->storage.size()==12u && serialize(sdk)==derived,"Independent mksfoex source-derived fixture mismatch");
    std::cout<<"Owned 472-byte PARAM.SFO: all ten entries/capacities + byte-exact roundtrip; primary SDK layout/malformed literal PASS\n";
}
void savedata_shape(){
    const std::array<std::uint8_t,7> raw{0u,0xFFu,0x80u,0xC0u,0u,0x42u,0x7Fu};
    Document doc{{Entry::text("TITLE","RAM",128u),Entry::integer("PARENTAL_LEVEL",9u),Entry::text("CATEGORY","MS",4u),
        Entry::text("SAVEDATA_DIRECTORY","ULUS10216DATA00",64u),Entry::text("SAVEDATA_TITLE","RAM",128u),
        Entry::text("SAVEDATA_DETAIL","0%",1024u),Entry::blob("USER_BLOB",raw,11u),Entry::text("UTF8_TITLE","\xC3\xA9\xE6\x97\xA5\xF0\x9F\x8C\xB6",32u),
        Entry::integer("HIGH_INTEGER",0xFFFFFFFFu),Entry::blob("EMPTY_BLOB",{}),Entry::text("EMPTY_TEXT","",1u)}};
    // Reserved bytes are opaque and retained, rather than reset during decoding.
    doc.find("USER_BLOB")->storage.at(10u)=0xA5u;
    const auto wire=serialize(doc);const auto decoded=parse(wire);
    require(decoded==doc && serialize(decoded)==wire,"Savedata-shaped/opaque reserved bytes changed during roundtrip");
    require(decoded.find("USER_BLOB")->blob_value().size()==raw.size() && std::equal(raw.begin(),raw.end(),decoded.find("USER_BLOB")->blob_value().begin()) &&
        decoded.find("USER_BLOB")->storage.back()==0xA5u,"Raw blob was interpreted as UTF-8 or lost reserved bytes");
    require(decoded.find("EMPTY_BLOB")->blob_value().empty() && decoded.find("EMPTY_TEXT")->text_value().empty() && decoded.find("HIGH_INTEGER")->integer_value()==0xFFFFFFFFu,"Empty/high-integer format vectors failed");
    const auto data=u32(wire,12u);require((data&3u)==0u,"Canonical data table unaligned");
    for(std::size_t i=0u;i<doc.entries.size();++i)require((u32(wire,20u+i*16u+12u)&3u)==0u,"Writer did not align an entry after odd capacity");
    require(!decoded.find("SAVEDATA_PARAMS") && !decoded.find("SAVEDATA_FILE_LIST"),"Synthetic fixture invented secure metadata");
    rejects(ErrorCode::WrongType,[&]{doc.find("TITLE")->integer_value();});
    rejects(ErrorCode::WrongType,[&]{doc.find("HIGH_INTEGER")->blob_value();});
    rejects(ErrorCode::WrongType,[&]{doc.find("USER_BLOB")->text_value();});
    std::cout<<"Savedata-shaped entries: UTF-8, opaque blob/reserved bytes, maxlen, odd capacity alignment, empty values, unsigned integers PASS\n";
}
void malformed(const Bytes &valid){
    auto check=[&](ErrorCode code,auto edit){auto b=valid;edit(b);rejects(code,[&]{parse(b);});};
    rejects(ErrorCode::Truncated,[&]{parse(std::span(valid).first(19u));});
    check(ErrorCode::InvalidMagic,[](auto &b){b[0]=1u;});
    check(ErrorCode::UnsupportedVersion,[](auto &b){put32(b,4u,0x102u);});
    check(ErrorCode::Truncated,[](auto &b){put32(b,16u,4096u);});
    check(ErrorCode::LimitExceeded,[](auto &b){put32(b,16u,0xFFFFFFFFu);});
    check(ErrorCode::InvalidTables,[](auto &b){put32(b,8u,20u);});
    check(ErrorCode::InvalidTables,[](auto &b){put32(b,12u,u32(b,8u)-4u);});
    check(ErrorCode::InvalidTables,[](auto &b){put32(b,12u,0xFFFFFFFCu);});
    check(ErrorCode::InvalidAlignment,[](auto &b){put32(b,12u,u32(b,12u)+1u);});
    check(ErrorCode::InvalidAlignment,[](auto &b){put32(b,20u+12u,1u);});
    check(ErrorCode::InvalidLength,[](auto &b){put32(b,20u+4u,5u);});
    check(ErrorCode::InvalidLength,[](auto &b){put32(b,20u+4u,3u);put32(b,20u+8u,3u);});
    check(ErrorCode::InvalidKey,[](auto &b){put16(b,20u,0xFFFFu);});
    check(ErrorCode::InvalidKey,[](auto &b){b[u32(b,8u)]=0u;});
    check(ErrorCode::InvalidKey,[](auto &b){const auto k=u32(b,8u),d=u32(b,12u);std::fill(b.begin()+k,b.begin()+d,std::uint8_t{0x41u});});
    check(ErrorCode::InvalidKey,[](auto &b){b[u32(b,8u)]=0xFFu;});
    check(ErrorCode::Overlap,[](auto &b){put16(b,36u,1u);});
    check(ErrorCode::Overlap,[](auto &b){put32(b,36u+12u,0u);});
    check(ErrorCode::Truncated,[](auto &b){put32(b,20u+8u,0xFFFFFFFFu);});
    check(ErrorCode::Truncated,[](auto &b){put32(b,20u+12u,0xFFFFFFFCu);});
    check(ErrorCode::UnsupportedFormat,[](auto &b){put16(b,20u+2u,0x0104u);});
    const auto category=u32(valid,12u)+u32(valid,36u+12u);
    check(ErrorCode::InvalidText,[&](auto &b){b[category+2u]=0x41u;});
    check(ErrorCode::InvalidText,[&](auto &b){b[category]=0u;});
    check(ErrorCode::InvalidText,[&](auto &b){b[category]=0xC0u;b[category+1u]=0xAFu;});
    check(ErrorCode::InvalidText,[](auto &b){put32(b,36u+4u,0u);});
    for(std::size_t i=0u;i<valid.size();++i){try{parse(std::span(valid).first(i));}catch(const Error &){continue;}throw std::runtime_error("Truncated owned SFO was accepted");}
    Document duplicate{{Entry::text("A","x"),Entry::text("A","y")}};
    rejects(ErrorCode::DuplicateKey,[&]{serialize(duplicate);});
    auto dup=serialize(Document{{Entry::text("A","x"),Entry::text("B","y")}});dup[u32(dup,8u)+2u]='A';rejects(ErrorCode::DuplicateKey,[&]{parse(dup);});
    std::cout<<"Malformed input: every owned-file truncation, overflow bounds, duplicate/overlapping keys/slots, magic/version/format/string/alignment errors PASS\n";
}
void builder_bounds(){
    rejects(ErrorCode::InvalidLength,[]{Entry::text("TITLE","RAM",3u);});
    const std::array<std::uint8_t,2> raw{1u,2u};rejects(ErrorCode::InvalidLength,[&]{Entry::blob("B",raw,1u);});
    rejects(ErrorCode::LimitExceeded,[]{Entry::text("T","",0xFFFFFFFFu);});
    rejects(ErrorCode::LimitExceeded,[&]{Entry::blob("B",raw,0xFFFFFFFFu);});
    rejects(ErrorCode::InvalidKey,[]{Entry::integer("",0u);});
    rejects(ErrorCode::InvalidKey,[]{Entry::integer(std::string("A\0B",3u),0u);});
    for(const auto text:std::array<std::string,7>{std::string("a\0b",3u),"\xC0\xAF","\xE0\x80\x80","\xED\xA0\x80","\xF4\x90\x80\x80","\xF5\x80\x80\x80","\xE2\x82"})
        rejects(ErrorCode::InvalidText,[&]{Entry::text("T",text);});
    Document doc{{Entry::text("T","x")}};const auto saved=doc;
    rejects(ErrorCode::LimitExceeded,[&]{serialize(doc,{20u,4096u});});require(doc==saved,"Failed serialization mutated input document");
    rejects(ErrorCode::LimitExceeded,[&]{serialize(doc,{1024u,0u});});
    auto too_many_keys=Document{{Entry::blob(std::string(65536u,'A'),{}),Entry::integer("B",0u)}};
    rejects(ErrorCode::LimitExceeded,[&]{serialize(too_many_keys);});
    doc.entries[0].used_length=9u;rejects(ErrorCode::InvalidLength,[&]{serialize(doc);});
    std::cout<<"Builder/resource bounds: no truncation, invalid UTF-8/NUL/type/length rejection, 16-bit key offsets, explicit allocation limits, atomic results PASS\n";
}
void deterministic_stress(const Bytes &valid){
    std::mt19937 rng(0x10216u);std::size_t accepted=0u,rejected=0u;
    for(unsigned trial=0u;trial<5000u;++trial){
        auto b=valid;const auto edits=1u+rng()%8u;
        for(unsigned i=0u;i<edits;++i){const auto p=rng()%b.size();b[p]=static_cast<std::uint8_t>(rng());}
        try{const auto doc=parse(b);const auto canonical=serialize(doc);require(parse(canonical)==doc,"Accepted mutation did not roundtrip");++accepted;}catch(const Error &){++rejected;}
    }
    require(accepted && rejected,"Mutation stress did not exercise both outcomes");
    for(unsigned trial=0u;trial<200u;++trial){
        Document doc;
        for(unsigned i=0u;i<12u;++i){const auto key="K"+std::to_string(i);const auto type=rng()%3u;
            if(type==0u)doc.entries.push_back(Entry::integer(key,rng()));
            else if(type==1u)doc.entries.push_back(Entry::text(key,std::string(rng()%31u,'x')));
            else{Bytes raw(rng()%63u);for(auto &byte:raw)byte=static_cast<std::uint8_t>(rng());doc.entries.push_back(Entry::blob(key,raw));}
        }
        require(parse(serialize(doc))==doc,"Generated mixed-type document failed semantic roundtrip");
    }
    std::cout<<"Deterministic malformed/mixed-data stress: 5000 wire mutations (accepted="<<accepted<<", rejected="<<rejected<<") + 200 documents PASS\n";
}
}
int main(int argc,char **argv){try{require(argc==4,"Expected owned/old-sdk/source-derived SFO paths");const auto owned=load(argv[1]);owned_and_primary(owned,load(argv[2]),load(argv[3]));savedata_shape();malformed(owned);builder_bounds();deterministic_stress(owned);std::cout<<"PASS: independent native PARAM.SFO format foundation\n";return 0;}catch(const std::exception &error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
