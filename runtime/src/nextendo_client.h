#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nx::nextendo {

struct Status {
    bool enabled = false;
    bool signed_in = false;
    bool login_in_progress = false;
    std::string client_id;
    uint64_t pid = 0;
    std::string username;
    std::string friend_code;
    std::string last_error;
};

void init(const std::string& data_root);
void shutdown();

Status status();
void set_enabled(bool enabled);
void set_client_id(const std::string& client_id);

// Starts OAuth 2.0 PKCE login on http://127.0.0.1:47823/callback and opens the system browser.
bool begin_oauth_login();
void sign_out();

// Ensures we have a valid nex_token (refreshing via OAuth if needed) and returns a signed RS256 BAAS id_token JWT.
std::string get_baas_id_token(const std::string& game_version = "1.2.1");
uint64_t current_pid();

// Starts the /api/nex-session heartbeat thread when online mode is active.
void start_session_heartbeat();

struct FriendInfo {
    uint64_t pid = 0;
    std::string username;
    uint32_t presence_status = 2;  // 0=Offline, 1=Online, 2=OnlinePlay
    bool same_app = true;
};

// Returns friends from <data_root>/friends.txt (if present) or Nextendo GET /api/friends (cached 10s).
std::vector<FriendInfo> get_friends();

}  // namespace nx::nextendo
