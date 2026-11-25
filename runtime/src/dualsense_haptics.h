// A DualSense's voice coils for the game's HD rumble, played by RenHDrum (renhdrum.dll beside the game: MIT OR
// Apache-2.0, loaded at run time, so updating it is replacing the file). Over its cable the controller's own four-channel
// sound card carries them (its third and fourth channels). Without the DLL, without a cable (Bluetooth is not played) or
// without a DualSense, nothing here acts and every controller keeps the plain SDL rumble. NX_DUALSENSE_HAPTICS=0 turns it off.
#pragma once
#include <cstdint>

namespace nx::dualsense {

// The game's HD rumble for the DualSense with this SDL joystick id: {amplitude low, frequency low, amplitude high,
// frequency high} of its left and right actuator, as the game sent them (no strength applied: it comes after RenHDrum's
// own response curve). `strength` is the player's rumble setting, percent. Any thread; cheap, the controller is opened
// (and let go of when it has been quiet for long) on the module's own thread.
void play(uint32_t sdl_id, const float left[4], const float right[4], int strength);

// True while the voice coils of that controller are played by us: its plain SDL rumble must stay off then.
bool owns(uint32_t sdl_id);

}  // namespace nx::dualsense
