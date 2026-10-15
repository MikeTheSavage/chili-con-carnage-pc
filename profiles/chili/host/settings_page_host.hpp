#pragma once
// The settings page in the game, as main.cpp puts it there (host/settings_page.cpp; the page itself is
// settings_page.hpp, what the game's menus call to open it is settings_page_api.hpp).
//
//   page_start          once, after the texture mods have started and before the first draw, in a run that
//                       has the page (settings::feature_enabled): the page's textures get their keys
//   page_frame          at every frame start, on the game thread: the font, the test steps, the draw list
//   page_sample         for every pad sample handed to the game: an open page takes it and the game gets a
//                       neutral one; what was held when the page closed stays out until it is let go
//   page_window_message the window's messages, before the input layer: the pointer, the two buttons and the
//                       wheel belong to an open page
//   page_stop           before the settings object and the renderer go
//
// In a run without the page none of them does anything: page_start is not called, and the others return at
// their first test.
//
// Test variables (a scripted run; with CHILI_PAGE=1, which also switches the feature on):
//   CHILI_PAGE_AT=<frame>:<step>[;<frame>:<step>...]     steps at game frames, each one line in the log
//       open | open:<frame>[:<page>] | open:<page>      frame: main, pause (else by the game's menu); page: a
//                                                        section's name without spaces (display, graphics, controls,
//                                                        keyboard, gamepad, textures, sound)
//       up, down, left, right, ok, back, close          what the buttons do
//       pad:<buttons>[:<x>:<y>]                         one pad sample given to the page (hexadecimal buttons)
//       move:<x>:<y>, click:<x>:<y>, press:<x>:<y>, release:<x>:<y>, rclick, wheel:<notches>
//                                                        the pointer, in PSP pixels
//       post:<what>:<x>:<y>                             a real window message posted to the game's own window at
//                                                        that place of the picture: move, down, up, rdown, wheel
//                                                        (x = notches); it arrives with the window's other messages
//       set:<Section>/<Key>=<value>                     a value set as a row would set it
//       state                                           the page and the game's menu under it, one line
//       shot:<name>                                     <CHILI_PAGE_SHOTS>\<name>.png: the picture with the page,
//                                                        at the internal resolution
//       window:<name>                                   the same from the window's own back buffer, bars and all
//       size:<width>x<height>, fullscreen:on|off        the window's client area; borderless full screen
//   CHILI_PAGE_SHOTS=<folder>                            where shot and window write
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "controller_runtime.hpp"
#include "gpu/ge_gpu_backend.hpp"
#include "psprecomp/guest_memory.hpp"
#include "settings_page_api.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
namespace chili {
namespace settings { class Live; }
struct PageStart {
    // The run's settings; the page works through them.
    settings::Live *live{};
    // The Direct3D 12 renderer draws the page; the software renderer has none.
    bool gpu{};
    // The clock is the frame count (a thirtieth of a second each), so that a deterministic run repeats.
    bool frame_clock{};
    // A scripted run: the window's mouse messages are the page's also while a replay owns the pad.
    bool scripted{};
    std::function<HWND()> window;
    std::function<void(bool)> set_fullscreen;
    // The displayed picture with the page, written to a file (gpu_video.hpp's save_gpu_display on the list thread).
    std::function<bool(const std::filesystem::path &)> save_frame;
    // The window's own picture: asked for, and taken a present later (false until then).
    std::function<void()> request_window_capture;
    std::function<bool(vcs::GeGpuWindowCapture &)> take_window_capture;
    // What the font's D-pad-left and D-pad-right pictures are, as they are painted now: 0 two key caps, 1 a
    // controller's D-pad with one arm lit each (painted by the button prompts), 2 the PSP's own D-pad pictures.
    // With 1 and 2 the Change legend is one D-pad with both arms lit. Empty: two pictures side by side.
    std::function<int()> dpad_pictures;
};
// Returns lines for the log.
std::vector<std::string> page_start(PageStart start);
void page_frame(std::uint64_t frame, const psprecomp::GuestMemory &memory);
[[nodiscard]] ControllerInput page_sample(const ControllerInput &sample);
bool page_window_message(HWND window, UINT message, WPARAM w, LPARAM l, LRESULT &result);
// Lines for the end of the log.
std::vector<std::string> page_report();
void page_stop();
}
