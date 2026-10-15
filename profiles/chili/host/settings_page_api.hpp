#pragma once
// The game's own settings page, as the rest of the host sees it: where it is drawn, the request to open
// it, and whether it is open. The entry in the game's menus
// asks for the page with page_request_open(); the pad-sample path and the window's messages ask
// page_is_open(). The page itself is host/settings_page.hpp, put into the game by host/settings_page.cpp
// (what main.cpp calls there is settings_page_host.hpp).
//
// For the entry: call page_request_open() on the game's thread, from the pad-sample path or from a frame
// start, when the player has chosen the Settings line. The page is open when the call returns (unless the
// run cannot have one: the software renderer, the game's font not loaded yet; a line in the log says so),
// and from the next pad sample on, this one included when the call is made before main.cpp hands the
// sample to chili::page_sample(), the game gets neutral samples. Whatever is held at that moment (the Cross
// that chose the line) does nothing on the page until it is let go. The page closes by itself (Circle on
// its top page, Start, a right click) and keeps the closing button from the game until it is let go; the
// game's menu is then where it was. A second request while the page is open does nothing.
namespace chili {
// MainMenu: over the menu scene (the main menu, Profile Options, the game's own Settings screen).
// Pause: inside the pause menu's box, in a mission.
enum class PageFrame { MainMenu, Pause };
// Opens the page in this frame, on its top page. On the game's thread.
void page_request_open(PageFrame frame);
bool page_is_open();
}
