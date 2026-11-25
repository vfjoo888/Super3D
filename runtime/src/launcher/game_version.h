// Game version and compatibility detection for Super Mario Bros. Wonder ExeFS, RomFS, and output folders.
#pragma once

#include <filesystem>
#include <string>

namespace nx::launcher {

struct ExefsInfo {
    bool present = false;
    bool valid_nso = false;
    bool wrong_game = false;
    bool compatible = false;
    std::string version;
    std::string build_id;
    std::string program_id;
    int expected_romfs_product = 0;
    std::string summary;
};

struct RomfsInfo {
    bool present = false;
    bool valid = false;
    bool compatible = false;
    int product = 0;
    std::string version_label;
    std::string summary;
};

enum class PairState {
    Incomplete,
    Compatible,
    Mismatch,
    Unsupported,
    WrongGame,
    Invalid,
};

struct PairInfo {
    PairState state = PairState::Incomplete;
    std::string badge;
    std::string detail;
};

struct OutputInfo {
    bool ready = false;
    bool has_gamelib = false;
    bool has_analysis = false;
    bool has_shaders = false;
    bool has_romfs = false;
    std::string installed_version;
    std::string missing_reason;
};

ExefsInfo inspect_exefs(const std::filesystem::path& dir);
RomfsInfo inspect_romfs(const std::filesystem::path& dir);
PairInfo evaluate_pair(const ExefsInfo& ex, const RomfsInfo& rom);
OutputInfo inspect_output_dir(const std::filesystem::path& dir, const std::string& gamelib_filename,
                              const std::string& fallback_gamelib_path = {});

}  // namespace nx::launcher
