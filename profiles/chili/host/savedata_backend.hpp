#pragma once
#include "savedata_request.hpp"
#include "save_slot_presenter.hpp"
#include <filesystem>
namespace chili {
struct SavedataSlotInfo {
    SaveSlot slot;
    std::uint64_t written_filetime{};
};
struct SavedataLoadedProfile {
    std::vector<std::uint8_t> payload;
    std::array<std::vector<std::uint8_t>,4> media;
    std::string title,savedata_title,detail;
    std::uint8_t parental{};
};
struct SavedataBackendError:std::runtime_error {
    std::uint32_t result;
    SavedataBackendError(std::uint32_t value,const std::string&message):std::runtime_error(message),result(value){}
};
// What a committed save did to the slot, for the log.
struct SavedataSaveReport {
    bool replaced{};       // the slot held a save before
    std::string previous;  // where that earlier save is kept now; empty when there was none or it could not be kept
    std::string note;      // why it could not be kept, or what was left to clean up; empty when there is nothing to say
};
// Backend operations are complete or raise. A successful save must represent
// committed actual files; unsupported PSP containers never return fake data.
class SavedataBackend {
public:
    virtual ~SavedataBackend()=default;
    virtual std::vector<SavedataSlotInfo> inspect(const save_request::Request&)=0;
    // replace_existing says whether a save that is already in the slot may be
    // replaced. The caller decides it (an automatic save: always; a list save:
    // when the player confirmed it); the request's word at offset 56 does not.
    // Without it a slot that holds a save is refused and stays as it is.
    virtual SavedataSaveReport save(const save_request::Request&,const std::string&slot,bool replace_existing)=0;
    virtual SavedataLoadedProfile load(const save_request::Request&,const std::string&slot)=0;
    // Takes the slot out of the list without erasing it; returns where its folder went.
    virtual std::string remove(const save_request::Request&,const std::string&slot)=0;
    // For the dialog's texts: where a replaced or deleted save is kept, as the
    // player would find it from the saves folder. Empty: nothing is kept.
    virtual std::string previous_place()const{return {};}
};
}
