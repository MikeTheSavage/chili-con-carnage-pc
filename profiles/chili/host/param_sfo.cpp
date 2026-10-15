#include "param_sfo.hpp"
#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace chili::sfo {
namespace {
constexpr std::uint32_t magic=0x46535000u, version=0x00000101u;
constexpr std::size_t header_size=20u, index_size=16u;
[[noreturn]] void fail(ErrorCode code,const char *message){throw Error(code,message);}
bool fits(std::size_t offset,std::size_t count,std::size_t size){return offset<=size && count<=size-offset;}
std::uint16_t read16(std::span<const std::uint8_t> b,std::size_t p){return std::uint16_t(b[p]) | (std::uint16_t(b[p+1])<<8u);}
std::uint32_t read32(std::span<const std::uint8_t> b,std::size_t p){return std::uint32_t(b[p]) | (std::uint32_t(b[p+1])<<8u) | (std::uint32_t(b[p+2])<<16u) | (std::uint32_t(b[p+3])<<24u);}
void write16(Bytes &b,std::size_t p,std::uint16_t value){b[p]=static_cast<std::uint8_t>(value);b[p+1]=static_cast<std::uint8_t>(value>>8u);}
void write32(Bytes &b,std::size_t p,std::uint32_t value){for(unsigned i=0;i<4u;++i)b[p+i]=static_cast<std::uint8_t>(value>>(i*8u));}
std::size_t add(std::size_t a,std::size_t b){if(b>std::numeric_limits<std::size_t>::max()-a)fail(ErrorCode::LimitExceeded,"SFO size overflow");return a+b;}
std::size_t align4(std::size_t n){return add(n,3u)&~std::size_t(3u);}
std::uint32_t narrow(std::size_t n){if(n>std::numeric_limits<std::uint32_t>::max())fail(ErrorCode::LimitExceeded,"SFO 32-bit size limit");return static_cast<std::uint32_t>(n);}
bool utf8(std::span<const std::uint8_t> bytes){
    for(std::size_t i=0;i<bytes.size();){
        const auto first=bytes[i++];if(first<0x80u)continue;
        unsigned following;std::uint32_t cp,min;
        if(first>=0xC2u && first<=0xDFu){following=1u;cp=first&0x1Fu;min=0x80u;}
        else if(first>=0xE0u && first<=0xEFu){following=2u;cp=first&0x0Fu;min=0x800u;}
        else if(first>=0xF0u && first<=0xF4u){following=3u;cp=first&7u;min=0x10000u;}
        else return false;
        if(!fits(i,following,bytes.size()))return false;
        while(following--){const auto next=bytes[i++];if((next&0xC0u)!=0x80u)return false;cp=(cp<<6u)|(next&0x3Fu);}
        if(cp<min || cp>0x10FFFFu || (cp>=0xD800u && cp<=0xDFFFu))return false;
    }
    return true;
}
void validate_key(std::string_view key){
    if(key.empty() || key.find('\0')!=std::string_view::npos || !utf8({reinterpret_cast<const std::uint8_t *>(key.data()),key.size()}))
        fail(ErrorCode::InvalidKey,"SFO key is empty, contains NUL or is invalid UTF-8");
}
void validate(const Entry &entry){
    validate_key(entry.key);
    if(entry.used_length>entry.storage.size() || entry.storage.size()>std::numeric_limits<std::uint32_t>::max())
        fail(ErrorCode::InvalidLength,"SFO used length exceeds reserved slot");
    switch(entry.format){
    case Format::Integer:
        if(entry.used_length!=4u || entry.storage.size()!=4u)fail(ErrorCode::InvalidLength,"SFO integer must have exact length/capacity four");
        break;
    case Format::Text: {
        if(!entry.used_length || entry.storage[entry.used_length-1u]!=0u)fail(ErrorCode::InvalidText,"SFO text length must include NUL terminator");
        const auto value=std::span(entry.storage).first(entry.used_length-1u);
        if(std::find(value.begin(),value.end(),0u)!=value.end() || !utf8(value))fail(ErrorCode::InvalidText,"SFO text contains premature NUL or invalid UTF-8");
        break;
    }
    case Format::Blob:break;
    default:fail(ErrorCode::UnsupportedFormat,"SFO data format is unsupported");
    }
}
void unique_entries(const Document &doc,Limits limits){
    if(doc.entries.size()>limits.entries || doc.entries.size()>std::numeric_limits<std::uint32_t>::max())fail(ErrorCode::LimitExceeded,"SFO entry resource limit");
    std::set<std::string_view> names;
    for(const auto &e:doc.entries){validate(e);if(!names.emplace(e.key).second)fail(ErrorCode::DuplicateKey,"Duplicate SFO key");}
}
void validate_ranges(std::vector<std::pair<std::size_t,std::size_t>> ranges){
    std::sort(ranges.begin(),ranges.end());
    for(std::size_t i=1u;i<ranges.size();++i)if(ranges[i].first<ranges[i-1u].second)fail(ErrorCode::Overlap,"SFO reserved slots or key strings overlap");
}
}

Entry Entry::text(std::string key,std::string_view value,std::uint32_t capacity,Limits limits){
    const auto used=add(value.size(),1u);const auto max=capacity?capacity:align4(used);
    if(used>max)fail(ErrorCode::InvalidLength,"SFO text exceeds requested capacity");
    if(max>limits.file_bytes || key.size()>limits.file_bytes)fail(ErrorCode::LimitExceeded,"SFO entry resource limit");
    Entry e{std::move(key),Format::Text,narrow(used),Bytes(max,0u)};
    std::copy(value.begin(),value.end(),e.storage.begin());validate(e);return e;
}
Entry Entry::integer(std::string key,std::uint32_t value){
    Entry e{std::move(key),Format::Integer,4u,Bytes(4u)};write32(e.storage,0u,value);validate(e);return e;
}
Entry Entry::blob(std::string key,std::span<const std::uint8_t> value,std::uint32_t capacity,Limits limits){
    const auto max=capacity?std::size_t(capacity):align4(value.size());
    if(value.size()>max)fail(ErrorCode::InvalidLength,"SFO blob exceeds requested capacity");
    if(max>limits.file_bytes || key.size()>limits.file_bytes)fail(ErrorCode::LimitExceeded,"SFO entry resource limit");
    Entry e{std::move(key),Format::Blob,narrow(value.size()),Bytes(max,0u)};
    std::copy(value.begin(),value.end(),e.storage.begin());validate(e);return e;
}
std::string_view Entry::text_value() const{
    if(format!=Format::Text)fail(ErrorCode::WrongType,"SFO entry is not text");validate(*this);
    return {reinterpret_cast<const char *>(storage.data()),used_length-1u};
}
std::uint32_t Entry::integer_value() const{
    if(format!=Format::Integer)fail(ErrorCode::WrongType,"SFO entry is not integer");validate(*this);return read32(storage,0u);
}
std::span<const std::uint8_t> Entry::blob_value() const{
    if(format!=Format::Blob)fail(ErrorCode::WrongType,"SFO entry is not blob");validate(*this);return std::span(storage).first(used_length);
}
const Entry *Document::find(std::string_view key) const noexcept{
    const auto i=std::find_if(entries.begin(),entries.end(),[&](const auto &e){return e.key==key;});return i==entries.end()?nullptr:&*i;
}
Entry *Document::find(std::string_view key) noexcept{
    const auto i=std::find_if(entries.begin(),entries.end(),[&](const auto &e){return e.key==key;});return i==entries.end()?nullptr:&*i;
}
Document parse(std::span<const std::uint8_t> bytes,Limits limits){
    if(bytes.size()>limits.file_bytes)fail(ErrorCode::LimitExceeded,"SFO file resource limit");
    if(bytes.size()<header_size)fail(ErrorCode::Truncated,"SFO header is truncated");
    if(read32(bytes,0u)!=magic)fail(ErrorCode::InvalidMagic,"SFO PSF magic is invalid");
    if(read32(bytes,4u)!=version)fail(ErrorCode::UnsupportedVersion,"SFO version requires verified support");
    const std::size_t keys=read32(bytes,8u),data=read32(bytes,12u),count=read32(bytes,16u);
    if(count>limits.entries)fail(ErrorCode::LimitExceeded,"SFO entry resource limit");
    if(count>(bytes.size()-header_size)/index_size)fail(ErrorCode::Truncated,"SFO index table is truncated");
    const auto index_end=header_size+count*index_size;
    if(keys<index_end || data<keys || data>bytes.size())fail(ErrorCode::InvalidTables,"SFO key/data tables cross index or file bounds");
    if((keys&3u)||(data&3u))fail(ErrorCode::InvalidAlignment,"SFO table is not four-byte aligned");
    struct Layout {std::string_view key;Format format;std::uint32_t used;std::size_t capacity,start;};
    std::vector<Layout> layouts;layouts.reserve(count);
    std::vector<std::pair<std::size_t,std::size_t>> key_ranges,data_ranges;
    for(std::size_t i=0u;i<count;++i){
        const auto p=header_size+i*index_size;
        const auto key_offset=read16(bytes,p),format=read16(bytes,p+2u);
        const std::size_t used=read32(bytes,p+4u),capacity=read32(bytes,p+8u),offset=read32(bytes,p+12u);
        if(key_offset>=data-keys)fail(ErrorCode::InvalidKey,"SFO key offset is outside key table");
        const auto start=keys+key_offset;
        const auto nul=std::find(bytes.begin()+start,bytes.begin()+data,0u);
        if(nul==bytes.begin()+data)fail(ErrorCode::InvalidKey,"SFO key has no bounded NUL terminator");
        const auto end=static_cast<std::size_t>(nul-bytes.begin());
        key_ranges.emplace_back(start,end+1u);
        if(used>capacity)fail(ErrorCode::InvalidLength,"SFO used length exceeds reserved slot");
        if(offset&3u)fail(ErrorCode::InvalidAlignment,"SFO data entry is not four-byte aligned");
        if(!fits(offset,capacity,bytes.size()-data))fail(ErrorCode::Truncated,"SFO reserved data slot is truncated");
        if(capacity)data_ranges.emplace_back(data+offset,data+offset+capacity);
        layouts.push_back({{reinterpret_cast<const char *>(bytes.data()+start),end-start},static_cast<Format>(format),static_cast<std::uint32_t>(used),capacity,data+offset});
    }
    // Reject overlap and duplicates before copying attacker-selected capacities.
    validate_ranges(std::move(key_ranges));validate_ranges(std::move(data_ranges));
    std::set<std::string_view> names;
    for(const auto &layout:layouts){validate_key(layout.key);if(!names.emplace(layout.key).second)fail(ErrorCode::DuplicateKey,"Duplicate SFO key");}
    Document doc;doc.entries.reserve(count);
    for(const auto &layout:layouts){
        Entry e{std::string(layout.key),layout.format,layout.used,Bytes(bytes.begin()+layout.start,bytes.begin()+layout.start+layout.capacity)};
        validate(e);doc.entries.push_back(std::move(e));
    }
    return doc;
}
Bytes serialize(const Document &doc,Limits limits){
    unique_entries(doc,limits);
    // Check every dimension before allocating or writing an output buffer.
    const auto count=doc.entries.size();
    if(count>(std::numeric_limits<std::size_t>::max()-header_size)/index_size)fail(ErrorCode::LimitExceeded,"SFO index size overflow");
    const auto keys=header_size+count*index_size;
    std::size_t key_size=0u,data_size=0u;
    std::vector<std::size_t> key_offsets,data_offsets;key_offsets.reserve(count);data_offsets.reserve(count);
    for(const auto &entry:doc.entries){
        if(key_size>std::numeric_limits<std::uint16_t>::max())fail(ErrorCode::LimitExceeded,"SFO key offset exceeds 16-bit field");
        key_offsets.push_back(key_size);key_size=add(key_size,add(entry.key.size(),1u));
        data_size=align4(data_size);data_offsets.push_back(data_size);narrow(data_size);data_size=add(data_size,entry.storage.size());
    }
    const auto data=align4(add(keys,key_size)),total=add(data,data_size);narrow(keys);narrow(data);narrow(total);
    if(total>limits.file_bytes)fail(ErrorCode::LimitExceeded,"SFO file resource limit");
    Bytes bytes(total,0u);write32(bytes,0u,magic);write32(bytes,4u,version);write32(bytes,8u,narrow(keys));write32(bytes,12u,narrow(data));write32(bytes,16u,narrow(count));
    for(std::size_t i=0u;i<count;++i){
        const auto &entry=doc.entries[i];const auto p=header_size+i*index_size;
        write16(bytes,p,static_cast<std::uint16_t>(key_offsets[i]));write16(bytes,p+2u,static_cast<std::uint16_t>(entry.format));
        write32(bytes,p+4u,entry.used_length);write32(bytes,p+8u,narrow(entry.storage.size()));write32(bytes,p+12u,narrow(data_offsets[i]));
        std::copy(entry.key.begin(),entry.key.end(),bytes.begin()+keys+key_offsets[i]);
        std::copy(entry.storage.begin(),entry.storage.end(),bytes.begin()+data+data_offsets[i]);
    }
    return bytes;
}
}
