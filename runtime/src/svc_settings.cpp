// nn::settings::ISettingsServer: system language codes used by the game's locale selection.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>

#include "ipc.h"
#include "svc_am.h"

namespace nx {
namespace {

constexpr const char* kLanguages[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt",
                                      "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR", "pl", "th"};
constexpr size_t kLegacyCount = 15;
constexpr uint32_t kInvalidLanguage = (625u << 9) | 105u;  // Settings module, invalid language

uint64_t language_code(const char* language) {
    uint64_t result = 0;
    std::memcpy(&result, language, std::strlen(language));
    return result;
}

uint32_t available(IpcCtx& c, size_t limit) {
    const Buffer out = c.out_buf(0);
    const size_t count = out.addr ? std::min(limit, size_t(out.size / sizeof(uint64_t))) : 0;
    for (size_t i = 0; i < count; ++i) st<uint64_t>(out.addr + i * sizeof(uint64_t), language_code(kLanguages[i]));
    c.push<int32_t>(int32_t(count));
    return 0;
}

ServiceRegistrar settings("set", [] {
    auto service = std::make_shared<Service>("ISettingsServer");
    service->cmds[0] = [](IpcCtx& c) { c.push<uint64_t>(game_language_code()); return 0u; };
    service->cmds[1] = [](IpcCtx& c) { return available(c, kLegacyCount); };
    service->cmds[2] = [](IpcCtx& c) {
        const int32_t language = c.arg<int32_t>(0);
        if (language < 0 || size_t(language) >= std::size(kLanguages)) return kInvalidLanguage;
        c.push<uint64_t>(language_code(kLanguages[language]));
        return 0u;
    };
    service->cmds[3] = [](IpcCtx& c) { c.push<int32_t>(int32_t(kLegacyCount)); return 0u; };
    service->cmds[4] = [](IpcCtx& c) { c.push<uint32_t>(1); return 0u; };  // USA region, matching the default locale
    service->cmds[5] = [](IpcCtx& c) { return available(c, std::size(kLanguages)); };
    service->cmds[6] = [](IpcCtx& c) { c.push<int32_t>(int32_t(std::size(kLanguages))); return 0u; };
    return service;
});

}  // namespace
}  // namespace nx
