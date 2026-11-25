// Standalone tests for ExeFS, RomFS, pair compatibility, and compiled output directory checks.
#include "launcher/game_version.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> hex_bytes(const char* hex) {
    std::vector<uint8_t> out;
    const size_t n = std::strlen(hex);
    assert(n % 2 == 0);
    for (size_t i = 0; i < n; i += 2) {
        unsigned b = 0;
        for (int k = 0; k < 2; ++k) {
            char c = hex[i + k];
            b <<= 4;
            if (c >= '0' && c <= '9') b |= unsigned(c - '0');
            else if (c >= 'A' && c <= 'F') b |= unsigned(c - 'A' + 10);
            else if (c >= 'a' && c <= 'f') b |= unsigned(c - 'a' + 10);
            else assert(false);
        }
        out.push_back(uint8_t(b));
    }
    return out;
}

void write_bytes(const fs::path& p, const std::vector<uint8_t>& data) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    assert(f.good());
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    assert(f.good());
}

void write_text(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::trunc);
    assert(f.good());
    f << text;
    assert(f.good());
}

std::vector<uint8_t> make_nso(const char* build_id_hex = nullptr) {
    std::vector<uint8_t> hdr(0x80, 0);
    std::memcpy(hdr.data(), "NSO0", 4);
    if (build_id_hex) {
        const auto bid = hex_bytes(build_id_hex);
        assert(bid.size() <= 32);
        std::memcpy(hdr.data() + 0x40, bid.data(), bid.size());
    }
    return hdr;
}

std::vector<uint8_t> make_npdm(uint64_t title_id) {
    std::vector<uint8_t> buf(0x40, 0);
    std::memcpy(buf.data(), "META", 4);
    std::memcpy(buf.data() + 0x20, "ACI0", 4);
    std::memcpy(buf.data() + 0x30, &title_id, sizeof title_id);
    return buf;
}

void make_exefs(const fs::path& dir, const char* build_id_hex, uint64_t title_id = 0x010015100B514000ull) {
    write_bytes(dir / "main", make_nso(build_id_hex));
    write_bytes(dir / "sdk", make_nso("3053B22935E32DDF15EF81997603E5C267534A90"));
    write_bytes(dir / "main.npdm", make_npdm(title_id));
}

void make_romfs(const fs::path& dir, const std::vector<int>& rstb_products) {
    write_bytes(dir / "Shader" / "Secred.Nin_NX_NVN.release.sarc.zs", {0x28, 0xB5, 0x2F, 0xFD});
    for (int p : rstb_products) {
        write_bytes(dir / "System" / "Resource" / ("ResourceSizeTable.Product." + std::to_string(p) + ".rsizetable.zs"),
                    {0x28, 0xB5, 0x2F, 0xFD});
    }
}

void make_output(const fs::path& dir, const char* version = nullptr, const char* main_bin_size_tag = nullptr) {
    write_text(dir / "game" / "wonder_game.dll", "stub");
    if (version) {
        write_text(dir / "game" / "build-id.txt",
                   std::string("kit kit1\ninput inp1\nversion ") + version + "\n");
    } else {
        write_text(dir / "game" / "build-id.txt", "kit kit1\ninput inp1\n");
    }
    if (main_bin_size_tag && std::strcmp(main_bin_size_tag, "1.2.1") == 0) {
        std::vector<uint8_t> big(0x3728df8ull, 0);
        write_bytes(dir / "analysis" / "main.bin", big);
    } else if (main_bin_size_tag && std::strcmp(main_bin_size_tag, "1.0.0") == 0) {
        std::vector<uint8_t> big(0x359cc28ull, 0);
        write_bytes(dir / "analysis" / "main.bin", big);
    } else {
        write_text(dir / "analysis" / "main.bin", "main");
    }
    write_text(dir / "analysis" / "sdk.bin", "sdk");
    write_text(dir / "shader_programs.bin", "cat");
    write_text(dir / "spirv" / "results.tsv", "ok\n");
    make_romfs(dir / "extract" / "romfs", {120});
}

}  // namespace

int main() {
    using namespace nx::launcher;
    const fs::path tmp = fs::temp_directory_path() /
                         ("superwonders_ver_test_" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(tmp);

    // 1. Supported ExeFS + RomFS: 1.0.0 and 1.2.1
    const fs::path ex100_dir = tmp / "ex100";
    const fs::path rom100_dir = tmp / "rom100";
    make_exefs(ex100_dir, "CD6E42AEE7934F4D8393CD43893AE25EDC83D338");
    make_romfs(rom100_dir, {100});
    const ExefsInfo ex100 = inspect_exefs(ex100_dir);
    const RomfsInfo rom100 = inspect_romfs(rom100_dir);
    assert(ex100.valid_nso && ex100.compatible && ex100.version == "1.0.0" && ex100.expected_romfs_product == 100);
    assert(rom100.valid && rom100.compatible && rom100.product == 100);
    assert(evaluate_pair(ex100, rom100).state == PairState::Compatible);

    const fs::path ex121_dir = tmp / "ex121";
    const fs::path rom121_dir = tmp / "rom121_merged";
    make_exefs(ex121_dir, "FF773E90972D544EB79406EAA65396D53C43EFB9");
    make_romfs(rom121_dir, {100, 120});  // merged base + update must pick Product.120!
    const ExefsInfo ex121 = inspect_exefs(ex121_dir);
    const RomfsInfo rom121 = inspect_romfs(rom121_dir);
    assert(ex121.valid_nso && ex121.compatible && ex121.version == "1.2.1" && ex121.expected_romfs_product == 120);
    assert(rom121.valid && rom121.compatible && rom121.product == 120);
    assert(evaluate_pair(ex121, rom121).state == PairState::Compatible);

    // 2. Version mismatch
    assert(evaluate_pair(ex121, rom100).state == PairState::Mismatch);
    assert(evaluate_pair(ex100, rom121).state == PairState::Mismatch);

    // 3. Known unsupported versions (1.0.1 and 1.2.0) and unknown Build ID
    const fs::path ex101_dir = tmp / "ex101";
    const fs::path rom101_dir = tmp / "rom101";
    make_exefs(ex101_dir, "F91868B88F60D3D500112233445566778899AABB");
    make_romfs(rom101_dir, {101});
    const ExefsInfo ex101 = inspect_exefs(ex101_dir);
    const RomfsInfo rom101 = inspect_romfs(rom101_dir);
    assert(ex101.valid_nso && !ex101.compatible && ex101.version == "1.0.1");
    assert(rom101.valid && !rom101.compatible && rom101.product == 101);
    assert(evaluate_pair(ex101, rom101).state == PairState::Unsupported);

    const fs::path ex120_dir = tmp / "ex120";
    make_exefs(ex120_dir, "B37398A1B68747B700112233445566778899AABB");
    const ExefsInfo ex120 = inspect_exefs(ex120_dir);
    assert(ex120.valid_nso && !ex120.compatible && ex120.version == "1.2.0");
    assert(evaluate_pair(ex120, rom121).state == PairState::Unsupported);

    const fs::path exunk_dir = tmp / "exunk";
    make_exefs(exunk_dir, "0123456789ABCDEF0123456789ABCDEF01234567");
    const ExefsInfo exunk = inspect_exefs(exunk_dir);
    assert(exunk.valid_nso && !exunk.compatible && exunk.version.empty());
    assert(evaluate_pair(exunk, rom121).state == PairState::Unsupported);

    // 4. Wrong game & invalid files
    const fs::path wrong_dir = tmp / "wrong";
    make_exefs(wrong_dir, "FF773E90972D544EB79406EAA65396D53C43EFB9", 0x0100000000010000ull);
    const ExefsInfo ex_wrong = inspect_exefs(wrong_dir);
    assert(ex_wrong.wrong_game && !ex_wrong.compatible);
    assert(evaluate_pair(ex_wrong, rom121).state == PairState::WrongGame);

    const fs::path bad_nso = tmp / "bad_nso";
    write_text(bad_nso / "main", "not nso");
    write_text(bad_nso / "sdk", "not nso");
    assert(evaluate_pair(inspect_exefs(bad_nso), rom100).state == PairState::Invalid);
    assert(evaluate_pair(ex100, inspect_romfs(tmp / "missing_romfs")).state == PairState::Invalid);
    assert(evaluate_pair(ExefsInfo{}, rom100).state == PairState::Incomplete);

    // 5. Output folder completeness checks
    const fs::path out_empty = tmp / "out_empty";
    fs::create_directories(out_empty);
    assert(!inspect_output_dir(out_empty, "wonder_game.dll").ready);

    const fs::path out_ok = tmp / "out_ok";
    make_output(out_ok, "1.2.1");
    const OutputInfo oi = inspect_output_dir(out_ok, "wonder_game.dll");
    assert(oi.ready && oi.installed_version == "1.2.1");

    // Legacy output without `version` line in build-id.txt infers 1.2.1 from main.bin size
    const fs::path out_legacy = tmp / "out_legacy";
    make_output(out_legacy, nullptr, "1.2.1");
    const OutputInfo oi_legacy = inspect_output_dir(out_legacy, "wonder_game.dll");
    assert(oi_legacy.ready && oi_legacy.installed_version == "1.2.1");

    // Missing any required part makes ready=false
    const fs::path out_missing_shaders = tmp / "out_missing_shaders";
    make_output(out_missing_shaders, "1.0.0");
    fs::remove(out_missing_shaders / "shader_programs.bin");
    assert(!inspect_output_dir(out_missing_shaders, "wonder_game.dll").ready);

    fs::remove_all(tmp);
    return 0;
}
