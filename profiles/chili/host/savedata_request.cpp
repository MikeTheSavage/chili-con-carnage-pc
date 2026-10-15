#include "savedata_request.hpp"
#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
namespace chili::save_request {
namespace {
constexpr std::size_t max_payload=8u*1024u*1024u,max_media=4u*1024u*1024u;
void span(const psprecomp::GuestMemory&m,std::uint32_t a,std::size_t n,bool aligned=false){
    const auto physical=psprecomp::GuestMemory::canonical(a);
    // Adopt only user-main-RAM buffers for these operations. This also rejects
    // VRAM mirror aliases, scratchpad and unadopted volatile-memory sources.
    if(!a||(a&0x80000000u)||(aligned&&(a&3u))||physical<0x08800000u||physical>=0x0A000000u||n>0x0A000000u-physical||!m.contains(a,n))throw Error(Failure::Address,"Invalid or unsupported savedata user-RAM span");
}
bool overlap(std::uint32_t a,std::size_t an,std::uint32_t b,std::size_t bn){
    const auto x=std::uint64_t(psprecomp::GuestMemory::canonical(a)),y=std::uint64_t(psprecomp::GuestMemory::canonical(b));
    return an&&bn&&x<y+bn&&y<x+an;
}
std::string text(const psprecomp::GuestMemory&m,std::uint32_t a,unsigned n){
    std::string out;
    for(unsigned i=0;i<n;++i){span(m,a+i,1u);const auto c=m.load8(a+i);if(!c)return out;out.push_back(char(c));}
    throw Error(Failure::Parameter,"Unterminated savedata text field");
}
void component(const std::string&s,unsigned capacity){
    if(s.empty()||s.size()>=capacity||s=="."||s==".."||s.back()=='.'||s.back()==' ')throw Error(Failure::Parameter,"Invalid savedata component");
    for(unsigned char c:s)if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))throw Error(Failure::Parameter,"Unsupported savedata filename component");
    auto stem=s.substr(0,s.find('.'));for(auto&c:stem)c=char(std::toupper(static_cast<unsigned char>(c)));
    if(stem=="CON"||stem=="PRN"||stem=="AUX"||stem=="NUL"||(stem.size()==4u&&(stem.starts_with("COM")||stem.starts_with("LPT"))&&stem[3]>='1'&&stem[3]<='9'))throw Error(Failure::Parameter,"Reserved native savedata filename");
}
Buffer buffer(const psprecomp::GuestMemory&m,std::uint32_t p,unsigned off,bool save,std::size_t limit){
    Buffer b{m.load32(p+off),m.load32(p+off+4),m.load32(p+off+8)};
    if(b.capacity>limit||b.size>b.capacity)throw Error(Failure::Parameter,"Savedata buffer exceeds capacity or native limit");
    if(b.capacity){span(m,b.address,b.capacity);if(overlap(p,1536u,b.address,b.capacity))throw Error(Failure::Address,"Savedata buffer overlaps parameters");}
    else if(b.size)throw Error(Failure::Parameter,"Savedata empty buffer has data");
    if(save&&b.size)span(m,b.address,b.size);
    return b;
}
}
Request parse(const psprecomp::GuestMemory&m,std::uint32_t p){
    span(m,p,4u,true);if(m.load32(p)!=1536u)throw Error(Failure::Size,"Native savedata operation requires1536-byte request");span(m,p,1536u,true);
    Request r;r.params=p;m.copy_out(p,r.original);r.mode=m.load32(p+48u);
    if(r.mode!=0u&&r.mode!=1u&&r.mode!=4u&&r.mode!=5u&&r.mode!=7u)throw Error(Failure::Unsupported,"Unsupported native savedata operation");
    r.language=m.load32(p+4u);r.button_swap=m.load32(p+8u);r.focus=m.load32(p+1480u);r.secure_version=m.load32(p+1516u);
    // Read, never judged: the firmware takes any value here (see Request::overwrite_word).
    r.overwrite_word=m.load32(p+56u);
    if(r.language>11u||r.button_swap>1u||r.focus>8u||m.load32(p+52u))throw Error(Failure::Unsupported,"Unsupported savedata language, mapping, focus or bind");
    r.game=text(m,p+60u,13u);component(r.game,13u);
    r.slot=text(m,p+76u,20u);if(!r.slot.empty())component(r.slot,20u);
    r.file=text(m,p+100u,13u);component(r.file,13u);
    // A delete transfers nothing: its payload and media fields are not looked at.
    const bool transfers=!r.is_delete();
    if(transfers){
        r.data=buffer(m,p,116u,r.is_save(),max_payload);
        if(!r.data.capacity)throw Error(Failure::Parameter,"Savedata payload has no output/input capacity");
    }
    r.title=text(m,p+128u,128u);r.savedata_title=text(m,p+256u,128u);r.detail=text(m,p+384u,1024u);r.parental=m.load8(p+1408u);
    if(transfers)for(unsigned i=0;i<4;++i){const auto off=1412u+16u*i;r.media[i]=buffer(m,p,off,r.is_save(),max_media);if(m.load32(p+off+12u))throw Error(Failure::Unsupported,"Unsupported savedata media flags");}
    m.copy_out(p+1500u,r.key);
    const bool loads=transfers&&!r.is_save();
    if(loads){
        std::array<Buffer,5> outputs{r.data,r.media[0],r.media[1],r.media[2],r.media[3]};
        for(unsigned i=0;i<outputs.size();++i)for(unsigned j=0;j<i;++j)
            if(overlap(outputs[i].address,outputs[i].capacity,outputs[j].address,outputs[j].capacity))throw Error(Failure::Address,"Savedata load buffers overlap");
    }
    const auto list=m.load32(p+96u);
    if(r.interactive()){
        if(!list)throw Error(Failure::Address,"Interactive savedata requires a slot list");
        std::set<std::string> unique;bool terminated=false;
        for(unsigned i=0;i<=128u;++i){
            const auto delta=20u*i;if(list>0xFFFFFFFFu-delta)throw Error(Failure::Address,"Savedata list address overflow");
            const auto a=list+delta;span(m,a,20u);if(overlap(p,1536u,a,20u))throw Error(Failure::Address,"Savedata list overlaps parameters");
            if(loads){
                if(overlap(a,20u,r.data.address,r.data.capacity))throw Error(Failure::Address,"Savedata load buffer overlaps slot list");
                for(const auto&b:r.media)if(overlap(a,20u,b.address,b.capacity))throw Error(Failure::Address,"Savedata media output overlaps slot list");
            }
            const auto name=text(m,a,20u);if(name.empty()){terminated=true;break;}if(i==128u)break;component(name,20u);
            auto folded=name;for(auto&c:folded)c=char(std::toupper(static_cast<unsigned char>(c)));
            if(!unique.insert(folded).second)throw Error(Failure::Parameter,"Duplicate savedata slot name");r.slots.push_back(name);
        }
        if(!terminated||r.slots.empty())throw Error(Failure::Parameter,"Savedata slot list missing terminator or entries");
    }else if(r.slot.empty())throw Error(Failure::Parameter,"Automatic savedata requires a slot name");
    const auto new_data=m.load32(p+1476u);
    if(new_data&&r.mode==5u){span(m,new_data,20u,true);const auto t=m.load32(new_data+16u);if(t)r.new_title=text(m,t,128u);}
    return r;
}
std::vector<std::uint8_t> bytes(const psprecomp::GuestMemory&m,Buffer b){
    if(b.size>b.capacity||b.size>max_payload)throw Error(Failure::Parameter,"Savedata snapshot exceeds limits");
    std::vector<std::uint8_t> out(b.size);if(b.size){span(m,b.address,b.size);m.copy_out(b.address,out);}return out;
}
bool header_unchanged(const psprecomp::GuestMemory&m,const Request&r){
    if(!m.contains(r.params,r.original.size()))return false;std::array<std::uint8_t,1536> now{};m.copy_out(r.params,now);return now==r.original;
}
}
