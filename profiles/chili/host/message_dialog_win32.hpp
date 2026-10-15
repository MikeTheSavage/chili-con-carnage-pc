#pragma once
#include "message_dialog_runtime.hpp"
#include <memory>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace chili {
// Construction does not create or show a window. open() creates a modeless
// native window when the guest first updates an actual running request.
// Guest button_swap controls sampled PSP confirm/back; native default captions are
// English, with UTF-8 custom captions/body. No guessed PSP error translations.
// visible=false suppresses window showing, initial focus and sound; it does
// not choose or dismiss anything. Intended for hidden native fixture windows.
class Win32MessageDialogPresenter final : public MessageDialogPresenter {
public:
    using Parent = std::function<HWND()>;
    // Valid 0 is active release; nullopt is inactive input. Open/resume baseline
    // held bits; uint32-returning existing callbacks remain convertible.
    using Buttons = std::function<std::optional<std::uint32_t>()>;
    explicit Win32MessageDialogPresenter(Parent parent = {}, bool visible = true, Buttons buttons = {});
    ~Win32MessageDialogPresenter() override;
    Win32MessageDialogPresenter(const Win32MessageDialogPresenter &) = delete;
    Win32MessageDialogPresenter &operator=(const Win32MessageDialogPresenter &) = delete;
    void open(const MessageDialogRequest &) override;
    std::optional<MessageDialogSelection> poll(std::uint64_t generation) override;
    void close(std::uint64_t generation) noexcept override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace chili
