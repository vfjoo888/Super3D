#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>

#include "ipc.h"
#include "svc_am.h"

namespace nx {
ServiceFactory& settings_factory() { static ServiceFactory factory; return factory; }
void register_service(const std::string& name, ServiceFactory f) {
    if (name == "set") settings_factory() = std::move(f);
}
uint32_t Service::handle(IpcCtx& ctx) { return cmds.at(ctx.cmd)(ctx); }
uint64_t game_language_code() { return 0x736e61482d687aull; }  // zh-Hans
}

template <typename T> T response(const nx::IpcCtx& c) {
    assert(c.out.size() == sizeof(T));
    T value;
    std::memcpy(&value, c.out.data(), sizeof value);
    return value;
}

int main() {
    assert(nx::settings_factory());
    auto service = nx::settings_factory()();
    nx::IpcCtx c;
    c.cmd = 0;
    assert(service->handle(c) == 0 && response<uint64_t>(c) == nx::game_language_code());

    uint64_t codes[20]{};
    c = {};
    c.cmd = 5;
    c.b.push_back({reinterpret_cast<uint64_t>(codes), sizeof codes});
    assert(service->handle(c) == 0 && response<int32_t>(c) == 20);
    assert(std::memcmp(&codes[15], "zh-Hans", 7) == 0);
    assert(std::memcmp(&codes[16], "zh-Hant", 7) == 0);
    assert(std::memcmp(&codes[17], "pt-BR", 5) == 0);
    assert(std::memcmp(&codes[18], "pl", 2) == 0);
    assert(std::memcmp(&codes[19], "th", 2) == 0);

    c = {};
    c.cmd = 6;
    assert(service->handle(c) == 0 && response<int32_t>(c) == 20);
    c = {};
    c.cmd = 3;
    assert(service->handle(c) == 0 && response<int32_t>(c) == 15);
    c = {};
    c.cmd = 5;
    assert(service->handle(c) == 0 && response<int32_t>(c) == 0);

    c = {};
    c.cmd = 1;
    c.c.push_back({reinterpret_cast<uint64_t>(codes), 2 * sizeof(uint64_t)});
    assert(service->handle(c) == 0 && response<int32_t>(c) == 2);
    assert(std::memcmp(&codes[0], "ja", 2) == 0 && std::memcmp(&codes[1], "en-US", 5) == 0);

    int32_t index = 16;
    c = {};
    c.cmd = 2;
    c.in = reinterpret_cast<const uint8_t*>(&index);
    c.in_size = sizeof index;
    assert(service->handle(c) == 0 && response<uint64_t>(c) == codes[16]);
    index = -1;
    c.out.clear();
    assert(service->handle(c) != 0 && c.out.empty());
}
