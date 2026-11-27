#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nx::profiles {
struct User {
    uint64_t lo = 1, hi = 0x4E58485553455231ull;
    std::string name = "Super3D";
    std::string icon;
};
struct Database {
    std::vector<User> users{User{}};
    size_t selected = 0;
    bool valid = true;
};
const User& current();
Database load(const std::filesystem::path& root);
bool save(const std::filesystem::path& root, const Database& db);
bool valid_icon(const std::vector<uint8_t>& image);
User make_user(const Database& db, const std::string& name);
std::filesystem::path save_path(const std::filesystem::path& root, const User& user);
// Copies through a staging directory and preserves any old destination as a sibling backup.
bool copy_save(const std::filesystem::path& root, const User& source, const User& destination,
               std::filesystem::path& backup, std::string& error);
struct EmulatorSave {
    std::string label;
    std::filesystem::path path;
};
std::vector<EmulatorSave> detect_emulator_saves();
bool import_save(const std::filesystem::path& root, const std::filesystem::path& source_dir,
                 const User& destination, std::filesystem::path& backup, std::string& error);
}
