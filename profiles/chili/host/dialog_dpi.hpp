#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace chili {
// The native dialogs are laid out in 96-dpi pixels, while the game process is
// per-monitor DPI aware for its swapchain. A dialog created inside this scope
// is a DPI-unaware window, which Windows scales with the desktop like any
// classic dialog; the game window keeps its real pixels.
class DialogDpiScope {
    DPI_AWARENESS_CONTEXT previous_{};
public:
    DialogDpiScope() noexcept {
        previous_=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED);
        if(!previous_)previous_=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
    }
    ~DialogDpiScope(){if(previous_)SetThreadDpiAwarenessContext(previous_);}
    DialogDpiScope(const DialogDpiScope &)=delete;
    DialogDpiScope &operator=(const DialogDpiScope &)=delete;
};
}
