// nn::pl (shared system fonts) at the API level.
//
// On the console the pl sysmodule loads the system fonts (system archives 0100000000000810-814) into shared memory
// in BFTTF form: an 8-byte header {magic, size} and the TrueType data, both XORed with a key. The game hands that
// to nn::fontll::ScalableFontEngineHelper::Decode (sdk+3093c0 in SDK 16), which accepts the data already decoded
// when the first word is the plain magic 0x18029A7F, and then only skips the header.
//
// So any TrueType font works: <data root>/fonts/shared_<type>.ttf (0 standard, 1 simplified Chinese, 2 its
// extension, 3 traditional Chinese, 4 Korean, 5 Nintendo extension), else shared.ttf, else the host's own
// (NX_SHARED_FONT, or a system font on macOS, Android and Windows). The fonts on our own firmware dump can go there once decoded.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <nx/runtime.h>

#include "platform.h"
#include "ttf_cmap_remap.h"

namespace nx {

namespace {

#if defined(__has_embed)
#if __has_embed("../../third_party/fonts/Nunito/Nunito-SemiBold.ttf")
#define NX_PL_EMBED_FONT 1
static constexpr uint8_t kFallbackFont[] = {
#embed "../../third_party/fonts/Nunito/Nunito-SemiBold.ttf"
};
#endif
#endif

struct Font {
    uint64_t addr = 0, size = 0;  // header + data, in guest memory
};

std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> v;
    FILE* f = plat::fopen_utf8(path, "rb");
    if (!f) return v;
    plat::fseek64(f, 0, SEEK_END);
    int64_t n = plat::ftell64(f);
    plat::fseek64(f, 0, SEEK_SET);
    if (n > 0) {
        v.resize(size_t(n));
        v.resize(std::fread(v.data(), 1, v.size(), f));
    }
    std::fclose(f);
    return v;
}

const Font& font(uint32_t type) {
    static std::mutex lock;
    static Font fonts[6];
    std::lock_guard lk(lock);
    Font& f = fonts[type < 6 ? type : 0];
    if (f.addr) return f;
    std::string root = plat::data_root() + "/fonts/";
    std::vector<std::string> candidates = {root + "shared_" + std::to_string(type) + ".ttf", root + "shared.ttf"};
    if (const char* e = std::getenv("NX_SHARED_FONT")) candidates.push_back(e);
#if defined(__APPLE__)
    candidates.push_back("/System/Library/Fonts/Supplemental/Arial Unicode.ttf");
    candidates.push_back("/System/Library/Fonts/Supplemental/Arial.ttf");
#elif defined(__ANDROID__)
    candidates.push_back("/system/fonts/Roboto-Regular.ttf");
#elif defined(_WIN32)
    // Arial Unicode (Office installs it) covers the most; else Arial, as on the Mac.
    candidates.push_back("C:/Windows/Fonts/arialuni.ttf");
    candidates.push_back("C:/Windows/Fonts/arial.ttf");
#elif defined(__linux__)
    // Common distribution fonts across Debian/Ubuntu, Fedora, Arch, openSUSE, and SteamOS.
    candidates.push_back("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/google-noto/NotoSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/noto/NotoSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/liberation/LiberationSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    candidates.push_back("/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf");
    candidates.push_back("/usr/share/fonts/TTF/DejaVuSans.ttf");
    candidates.push_back("/usr/share/fonts/gnu-free/FreeSans.ttf");
#endif
    std::vector<uint8_t> ttf;
    for (const auto& path : candidates) {
        ttf = read_file(path);
        if (!ttf.empty()) {
            log("pl: shared font %u <- %s (%zu bytes)", type, path.c_str(), ttf.size());
            break;
        }
    }
#ifdef NX_PL_EMBED_FONT
    if (ttf.empty()) {
        ttf.assign(kFallbackFont, kFallbackFont + sizeof(kFallbackFont));
        log("pl: shared font %u <- embedded Nunito fallback (%zu bytes)", type, ttf.size());
    }
#endif
    if (ttf.empty() && type != 5) {
        ttf = read_file(root + "shared_5.ttf");
        if (!ttf.empty()) log("pl: shared font %u <- %sshared_5.ttf (%zu bytes)", type, root.c_str(), ttf.size());
    }
    if (ttf.empty()) log("pl: no font for shared font %u (put one in %s)", type, root.c_str());
#if (defined(__linux__) && !defined(__ANDROID__)) || defined(_WIN32)
    if (type == 5 && !ttf.empty()) {
        const plat::ButtonPromptConfig cfg = plat::button_prompt_config();
        bool remapped = remap_button_font_cmap(ttf, cfg.style, cfg.bindings);
        if (!remapped) {
            log("pl: shared_5.ttf lacks dynamic button icons; regenerating...");
            plat::make_dirs(root);
            const std::string target = root + "shared_5.ttf";
            if (plat::upgrade_button_font(target)) {
                std::vector<uint8_t> upgraded = read_file(target);
                if (!upgraded.empty() && remap_button_font_cmap(upgraded, cfg.style, cfg.bindings)) {
                    ttf = std::move(upgraded);
                    remapped = true;
                }
            }
        }
        if (remapped) {
            const char* style_name = cfg.style == 3 ? "PlayStation" : cfg.style == 2 ? "Xbox" : "Nintendo";
            log("pl: remapped button icons (%s, A->phy %d, B->phy %d, X->phy %d, Y->phy %d)",
                style_name, cfg.bindings[0], cfg.bindings[1], cfg.bindings[2], cfg.bindings[3]);
        } else {
            log("pl: button icons not remapped (update data/fonts/shared_5.ttf with make_button_font.py)");
        }
    }
#endif
    // SM3DW passes GetSharedFontAddress() directly to ScalableFontEngine::LoadFont without calling Decode().
    f.size = ttf.size();
    f.addr = kernel_alloc(f.size ? f.size : 8);
    if (!ttf.empty()) std::memcpy(reinterpret_cast<void*>(f.addr), ttf.data(), ttf.size());
    return f;
}

void GetSharedFontLoadState(CPU& c) { c.x[0] = 1; }  // SharedFontLoadState_Loaded
void GetSharedFontAddress(CPU& c) { c.x[0] = font(uint32_t(c.x[0])).addr; }
void GetSharedFontSize(CPU& c) { c.x[0] = font(uint32_t(c.x[0])).size; }

}  // namespace

NX_HLE("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", GetSharedFontLoadState);
NX_HLE("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", GetSharedFontAddress);
NX_HLE("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", GetSharedFontSize);

}  // namespace nx
