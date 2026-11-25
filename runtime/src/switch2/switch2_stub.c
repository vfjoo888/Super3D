// Switch 2 controllers over Bluetooth LE need llvm-mingw's WinRT C headers (switch2_win.c); a build with another
// toolchain has none of them, and plays every other controller as before.
#include "switch2.h"

void switch2_start(void) { rt_log("input: Switch 2 controllers over Bluetooth: not in this build (needs llvm-mingw)"); }
int switch2_kind(uint32_t sdl_id) { (void)sdl_id; return SWITCH2_NONE; }
void switch2_hd_rumble(uint32_t sdl_id, const float left[4], const float right[4]) { (void)sdl_id, (void)left, (void)right; }
