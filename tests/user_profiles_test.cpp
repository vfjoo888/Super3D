#ifdef NDEBUG
#undef NDEBUG
#endif
#include "user_profiles.h"
#include "profile_image.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace nx::plat { std::string data_root() { return {}; } }
int main() {
    namespace fs = std::filesystem;
    namespace p = nx::profiles;
    auto root = fs::temp_directory_path() / ("superwonders-profile-test-" + std::to_string(std::random_device{}()));
    const std::vector<uint8_t> jpg = {0xff,0xd8,0xff,0xc0,0,8,8,1,0,1,0,3};
    assert(p::valid_icon(jpg));
    auto large = jpg; large[7] = 2;
    assert(!p::valid_icon(large));
    assert(!p::valid_icon({0xff, 0xd8}));
    fs::create_directories(root);
    for (int i = 0; i < 6; ++i) assert(p::valid_icon(p::builtin_icon(i)));
    assert(p::builtin_icon(-1).empty());
    auto generated = p::builtin_icon(0);
    { std::ofstream f(root / "input.jpg", std::ios::binary); f.write((const char*)generated.data(), generated.size()); }
    assert(p::valid_icon(p::import_icon(root / "input.jpg")));
    assert(p::import_icon(root / "missing.png").empty());
    auto original = p::load(root);
    assert(original.users.size() == 1 && original.users[0].name == "Zagreus");
    assert(p::save_path(root, original.users[0]) == root / "user" / "save");
    auto user = p::make_user(original, "New user");
    user.icon = "/some path/image.jpg";
    original.users.push_back(user);
    original.selected = 1;
    assert(p::save(root, original));
    auto loaded = p::load(root);
    assert(loaded.selected == 1 && loaded.users.size() == 2);
    assert(loaded.users[1].lo == user.lo && loaded.users[1].hi == user.hi);
    assert(loaded.users[1].name == user.name && loaded.users[1].icon == user.icon);
    assert(p::save_path(root, loaded.users[1]) != p::save_path(root, loaded.users[0]));
    fs::path backup;
    std::string error;
    assert(!p::copy_save(root, loaded.users[0], loaded.users[0], backup, error));
    assert(!p::copy_save(root, loaded.users[0], loaded.users[1], backup, error));
    auto src = p::save_path(root, loaded.users[0]);
    auto dst = p::save_path(root, loaded.users[1]);
    fs::create_directories(src / "sub");
    { std::ofstream f(src / "game_data.sav"); f << "source"; }
    { std::ofstream f(src / "sub" / "other.sav"); f << "nested"; }
    assert(p::copy_save(root, loaded.users[0], loaded.users[1], backup, error));
    assert(backup.empty() && fs::exists(dst / "game_data.sav") && fs::exists(dst / "sub" / "other.sav"));
    { std::ofstream f(dst / "old.sav"); f << "old"; }
    assert(p::copy_save(root, loaded.users[0], loaded.users[1], backup, error));
    assert(!backup.empty() && fs::exists(backup / "old.sav") && !fs::exists(dst / "old.sav"));
    assert(fs::exists(dst / "game_data.sav"));
    auto first_backup = backup;
    assert(p::copy_save(root, loaded.users[0], loaded.users[1], backup, error));
    assert(backup != first_backup && fs::exists(first_backup / "old.sav"));
#ifndef _WIN32
    fs::create_symlink("/tmp", src / "link");
    assert(!p::copy_save(root, loaded.users[0], loaded.users[1], backup, error));
    assert(fs::exists(dst / "game_data.sav"));
#endif
    loaded.users[1].name = "Renamed";
    assert(p::save(root, loaded));
    assert(p::load(root).users[1].name == "Renamed");
    { std::ofstream f(root / "user" / "profiles.txt", std::ios::trunc); f << "invalid"; }
    auto corrupt = p::load(root);
    assert(!corrupt.valid && !p::save(root, corrupt));
    fs::remove_all(root);
}
