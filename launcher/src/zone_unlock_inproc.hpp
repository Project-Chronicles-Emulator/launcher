#pragma once

namespace chronicles::launcher {

// Starts, at most once per process, a background thread inside the game that keeps the
// researched zone gates enabled. It is meant to be called from the Steam compatibility library
// that the game loads, so the unlock needs no launcher, helper process or cross-process access.
//
// Creating a file named steam_settings/disable_zone_unlock.txt beside the library turns the
// thread off. The thread only changes catalog data that the researched client build lays out as
// expected; any other build leaves memory untouched.
void start_in_process_zone_unlock();

} // namespace chronicles::launcher
