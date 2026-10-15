#include "savedata_storage.hpp"
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

namespace {
using namespace chili::savedata_storage;
namespace fs=std::filesystem;
const Slot slot{"ULUS10216","DATA00"};
void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
template<class F>void rejects(ErrorCode code,F action){try{action();}catch(const Error &e){require(e.code()==code,"Wrong storage error classification");return;}throw std::runtime_error("Unsafe storage request silently succeeded");}
struct FixtureRoot {
    fs::path workspace,path;
    explicit FixtureRoot(const fs::path &work):workspace(fs::absolute(work).lexically_normal()){
        require(workspace.filename()=="chili-savedata-storage-prep" && fs::is_directory(workspace),"Fixture root is not the owned allowed workspace directory");
        const auto id=std::to_string(GetCurrentProcessId())+"_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        path=workspace/"fixture_roots"/id;fs::create_directories(path);
    }
    ~FixtureRoot(){const auto relative=path.lexically_relative(workspace);if(relative.empty()||relative.has_root_path()||*relative.begin()=="..")std::terminate();std::error_code error;fs::remove_all(path,error);}
    fs::path ms0(const std::string &name){const auto result=path/name;require(fs::create_directory(result),"Cannot create unique fixture ms0 root");return result;}
};
std::vector<File> files(unsigned seed=1u){
    std::vector<File> result{{"PARAM.SFO",{0u,0x50u,0x53u,0x46u,0xFFu}},{"SECURE.BIN",Bytes(1088u)},{"ICON0.PNG",Bytes(20905u)},
        {"PIC1.PNG",Bytes(112530u)},{"SND0.AT3",Bytes(251708u)}};
    for(std::size_t f=1u;f<result.size();++f)for(std::size_t i=0u;i<result[f].bytes.size();++i)result[f].bytes[i]=static_cast<std::uint8_t>(i*13u+f*17u+seed);
    return result;
}
void equal_files(Store &store,const Slot &s,const std::vector<File> &expected){
    const auto metadata=store.query(s);require(metadata.exists&&metadata.files.size()==expected.size(),"Committed native file set mismatch");std::uint64_t total{};
    for(const auto &file:expected){const auto found=std::find_if(metadata.files.begin(),metadata.files.end(),[&](const auto &info){return info.name==file.name;});
        require(found!=metadata.files.end()&&found->bytes==file.bytes.size()&&found->created_filetime&&found->written_filetime,"Native size/timestamp metadata mismatch");
        require(store.read(s,file.name,file.bytes.size())==file.bytes,"Opaque native file bytes changed");total+=file.bytes.size();}
    require(metadata.bytes==total,"Native aggregate metadata mismatch");
}
fs::path kept(const fs::path &ms0,const Slot &s=slot){return ms0/"PSP"/"SAVEDATA_PREVIOUS"/(s.game+s.save);}
// A folder outside the store's slot list, compared file by file with what a save held.
void equal_folder(const fs::path &folder,const std::vector<File> &expected){
    std::size_t count{};for(const auto &entry:fs::directory_iterator(folder)){require(entry.is_regular_file(),"A kept generation holds something that is not a file");++count;}
    require(count==expected.size(),"A kept generation has another number of files than the save it was");
    for(const auto &file:expected){std::ifstream in(folder/file.name,std::ios::binary);require(bool(in),"A file of the kept generation is missing");
        const Bytes bytes{std::istreambuf_iterator<char>{in},std::istreambuf_iterator<char>{}};require(bytes==file.bytes,"The kept generation differs from the save that was replaced");}
}
std::size_t entries(const fs::path &folder){std::size_t count{};if(fs::exists(folder))for(const auto &entry:fs::directory_iterator(folder)){(void)entry;++count;}return count;}
bool no_transactions(const fs::path &ms0){const auto root=ms0/"PSP"/"SAVEDATA";if(!fs::exists(root))return true;for(const auto &entry:fs::directory_iterator(root))if(entry.path().filename().string().starts_with("._CHILI_TXN_"))return false;return true;}
void inject(){throw Error(ErrorCode::InjectedFailure,"Deterministic fixture operation failure");}
void new_and_replace(FixtureRoot &roots){
    const auto mount=roots.ms0("new_and_replace");Store store(mount);require(fs::is_empty(mount)&&!store.query(slot).exists&&fs::is_empty(mount),"Construction/query fabricated a save directory");
    const auto original=files();require(store.commit(slot,original).outcome==Outcome::Committed,"New slot publication failed");equal_files(store,slot,original);require(no_transactions(mount),"Successful publication leaked transaction directory");
    const auto modified=files(2u);const auto denied=store.commit(slot,modified);require(denied.outcome==Outcome::FailedUnchanged&&denied.failure&&denied.failure->code==ErrorCode::AlreadyExists,"Implicit overwrite did not reject existing save");equal_files(store,slot,original);
    require(store.commit(slot,modified,Overwrite::ReplaceWithRollback).outcome==Outcome::Committed,"Explicit native replacement failed");equal_files(store,slot,modified);require(no_transactions(mount),"Replacement leaked old transaction");
    require(store.read({"ulus10216","data00"},"secure.bin",1088u)==modified[1].bytes,"FAT-style case lookup failed");
    rejects(ErrorCode::LimitExceeded,[&]{store.read(slot,"SECURE.BIN",1087u);});rejects(ErrorCode::NotFound,[&]{store.read(slot,"ABSENT.BIN",100u);});
    std::vector<File> fewer{{"EMPTY.BIN",{}},{"LARGE.BIN",Bytes(2u*1024u*1024u+7u,0xA5u)}};
    require(store.commit(slot,fewer,Overwrite::ReplaceWithRollback).outcome==Outcome::Committed,"Complete file-set replacement failed");equal_files(store,slot,fewer);
    require(store.read(slot,"EMPTY.BIN",0u).empty()&&!fs::exists(mount/"PSP"/"SAVEDATA"/"ULUS10216DATA00"/"SECURE.BIN"),"Empty file/removed auxiliary semantics wrong");
    std::cout<<"Native opaque storage: five exact supplied files, real metadata/bounded reads, new-slot publication, explicit complete replacement, empty/2MiB files PASS\n";
}
void rollback_failures(FixtureRoot &roots){
    for(const auto phase:{Phase::StageCreated,Phase::FileStaged,Phase::StageComplete,Phase::Published,Phase::BeforeCleanup}){
        const auto mount=roots.ms0("new_failure_"+std::to_string(static_cast<unsigned>(phase)));Store store(mount);
        const auto result=store.commit(slot,files(),Overwrite::Reject,[&](Phase p){if(p==phase)inject();});
        require(result.outcome==Outcome::FailedUnchanged&&result.failure&&result.failure->code==ErrorCode::InjectedFailure&&!result.rollback_failure&&fs::is_empty(mount)&&!store.query(slot).exists,"Failed new-slot operation did not cleanly restore original empty mount");
    }
    const auto mount=roots.ms0("replacement_failures");Store store(mount);const auto original=files();require(store.commit(slot,original).outcome==Outcome::Committed,"Replacement failure setup failed");
    for(const auto phase:{Phase::StageCreated,Phase::FileStaged,Phase::StageComplete,Phase::OldMoved,Phase::Published,Phase::BeforeCleanup}){
        const auto result=store.commit(slot,files(9u),Overwrite::ReplaceWithRollback,[&](Phase p){if(p==phase)inject();});
        require(result.outcome==Outcome::FailedUnchanged&&result.failure&&result.failure->code==ErrorCode::InjectedFailure&&!result.rollback_failure&&no_transactions(mount),"Replacement failure did not remove staging/restore old directory");equal_files(store,slot,original);
    }
    std::cout<<"Handled failure rollback: staged partial set, before/after publication, old-directory move, cleanup boundary; old bytes/empty mount retained PASS\n";
}
// The generation a replacement removes from the slot is kept in
// PSP/SAVEDATA_PREVIOUS/<game+save>, one per slot, replaced at every save.
void kept_generation(FixtureRoot &roots){
    const auto mount=roots.ms0("kept_generation");Store store(mount);const auto root=mount/"PSP"/"SAVEDATA";
    require(store.previous_root()==mount/"PSP"/"SAVEDATA_PREVIOUS","The kept place is not beside SAVEDATA");
    const auto first=store.commit(slot,files(1u),Overwrite::ReplaceWithRollback);
    require(first.outcome==Outcome::Committed&&!first.replaced&&first.previous_directory.empty()&&!first.previous_failure&&!fs::exists(store.previous_root()),"A save into an empty slot reported or kept an earlier generation");
    const auto second=store.commit(slot,files(2u),Overwrite::ReplaceWithRollback);
    require(second.outcome==Outcome::Committed&&second.replaced&&second.previous_directory==kept(mount)&&!second.previous_failure,"A replacement did not report where the old generation is kept");
    equal_folder(kept(mount),files(1u));equal_files(store,slot,files(2u));
    // A hundred saves in a row: each one commits, keeps the one before it, and leaves nothing else.
    for(unsigned i=3u;i<=102u;++i){const auto result=store.commit(slot,files(i),Overwrite::ReplaceWithRollback);
        require(result.outcome==Outcome::Committed&&result.replaced&&result.previous_directory==kept(mount),"One of a hundred replacements in a row failed or did not keep the generation before it");}
    equal_files(store,slot,files(102u));equal_folder(kept(mount),files(101u));
    require(no_transactions(mount)&&entries(root)==1u&&entries(store.previous_root())==1u&&entries(mount/"PSP")==2u&&store.sweep().empty(),"A hundred replacements left something behind");
    // The old generation cannot be kept at the moment of the commit: the save is
    // committed, the old generation waits whole in the transaction directory, the
    // one kept before is untouched, and the next sweep keeps the waiting one.
    const auto failed=store.commit(slot,files(103u),Overwrite::ReplaceWithRollback,[](Phase p){if(p==Phase::BeforeKeep)inject();});
    require(failed.outcome==Outcome::CommittedCleanupRequired&&failed.replaced&&failed.previous_directory.empty()&&failed.previous_failure&&
        failed.previous_failure->code==ErrorCode::InjectedFailure&&fs::is_directory(failed.backup_directory),"A failed keep failed the save or was not reported");
    equal_files(store,slot,files(103u));equal_folder(kept(mount),files(101u));equal_folder(failed.backup_directory,files(102u));
    auto reports=store.sweep();
    require(reports.size()==1u&&reports[0].action==SweepAction::KeptPrevious&&reports[0].destination==kept(mount)&&reports[0].slot_directory=="ULUS10216DATA00"&&!reports[0].failure&&no_transactions(mount),"The waiting generation was not kept by the next sweep");
    equal_folder(kept(mount),files(102u));require(store.sweep().empty(),"A second sweep found something to do");
    // The kept place cannot be used (something in it that the store did not put
    // there): the save is committed, the sweep gives the old generation up, the
    // kept folder is left as it is, nothing is left in SAVEDATA.
    fs::create_directory(kept(mount)/"foreign");
    for(unsigned i=104u;i<=106u;++i){
        const auto blocked=store.commit(slot,files(i),Overwrite::ReplaceWithRollback);
        require(blocked.outcome==Outcome::CommittedCleanupRequired&&blocked.replaced&&blocked.previous_failure,"A save with an unusable kept place failed or did not say so");equal_files(store,slot,files(i));
        reports=store.sweep();
        require(reports.size()==1u&&reports[0].action==SweepAction::RemovedPrevious&&reports[0].failure&&no_transactions(mount)&&entries(root)==1u&&fs::is_directory(kept(mount)/"foreign"),"An old generation that cannot be kept was not given up cleanly");
    }
    require(fs::remove(kept(mount)/"foreign"),"Cannot remove the fixture's foreign folder");equal_folder(kept(mount),files(102u));
    // A run that ended between moving the old generation away and publishing the
    // new one: the slot is missing, the old generation and complete staging are in
    // the transaction directory. The sweep puts the old generation back.
    const auto stuck=store.commit(slot,files(107u),Overwrite::ReplaceWithRollback,[](Phase p){if(p==Phase::OldMoved||p==Phase::BeforeRestore)inject();});
    require(stuck.outcome==Outcome::RecoveryRequired&&!store.query(slot).exists,"Interrupted replacement setup failed");
    reports=store.sweep();
    require(reports.size()==1u&&reports[0].action==SweepAction::RestoredSlot&&reports[0].destination==root/"ULUS10216DATA00"&&no_transactions(mount)&&entries(root)==1u,"An interrupted replacement was not put back");
    equal_files(store,slot,files(106u));equal_folder(kept(mount),files(102u));
    // Staging of a save that was never published is removed; so is a half-deleted "discard".
    const Slot other{"ULUS10216","DATA01"};
    const auto half=root/"._CHILI_TXN_1_2_3_ULUS10216DATA01";fs::create_directories(half/"new");fs::create_directories(half/"discard");
    {std::ofstream a(half/"new"/"PARAM.SFO",std::ios::binary);a<<"half";std::ofstream b(half/"discard"/"SND0.AT3",std::ios::binary);b<<"rest";}
    const auto old_format=root/"._CHILI_TXN_1_2_6";fs::create_directories(old_format/"new");
    reports=store.sweep();
    require(reports.size()==2u&&reports[0].action==SweepAction::Removed&&reports[1].action==SweepAction::Removed&&no_transactions(mount)&&!store.query(other).exists,"Unpublished staging was not removed");
    // What the sweep cannot make sense of stays as it is and is reported.
    const auto unknown=root/"._CHILI_TXN_not_a_transaction",foreign=root/"._CHILI_TXN_1_2_4_ULUS10216DATA01",nameless=root/"._CHILI_TXN_1_2_5";
    fs::create_directories(unknown/"old");fs::create_directories(foreign/"mine");fs::create_directories(nameless/"old");
    reports=store.sweep();
    require(reports.size()==3u&&fs::is_directory(unknown/"old")&&fs::is_directory(foreign/"mine")&&fs::is_directory(nameless/"old"),"The sweep touched a directory it does not know");
    for(const auto &report:reports)require(report.action==SweepAction::LeftAlone&&report.failure,"A directory the sweep does not know was not reported as left alone");
    // Saves go on beside them.
    require(store.commit(slot,files(108u),Overwrite::ReplaceWithRollback).outcome==Outcome::Committed,"A save beside unknown transaction directories failed");equal_folder(kept(mount),files(106u));
    std::error_code ignored;fs::remove_all(unknown,ignored);fs::remove_all(foreign,ignored);fs::remove_all(nameless,ignored);require(no_transactions(mount),"Fixture cleanup failed");
    // A transaction of another process that is still running is not touched;
    // once that process has ended it is. A directory older than the process that
    // has its number belongs to a process that is gone.
    const auto start=[](PROCESS_INFORMATION &child){STARTUPINFOW startup{};startup.cb=sizeof(startup);wchar_t command[]=L"ping.exe -n 60 127.0.0.1";
        require(CreateProcessW(nullptr,command,nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child)!=FALSE,"Cannot start the fixture's other process");};
    const auto stop=[](PROCESS_INFORMATION &child){TerminateProcess(child.hProcess,0u);WaitForSingleObject(child.hProcess,10000u);CloseHandle(child.hThread);CloseHandle(child.hProcess);};
    PROCESS_INFORMATION child{};start(child);
    const auto live=root/("._CHILI_TXN_"+std::to_string(child.dwProcessId)+"_1_1_ULUS10216DATA01");fs::create_directories(live/"new");
    reports=store.sweep();const bool untouched=reports.empty()&&fs::is_directory(live/"new");stop(child);
    require(untouched,"The sweep touched the transaction of a process that is still running");
    reports=store.sweep();require(reports.size()==1u&&reports[0].action==SweepAction::Removed&&no_transactions(mount),"The transaction of a process that has ended was not removed");
    const auto early=root/"early";fs::create_directories(early/"new");Sleep(100u);PROCESS_INFORMATION later{};start(later);
    const auto reused=root/("._CHILI_TXN_"+std::to_string(later.dwProcessId)+"_1_1_ULUS10216DATA01");fs::rename(early,reused);
    reports=store.sweep();const bool removed=reports.size()==1u&&reports[0].action==SweepAction::Removed&&!fs::exists(reused);stop(later);
    require(removed,"A transaction older than the process that has its number was taken for that process's");
    // retire: the slot leaves SAVEDATA and its folder becomes the kept generation.
    const auto gone=store.retire(slot);
    require(gone==kept(mount)&&!store.query(slot).exists&&entries(root)==0u,"A retired slot is still in SAVEDATA");equal_folder(kept(mount),files(108u));
    rejects(ErrorCode::NotFound,[&]{store.retire(slot);});rejects(ErrorCode::InvalidName,[&]{store.retire({"ULUS10216","../X"});});
    require(store.commit(slot,files(109u)).outcome==Outcome::Committed,"A save into a retired slot failed");equal_folder(kept(mount),files(108u));
    require(store.retire(slot)==kept(mount),"A second retire failed");equal_folder(kept(mount),files(109u));
    std::cout<<"Kept generation: replaced save kept beside SAVEDATA, a hundred replacements in a row, keep failure never fails the save, sweep keeps/gives up/puts back/removes/leaves alone, running-process transactions untouched, retire PASS\n";
}
void twenty_slots(FixtureRoot &roots){
    const auto mount=roots.ms0("twenty_empty_slots");Store store(mount);const std::vector<File> opaque{{"PARAM.SFO",{0u,0x50u,0x53u,0x46u}},{"SECURE.BIN",{0u,0xFFu,0x01u,0xA5u}}};
    for(unsigned i=0u;i<20u;++i){const Slot selected{"ULUS10216","DATA"+std::string(i<10u?"0":"")+std::to_string(i)};
        require(!store.query(selected).exists&&store.commit(selected,opaque).outcome==Outcome::Committed,"Actual twenty-slot namespace new publication failed");equal_files(store,selected,opaque);}
    std::size_t count{};for(const auto &entry:fs::directory_iterator(mount/"PSP"/"SAVEDATA")){require(entry.is_directory(),"Unexpected file in twenty-slot root");++count;}
    require(count==20u&&no_transactions(mount),"Twenty-slot publication leaked or mixed slot files");
    std::cout<<"Actual DATA00..DATA19 namespace: twenty explicitly supplied empty-slot commits and isolated bounded reads PASS\n";
}
void recovery_status(FixtureRoot &roots){
    const auto mount=roots.ms0("restore_failure");Store store(mount);require(store.commit(slot,files()).outcome==Outcome::Committed,"Restore failure setup failed");
    const auto result=store.commit(slot,files(4u),Overwrite::ReplaceWithRollback,[](Phase p){if(p==Phase::OldMoved||p==Phase::BeforeRestore)inject();});
    require(result.outcome==Outcome::RecoveryRequired&&result.failure&&result.rollback_failure&&!result.recovery_directory.empty()&&
        fs::is_directory(result.recovery_directory)&&fs::is_directory(result.backup_directory)&&fs::is_directory(result.staged_directory)&&!store.query(slot).exists,"Failed restoration did not preserve/report actual backup and stage");
    require(fs::file_size(result.backup_directory/"SECURE.BIN")==1088u,"Recovery backup payload was lost");
    const auto cleanup_mount=roots.ms0("cleanup_failure");Store cleanup(cleanup_mount);const auto original=files();require(cleanup.commit(slot,original).outcome==Outcome::Committed,"Cleanup failure setup failed");
    HANDLE pin=INVALID_HANDLE_VALUE;
    const auto after=cleanup.commit(slot,files(5u),Overwrite::ReplaceWithRollback,[&](Phase p){if(p==Phase::BeforeCleanup){
        const auto root=cleanup_mount/"PSP"/"SAVEDATA";for(const auto &entry:fs::directory_iterator(root))if(entry.path().filename().string().starts_with("._CHILI_TXN_")){
            pin=CreateFileW((entry.path()/"old"/"SECURE.BIN").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0u,nullptr);require(pin!=INVALID_HANDLE_VALUE,"Cannot pin actual backup file to force native cleanup error");}
    }});
    require(pin!=INVALID_HANDLE_VALUE,"Backup cleanup injection did not open native file");CloseHandle(pin);
    require(after.outcome==Outcome::CommittedCleanupRequired&&after.failure&&after.failure->code==ErrorCode::NativeIo&&!after.recovery_directory.empty()&&fs::exists(after.backup_directory/"SECURE.BIN"),"Native postcommit cleanup failure was hidden or rolled back with partial backup");equal_files(cleanup,slot,files(5u));
    // The generation that could not be moved to the kept place while one of its
    // files was held open waits whole (none of its files is deleted) and is kept
    // by the next sweep.
    require(after.replaced&&after.previous_failure&&after.previous_directory.empty(),"A replacement that could not keep the old generation did not say so");equal_folder(after.backup_directory,original);
    const auto swept=cleanup.sweep();
    require(swept.size()==1u&&swept[0].action==SweepAction::KeptPrevious&&swept[0].destination==kept(cleanup_mount)&&!swept[0].failure&&no_transactions(cleanup_mount),"The waiting old generation was not kept by the next sweep");
    equal_folder(kept(cleanup_mount),original);equal_files(cleanup,slot,files(5u));
    std::cout<<"Recovery status: failed restore preserves old/staged data; real denied-delete backup handle reports committed cleanup requirement PASS\n";
}
void names_limits_and_io(FixtureRoot &roots){
    const auto mount=roots.ms0("validation");Store store(mount);
    for(const auto &bad:std::array<std::string,12>{"", ".", "..", "../X", "DATA/0", "DATA\\0", "C:DATA", "DATA*", "DATA?", "CON", "DATA00.",std::string(20u,'X')})
        rejects(ErrorCode::InvalidName,[&]{store.commit({"ULUS10216",bad},files());});
    for(const auto &bad:std::array<std::string,11>{"", "../DATA", "SECURE.BIN.", "NUL.BIN", "COM1.BIN", "A:B", "A/B", "A\\B", ".SECRET",std::string(13u,'X'),std::string("X\0Y",3u)}){
        auto data=files();data[0].name=bad;rejects(ErrorCode::InvalidName,[&]{store.commit(slot,data);});}
    auto duplicate=files();duplicate.push_back({"secure.bin",{1u}});rejects(ErrorCode::InvalidName,[&]{store.commit(slot,duplicate);});
    rejects(ErrorCode::InvalidRoot,[&]{Store invalid("relative/ms0");});rejects(ErrorCode::InvalidRoot,[&]{Store invalid(mount/".."/"validation");});
    Store limited(mount,{100u,200u,2u,4096u});rejects(ErrorCode::LimitExceeded,[&]{limited.commit(slot,files());});
    std::vector<File> too_large{{"A.BIN",Bytes(101u)}};rejects(ErrorCode::LimitExceeded,[&]{limited.commit(slot,too_large);});
    too_large={{"A.BIN",Bytes(100u)},{"B.BIN",Bytes(100u)}};require(limited.commit(slot,too_large).outcome==Outcome::Committed,"Exact native limits rejected valid file set");
    Store small_reader(mount,{99u,200u,2u,4096u});rejects(ErrorCode::LimitExceeded,[&]{small_reader.query(slot);});
    const auto target=mount/"PSP"/"SAVEDATA"/"ULUS10216DATA00";HANDLE hold=CreateFileW(target.c_str(),FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    require(hold!=INVALID_HANDLE_VALUE,"Cannot pin actual slot directory for native rename failure");const auto result=store.commit(slot,files(),Overwrite::ReplaceWithRollback);CloseHandle(hold);
    require(result.outcome==Outcome::FailedUnchanged&&result.failure&&result.failure->code==ErrorCode::NativeIo&&result.failure->native_error&&no_transactions(mount),"Real native rename denial did not restore prior state");equal_files(limited,slot,too_large);
    std::cout<<"Validation/resource/native IO: traversal/device/alias names, duplicate names, explicit limits, actual sharing-denied rename with rollback PASS\n";
}
void junction(const fs::path &link,const fs::path &destination){
    require(fs::create_directory(link),"Cannot create fixture junction directory");const auto substitute=L"\\??\\"+destination.native();const auto print=destination.native();
    const auto sub_bytes=substitute.size()*sizeof(wchar_t),print_bytes=print.size()*sizeof(wchar_t);const auto data_bytes=8u+sub_bytes+sizeof(wchar_t)+print_bytes+sizeof(wchar_t);
    require(data_bytes<0xFFFFu,"Fixture junction path too long");Bytes raw(8u+data_bytes,0u);
    auto put16=[&](std::size_t p,std::uint16_t n){raw[p]=static_cast<std::uint8_t>(n);raw[p+1u]=static_cast<std::uint8_t>(n>>8u);};
    const DWORD tag=IO_REPARSE_TAG_MOUNT_POINT;std::memcpy(raw.data(),&tag,4u);put16(4u,static_cast<std::uint16_t>(data_bytes));put16(8u,0u);put16(10u,static_cast<std::uint16_t>(sub_bytes));put16(12u,static_cast<std::uint16_t>(sub_bytes+sizeof(wchar_t)));put16(14u,static_cast<std::uint16_t>(print_bytes));
    std::memcpy(raw.data()+16u,substitute.data(),sub_bytes);std::memcpy(raw.data()+16u+sub_bytes+sizeof(wchar_t),print.data(),print_bytes);
    HANDLE h=CreateFileW(link.c_str(),GENERIC_WRITE,0u,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);require(h!=INVALID_HANDLE_VALUE,"Cannot open junction fixture handle");DWORD returned{};
    const auto ok=DeviceIoControl(h,FSCTL_SET_REPARSE_POINT,raw.data(),static_cast<DWORD>(raw.size()),nullptr,0u,&returned,nullptr);const auto code=GetLastError();CloseHandle(h);
    if(!ok)throw std::runtime_error("Actual fixture junction creation failed: "+std::to_string(code));
    require((GetFileAttributesW(link.c_str())&FILE_ATTRIBUTE_REPARSE_POINT)!=0u,"Fixture junction did not establish real reparse point");
}
void reparse_boundaries(FixtureRoot &roots){
    const auto outside=roots.ms0("outside_sentinel");std::ofstream(outside/"sentinel.bin",std::ios::binary)<<"unaltered";
    const auto root_link=roots.path/"root_link";junction(root_link,outside);rejects(ErrorCode::UnsafePath,[&]{Store rejected(root_link);});require(RemoveDirectoryW(root_link.c_str()),"Cannot remove owned root junction");
    for(const auto point:{0u,1u,2u,3u}){
        const auto mount=roots.ms0("reparse_"+std::to_string(point));Store store(mount);fs::path link;
        if(point==0u){link=mount/"PSP";}
        else {fs::create_directories(mount/"PSP"/"SAVEDATA");if(point==1u){fs::remove(mount/"PSP"/"SAVEDATA");link=mount/"PSP"/"SAVEDATA";}
            else if(point==2u)link=mount/"PSP"/"SAVEDATA"/"ULUS10216DATA00";
            else {fs::create_directory(mount/"PSP"/"SAVEDATA"/"ULUS10216DATA00");link=mount/"PSP"/"SAVEDATA"/"ULUS10216DATA00"/"PARAM.SFO";}}
        junction(link,outside);
        if(point<3u){rejects(ErrorCode::UnsafePath,[&]{store.query(slot);});const auto result=store.commit(slot,files(),Overwrite::ReplaceWithRollback);require(result.outcome==Outcome::FailedUnchanged&&result.failure&&result.failure->code==ErrorCode::UnsafePath,"Commit followed a reparse ancestor/slot");}
        else rejects(ErrorCode::UnsafePath,[&]{store.query(slot);});
        require(RemoveDirectoryW(link.c_str()),"Cannot remove owned component junction");
    }
    const auto mount=roots.ms0("staging_reparse");Store store(mount);fs::path link;
    const auto result=store.commit(slot,files(),Overwrite::Reject,[&](Phase p){if(p==Phase::StageComplete){for(const auto &entry:fs::directory_iterator(mount/"PSP"/"SAVEDATA"))if(entry.path().filename().string().starts_with("._CHILI_TXN_")){
        link=entry.path()/"new";fs::rename(link,entry.path()/"retained");junction(link,outside);}}});
    require(result.outcome==Outcome::RecoveryRequired&&result.failure&&result.failure->code==ErrorCode::UnsafePath&&result.rollback_failure&&!store.query(slot).exists,"Reparse inserted after staging silently published or unsafe-cleaned");
    require(RemoveDirectoryW(link.c_str()),"Cannot remove owned staged junction");require(fs::file_size(outside/"sentinel.bin")==9u&&fs::directory_iterator(outside)!=fs::directory_iterator{},"Reparse path altered outside sentinel");
    std::cout<<"Real reparse boundaries: mount/PSP/SAVEDATA/slot/file and after-staging junctions rejected, outside sentinel preserved/recovery explicit PASS\n";
}
void reader_serialization(FixtureRoot &roots){
    const auto mount=roots.ms0("serialized");auto mutex=std::make_shared<std::recursive_mutex>();Store writer(mount,{},mutex),reader(mount,{},mutex);
    require(writer.commit(slot,files()).outcome==Outcome::Committed,"Serialized reader setup failed");std::atomic<bool> entered{},finished{};std::thread pending;
    const auto result=writer.commit(slot,files(7u),Overwrite::ReplaceWithRollback,[&](Phase p){if(p==Phase::OldMoved){
        pending=std::thread([&]{entered=true;const auto bytes=reader.read(slot,"SECURE.BIN",1088u);require(bytes==files(7u)[1].bytes,"Serialized reader observed partial/old transaction");finished=true;});
        while(!entered.load())std::this_thread::yield();require(!finished.load(),"Reader bypassed caller serialization during overwrite gap");
    }});
    if(pending.joinable())pending.join();require(result.outcome==Outcome::Committed&&finished.load(),"Serialized reader failed to resume after commit");
    std::cout<<"Caller serialization: shared store guard blocks in-process reader through overwrite gap; reader receives complete committed bytes PASS\n";
}
}
int main(int argc,char **argv){try{require(argc==2,"Expected owned workspace fixture directory");FixtureRoot roots(argv[1]);new_and_replace(roots);kept_generation(roots);twenty_slots(roots);rollback_failures(roots);recovery_status(roots);names_limits_and_io(roots);reparse_boundaries(roots);reader_serialization(roots);std::cout<<"PASS: native opaque savedata storage foundation\n";return 0;}catch(const std::exception &e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
