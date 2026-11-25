// Switch 2 controllers over Bluetooth LE (switch2.h): the protocol on top of a transport, and each controller shown as
// a virtual SDL gamepad of its own. Adapted from adriadam10/mkw-recomp's runtime/input/switch2.c (MIT), whose flow
// follows seedds/mkw-neo's CoreBluetooth session (MIT): identity, pairing when the controller is in Sync, stick
// calibration, features, then reports; the player lights are written once a second, since the link drops after about
// ten seconds without a write from the host. Differences from mkw-recomp: Joy-Cons are never merged here (each one is
// its own gamepad, upright, and platform_win.cpp pairs them or turns them sideways, as it does Switch 1 Joy-Cons), the
// game's HD rumble (amplitude and frequency per band and side) is played as it is, and there is no motion (Super Mario
// Bros. Wonder reads none). Everything runs on the transport's thread, but for the gamepad's callbacks and
// switch2_kind / switch2_hd_rumble, which only touch the atomics of the slot table.
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "switch2.h"
#include "switch2_proto.h"

enum { S2_REPLY_MAX = 0x50 + S2_REPLY_DATA, S2_TIMEOUT_MS = 2000, S2_LED_NS = 1000000000 };
#define S2_HD_FRESH_NS 200000000ull  // HD values older than this: back to the plain rumble

// ---- the slot table: what other threads may read or set of a pad

enum { S2_SLOTS = 8 };
typedef struct {
    _Atomic uint32_t id;       // its SDL joystick id, 0 free
    _Atomic int kind;          // SWITCH2_*
    _Atomic int player;        // SDL_SetGamepadPlayerIndex's, -1 none
    _Atomic uint32_t rumble;   // low << 16 | high (SDL_RumbleGamepad, any thread)
    _Atomic uint64_t hd[2];    // left, right: an s2_haptic packed (hd_pack)
    _Atomic uint64_t hd_at;    // when they were set, ns
} S2Slot;
static S2Slot slots[S2_SLOTS];

static uint64_t hd_pack(s2_haptic h) {
    return (uint64_t)h.lf_hz | (uint64_t)h.lf_amp << 16 | (uint64_t)h.hf_hz << 32 | (uint64_t)h.hf_amp << 48;
}
static s2_haptic hd_unpack(uint64_t v) {
    s2_haptic h = {(uint16_t)v, (uint16_t)(v >> 16), (uint16_t)(v >> 32), (uint16_t)(v >> 48)};
    return h;
}

static S2Slot *slot_of(uint32_t id) {
    for (int i = 0; id && i < S2_SLOTS; i++)
        if (atomic_load(&slots[i].id) == id) return &slots[i];
    return NULL;
}

int switch2_kind(uint32_t sdl_id) {
    S2Slot *s = slot_of(sdl_id);
    return s ? atomic_load(&s->kind) : SWITCH2_NONE;
}

static s2_haptic haptic(const float v[4]) {
    float low = v[0], high = v[2];
    if (low < 0) low = 0;
    if (high < 0) high = 0;
    s2_rumble_cap(&low, &high);
    return s2_haptic_from(low, v[1] > 0 ? v[1] : 160.f, high, v[3] > 0 ? v[3] : 320.f);
}

void switch2_hd_rumble(uint32_t sdl_id, const float left[4], const float right[4]) {
    S2Slot *s = slot_of(sdl_id);
    if (!s) return;
    atomic_store(&s->hd[0], hd_pack(haptic(left)));
    atomic_store(&s->hd[1], hd_pack(haptic(right)));
    atomic_store(&s->hd_at, switch2_now_ns());
}

// ---- sessions

struct S2Session {
    S2Link *link;
    const S2LinkOps *ops;
    uint16_t pid;
    int use;
    uint8_t host[6];
    s2_dev dev;                   // its last report and calibration
    s2_cal cal[2];
    uint8_t want;                 // the command a reply is awaited for, 0 none
    uint8_t reply[S2_REPLY_MAX];
    size_t reply_n;
    int got;
    uint8_t vseq;
    int quiet;                    // silent rumble packets still to send after the motors stopped
    int index;                    // its slot, and the player lights when the game set none
    SDL_JoystickID id;
    SDL_Joystick *js;
    uint64_t next_led;
    struct S2Session *next;
};

static S2Session *sessions;

uint64_t switch2_now_ns(void) { return SDL_GetTicksNS(); }

static const char *kind_name(uint16_t pid) {
    return pid == S2_PID_PRO ? "Pro Controller 2" : pid == S2_PID_JOYCON_L ? "Joy-Con 2 (L)" : "Joy-Con 2 (R)";
}

// ---- commands: one in flight (the controller has one reply channel); the transport pumps until the reply

static int command(S2Session *s, uint8_t cmd, uint8_t sub, const uint8_t *payload, size_t n) {
    uint8_t f[8 + 0xFF];
    size_t len = s2_command(f, sizeof f, cmd, sub, payload, n);
    s->want = cmd, s->got = 0;
    if (!len || !s->ops->write_cmd(s->link, f, len)) return s->want = 0, 0;
    for (uint64_t end = switch2_now_ns() + S2_TIMEOUT_MS * 1000000ull; !s->got && switch2_now_ns() < end;)
        s->ops->pump(10);
    if (!s->got) rt_log("input: %s: command 0x%02x timed out", kind_name(s->pid), cmd);
    s->want = 0;
    return s->got;
}

void switch2_reply(S2Session *s, const uint8_t *d, size_t n) {
    if (!s->want || !s2_reply_is(d, n, s->want)) return;
    s->reply_n = n < sizeof s->reply ? n : sizeof s->reply;
    memcpy(s->reply, d, s->reply_n);
    s->got = 1;
}

// len bytes of the controller's memory at addr into out; 0 when it did not answer them
static int read_memory(S2Session *s, uint8_t len, uint32_t addr, uint8_t *out) {
    uint8_t p[8];
    s2_memory_payload(p, len, addr);
    if (!command(s, S2_CMD_MEMORY, S2_SUB_MEMORY_READ, p, 8)) return 0;
    if (s->reply_n < (size_t)S2_REPLY_DATA + len || s->reply[8] != len) return 0;
    memcpy(out, s->reply + S2_REPLY_DATA, len);
    return 1;
}

static void read_cal(S2Session *s, int slot) {   // the player's own calibration when set, else the factory's
    uint8_t d[0x0B];
    if (read_memory(s, sizeof d, slot ? S2_ADDR_USER_STICK2 : S2_ADDR_USER_STICK1, d) && s2_cal_parse(d, sizeof d, &s->cal[slot]))
        return;
    if (read_memory(s, sizeof d, slot ? S2_ADDR_FACTORY_STICK2 : S2_ADDR_FACTORY_STICK1, d)) s2_cal_parse(d, sizeof d, &s->cal[slot]);
}

// The console's pairing: this host's address and two keys of the host's choosing (switch2_proto.h, Bonding). From then
// on a button press wakes the controller toward this host. A failed step leaves it playing, but needing Sync again.
static void bond(S2Session *s) {
    uint8_t host[S2_PAIR_HOST_LEN], k[16], key[S2_PAIR_KEY_LEN], zero = 0;
    s2_pair_host_payload(host, s->host);
    int ok = command(s, S2_CMD_PAIR, S2_SUB_PAIR_HOST, host, sizeof host);
    for (int i = 0; ok && i < 2; i++) {
        for (int j = 0; j < 16; j++) k[j] = (uint8_t)SDL_rand_bits();
        s2_pair_key_payload(key, k);
        ok = command(s, S2_CMD_PAIR, i ? S2_SUB_PAIR_KEY2 : S2_SUB_PAIR_KEY1, key, sizeof key);
    }
    ok = ok && command(s, S2_CMD_PAIR, S2_SUB_PAIR_DONE, &zero, 1);
    rt_log("input: %s %s", kind_name(s->pid), ok ? "paired with this PC: from now on a press of any of its buttons connects it"
                                                 : "not paired (no answer): it plays now, but needs Sync again next time");
}

// ---- the gamepad

static bool SDLCALL pad_rumble(void *userdata, Uint16 low, Uint16 high) {
    atomic_store(&((S2Slot *)userdata)->rumble, (uint32_t)low << 16 | high);
    return true;
}
static void SDLCALL pad_player(void *userdata, int player_index) { atomic_store(&((S2Slot *)userdata)->player, player_index); }

static int attach(S2Session *s) {
    int used = 0;
    for (S2Session *o = sessions; o; o = o->next) used |= 1 << o->index;
    s->index = 0;
    while (s->index < S2_SLOTS && (used & 1 << s->index)) s->index++;
    if (s->index >= S2_SLOTS) return rt_log("input: %s: too many Switch 2 controllers", kind_name(s->pid)), 0;
    S2Slot *slot = &slots[s->index];
    atomic_store(&slot->rumble, 0);
    atomic_store(&slot->hd_at, 0);
    atomic_store(&slot->player, -1);
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    // Nintendo's ids: SDL then types it as a Switch Pro Controller or a left / right Joy-Con
    d.vendor_id = S2_VENDOR_ID;
    d.product_id = s->pid;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;   // every SDL gamepad button, so a button's index is its SDL_GamepadButton
    d.name = s->pid == S2_PID_PRO ? "Pro Controller 2" : s->pid == S2_PID_JOYCON_L ? "Joy-Con 2 (L)" : "Joy-Con 2 (R)";
    d.userdata = slot;
    d.Rumble = pad_rumble;
    d.SetPlayerIndex = pad_player;
    s->id = SDL_AttachVirtualJoystick(&d);
    s->js = s->id ? SDL_OpenJoystick(s->id) : NULL;
    if (!s->js) {
        rt_log("input: Switch 2 controller: no virtual gamepad (%s)", SDL_GetError());
        if (s->id) SDL_DetachVirtualJoystick(s->id), s->id = 0;
        return 0;
    }
    atomic_store(&slot->kind, s->pid == S2_PID_PRO ? SWITCH2_PRO : s->pid == S2_PID_JOYCON_L ? SWITCH2_JOYCON_L : SWITCH2_JOYCON_R);
    atomic_store(&slot->id, (uint32_t)s->id);
    return 1;
}

static void detach(S2Session *s) {
    if (!s->js) return;
    atomic_store(&slots[s->index].id, 0);
    atomic_store(&slots[s->index].kind, SWITCH2_NONE);
    SDL_CloseJoystick(s->js);
    SDL_DetachVirtualJoystick(s->id);
    s->js = NULL, s->id = 0;
}

// Triggers: SDL's virtual gamepads read -32768 as released and 0 as half pressed (SDL's own Switch drivers send the same)
static void button(SDL_Joystick *js, SDL_GamepadButton b, int down) { SDL_SetJoystickVirtualButton(js, (int)b, down != 0); }
static void axis(SDL_Joystick *js, SDL_GamepadAxis a, int32_t v) {
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    SDL_SetJoystickVirtualAxis(js, (int)a, (Sint16)v);
}

// Its report as an SDL gamepad. A Pro Controller 2: A east, as SDL lays out a Switch Pro Controller. A Joy-Con: held
// upright, as SDL lays out a Switch 1 Joy-Con with SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS: the left one's d-pad,
// L, ZL (left trigger), Minus (back), stick and Capture (misc1), the right one's face buttons, R, ZR (right trigger),
// Plus (start), stick (the right stick), Home (guide) and C (misc2); its rail buttons as the paddles SDL uses for
// them: left SL / SR = left paddle 1 / 2, right SR / SL = right paddle 1 / 2. Sticks: y up here, down in SDL.
static void push(S2Session *s) {
    SDL_Joystick *js = s->js;
    uint32_t b = s->dev.r.buttons;
    if (s->pid == S2_PID_PRO) {
        s2_pad_state st;
        memset(&st, 0, sizeof st);
        st.pro = s->dev;
        uint64_t m;
        int32_t sticks[4];
        if (!s2_pad_input(&st, &m, sticks)) return;
        button(js, SDL_GAMEPAD_BUTTON_EAST, m >> B_A & 1);
        button(js, SDL_GAMEPAD_BUTTON_SOUTH, m >> B_B & 1);
        button(js, SDL_GAMEPAD_BUTTON_NORTH, m >> B_X & 1);
        button(js, SDL_GAMEPAD_BUTTON_WEST, m >> B_Y & 1);
        button(js, SDL_GAMEPAD_BUTTON_LEFT_STICK, m >> B_STICK_L & 1);
        button(js, SDL_GAMEPAD_BUTTON_RIGHT_STICK, m >> B_STICK_R & 1);
        button(js, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, m >> B_L & 1);
        button(js, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, m >> B_R & 1);
        button(js, SDL_GAMEPAD_BUTTON_START, m >> B_PLUS & 1);
        button(js, SDL_GAMEPAD_BUTTON_BACK, m >> B_MINUS & 1);
        button(js, SDL_GAMEPAD_BUTTON_DPAD_LEFT, m >> B_LEFT & 1);
        button(js, SDL_GAMEPAD_BUTTON_DPAD_UP, m >> B_UP & 1);
        button(js, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, m >> B_RIGHT & 1);
        button(js, SDL_GAMEPAD_BUTTON_DPAD_DOWN, m >> B_DOWN & 1);
        button(js, SDL_GAMEPAD_BUTTON_GUIDE, b & S2_HOME);
        button(js, SDL_GAMEPAD_BUTTON_MISC1, b & S2_CAPTURE);
        button(js, SDL_GAMEPAD_BUTTON_MISC2, b & S2_C);
        button(js, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, b & S2_GR);
        button(js, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, b & S2_GL);
        axis(js, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, (m >> B_ZL & 1) ? 32767 : -32768);
        axis(js, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, (m >> B_ZR & 1) ? 32767 : -32768);
        axis(js, SDL_GAMEPAD_AXIS_LEFTX, sticks[0]);
        axis(js, SDL_GAMEPAD_AXIS_LEFTY, -sticks[1]);
        axis(js, SDL_GAMEPAD_AXIS_RIGHTX, sticks[2]);
        axis(js, SDL_GAMEPAD_AXIS_RIGHTY, -sticks[3]);
        return;
    }
    int32_t st[2];
    const int right = s->pid == S2_PID_JOYCON_R;
    s2_joy_stick(&s->dev, right, st);
    if (!right) {
        button(js, SDL_GAMEPAD_BUTTON_DPAD_LEFT, b & S2_LEFT);
        button(js, SDL_GAMEPAD_BUTTON_DPAD_UP, b & S2_UP);
        button(js, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, b & S2_RIGHT);
        button(js, SDL_GAMEPAD_BUTTON_DPAD_DOWN, b & S2_DOWN);
        button(js, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, b & S2_L);
        button(js, SDL_GAMEPAD_BUTTON_BACK, b & S2_MINUS);
        button(js, SDL_GAMEPAD_BUTTON_LEFT_STICK, b & S2_LSTICK);
        button(js, SDL_GAMEPAD_BUTTON_MISC1, b & S2_CAPTURE);
        button(js, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, b & S2_SL_L);
        button(js, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, b & S2_SR_L);
        axis(js, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, (b & S2_ZL) ? 32767 : -32768);
        axis(js, SDL_GAMEPAD_AXIS_LEFTX, st[0]);
        axis(js, SDL_GAMEPAD_AXIS_LEFTY, -st[1]);
    } else {
        button(js, SDL_GAMEPAD_BUTTON_EAST, b & S2_A);
        button(js, SDL_GAMEPAD_BUTTON_SOUTH, b & S2_B);
        button(js, SDL_GAMEPAD_BUTTON_NORTH, b & S2_X);
        button(js, SDL_GAMEPAD_BUTTON_WEST, b & S2_Y);
        button(js, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, b & S2_R);
        button(js, SDL_GAMEPAD_BUTTON_START, b & S2_PLUS);
        button(js, SDL_GAMEPAD_BUTTON_RIGHT_STICK, b & S2_RSTICK);
        button(js, SDL_GAMEPAD_BUTTON_GUIDE, b & S2_HOME);
        button(js, SDL_GAMEPAD_BUTTON_MISC2, b & S2_C);
        button(js, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, b & S2_SR_R);
        button(js, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, b & S2_SL_R);
        axis(js, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, (b & S2_ZR) ? 32767 : -32768);
        axis(js, SDL_GAMEPAD_AXIS_RIGHTX, st[0]);
        axis(js, SDL_GAMEPAD_AXIS_RIGHTY, -st[1]);
    }
}

static void set_leds(S2Session *s) {
    int player = atomic_load(&slots[s->index].player);
    uint8_t pl[4] = {s2_led_pattern[(player >= 0 ? player : s->index) & 7], 0, 0, 0}, f[12];
    size_t n = s2_command(f, sizeof f, S2_CMD_LEDS, S2_SUB_LEDS_SET, pl, 4);
    if (n) s->ops->write_cmd(s->link, f, n);
}

// ---- a session

S2Session *switch2_new(S2Link *l, const S2LinkOps *ops, uint16_t pid, int use, const uint8_t host[6]) {
    S2Session *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->link = l, s->ops = ops, s->pid = pid, s->use = use;
    if (host) memcpy(s->host, host, 6);
    s2_cal_default(&s->cal[0]);
    s2_cal_default(&s->cal[1]);
    return s;
}

int switch2_handshake(S2Session *s) {
    const uint16_t pid = s->pid;
    uint8_t info[0x40];
    uint16_t vid, real;
    char serial[15];
    if (!read_memory(s, sizeof info, S2_ADDR_INFO, info) || !s2_info_parse(info, sizeof info, &vid, &real, serial) ||
        vid != S2_VENDOR_ID) {
        rt_log("input: %s: it did not answer the identity read", kind_name(pid));
        return 0;
    }
    s->pid = real;
    if (s->pid != S2_PID_PRO && s->pid != S2_PID_JOYCON_L && s->pid != S2_PID_JOYCON_R) {
        rt_log("input: Switch 2 controller of product 0x%04x: not a Pro Controller 2 or a Joy-Con 2", s->pid);
        return 0;
    }
    if (s->use == S2_ADV_PAIR) bond(s);
    read_cal(s, 0);
    if (s->pid == S2_PID_PRO) read_cal(s, 1);
    uint8_t feat[4] = {S2_FEATURES, 0, 0, 0};
    command(s, S2_CMD_FEATURE, S2_SUB_FEATURE_SET, feat, 4);
    if (!command(s, S2_CMD_FEATURE, S2_SUB_FEATURE_ENABLE, feat, 4)) {
        rt_log("input: %s: enabling its reports was refused", kind_name(s->pid));
        return 0;
    }
    if (!attach(s)) return 0;
    memset(&s->dev, 0, sizeof s->dev);
    s->dev.have = 1, s->dev.pid = s->pid, s->dev.cal[0] = s->cal[0], s->dev.cal[1] = s->cal[1];
    s->next = sessions, sessions = s;
    set_leds(s);
    s->next_led = switch2_now_ns() + S2_LED_NS;
    if (!s->ops->start_input(s->link)) rt_log("input: %s: its reports could not be subscribed", kind_name(s->pid));
    rt_log("input: %s %.14s connected over Bluetooth", kind_name(s->pid), serial);
    return 1;
}

void switch2_report(S2Session *s, const uint8_t *d, size_t n, uint64_t at) {
    (void)at;
    if (!s->js || !s2_report_parse(d, n, &s->dev.r)) return;
    push(s);
}

// What it plays: the game's HD values while they keep coming (switch2_hd_rumble), else SDL_RumbleGamepad's two motors
// as a plain rumble (low band at 160 Hz, high at 320 Hz); three 5 ms samples a packet every ~15 ms while it runs, then
// three silent packets (one write without response can be lost)
static void rumble(S2Session *s) {
    if (!s->ops->write_vib) return;
    S2Slot *slot = &slots[s->index];
    s2_haptic a, b;
    uint64_t at = atomic_load(&slot->hd_at);
    if (at && switch2_now_ns() - at < S2_HD_FRESH_NS) {
        a = hd_unpack(atomic_load(&slot->hd[0]));
        b = s->pid == S2_PID_PRO ? hd_unpack(atomic_load(&slot->hd[1])) : a;
    } else {
        uint32_t v = atomic_load(&slot->rumble);
        float low = (float)(v >> 16) / 65535.f, high = (float)(v & 0xFFFF) / 65535.f;
        s2_rumble_cap(&low, &high);
        a = b = s2_haptic_from(low, 160.f, high, 320.f);
    }
    if (!s2_haptic_on(&a) && !s2_haptic_on(&b)) {
        if (!s->quiet) return;
        s->quiet--;
    } else s->quiet = 3;
    s2_haptic first[3] = {a, a, a}, second[3] = {b, b, b};
    uint8_t out[33];
    size_t n = s2_rumble_packet3(out, first, second, s->vseq++);
    s->ops->write_vib(s->link, out, n);
}

void switch2_tick(void) {
    uint64_t now = switch2_now_ns();
    for (S2Session *s = sessions; s; s = s->next) {
        rumble(s);
        if (now >= s->next_led) set_leds(s), s->next_led = now + S2_LED_NS;
    }
}

void switch2_close(S2Session *s) {
    S2Session **l = &sessions;
    while (*l && *l != s) l = &(*l)->next;
    if (*l) {
        *l = s->next;
        rt_log("input: %s disconnected", kind_name(s->pid));
    }
    detach(s);
    free(s);
}
