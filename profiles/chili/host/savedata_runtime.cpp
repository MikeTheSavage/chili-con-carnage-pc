#include "savedata_runtime.hpp"
#include "savedata_size_query.hpp"
#include "fixed_file_time.hpp"
#include "thread_runtime.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <limits>
#include <iostream>
#include <sys/stat.h>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace chili {
namespace {
constexpr std::uint32_t invalid_status = 0x80110001u;
constexpr std::uint32_t invalid_address = 0x80110002u;
constexpr std::uint32_t invalid_param_size = 0x80110004u;
constexpr std::uint32_t wrong_type = 0x80110005u;
constexpr std::uint32_t result_offset = 28u, id_list_offset = 1524u;
// common.result of an operation the player backed out of (SCE_UTILITY_DIALOG_RESULT_CANCEL).
constexpr std::uint32_t result_cancelled = 1u;
const char *mode_name(std::uint32_t mode) {
    switch (mode) {
    case 0u: return "automatic load";
    case 1u: return "automatic save";
    case 2u: return "load";
    case 3u: return "save";
    case 4u: return "load from a list";
    case 5u: return "save to a list";
    case 6u: return "delete";
    case 7u: return "delete from a list";
    case 8u: return "sizes";
    case 11u: return "list";
    default: return "not a mode this port knows";
    }
}

bool guest_span(const psprecomp::GuestMemory &memory, std::uint32_t address, std::uint64_t bytes) {
    return address && !(address & 0x80000003u) && bytes <= std::numeric_limits<std::size_t>::max() &&
        memory.contains(address, static_cast<std::size_t>(bytes));
}
bool overlap(std::uint32_t a, std::uint64_t as, std::uint32_t b, std::uint64_t bs) {
    const auto ca = static_cast<std::uint64_t>(psprecomp::GuestMemory::canonical(a));
    const auto cb = static_cast<std::uint64_t>(psprecomp::GuestMemory::canonical(b));
    return as && bs && ca < cb + bs && cb < ca + as;
}
std::string fixed_string(const psprecomp::GuestMemory &memory, std::uint32_t address, unsigned bytes) {
    std::string value;
    for (unsigned i = 0; i < bytes; ++i) {
        const auto c = memory.load8(address + i);
        if (!c) break;
        value.push_back(static_cast<char>(c));
    }
    return value;
}
bool component_valid(const std::string &value, bool pattern) {
    if (value == "." || value == "..") return false;
    for (const auto c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 32u || byte > 126u || c == '/' || c == '\\' || c == ':' ||
            c == '"' || c == '<' || c == '>' || c == '|' || (!pattern && (c == '*' || c == '?')))
            return false;
    }
    return true;
}
unsigned char fold(unsigned char c) { return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c; }
std::string folded(std::string value) {
    for (auto &c : value) c = static_cast<char>(fold(static_cast<unsigned char>(c)));
    return value;
}
// FAT-style ASCII comparison, with * and ? restricted to the save-name suffix.
bool wildcard(const std::string &pattern, const std::string &name) {
    std::size_t p{}, n{}, star = std::string::npos, retry{};
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' ||
            fold(static_cast<unsigned char>(pattern[p])) == fold(static_cast<unsigned char>(name[n])))) {
            ++p; ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++; retry = n;
        } else if (star != std::string::npos) {
            p = star + 1u; n = ++retry;
        } else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}
void reject_reparse(const std::filesystem::path &path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        throw psprecomp::Error("Cannot inspect savedata path attributes");
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        throw psprecomp::Error("Native savedata reparse/symlink paths are unsupported");
}
bool directory_or_missing(const std::filesystem::path &path) {
    const auto state = std::filesystem::symlink_status(path);
    if (!std::filesystem::exists(state)) return false;
    reject_reparse(path);
    if (!std::filesystem::is_directory(state))
        throw psprecomp::Error("Native savedata directory path is not a directory");
    return true;
}
using Entry = std::array<std::uint8_t, SavedataRuntime::id_entry_size>;
void put16(Entry &entry, unsigned offset, std::uint16_t value) {
    entry[offset] = static_cast<std::uint8_t>(value);
    entry[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
}
Entry native_entry(const std::filesystem::path &directory, const std::string &suffix) {
    Entry entry{};
    // Hardware idlist.expected observes 0x11FF (directory plus rwx permissions).
    entry[0] = 0xFFu; entry[1] = 0x11u;
    auto timestamp_path = directory;
    const auto sfo = directory / "PARAM.SFO";
    const auto sfo_state = std::filesystem::symlink_status(sfo);
    if (std::filesystem::exists(sfo_state)) {
        reject_reparse(sfo);
        if (!std::filesystem::is_regular_file(sfo_state))
            throw psprecomp::Error("Native savedata PARAM.SFO is not a regular file");
        timestamp_path = sfo;
    }
    struct _stat64 info{};
    if (_wstat64(timestamp_path.c_str(), &info) != 0)
        throw psprecomp::Error("Cannot stat native savedata timestamps");
    const auto fixed = fixed_file_time;
    const __time64_t times[] = {fixed ? *fixed : info.st_ctime, fixed ? *fixed : info.st_atime, fixed ? *fixed : info.st_mtime};
    for (unsigned i = 0; i < 3u; ++i) {
        std::tm date{};
        if (_localtime64_s(&date, &times[i]) != 0 || date.tm_year + 1900 < 0 || date.tm_year + 1900 > 65535)
            throw psprecomp::Error("Native savedata timestamp is outside the guest date range");
        const unsigned values[] = {static_cast<unsigned>(date.tm_year + 1900), static_cast<unsigned>(date.tm_mon + 1),
            static_cast<unsigned>(date.tm_mday), static_cast<unsigned>(date.tm_hour),
            static_cast<unsigned>(date.tm_min), static_cast<unsigned>(date.tm_sec)};
        for (unsigned j = 0; j < 6u; ++j) put16(entry, 4u + 16u * i + 2u * j, static_cast<std::uint16_t>(values[j]));
        // _stat64 has real second-resolution timestamps, consistent with FileRuntime.
        // The remaining guest microsecond word is therefore zero.
    }
    std::copy(suffix.begin(), suffix.end(), entry.begin() + 52u);
    return entry;
}
}

SavedataRuntime::SavedataRuntime(psprecomp::Runtime &runtime, std::filesystem::path memory_stick_root,
    ThreadRuntime *threads, UtilityDialogOwner *owner,SaveSlotPresenter *presenter,SavedataBackend *backend) : runtime_(runtime),
    memory_stick_root_(std::move(memory_stick_root)), threads_(threads), owner_(owner ? owner : &private_owner_),presenter_(presenter),backend_(backend) {
    runtime.register_hle("sceUtility", 0x50C4CD57u, [this](auto &, auto &c) { init_start(c); });
    runtime.register_hle("sceUtility", 0xD4B95FFBu, [this](auto &, auto &c) { update(c); });
    runtime.register_hle("sceUtility", 0x8874DBE0u, [this](auto &, auto &c) { get_status(c); });
    runtime.register_hle("sceUtility", 0x9790B33Cu, [this](auto &, auto &c) { shutdown_start(c); });
}
SavedataRuntime::~SavedataRuntime() {
    close_presenter();
    if (owns_slot_) owner_->release(UtilityDialogType::Savedata);
}

std::uint64_t SavedataRuntime::now_us() const {
    if (threads_) return threads_->now_us();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
void SavedataRuntime::refresh_status() {
    if (now_us() < deadline_) return;
    if (status_ == Status::Init) status_ = Status::Running;
    else if (status_ == Status::Shutdown) {
        if (owns_slot_) owner_->release(UtilityDialogType::Savedata);
        owns_slot_ = false;
        status_ = Status::None;
        params_ = id_list_ = mode_ = 0u;
        game_name_.clear(); save_pattern_.clear();
        operation_={};slot_request_={};operation_error_=0u;declined_=false;
    }
}

void SavedataRuntime::init_start(psprecomp::AllegrexContext &ctx) {
    refresh_status();
    if (status_ != Status::None || owner_->busy()) { ctx.set_gpr(2u, invalid_status); return; }
    const auto params = ctx.gpr[4];
    auto &memory = runtime_.memory();
    if (!guest_span(memory, params, 4u)) { ctx.set_gpr(2u, invalid_address); return; }
    const auto size = memory.load32(params);
    if (size != 0x5C8u && size != 0x5DCu && size != param_size) {
        ctx.set_gpr(2u, invalid_param_size); return;
    }
    if (!guest_span(memory, params, size)) { ctx.set_gpr(2u, invalid_address); return; }
    const auto mode = memory.load32(params + 48u);
    declined_ = false;
    if(mode==0u||mode==1u||mode==4u||mode==5u||mode==7u){begin_operation(params,ctx);return;}
    if (mode != list_mode && mode != sizes_mode) {
        // A mode this port does not implement. A choice in one of the game's
        // menus must never end the game, so the request runs through the
        // utility's ordinary life cycle and ends as if the player had backed
        // out of its screen. Nothing is read or written for it.
        begin_declined(params, mode, "this port does not implement it", ctx);
        return;
    }
    if (mode == sizes_mode) {
        if (size != 0x5DCu && size != param_size) { ctx.set_gpr(2u, invalid_param_size); return; }
        const std::array containers{
            std::pair{memory.load32(params + 1488u),20u},
            std::pair{memory.load32(params + 1492u),64u},
            std::pair{memory.load32(params + 1496u),28u}};
        for (unsigned i=0; i<containers.size(); ++i) {
            const auto [address,length]=containers[i];
            if (!address) continue;
            if (!guest_span(memory,address,length) || overlap(params,size,address,length)) {
                ctx.set_gpr(2u,invalid_address); return;
            }
            for (unsigned j=0; j<i; ++j) {
                if (containers[j].first && overlap(address,length,containers[j].first,containers[j].second)) {
                    ctx.set_gpr(2u,invalid_address); return;
                }
            }
        }
        if (!owner_->try_acquire(UtilityDialogType::Savedata)) { ctx.set_gpr(2u,invalid_status); return; }
        owns_slot_=true; params_=params; id_list_=0u; mode_=mode;
        game_name_.clear(); save_pattern_.clear();
        status_=Status::Init; deadline_=now_us()+init_delay_us;
        ctx.set_gpr(2u,0u); return;
    }
    if (size != param_size) { ctx.set_gpr(2u, invalid_param_size); return; }
    const auto game_name = fixed_string(memory, params + 60u, 13u);
    const auto pattern = fixed_string(memory, params + 76u, 20u);
    if (game_name.empty() || !component_valid(game_name, false) || !component_valid(pattern, true)) {
        runtime_.stop("Unsupported native savedata game name or save-name pattern"); return;
    }
    const auto id_list = memory.load32(params + id_list_offset);
    if (!guest_span(memory, id_list, 12u) || overlap(params, size, id_list, 12u)) {
        ctx.set_gpr(2u, invalid_address); return;
    }
    const auto max_count = memory.load32(id_list), entries = memory.load32(id_list + 8u);
    const auto entry_bytes = static_cast<std::uint64_t>(max_count) * id_entry_size;
    if (max_count > 0x7FFFFFFFu || (max_count && (!guest_span(memory, entries, entry_bytes) ||
        overlap(params, size, entries, entry_bytes) || overlap(id_list, 12u, entries, entry_bytes)))) {
        ctx.set_gpr(2u, invalid_address); return;
    }
    if (!owner_->try_acquire(UtilityDialogType::Savedata)) { ctx.set_gpr(2u, invalid_status); return; }
    owns_slot_ = true;
    params_ = params; id_list_ = id_list; mode_ = mode;
    game_name_ = game_name; save_pattern_ = pattern;
    status_ = Status::Init; deadline_ = now_us() + init_delay_us;
    ctx.set_gpr(2u, 0u);
}

void SavedataRuntime::get_status(psprecomp::AllegrexContext &ctx) {
    refresh_status();
    ctx.set_gpr(2u, owner_->is_current_type(UtilityDialogType::Savedata) ?
        static_cast<std::uint32_t>(status_) : wrong_type);
}
void SavedataRuntime::update(psprecomp::AllegrexContext &ctx) {
    refresh_status();
    if (!owner_->is_current_type(UtilityDialogType::Savedata)) { ctx.set_gpr(2u, wrong_type); return; }
    if (status_ != Status::Running) { ctx.set_gpr(2u, invalid_status); return; }
    const bool done=declined_?decline():mode_==sizes_mode?query_sizes(ctx):mode_==list_mode?enumerate(ctx):operate(ctx);
    if (done) {
        status_ = Status::Finished;
        ctx.set_gpr(2u, 0u);
    }
}
void SavedataRuntime::shutdown_start(psprecomp::AllegrexContext &ctx) {
    refresh_status();
    if (!owner_->is_current_type(UtilityDialogType::Savedata)) { ctx.set_gpr(2u, wrong_type); return; }
    if (status_ != Status::Finished) { ctx.set_gpr(2u, invalid_status); return; }
    status_ = Status::Shutdown; deadline_ = now_us() + shutdown_delay_us;
    ctx.set_gpr(2u, 0u);
}

void SavedataRuntime::close_presenter() noexcept {
    if(opened_&&presenter_)presenter_->close(slot_request_.generation);
    opened_=false;
}
bool SavedataRuntime::begin_declined(std::uint32_t params,std::uint32_t mode,const std::string&why,psprecomp::AllegrexContext &ctx){
    if(!owner_->try_acquire(UtilityDialogType::Savedata)){ctx.set_gpr(2u,invalid_status);return false;}
    owns_slot_=true;params_=params;id_list_=0u;mode_=mode;declined_=true;
    operation_={};slot_request_={};operation_error_=0u;opened_=false;
    game_name_.clear();save_pattern_.clear();status_=Status::Init;deadline_=now_us()+init_delay_us;
    std::cout<<"[savedata] request mode="<<mode<<" ("<<mode_name(mode)<<") is answered as cancelled (result 1), nothing read or written: "<<why<<std::endl;
    ctx.set_gpr(2u,0u);return true;
}
bool SavedataRuntime::decline(){
    runtime_.memory().store32(params_+result_offset,result_cancelled);
    return true;
}
bool SavedataRuntime::begin_operation(std::uint32_t params,psprecomp::AllegrexContext &ctx){
    if(!backend_){runtime_.stop("Native savedata operation requires a complete native save/load backend");return false;}
    save_request::Request request;
    try{request=save_request::parse(runtime_.memory(),params);}
    catch(const save_request::Error &e){
        // A language, button mapping, focus, bind or media flag outside what
        // this port handles: answered as cancelled, like a mode it does not have.
        if(e.kind==save_request::Failure::Unsupported)return begin_declined(params,runtime_.memory().load32(params+48u),e.what(),ctx);
        ctx.set_gpr(2u,e.kind==save_request::Failure::Address?invalid_address:e.kind==save_request::Failure::Size?invalid_param_size:0x80110388u);return false;
    }
    if(request.interactive()&&!presenter_){runtime_.stop("Native savedata interactive request requires an actual user-input presenter");return false;}
    SaveSlotRequest dialog;dialog.generation=++next_generation_;
    dialog.title=request.is_delete()?"Delete profile":request.is_save()?"Save profile":"Load profile";
    dialog.language=request.language;dialog.button_swap=request.button_swap;
    dialog.operation=request.is_delete()?SaveSlotOperation::Delete:request.is_save()?SaveSlotOperation::Save:SaveSlotOperation::Load;
    if(const auto place=backend_->previous_place();!place.empty()){
        if(request.is_delete())dialog.confirmation_note="Its files are not erased: the folder is moved to "+place+" in the saves folder, in place of the one kept there for this slot.";
        else if(request.is_save())dialog.confirmation_note="The save it replaces is kept in "+place+" in the saves folder until the next save over this slot.";
    }
    std::uint32_t backend_error{};
    try{
        const auto slots=backend_->inspect(request);
        const auto names=request.interactive()?request.slots:std::vector<std::string>{request.slot};
        if(slots.size()!=names.size())throw std::runtime_error("Savedata backend returned an inconsistent slot list");
        for(std::size_t i=0;i<slots.size();++i){if(slots[i].slot.name!=names[i])throw std::runtime_error("Savedata backend reordered slot names");dialog.slots.push_back(slots[i].slot);}
        std::size_t chosen{};bool found=false;
        if(request.focus==2u)chosen=slots.size()-1u;
        else if(request.focus==3u||request.focus==4u){
            for(std::size_t i=0;i<slots.size();++i)if(slots[i].slot.exists&&(!found||(request.focus==3u?slots[i].written_filetime>slots[chosen].written_filetime:slots[i].written_filetime<slots[chosen].written_filetime))){chosen=i;found=true;}
        }else if(request.focus>=5u){
            const bool want_data=request.focus==5u||request.focus==6u,last=request.focus==6u||request.focus==8u;
            for(std::size_t i=0;i<slots.size();++i)if(slots[i].slot.exists==want_data){chosen=i;found=true;if(!last)break;}
        }
        // A delete starts on a save that can be deleted when the focused slot is empty.
        if(request.is_delete()&&!slots[chosen].slot.exists)for(std::size_t i=0;i<slots.size();++i)if(slots[i].slot.exists){chosen=i;break;}
        dialog.initial_index=chosen;
    }catch(const SavedataBackendError&e){backend_error=e.result;}
    catch(const std::exception&e){runtime_.stop(std::string("Native savedata metadata preparation failed: ")+e.what());return false;}
    if(!owner_->try_acquire(UtilityDialogType::Savedata)){ctx.set_gpr(2u,invalid_status);return false;}
    owns_slot_=true;params_=params;id_list_=0u;mode_=request.mode;operation_=std::move(request);slot_request_=std::move(dialog);operation_error_=backend_error;opened_=false;
    game_name_.clear();save_pattern_.clear();status_=Status::Init;deadline_=now_us()+init_delay_us;
    // One line per request: the mode, the slot (or how many the list offers) and
    // the word at offset 56 as the game wrote it, which decides nothing here.
    std::cout<<"[savedata] request mode="<<mode_<<" ("<<mode_name(mode_)<<") ";
    if(operation_.interactive())std::cout<<"slots="<<operation_.slots.size();else std::cout<<"slot="<<operation_.slot;
    std::cout<<" word56="<<operation_.overwrite_word<<std::endl;
    ctx.set_gpr(2u,0u);return true;
}
void SavedataRuntime::commit_loaded(const SavedataLoadedProfile &loaded,const std::string&slot){
    auto&m=runtime_.memory();
    struct Output{std::uint32_t address;std::vector<std::uint8_t> bytes;};std::vector<Output> output;
    auto add=[&](std::uint32_t address,std::vector<std::uint8_t> bytes){
        if(bytes.empty())return;
        if(!m.contains(address,bytes.size()))throw SavedataBackendError(0x80110308u,"Invalid load output span");
        for(const auto&o:output)if(overlap(address,bytes.size(),o.address,o.bytes.size()))throw SavedataBackendError(0x80110308u,"Overlapping load outputs");
        output.push_back({address,std::move(bytes)});
    };
    auto word=[&](std::uint32_t address,std::uint32_t value){std::vector<std::uint8_t>b(4u);for(unsigned i=0;i<4u;++i)b[i]=static_cast<std::uint8_t>(value>>(8u*i));add(address,std::move(b));};
    auto string=[&](std::uint32_t address,const std::string&value,unsigned capacity){if(value.size()>=capacity||value.find('\0')!=std::string::npos)throw SavedataBackendError(0x80110306u,"Invalid load metadata length");std::vector<std::uint8_t>b(capacity);std::copy(value.begin(),value.end(),b.begin());add(address,std::move(b));};
    if(loaded.payload.size()>operation_.data.capacity)throw SavedataBackendError(0x80110308u,"Loaded profile exceeds guest capacity");
    add(operation_.data.address,loaded.payload);word(params_+124u,static_cast<std::uint32_t>(loaded.payload.size()));
    for(unsigned i=0;i<4u;++i){
        // A null/zero-capacity optional media buffer requests no media output.
        const auto&b=operation_.media[i];if(!b.capacity)continue;
        if(loaded.media[i].size()>b.capacity)throw SavedataBackendError(0x80110308u,"Loaded media exceeds guest capacity");
        add(b.address,loaded.media[i]);word(params_+1412u+16u*i+8u,static_cast<std::uint32_t>(loaded.media[i].size()));
    }
    string(params_+128u,loaded.title,128u);string(params_+256u,loaded.savedata_title,128u);string(params_+384u,loaded.detail,1024u);add(params_+1408u,{loaded.parental});
    if(operation_.interactive())string(params_+76u,slot,20u);word(params_+28u,0u);
    for(const auto&o:output)m.copy_in(o.address,o.bytes);
}
bool SavedataRuntime::operate(psprecomp::AllegrexContext &ctx){
    auto&m=runtime_.memory();
    if(!save_request::header_unchanged(m,operation_)){close_presenter();runtime_.stop("Native savedata request header changed while pending");return false;}
    // The argument is firmware UI animation speed, not an operation selector.
    // The original game supplies2. Win32 presents the native utility without
    // PSP fade animations; input and commit/lifecycle timing remain separate.
    if(operation_error_){
        std::cerr<<"[savedata] native operation failed result="<<psprecomp::hex32(operation_error_)<<": the slots could not be read"<<std::endl;
        m.store32(params_+28u,operation_error_);return true;
    }
    // Whether a save that is already in the slot may be replaced. An automatic
    // save (mode 1) replaces it without a question, as a PSP does: the game asks
    // for it after every mission and every change of the profile's options, to
    // the slot the profile was loaded from. A list save (mode 5) replaces it
    // when the player answered the dialog's question about that slot. The
    // request's word at offset 56 has no say in either (the game leaves it 0 in
    // its automatic save; read as a permission, it made every save after the
    // first fail with 0x80110385).
    std::string slot=operation_.slot;bool replace=operation_.is_save()&&!operation_.interactive();
    if(operation_.interactive()){
        std::optional<SaveSlotSelection> selection;
        try{if(!opened_){
            opened_=true;presenter_->open(slot_request_);
            std::cout<<"[savedata] native slot dialog opened mode="<<mode_<<" slots="<<slot_request_.slots.size()<<'\n';
        }selection=presenter_->poll(slot_request_.generation);}
        catch(const std::exception&e){close_presenter();runtime_.stop(std::string("Native savedata presenter failed: ")+e.what());return false;}
        if(!selection||selection->generation!=slot_request_.generation){ctx.set_gpr(2u,0u);return false;}
        if(selection->choice==SaveSlotChoice::Cancel){close_presenter();m.store32(params_+28u,1u);std::cout<<"[savedata] slot selection cancelled\n";return true;}
        if(selection->choice!=SaveSlotChoice::Choose||selection->index>=slot_request_.slots.size()){
            close_presenter();runtime_.stop("Native savedata presenter returned an invalid selection");return false;
        }
        const auto&selected=slot_request_.slots[selection->index];slot=selected.name;
        if(!operation_.is_save()&&!selected.exists){close_presenter();runtime_.stop(operation_.is_delete()?"Native savedata delete selected an empty slot":"Native savedata load selected an empty slot");return false;}
        if((operation_.is_save()||operation_.is_delete())&&selected.exists&&!selection->confirmed){
            // The dialog asks a separate question before a save is written over or
            // deleted. A choice of such a slot without its answer permits nothing.
            close_presenter();m.store32(params_+28u,result_cancelled);
            std::cout<<"[savedata] slot "<<slot<<" holds a save and the question about it was not answered: taken as cancelled"<<std::endl;
            return true;
        }
        // Only a save the player saw in the dialog and confirmed may be replaced.
        // A slot shown as empty that holds files by now is still refused.
        replace=operation_.is_save()&&selected.exists;
        close_presenter();
    }
    try{
        if(operation_.is_delete()){
            const auto kept=backend_->remove(operation_,slot);
            // A PSP's delete list comes back after each deletion and ends when the
            // player backs out of it, with the result of a cancelled dialog. The
            // game gets that result here after the one deletion this dialog makes.
            m.store32(params_+28u,result_cancelled);
            std::cout<<"[savedata] native profile deleted slot="<<slot<<": not erased, its folder was moved to "<<kept<<std::endl;
        }else if(operation_.is_save()){
            const auto report=backend_->save(operation_,slot,replace);
            if(operation_.interactive())for(unsigned i=0;i<20u;++i)m.store8(params_+76u+i,i<slot.size()?static_cast<std::uint8_t>(slot[i]):0u);
            m.store32(params_+28u,0u);
            std::cout<<"[savedata] native profile committed slot="<<slot;
            if(!report.replaced)std::cout<<" (the slot was empty)";
            else if(!report.previous.empty())std::cout<<" over the save that was there; the save before it is kept in "<<report.previous;
            else std::cout<<" over the save that was there; "<<(report.note.empty()?std::string("the save before it was not kept"):report.note);
            if(!report.note.empty()&&(!report.replaced||!report.previous.empty()))std::cout<<" ("<<report.note<<")";
            std::cout<<std::endl;
        }else{
            const auto loaded=backend_->load(operation_,slot);commit_loaded(loaded,slot);
            std::cout<<"[savedata] native profile loaded slot="<<slot<<" bytes="<<loaded.payload.size()<<'\n';
        }
    }catch(const SavedataBackendError&e){
        std::cerr<<"[savedata] native operation failed result="<<psprecomp::hex32(e.result)<<": "<<e.what()<<'\n';
        m.store32(params_+28u,e.result);
    }
    catch(const std::exception&e){runtime_.stop(std::string("Native savedata backend failed: ")+e.what());return false;}
    return true;
}

bool SavedataRuntime::query_sizes(psprecomp::AllegrexContext &ctx) {
    try {
        auto &memory=runtime_.memory();
        if (!guest_span(memory,params_,52u)) { ctx.set_gpr(2u,invalid_address); return false; }
        if (memory.load32(params_+48u)!=sizes_mode) {
            runtime_.stop("Native savedata SIZES request mode changed after Init"); return false;
        }
        const auto plan=savedata_size_prep::prepare(memory,params_,memory_stick_root_);
        plan.commit(memory);
        return true;
    } catch (const savedata_size_prep::QueryFailure &error) {
        if (error.kind==savedata_size_prep::Failure::InvalidAddress) ctx.set_gpr(2u,invalid_address);
        else if (error.kind==savedata_size_prep::Failure::InvalidParamSize) ctx.set_gpr(2u,invalid_param_size);
        else runtime_.stop(std::string("Native savedata SIZES query failed: ")+error.what());
        return false;
    } catch (const std::exception &error) {
        runtime_.stop(std::string("Native savedata SIZES query failed: ")+error.what());
        return false;
    }
}

bool SavedataRuntime::enumerate(psprecomp::AllegrexContext &ctx) {
    auto &memory = runtime_.memory();
    if (!guest_span(memory, params_, param_size) || !guest_span(memory, id_list_, 12u)) {
        ctx.set_gpr(2u, invalid_address); return false;
    }
    const auto max_count = memory.load32(id_list_), entries = memory.load32(id_list_ + 8u);
    const auto entry_bytes = static_cast<std::uint64_t>(max_count) * id_entry_size;
    if (max_count > 0x7FFFFFFFu || overlap(params_, param_size, id_list_, 12u) ||
        (max_count && (!guest_span(memory, entries, entry_bytes) ||
        overlap(params_, param_size, entries, entry_bytes) || overlap(id_list_, 12u, entries, entry_bytes)))) {
        ctx.set_gpr(2u, invalid_address); return false;
    }
    std::vector<Entry> output;
    try {
        const auto psp_root = memory_stick_root_ / "PSP", savedata_root = psp_root / "SAVEDATA";
        if (directory_or_missing(memory_stick_root_) && directory_or_missing(psp_root) &&
            directory_or_missing(savedata_root)) {
            struct Match { std::string name; std::filesystem::path path; };
            std::vector<Match> matches;
            const auto prefix = folded(game_name_);
            for (const auto &item : std::filesystem::directory_iterator(savedata_root)) {
                const auto name = item.path().filename().string();
                if (name.size() < prefix.size() || folded(name.substr(0u, prefix.size())) != prefix) continue;
                const auto suffix = name.substr(prefix.size());
                if (!wildcard(save_pattern_, suffix)) continue;
                const auto state = item.symlink_status();
                reject_reparse(item.path());
                if (!std::filesystem::is_directory(state)) continue;
                if (suffix.size() > 20u || !component_valid(suffix, false))
                    throw psprecomp::Error("Native save directory name cannot be represented by the PSP LIST ABI");
                matches.push_back({suffix, item.path()});
            }
            std::sort(matches.begin(), matches.end(), [](const auto &a, const auto &b) {
                const auto fa = folded(a.name), fb = folded(b.name);
                return fa == fb ? a.name < b.name : fa < fb;
            });
            const auto count = std::min<std::size_t>(matches.size(), max_count);
            output.reserve(count);
            for (std::size_t i = 0; i < count; ++i) output.push_back(native_entry(matches[i].path, matches[i].name));
        }
    } catch (const std::exception &error) {
        runtime_.stop(std::string("Native savedata LIST filesystem query failed: ") + error.what());
        return false;
    }
    // All guest spans and native filesystem/stat work completed before the first
    // guest write. No partial count, entry, common.result or secure-data changes.
    for (std::size_t i = 0; i < output.size(); ++i)
        memory.copy_in(entries + static_cast<std::uint32_t>(i * id_entry_size), output[i]);
    memory.store32(id_list_ + 4u, static_cast<std::uint32_t>(output.size()));
    memory.store32(params_ + result_offset, 0u);
    return true;
}
} // namespace chili
