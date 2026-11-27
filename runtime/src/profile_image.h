#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>
namespace nx::profiles {
std::vector<uint8_t> builtin_icon(int index);
std::vector<uint8_t> default_game_icon();
std::vector<uint8_t> import_icon(const std::filesystem::path& path);
std::vector<uint8_t> decode_icon_rgba(const std::filesystem::path& path, int out_size = 128);
std::vector<uint8_t> decode_default_game_icon_rgba(int out_size = 128);
}
