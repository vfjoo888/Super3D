#include "user_profiles.h"
#include <nx/game.h>
#include <filesystem>
#include <string_view>
static inline std::filesystem::path nx_u8path(std::string_view s) { return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(s.data()), s.size())); }
#include "platform.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <system_error>

namespace nx::profiles {
namespace {
constexpr uint64_t legacy_hi = 0x4E58485553455231ull;
std::filesystem::path path(const std::filesystem::path& root) { return root / "user" / "profiles.txt"; }
}
const User& current() {
    static const User selected = [] {
        const Database db = load(nx_u8path(plat::data_root()));
        return db.users[db.selected];
    }();
    return selected;
}
Database load(const std::filesystem::path& root) {
    Database db;
    std::ifstream f(path(root));
    if (!f) {
        std::error_code ec;
        db.valid = !std::filesystem::exists(path(root), ec) && !ec;
        return db;
    }
    auto invalid = [&] { db.valid = false; return db; };
    std::string magic;
    size_t selected = 0, count = 0;
    if (!(f >> magic >> selected >> count) || magic != "SUPERWONDERS_PROFILES_1" || count == 0 || count > 8)
        return invalid();
    std::vector<User> users;
    for (size_t i = 0; i < count; ++i) {
        User u;
        if (!(f >> std::hex >> u.hi >> u.lo >> std::dec >> std::quoted(u.name) >> std::quoted(u.icon)) ||
            (!u.hi && !u.lo) || u.name.empty() || u.name.size() > 31 || u.icon.size() > 4096)
            return invalid();
        for (const auto& old : users) if (old.lo == u.lo && old.hi == u.hi) return invalid();
        users.push_back(std::move(u));
    }
    db.users = std::move(users);
    db.selected = selected < db.users.size() ? selected : 0;
    return db;
}
bool save(const std::filesystem::path& root, const Database& db) {
    if (!db.valid || db.users.empty() || db.users.size() > 8 || db.selected >= db.users.size()) return false;
    std::error_code ec;
    std::filesystem::create_directories(path(root).parent_path(), ec);
    if (ec) return false;
    auto tmp = path(root); tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return false;
        f << "SUPERWONDERS_PROFILES_1 " << db.selected << ' ' << db.users.size() << '\n';
        for (const User& u : db.users)
            f << std::hex << u.hi << ' ' << u.lo << std::dec << ' ' << std::quoted(u.name) << ' ' << std::quoted(u.icon) << '\n';
        f.flush();
        if (!f) return false;
    }
#if defined(_WIN32)
    // Windows rename cannot replace an existing file. Keep the original until replacement succeeds.
    auto backup = path(root); backup += ".bak";
    std::filesystem::remove(backup, ec); ec.clear();
    if (std::filesystem::exists(path(root))) {
        std::filesystem::rename(path(root), backup, ec);
        if (ec) return false;
    }
    std::filesystem::rename(tmp, path(root), ec);
    if (ec) { std::error_code ignored; std::filesystem::rename(backup, path(root), ignored); return false; }
    std::filesystem::remove(backup, ec);
    return true;
#else
    std::filesystem::rename(tmp, path(root), ec);
    return !ec;
#endif
}
bool valid_icon(const std::vector<uint8_t>& image) {
    if (image.size() < 4 || image.size() > 0x20000 || image[0] != 0xff || image[1] != 0xd8) return false;
    size_t i = 2;
    while (i + 3 < image.size()) {
        if (image[i++] != 0xff) return false;
        while (i < image.size() && image[i] == 0xff) ++i;
        if (i >= image.size()) return false;
        const uint8_t marker = image[i++];
        if (marker == 0xd9 || marker == 0xda) return false;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (i + 1 >= image.size()) return false;
        const size_t len = (size_t(image[i]) << 8) | image[i + 1];
        if (len < 2 || len > image.size() - i) return false;
        if ((marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
            (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf)) {
            if (len < 8) return false;
            const int height = (int(image[i + 3]) << 8) | image[i + 4];
            const int width = (int(image[i + 5]) << 8) | image[i + 6];
            return width == 256 && height == 256;
        }
        i += len;
    }
    return false;
}
User make_user(const Database& db, const std::string& name) {
    std::random_device rng;
    User u;
    u.name = name;
    do {
        u.lo = (uint64_t(rng()) << 32) | rng();
        u.hi = (uint64_t(rng()) << 32) | rng();
    } while ((!u.lo && !u.hi) || std::any_of(db.users.begin(), db.users.end(), [&](const User& old) {
        return old.lo == u.lo && old.hi == u.hi;
    }));
    return u;
}
std::filesystem::path save_path(const std::filesystem::path& root, const User& user) {
    if (user.lo == 1 && user.hi == legacy_hi) return root / "user" / "save";
    char uid[33];
    std::snprintf(uid, sizeof uid, "%016llx%016llx", (unsigned long long)user.hi, (unsigned long long)user.lo);
    return root / "user" / "profiles" / uid / "save";
}
static bool copy_save_dir(const std::filesystem::path& from, const std::filesystem::path& to,
                          std::filesystem::path& backup, std::string& error) {
    namespace fs = std::filesystem;
    backup.clear(); error.clear();
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(from, ec)) || ec || !fs::is_directory(from, ec) || ec) {
        error = "The source save directory is not accessible."; return false;
    }
    // Refuse links and special files so copies cannot escape their directories.
    for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        const auto status = it->symlink_status(ec);
        if (ec || (!fs::is_regular_file(status) && !fs::is_directory(status))) {
            error = "The source save contains an unsupported file or link."; return false;
        }
    }
    if (ec) { error = "Could not read the source save."; return false; }
    fs::create_directories(to.parent_path(), ec);
    if (ec) { error = "Could not prepare the destination directory."; return false; }
    const auto status = fs::symlink_status(to, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) { error = "Could not inspect the destination."; return false; }
    const bool exists = fs::exists(status);
    if (exists && (!fs::is_directory(status) || fs::is_symlink(status))) {
        error = "The destination is not a normal save directory."; return false;
    }
    ec.clear();
    fs::path stage;
    for (unsigned i = 0; i < 10000; ++i) {
        stage = to.parent_path() / ("save.copy-staging-" + std::to_string(i));
        if (fs::create_directory(stage, ec)) break;
        if (ec) { error = "Could not create the staging directory."; return false; }
        stage.clear();
    }
    if (stage.empty()) { error = "No staging directory is available."; return false; }
    fs::copy(from, stage, fs::copy_options::recursive, ec);
    if (ec) { std::error_code ignored; fs::remove_all(stage, ignored); error = "Could not copy the source save."; return false; }
    if (exists) {
        for (unsigned i = 0; i < 10000; ++i) {
            backup = to.parent_path() / ("save.backup-" + std::to_string(i));
            if (!fs::exists(backup, ec) && !ec) break;
            if (ec) break;
            backup.clear();
        }
        if (ec || backup.empty()) { std::error_code ignored; fs::remove_all(stage, ignored); error = "Could not reserve a backup name."; return false; }
        fs::rename(to, backup, ec);
        if (ec) { std::error_code ignored; fs::remove_all(stage, ignored); backup.clear(); error = "Could not back up the destination save."; return false; }
    }
    fs::rename(stage, to, ec);
    if (ec) {
        std::error_code ignored;
        if (exists) {
            fs::rename(backup, to, ignored);
            if (ignored) { error = "Copy failed; the original save remains at " + backup.string(); return false; }
            backup.clear();
        }
        fs::remove_all(stage, ignored);
        error = "Could not install the copied save."; return false;
    }
    return true;
}

bool copy_save(const std::filesystem::path& root, const User& source, const User& destination,
               std::filesystem::path& backup, std::string& error) {
    if (source.lo == destination.lo && source.hi == destination.hi) {
        backup.clear();
        error = "Choose two different profiles.";
        return false;
    }
    return copy_save_dir(save_path(root, source), save_path(root, destination), backup, error);
}

bool import_save(const std::filesystem::path& root, const std::filesystem::path& source_dir,
                 const User& destination, std::filesystem::path& backup, std::string& error) {
    namespace fs = std::filesystem;
    backup.clear(); error.clear();
    const bool sm3dw = game::kInfo.program_id == 0x010028600EBDA000ull;
    auto has_save = [sm3dw](const fs::path& dir) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec) || ec) return false;
        if (sm3dw) return fs::is_regular_file(dir / "GameData.bin", ec) && !ec;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!ec && entry.is_regular_file(ec) && entry.path().extension() == ".sav") return true;
        }
        return false;
    };
    fs::path from = source_dir;
    if (!has_save(from)) {
        if (has_save(source_dir / "0")) from = source_dir / "0";
        else {
            error = sm3dw ? "Selected folder does not contain Super3D save files (GameData.bin)."
                          : "Selected folder does not contain Wonder save files (*.sav).";
            return false;
        }
    }
    return copy_save_dir(from, save_path(root, destination), backup, error);
}

std::vector<EmulatorSave> detect_emulator_saves() {
    namespace fs = std::filesystem;
    std::vector<EmulatorSave> out;
    std::error_code ec;
    const bool sm3dw = game::kInfo.program_id == 0x010028600EBDA000ull;

    auto is_game_save = [sm3dw](const fs::path& dir) {
        std::error_code lec;
        if (!fs::is_directory(dir, lec) || lec) return false;
        if (sm3dw) return fs::is_regular_file(dir / "GameData.bin", lec) && !lec;
        return fs::is_regular_file(dir / "game_data.sav", lec) && !lec &&
               fs::is_regular_file(dir / "game_account.sav", lec) && !lec;
    };
    auto add_unique = [&](std::string label, const fs::path& p) {
        const fs::path norm = p.lexically_normal();
        for (const auto& existing : out) {
            if (existing.path == norm) return;
        }
        out.push_back({std::move(label), norm});
    };
    auto short_id = [](const std::string& id) {
        if (id.size() <= 8) return id;
        return id.substr(0, 4) + "..." + id.substr(id.size() - 4);
    };

    struct Root { const char* name; fs::path path; };
    std::vector<Root> nand_roots;
    std::vector<Root> ryu_roots;
    static constexpr std::pair<const char*, const char*> kNandEmus[] = {
        {"Eden", "eden"}, {"Yuzu", "yuzu"}, {"Sudachi", "sudachi"}, {"Suyu", "suyu"}, {"Citron", "citron"},
    };
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata) {
        const fs::path base(appdata);
        for (const auto& [name, dir] : kNandEmus)
            nand_roots.push_back({name, base / dir / "nand" / "user" / "save"});
        ryu_roots.push_back({"Ryujinx", base / "Ryujinx" / "bis" / "user" / "save"});
        ryu_roots.push_back({"Ryubing", base / "Ryubing" / "bis" / "user" / "save"});
    }
#else
    const char* home = std::getenv("HOME");
    const char* xdg_data = std::getenv("XDG_DATA_HOME");
    const char* xdg_cfg = std::getenv("XDG_CONFIG_HOME");
    const fs::path data_base = xdg_data && *xdg_data ? fs::path(xdg_data)
                                                     : (home && *home ? fs::path(home) / ".local" / "share" : fs::path{});
    const fs::path cfg_base = xdg_cfg && *xdg_cfg ? fs::path(xdg_cfg)
                                                  : (home && *home ? fs::path(home) / ".config" : fs::path{});
    if (!data_base.empty()) {
        for (const auto& [name, dir] : kNandEmus)
            nand_roots.push_back({name, data_base / dir / "nand" / "user" / "save"});
    }
    if (!cfg_base.empty()) {
        ryu_roots.push_back({"Ryujinx", cfg_base / "Ryujinx" / "bis" / "user" / "save"});
        ryu_roots.push_back({"Ryubing", cfg_base / "Ryubing" / "bis" / "user" / "save"});
    }
#endif

    const char* tids[2] = {sm3dw ? "010028600EBDA000" : "010015100B514000",
                           sm3dw ? "010028600ebda000" : "010015100b514000"};
    for (const auto& [emu_name, save_root] : nand_roots) {
        ec.clear();
        if (!fs::is_directory(save_root, ec) || ec) continue;
        for (const auto& acct : fs::directory_iterator(save_root, ec)) {
            if (ec) break;
            std::error_code dec;
            if (!acct.is_directory(dec) || dec) continue;
            for (const auto& uid_dir : fs::directory_iterator(acct.path(), dec)) {
                if (dec) break;
                std::error_code uec;
                if (!uid_dir.is_directory(uec) || uec) continue;
                for (const char* tid : tids) {
                    const fs::path candidate = uid_dir.path() / tid;
                    if (is_game_save(candidate)) {
                        add_unique(std::string(emu_name) + " (" + short_id(uid_dir.path().filename().string()) + ")",
                                   candidate);
                    }
                }
            }
        }
    }

    for (const auto& [emu_name, save_root] : ryu_roots) {
        ec.clear();
        if (!fs::is_directory(save_root, ec) || ec) continue;
        for (const auto& slot_parent : fs::directory_iterator(save_root, ec)) {
            if (ec) break;
            std::error_code sec;
            if (!slot_parent.is_directory(sec) || sec) continue;
            const fs::path slot0 = slot_parent.path() / "0";
            const fs::path slot1 = slot_parent.path() / "1";
            const fs::path candidate = is_game_save(slot0) ? slot0 : (is_game_save(slot1) ? slot1 : fs::path{});
            if (!candidate.empty()) {
                std::string id = slot_parent.path().filename().string();
                size_t nz = id.find_first_not_of('0');
                if (nz != std::string::npos) id = id.substr(nz);
                add_unique(std::string(emu_name) + " (save " + id + ")", candidate);
            }
        }
    }
    return out;
}

}
