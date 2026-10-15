#pragma once
namespace chili {
// A scripted run (deterministic, replay, scripted dialogs, script channel) runs beside whatever the person
// at the machine is doing. Its windows are shown without being activated and behind the others, so the
// window in front keeps the keyboard and the mouse. CHILI_WATCH=1 asks for an ordinary window instead.
// Set once at start-up, before any window exists.
inline bool background_windows = false;
}
