#pragma once
// The picture's rectangle in a window, as the Direct3D 12 renderer puts it there (picture_rect.hpp with the
// renderer's own two switches), and the mouse pointer mapped through it. For whatever handles the window's
// messages: a host page hit-tests in PSP pixels.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "gpu/ge_gpu_config.hpp"
#include "picture_rect.hpp"
namespace chili {
// Empty when the window has no client area (minimised, or gone). Read the client size per message: full
// screen and back (F11) and a resize change it.
[[nodiscard]] inline PictureRect window_picture_rect(HWND window) noexcept {
    RECT client{};
    if (!window || !GetClientRect(window, &client) || client.right <= client.left || client.bottom <= client.top) return {};
    return picture_rect(static_cast<std::uint32_t>(client.right - client.left), static_cast<std::uint32_t>(client.bottom - client.top),
                        vcs::ge_gpu_config().preserve_aspect, vcs::ge_gpu_config().integer_scale);
}
// The pointer of a mouse message (client coordinates in `l`, as WM_MOUSEMOVE and the button messages carry
// them) as a place in the picture in PSP pixels. False when it is over a black bar or outside the window;
// `x` and `y` are set either way, so a drag that leaves the picture keeps its direction.
[[nodiscard]] inline bool window_pointer_in_picture(HWND window, LPARAM l, float &x, float &y) noexcept {
    return window_picture_rect(window).to_picture(static_cast<std::int32_t>(static_cast<short>(LOWORD(l))),
                                                  static_cast<std::int32_t>(static_cast<short>(HIWORD(l))), x, y);
}
}
