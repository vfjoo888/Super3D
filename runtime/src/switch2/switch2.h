// Switch 2 controllers (Pro Controller 2, Joy-Con 2) over Bluetooth LE, which SDL does not drive (SDL 3.4 has them
// over USB only). Ported from adriadam10/mkw-recomp (MIT, runtime/input/switch2*.{c,h}), whose protocol comes from
// seedds/mkw-neo (MIT). A transport finds and connects them (switch2_win.c: WinRT) and hands each one to switch2.c,
// which speaks the protocol (switch2_proto.h) and shows the controller to the rest of the input as a virtual SDL
// gamepad of its own: a Pro Controller 2 as a full gamepad, each Joy-Con 2 as a Joy-Con held upright (the same layout
// SDL gives a Switch 1 Joy-Con with SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS), so platform_win.cpp pairs Joy-Cons of
// either generation, and turns them sideways, the same way.
// NX_SWITCH2=0 turns it off; NX_SWITCH2=2 also logs every Bluetooth LE device seen once. It runs on its own thread.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void switch2_start(void);  // once, after SDL_Init(SDL_INIT_GAMEPAD): the transport's thread

// What the virtual gamepad with this SDL joystick id is: 0 not ours, else a SWITCH2_* kind. Any thread.
enum { SWITCH2_NONE, SWITCH2_PRO, SWITCH2_JOYCON_L, SWITCH2_JOYCON_R };
int switch2_kind(uint32_t sdl_id);

// HD rumble for that gamepad: the game's vibration values (amplitude 0..1, frequency in Hz; low band then high band)
// for its left and right actuator. A Pro Controller 2 plays left on its left grip and right on its right; a Joy-Con
// plays `left` (the caller picks its side, or the stronger of the two when it is held sideways). They are played for
// as long as they keep coming (~200 a second from the game); after 200 ms without one the gamepad's plain
// SDL_RumbleGamepad values are played again. Any thread.
void switch2_hd_rumble(uint32_t sdl_id, const float left[4], const float right[4]);

// ---- between a transport and switch2.c

void rt_log(const char *fmt, ...);  // nx::log (platform_win.cpp)
int switch2_trace(void);            // NX_SWITCH2=2

typedef struct S2Link S2Link;   // one connected controller, the transport's
typedef struct S2Session S2Session;

typedef struct {
    int (*write_cmd)(S2Link *l, const uint8_t *d, size_t n);   // to S2_UUID_CMD_WR, without response; 0 on failure
    int (*write_vib)(S2Link *l, const uint8_t *d, size_t n);   // to the vibration characteristic, if it has one
    int (*start_input)(S2Link *l);                              // notifications of S2_UUID_INPUT on
    void (*pump)(int ms);   // the transport's events for up to ms (replies and reports reach the session from here)
    void (*log_link)(S2Link *l);   // optional: the link's own figures in the log (its connection interval)
} S2LinkOps;

uint64_t switch2_now_ns(void);
// A controller connected, its characteristics found and replies subscribed: its session (replies reach it from here
// on), then the handshake (identity, calibration, pairing when use is S2_ADV_PAIR, features) and a gamepad. 0 when
// the handshake failed: the transport closes the session and disconnects it.
S2Session *switch2_new(S2Link *l, const S2LinkOps *ops, uint16_t pid, int use, const uint8_t host[6]);
int switch2_handshake(S2Session *s);
void switch2_reply(S2Session *s, const uint8_t *d, size_t n);               // a S2_UUID_CMD_RSP notification
void switch2_report(S2Session *s, const uint8_t *d, size_t n, uint64_t at);  // a S2_UUID_INPUT notification
void switch2_tick(void);                                                     // every ~15 ms: rumble, keep-alive
void switch2_close(S2Session *s);                                            // disconnected: its gamepad goes

#ifdef __cplusplus
}
#endif
