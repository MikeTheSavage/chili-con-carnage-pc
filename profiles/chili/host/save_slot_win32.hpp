#pragma once
#include "save_slot_presenter.hpp"
#include <functional>
#include <memory>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace chili {
class Win32SaveSlotPresenter final:public SaveSlotPresenter {
public:
    using Parent=std::function<HWND()>;
    // A valid sample contains PSP button bits, including 0 for active release.
    // nullopt means input is inactive; held bits are baselined on open/resume.
    // Existing uint32-returning callbacks convert to this optional result.
    using Buttons=std::function<std::optional<std::uint32_t>()>;
    explicit Win32SaveSlotPresenter(Parent parent={},bool visible=true,Buttons buttons={});
    ~Win32SaveSlotPresenter() override;
    Win32SaveSlotPresenter(const Win32SaveSlotPresenter &)=delete;
    Win32SaveSlotPresenter &operator=(const Win32SaveSlotPresenter &)=delete;
    void open(const SaveSlotRequest &) override;
    std::optional<SaveSlotSelection> poll(std::uint64_t generation) override;
    void close(std::uint64_t generation) noexcept override;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
