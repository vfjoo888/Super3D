// Super Mario 3D World + Bowser's Fury (010028600EBDA000, 1.0.0 / 1.2.1): game-specific hooks for Super3D.
#include <nx/game.h>
#include <nx/runtime.h>

namespace nx {

namespace game {

const Info kInfo = {
    "sm3dw",
    "Super Mario 3D World + Bowser's Fury",
    0x010028600EBDA000ull,
    "com.super3d.sm3dw",
    "private/sm3dw",
    0,  // main.npdm META +0x14: system_resource_size = 0
};

void after_init() {
    log("sm3dw: after_init (Super3D)");
}

void renderer_start() {
    log("sm3dw: renderer_start");
}

void first_frame() {
    log("sm3dw: first_frame presented!");
}

void frame_presented(uint64_t /*now_ns*/, int /*present_interval*/) {}

void file_opened(const char* /*path*/) {}

}  // namespace game

}  // namespace nx
