// Lightweight Discord Rich Presence via Discord's local IPC pipe/socket (no external SDK required).
#pragma once

namespace nx::discord {

void init(const char* default_app_id);
void set_enabled(bool enabled);
void set_activity(const char* details, const char* state = nullptr);
void shutdown();

}  // namespace nx::discord
