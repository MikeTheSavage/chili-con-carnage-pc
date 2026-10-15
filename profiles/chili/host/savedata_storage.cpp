#include "savedata_storage.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <set>
#include <string_view>
#include <utility>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace chili::savedata_storage {
namespace {
namespace fs=std::filesystem;
std::atomic<std::uint64_t> sequence{};
[[noreturn]]void fail(ErrorCode code,const char *text){throw Error(code,text);}
[[noreturn]]void native(const char *text,DWORD code=GetLastError()){throw Error(ErrorCode::NativeIo,text,code);}
std::string fold(std::string value){for(auto &c:value)if(c>='a'&&c<='z')c=static_cast<char>(c-'a'+'A');return value;}
bool word(unsigned char c){return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-';}
bool device(const std::string &name){const auto base=fold(name.substr(0u,name.find('.')));return base=="CON"||base=="PRN"||base=="AUX"||base=="NUL"||
    (base.size()==4u && (base.starts_with("COM")||base.starts_with("LPT")) && base[3]>='1'&&base[3]<='9');}
void component(const std::string &name,std::size_t max,bool dot){
    if(name.empty()||name.size()>max||name.front()=='.'||name.back()=='.'||device(name))fail(ErrorCode::InvalidName,"Unsafe or unsupported savedata component");
    for(const auto c:name)if(!word(static_cast<unsigned char>(c)) && !(dot&&c=='.'))fail(ErrorCode::InvalidName,"Savedata component contains unsupported bytes");
}
std::wstring winpath(const fs::path &path){const auto s=path.native();return s.starts_with(L"\\\\?\\")?s:L"\\\\?\\"+s;}
DWORD attributes(const fs::path &path){
    const auto value=GetFileAttributesW(winpath(path).c_str());
    if(value==INVALID_FILE_ATTRIBUTES){const auto code=GetLastError();if(code==ERROR_FILE_NOT_FOUND||code==ERROR_PATH_NOT_FOUND)return value;native("Cannot inspect native savedata path",code);}
    if(value&FILE_ATTRIBUTE_REPARSE_POINT)fail(ErrorCode::UnsafePath,"Native savedata reparse/symlink path is forbidden");
    return value;
}
void verified(const fs::path &path){
    auto current=path.root_path();
    if(current.empty()||!path.is_absolute()||path.has_root_name()==false)fail(ErrorCode::InvalidRoot,"Native savedata mount is not an absolute local root");
    auto flags=attributes(current);if(flags==INVALID_FILE_ATTRIBUTES||!(flags&FILE_ATTRIBUTE_DIRECTORY))fail(ErrorCode::InvalidRoot,"Native savedata volume root is absent");
    for(const auto &part:path.relative_path()){
        if(part==L"."||part==L"..")fail(ErrorCode::UnsafePath,"Native savedata path contains traversal");
        current/=part;flags=attributes(current);if(flags==INVALID_FILE_ATTRIBUTES)fail(ErrorCode::NotFound,"Native savedata path is absent");
    }
}
void verified_directory(const fs::path &path){verified(path);if(!(attributes(path)&FILE_ATTRIBUTE_DIRECTORY))fail(ErrorCode::UnsafePath,"Native savedata path is not a directory");}
bool beneath(const fs::path &root,const fs::path &path){const auto rel=path.lexically_relative(root);return !rel.empty()&&!rel.has_root_path()&&*rel.begin()!=L"..";}
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit Handle(HANDLE h):value(h){if(value==INVALID_HANDLE_VALUE)native("Cannot open native savedata file");}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle &)=delete;Handle &operator=(const Handle &)=delete;
    void close(){const auto old=value;value=INVALID_HANDLE_VALUE;if(!CloseHandle(old))native("Cannot close native savedata file");}
};
FileInfo info(const fs::path &path,const std::string &name){
    verified(path);
    Handle h(CreateFileW(winpath(path).c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ, nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    FILE_ATTRIBUTE_TAG_INFO tag{};FILE_STANDARD_INFO standard{};FILE_BASIC_INFO basic{};
    if(!GetFileInformationByHandleEx(h.value,FileAttributeTagInfo,&tag,sizeof(tag))||
       !GetFileInformationByHandleEx(h.value,FileStandardInfo,&standard,sizeof(standard))||
       !GetFileInformationByHandleEx(h.value,FileBasicInfo,&basic,sizeof(basic)))native("Cannot query native savedata metadata");
    if(tag.FileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))fail(ErrorCode::UnsafePath,"Native savedata entry is not a plain file");
    if(standard.EndOfFile.QuadPart<0 || standard.AllocationSize.QuadPart<0)fail(ErrorCode::NativeIo,"Native savedata file has invalid size metadata");
    verified(path);
    return {name,static_cast<std::uint64_t>(standard.EndOfFile.QuadPart),static_cast<std::uint64_t>(standard.AllocationSize.QuadPart),
        static_cast<std::uint64_t>(basic.CreationTime.QuadPart),static_cast<std::uint64_t>(basic.LastAccessTime.QuadPart),static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart)};
}
std::optional<fs::path> child(const fs::path &parent,const std::string &name,Limits limits){
    verified_directory(parent);std::optional<fs::path> result;std::size_t count{};const auto wanted=fold(name);
    for(const auto &entry:fs::directory_iterator(parent)){
        if(++count>limits.directory_entries)fail(ErrorCode::LimitExceeded,"Native savedata directory enumeration limit");
        if(fold(entry.path().filename().string())!=wanted)continue;
        if(result)fail(ErrorCode::UnsafePath,"Native savedata case-folded path is ambiguous");
        verified(entry.path());result=entry.path();
    }
    verified_directory(parent);return result;
}
std::optional<fs::path> save_root(const fs::path &ms0,Limits limits){
    verified_directory(ms0);const auto psp=child(ms0,"PSP",limits);if(!psp)return {};
    verified_directory(*psp);const auto savedata=child(*psp,"SAVEDATA",limits);if(savedata)verified_directory(*savedata);return savedata;
}
void native_mkdir(const fs::path &path,std::vector<fs::path> *created=nullptr){verified_directory(path.parent_path());if(!CreateDirectoryW(winpath(path).c_str(),nullptr))native("Cannot create native savedata directory");if(created)created->push_back(path);verified_directory(path);}
fs::path ensure_root(const fs::path &ms0,Limits limits,std::vector<fs::path> &created){
    verified_directory(ms0);auto psp=child(ms0,"PSP",limits);if(!psp){psp=ms0/"PSP";native_mkdir(*psp,&created);}else verified_directory(*psp);
    auto root=child(*psp,"SAVEDATA",limits);if(!root){root=*psp/"SAVEDATA";native_mkdir(*root,&created);}else verified_directory(*root);return *root;
}
Metadata query_path(const fs::path &path,Limits limits){
    verified_directory(path);Metadata result{true,{},{}};std::set<std::string> names;
    for(const auto &entry:fs::directory_iterator(path)){
        if(result.files.size()>=limits.files)fail(ErrorCode::LimitExceeded,"Native savedata file-count limit");
        const auto name=entry.path().filename().string();Store::validate_filename(name);
        if(!names.emplace(fold(name)).second)fail(ErrorCode::UnsafePath,"Native savedata duplicate case-folded filename");
        auto item=info(entry.path(),name);
        if(item.bytes>limits.file_bytes || item.bytes>limits.slot_bytes-result.bytes)fail(ErrorCode::LimitExceeded,"Native savedata byte-count limit");
        result.bytes+=item.bytes;result.files.push_back(std::move(item));
    }
    verified_directory(path);std::sort(result.files.begin(),result.files.end(),[](const auto &a,const auto &b){return fold(a.name)<fold(b.name);});return result;
}
void write_file(const fs::path &path,std::span<const std::uint8_t> bytes){
    verified_directory(path.parent_path());Handle h(CreateFileW(winpath(path).c_str(),GENERIC_WRITE|FILE_READ_ATTRIBUTES,0u,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    std::size_t offset{};
    while(offset<bytes.size()){
        const auto requested=static_cast<DWORD>(std::min<std::size_t>(bytes.size()-offset,1024u*1024u));DWORD written{};
        if(!WriteFile(h.value,bytes.data()+offset,requested,&written,nullptr))native("Cannot write staged native savedata file");
        if(!written||written>requested)fail(ErrorCode::NativeIo,"Native savedata file write made invalid progress");offset+=written;
    }
    if(!FlushFileBuffers(h.value))native("Cannot flush staged native savedata file");h.close();verified(path);
}
void move_directory(const fs::path &from,const fs::path &to,bool *applied=nullptr){
    verified_directory(from);verified_directory(to.parent_path());
    if(attributes(to)!=INVALID_FILE_ATTRIBUTES)fail(ErrorCode::AlreadyExists,"Native savedata rename destination already exists");
    if(!MoveFileExW(winpath(from).c_str(),winpath(to).c_str(),MOVEFILE_WRITE_THROUGH))native("Cannot rename native savedata directory");
    if(applied)*applied=true;
    verified_directory(to);verified_directory(to.parent_path());
    if(attributes(from)!=INVALID_FILE_ATTRIBUTES)fail(ErrorCode::ConcurrentChange,"Native savedata rename source remains present");
}
std::set<std::string> names(const Metadata &metadata){std::set<std::string> result;for(const auto &file:metadata.files)result.emplace(fold(file.name));return result;}
void remove_flat(const fs::path &ms0,const fs::path &path,const std::set<std::string> &expected,bool exact,Limits limits){
    if(!beneath(ms0,path))fail(ErrorCode::UnsafePath,"Native savedata cleanup target escaped configured mount");
    if(attributes(path)==INVALID_FILE_ATTRIBUTES)return;
    const auto metadata=query_path(path,limits);const auto actual=names(metadata);
    if((exact&&actual!=expected)||!std::includes(expected.begin(),expected.end(),actual.begin(),actual.end()))fail(ErrorCode::ConcurrentChange,"Native savedata cleanup found unexpected files; recovery retained");
    for(const auto &file:metadata.files){const auto target=path/file.name;verified(target);if(!DeleteFileW(winpath(target).c_str()))native("Cannot remove owned native savedata file");verified_directory(path);}
    verified_directory(path);if(!RemoveDirectoryW(winpath(path).c_str()))native("Cannot remove owned native savedata directory");verified_directory(path.parent_path());
}
void remove_empty(const fs::path &ms0,const fs::path &path){
    if(!beneath(ms0,path))fail(ErrorCode::UnsafePath,"Native savedata empty cleanup target escaped configured mount");
    if(attributes(path)==INVALID_FILE_ATTRIBUTES)return;verified_directory(path);
    if(!RemoveDirectoryW(winpath(path).c_str()))native("Cannot remove empty native savedata directory");verified_directory(path.parent_path());
}
constexpr char transaction_prefix[]="._CHILI_TXN_";
// A flat directory of plain files below the mount, whatever their names: a kept
// generation, or staging that was never published.
void remove_plain(const fs::path &ms0,const fs::path &path,Limits limits){
    if(!beneath(ms0,path))fail(ErrorCode::UnsafePath,"Native savedata cleanup target escaped configured mount");
    if(attributes(path)==INVALID_FILE_ATTRIBUTES)return;
    const auto metadata=query_path(path,limits);
    for(const auto &file:metadata.files){const auto target=path/file.name;verified(target);if(!DeleteFileW(winpath(target).c_str()))native("Cannot remove native savedata file");verified_directory(path);}
    verified_directory(path);if(!RemoveDirectoryW(winpath(path).c_str()))native("Cannot remove native savedata directory");verified_directory(path.parent_path());
}
// Moves one complete generation (source) into the kept place of its slot,
// <PSP>/SAVEDATA_PREVIOUS/<name>. The generation kept there before is set aside
// first and removed only once the new one is in place; if the move fails it comes
// back, so a failure here never leaves the slot without an earlier generation
// that it had. Throws; then source is where it was.
fs::path keep_previous(const fs::path &ms0,const fs::path &save_root_path,const std::string &name,const fs::path &source,Limits limits){
    const auto psp=save_root_path.parent_path();verified_directory(psp);
    auto kept=child(psp,previous_directory_name,limits);
    if(!kept){kept=psp/previous_directory_name;native_mkdir(*kept);}else verified_directory(*kept);
    const auto destination=*kept/name,aside=*kept/("._CHILI_DISCARD_"+name);
    remove_plain(ms0,aside,limits); // left by a run that ended between the two steps below
    const auto before=child(*kept,name,limits);
    if(before){(void)query_path(*before,limits);move_directory(*before,aside);}
    try{move_directory(source,destination);}
    catch(...){if(before){try{move_directory(aside,destination);}catch(...){}}throw;}
    if(before){try{remove_plain(ms0,aside,limits);}catch(...){/* removed by the next keep */}}
    return destination;
}
struct TransactionName {bool valid{};DWORD process{};std::string slot;};
// "._CHILI_TXN_<process>_<sequence>_<clock>_<game+save>"; builds before the kept
// generation wrote no slot name.
TransactionName parse_transaction(const std::string &name){
    TransactionName result;std::string_view rest(name);
    if(!rest.starts_with(transaction_prefix))return result;rest.remove_prefix(sizeof(transaction_prefix)-1u);
    const auto number=[&](std::uint64_t &value){std::size_t digits{};value=0u;
        while(digits<rest.size()&&digits<19u&&rest[digits]>='0'&&rest[digits]<='9'){value=value*10u+static_cast<unsigned>(rest[digits]-'0');++digits;}
        rest.remove_prefix(digits);return digits!=0u;};
    const auto separator=[&]{if(rest.empty()||rest.front()!='_')return false;rest.remove_prefix(1u);return true;};
    std::uint64_t process{},sequence{},clock{};
    if(!number(process)||!separator()||!number(sequence)||!separator()||!number(clock)||process>0xFFFFFFFFull)return result;
    if(!rest.empty()){if(!separator()||rest.empty())return result;result.slot=std::string(rest);}
    result.valid=true;result.process=static_cast<DWORD>(process);return result;
}
// Whether the process that made a transaction directory may still be working in it.
bool owner_running(DWORD process,const fs::path &transaction){
    if(process==GetCurrentProcessId())return false; // this process sweeps between its own commits
    const HANDLE handle=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,process);
    if(!handle)return false; // ended, or not a process of this user
    bool running=WaitForSingleObject(handle,0u)==WAIT_TIMEOUT;
    if(running){
        // A process that started after the directory was made only has the same number.
        FILETIME created{},exited{},kernel{},user{};WIN32_FILE_ATTRIBUTE_DATA data{};
        if(GetProcessTimes(handle,&created,&exited,&kernel,&user)&&GetFileAttributesExW(winpath(transaction).c_str(),GetFileExInfoStandard,&data)&&
           CompareFileTime(&created,&data.ftCreationTime)>0)running=false;
    }
    CloseHandle(handle);return running;
}
Failure failure(const std::exception &e){if(const auto *typed=dynamic_cast<const Error *>(&e))return {typed->code(),typed->native_error(),typed->what()};if(const auto *filesystem=dynamic_cast<const fs::filesystem_error *>(&e))return {ErrorCode::NativeIo,static_cast<std::uint32_t>(filesystem->code().value()),filesystem->what()};return {ErrorCode::NativeIo,0u,e.what()};}
void hook(const FaultHook &callback,Phase phase){if(callback)callback(phase);}
}

Store::Store(fs::path ms0,Limits limits,std::shared_ptr<std::recursive_mutex> serialization):ms0_(std::move(ms0)),limits_(limits),serialization_(std::move(serialization)){
    if(!ms0_.is_absolute()||ms0_.native().starts_with(L"\\\\"))fail(ErrorCode::InvalidRoot,"Savedata mount must be an existing absolute local path without traversal");
    for(const auto &part:ms0_.relative_path())if(part==L"..")fail(ErrorCode::InvalidRoot,"Savedata mount contains traversal");
    ms0_=ms0_.lexically_normal();verified_directory(ms0_);
    if(!limits_.files||!limits_.directory_entries||!limits_.file_bytes||!limits_.slot_bytes)fail(ErrorCode::LimitExceeded,"Savedata native resource limits must be positive");
    if(!serialization_)serialization_=std::make_shared<std::recursive_mutex>();
}
std::unique_lock<std::recursive_mutex> Store::guard()const{return std::unique_lock<std::recursive_mutex>(*serialization_);}
void Store::validate_slot(const Slot &slot){component(slot.game,12u,false);component(slot.save,19u,false);}
void Store::validate_filename(const std::string &name){component(name,12u,true);}
Metadata Store::query(const Slot &slot)const{
    const auto lock=guard();validate_slot(slot);const auto root=save_root(ms0_,limits_);if(!root)return {};
    const auto target=child(*root,slot.game+slot.save,limits_);if(!target)return {};return query_path(*target,limits_);
}
Bytes Store::read(const Slot &slot,const std::string &name,std::uint64_t caller_limit)const{
    const auto lock=guard();validate_slot(slot);validate_filename(name);const auto root=save_root(ms0_,limits_);if(!root)fail(ErrorCode::NotFound,"Savedata root is absent");
    const auto target=child(*root,slot.game+slot.save,limits_);if(!target)fail(ErrorCode::NotFound,"Savedata slot is absent");verified_directory(*target);
    const auto file=child(*target,name,limits_);if(!file)fail(ErrorCode::NotFound,"Savedata file is absent");
    const auto metadata=info(*file,file->filename().string());
    if(metadata.bytes>caller_limit||metadata.bytes>limits_.file_bytes||metadata.bytes>std::numeric_limits<std::size_t>::max())fail(ErrorCode::LimitExceeded,"Savedata read exceeds caller/native bounds");
    Handle h(CreateFileW(winpath(*file).c_str(),GENERIC_READ|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    FILE_STANDARD_INFO standard{};FILE_ATTRIBUTE_TAG_INFO tag{};
    if(!GetFileInformationByHandleEx(h.value,FileStandardInfo,&standard,sizeof(standard))||!GetFileInformationByHandleEx(h.value,FileAttributeTagInfo,&tag,sizeof(tag)))native("Cannot verify native savedata read handle");
    if((tag.FileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||standard.EndOfFile.QuadPart<0||static_cast<std::uint64_t>(standard.EndOfFile.QuadPart)!=metadata.bytes)fail(ErrorCode::ConcurrentChange,"Savedata file changed before bounded read");
    Bytes bytes(static_cast<std::size_t>(metadata.bytes));std::size_t offset{};
    while(offset<bytes.size()){
        const auto requested=static_cast<DWORD>(std::min<std::size_t>(bytes.size()-offset,1024u*1024u));DWORD read{};
        if(!ReadFile(h.value,bytes.data()+offset,requested,&read,nullptr))native("Cannot read native savedata file");
        if(!read||read>requested)fail(ErrorCode::ConcurrentChange,"Savedata bounded read ended unexpectedly");offset+=read;
    }
    verified(*file);return bytes;
}
CommitResult Store::commit(const Slot &slot,std::span<const File> files,Overwrite overwrite,FaultHook fault){
    const auto lock=guard();validate_slot(slot);
    if(files.empty()||files.size()>limits_.files)fail(ErrorCode::LimitExceeded,"Savedata commit file-count limit");
    std::set<std::string> supplied;std::uint64_t bytes{};
    for(const auto &file:files){validate_filename(file.name);if(!supplied.emplace(fold(file.name)).second)fail(ErrorCode::InvalidName,"Savedata commit duplicate case-folded filename");
        if(file.bytes.size()>limits_.file_bytes||file.bytes.size()>limits_.slot_bytes-bytes)fail(ErrorCode::LimitExceeded,"Savedata commit byte-count limit");bytes+=file.bytes.size();}
    if(overwrite!=Overwrite::Reject&&overwrite!=Overwrite::ReplaceWithRollback)fail(ErrorCode::InvalidName,"Savedata overwrite policy is unknown");
    verified_directory(ms0_);
    CommitResult result;std::vector<fs::path> created;fs::path root,target,transaction,staged,backup;
    bool moved_old{},published{},cleanup_started{},owned_transaction{};
    auto recovery=[&]{result.recovery_directory=owned_transaction?transaction:created.empty()?fs::path{}:created.back();result.backup_directory=moved_old?backup:fs::path{};result.staged_directory=staged;};
    try{
        root=ensure_root(ms0_,limits_,created);const auto old=child(root,slot.game+slot.save,limits_);target=old?*old:root/(slot.game+slot.save);
        // Only a flat folder of plain files is moved aside as the old generation.
        if(old){if(overwrite==Overwrite::Reject)fail(ErrorCode::AlreadyExists,"Savedata slot already exists and overwrite was rejected");(void)query_path(*old,limits_);}
        for(unsigned attempt=0u;attempt<32u;++attempt){
            const auto id=std::to_string(GetCurrentProcessId())+"_"+std::to_string(++sequence)+"_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            transaction=root/(transaction_prefix+id+"_"+slot.game+slot.save);verified_directory(root);
            if(CreateDirectoryW(winpath(transaction).c_str(),nullptr)){owned_transaction=true;break;}
            const auto error=GetLastError();if(error!=ERROR_ALREADY_EXISTS)native("Cannot create exclusive savedata staging directory",error);transaction.clear();
        }
        if(transaction.empty())fail(ErrorCode::NativeIo,"Cannot allocate unique native savedata staging directory");
        verified_directory(transaction);staged=transaction/"new";backup=transaction/"old";native_mkdir(staged);hook(fault,Phase::StageCreated);
        for(const auto &file:files){write_file(staged/file.name,file.bytes);hook(fault,Phase::FileStaged);}
        if(names(query_path(staged,limits_))!=supplied)fail(ErrorCode::ConcurrentChange,"Savedata staging file set changed");hook(fault,Phase::StageComplete);
        if(old){move_directory(target,backup,&moved_old);hook(fault,Phase::OldMoved);}
        move_directory(staged,target,&published);hook(fault,Phase::Published);hook(fault,Phase::BeforeCleanup);
        if(names(query_path(target,limits_))!=supplied)fail(ErrorCode::ConcurrentChange,"Published savedata file set changed before backup cleanup");
        cleanup_started=true;
        if(moved_old){
            // The new generation is published: nothing below can fail the save.
            // The old one goes to the kept place. If that fails it stays whole in
            // the transaction directory and sweep() takes it up at the next
            // operation: kept if it can be by then, removed otherwise. Its files
            // are never deleted from here, so a directory named "old" is always a
            // complete generation.
            result.replaced=true;
            try{hook(fault,Phase::BeforeKeep);result.previous_directory=keep_previous(ms0_,root,slot.game+slot.save,backup,limits_);moved_old=false;}
            catch(const std::exception &error){result.previous_failure=failure(error);result.previous_directory.clear();throw;}
        }
        remove_empty(ms0_,transaction);verified_directory(target);result.outcome=Outcome::Committed;return result;
    }catch(const std::exception &error){
        result.failure=failure(error);
        if(cleanup_started){result.outcome=Outcome::CommittedCleanupRequired;recovery();return result;}
        try{
            if(published)remove_flat(ms0_,target,supplied,true,limits_);
            if(moved_old){hook(fault,Phase::BeforeRestore);move_directory(backup,target);moved_old=false;}
            if(!staged.empty())remove_flat(ms0_,staged,supplied,false,limits_);
            if(owned_transaction)remove_empty(ms0_,transaction);
            for(auto i=created.rbegin();i!=created.rend();++i)remove_empty(ms0_,*i);
            verified_directory(ms0_);result.outcome=Outcome::FailedUnchanged;
        }catch(const std::exception &rollback_error){result.rollback_failure=failure(rollback_error);result.outcome=Outcome::RecoveryRequired;recovery();}
        return result;
    }
}
fs::path Store::previous_root()const{return ms0_/"PSP"/previous_directory_name;}
fs::path Store::retire(const Slot &slot){
    const auto lock=guard();validate_slot(slot);verified_directory(ms0_);
    const auto root=save_root(ms0_,limits_);if(!root)fail(ErrorCode::NotFound,"Savedata root is absent");
    const auto target=child(*root,slot.game+slot.save,limits_);if(!target)fail(ErrorCode::NotFound,"Savedata slot is absent");
    (void)query_path(*target,limits_); // only a flat folder of plain files is a slot this store moves
    return keep_previous(ms0_,*root,slot.game+slot.save,*target,limits_);
}
std::vector<SweepReport> Store::sweep(){
    std::vector<SweepReport> reports;
    try{
        const auto lock=guard();const auto root=save_root(ms0_,limits_);if(!root)return reports;
        std::vector<fs::path> found;std::size_t entries{};
        for(const auto &entry:fs::directory_iterator(*root)){
            if(++entries>limits_.directory_entries)break;
            if(entry.path().filename().native().starts_with(L"._CHILI_TXN_"))found.push_back(entry.path());
        }
        for(const auto &transaction:found){
            SweepReport report{transaction,{},SweepAction::LeftAlone,{},{}};bool acted{};
            try{
                const auto parsed=parse_transaction(transaction.filename().string());
                if(!parsed.valid)fail(ErrorCode::UnsafePath,"Savedata transaction directory name is not one this version writes");
                if(owner_running(parsed.process,transaction))continue;
                report.slot_directory=parsed.slot;verified_directory(transaction);
                bool has_old{},has_new{},has_discard{};
                for(const auto &entry:fs::directory_iterator(transaction)){
                    const auto name=entry.path().filename().native();
                    if(name==L"old")has_old=true;else if(name==L"new")has_new=true;else if(name==L"discard")has_discard=true;
                    else fail(ErrorCode::UnsafePath,"Savedata transaction directory holds something this version did not put there");
                }
                // "old" is a whole generation. One that is given up is renamed to
                // "discard" before the first of its files is deleted, so a deletion
                // that stops half way never leaves a partial "old" behind.
                const auto old=transaction/"old",staged=transaction/"new",discard=transaction/"discard";
                if(has_discard){remove_plain(ms0_,discard,limits_);has_discard=false;}
                if(has_old){
                    if(parsed.slot.empty())fail(ErrorCode::UnsafePath,"Savedata transaction of an earlier version holds a previous generation of an unnamed slot");
                    component(parsed.slot,31u,false);(void)query_path(old,limits_);
                    if(!child(*root,parsed.slot,limits_)){
                        // The run ended between moving the old generation away and publishing the new one.
                        move_directory(old,*root/parsed.slot);report.action=SweepAction::RestoredSlot;report.destination=*root/parsed.slot;
                    }else{
                        try{report.destination=keep_previous(ms0_,*root,parsed.slot,old,limits_);report.action=SweepAction::KeptPrevious;}
                        catch(const std::exception &error){
                            const auto why=failure(error);move_directory(old,discard);has_discard=true;
                            report.failure=why;report.action=SweepAction::RemovedPrevious;
                        }
                    }
                    acted=true;
                }
                if(has_discard)remove_plain(ms0_,discard,limits_);
                if(has_new)remove_plain(ms0_,staged,limits_);
                remove_empty(ms0_,transaction);
                if(!acted)report.action=SweepAction::Removed;
            }catch(const std::exception &error){if(!acted)report.action=SweepAction::LeftAlone;report.failure=failure(error);}
            reports.push_back(std::move(report));
        }
    }catch(const std::exception &error){reports.push_back({{},{},SweepAction::LeftAlone,{},failure(error)});}
    return reports;
}
}
