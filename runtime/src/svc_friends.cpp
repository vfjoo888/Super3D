// friend:u / friend:a IPC service stub/bridge for online play (friend list availability & play history).
#include "ipc.h"
#include "kernel.h"
#include "hle_net.h"
#include "nextendo_client.h"

#include <nx/runtime.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <memory>
#include <random>

namespace nx {

namespace {

std::shared_ptr<Service> make_friend_service() {
    auto s = std::make_shared<Service>("IFriendService");
    auto ev = std::make_shared<KEvent>();
    ev->state = std::make_shared<EventState>();
    ev->state->signaled = true;
    // GetCompletionEvent -> signaled KEvent handle
    s->cmds[0] = [ev](IpcCtx& c) { c.out_copy.push_back(handle_add(ev)); return 0u; };
    // Cancel
    s->cmds[1] = [](IpcCtx&) { return 0u; };
    // GetFriendListIds(offset, filter, out buffer) -> u32 count
    s->cmds[10100] = [](IpcCtx& c) {
        const uint32_t offset = c.arg<uint32_t>(0);
        Buffer out = c.out_buf(0);
        uint32_t written = 0;
        if (net::online_enabled() && out.addr && out.size >= sizeof(uint64_t)) {
            auto friends = nextendo::get_friends();
            const uint32_t max_cnt = uint32_t(out.size / sizeof(uint64_t));
            auto* dst = reinterpret_cast<uint64_t*>(out.addr);
            for (size_t i = offset; i < friends.size() && written < max_cnt; ++i) {
                dst[written++] = friends[i].pid;
            }
        }
        log("friend: GetFriendListIds(off=%u) -> %u", offset, written);
        c.push<uint32_t>(written);
        return 0u;
    };
    // GetFriendList(offset, filter, out buffer) -> u32 count
    s->cmds[10101] = [](IpcCtx& c) {
        const uint32_t offset = c.arg<uint32_t>(0);
        Buffer out = c.out_buf(0);
        constexpr size_t kFriendSize = 0x200;
        uint32_t written = 0;
        if (net::online_enabled() && out.addr && out.size >= kFriendSize) {
            auto friends = nextendo::get_friends();
            const uint32_t max_cnt = uint32_t(out.size / kFriendSize);
            auto* base = reinterpret_cast<uint8_t*>(out.addr);
            for (size_t i = offset; i < friends.size() && written < max_cnt; ++i) {
                uint8_t* entry = base + written * kFriendSize;
                std::memset(entry, 0, kFriendSize);
                const auto& f = friends[i];
                // +0x00: nn::account::Uid (non-zero)
                std::memcpy(entry + 0x00, &f.pid, sizeof(uint64_t));
                const uint64_t uid_hi = 1;
                std::memcpy(entry + 0x08, &uid_hi, sizeof(uint64_t));
                // +0x10: NetworkServiceAccountId (pid)
                std::memcpy(entry + 0x10, &f.pid, sizeof(uint64_t));
                // +0x18: Nickname (char[33])
                const size_t nlen = std::min<size_t>(f.username.size(), 32);
                std::memcpy(entry + 0x18, f.username.data(), nlen);
                // +0x40: FriendPresence (LastPlayedApplication at +0x00, LastUpdateTime at +0x10, Status at +0x18, SameApp at +0x1c)
                const uint64_t title_id = 0x010015100B514000ULL;
                std::memcpy(entry + 0x40, &title_id, sizeof(uint64_t));
                std::memcpy(entry + 0x48, &title_id, sizeof(uint64_t));
                const int64_t now_ts = std::time(nullptr);
                std::memcpy(entry + 0x50, &now_ts, sizeof(int64_t));
                std::memcpy(entry + 0x58, &f.presence_status, sizeof(uint32_t));
                entry[0x5C] = f.same_app ? 1 : 0;
                // +0x128: IsValid
                entry[0x128] = 1;
                ++written;
            }
        }
        log("friend: GetFriendList(off=%u) -> %u", offset, written);
        {
            constexpr uint64_t main_base = 0x4000000000ull;
            const uint64_t g1 = ld<uint64_t>(main_base + 0x37be340);
            const uint64_t g1_90 = (g1 >= 0x1000000) ? ld<uint64_t>(g1 + 0x90) : 0;
            const uint64_t npln_fl = (g1_90 >= 0x1000000) ? ld<uint64_t>(g1_90 + 0x40) : 0;
            int32_t npln_cnt = -1;
            if (npln_fl >= 0x1000000) {
                const uint32_t idx = ld<uint32_t>(npln_fl + 0x18);
                const uint64_t slot = (idx < 2) ? (npln_fl + 8 + uint64_t(idx) * 8) : (npln_fl + 8);
                const uint64_t vec = ld<uint64_t>(slot);
                if (vec >= 0x1000000) {
                    npln_cnt = ld<int32_t>(vec + 0x18);
                    const uint64_t arr = ld<uint64_t>(vec + 0x20);
                    if (arr >= 0x1000000 && npln_cnt > 0 && npln_cnt <= 32) {
                        for (int32_t k = 0; k < npln_cnt; ++k) {
                            const uint64_t fu = ld<uint64_t>(arr + uint64_t(k) * 8);
                            if (fu >= 0x1000000) {
                                const uint8_t sso = ld<uint8_t>(fu + 0x8);
                                const uint64_t uid_p = (sso & 1) ? ld<uint64_t>(fu + 0x18) : (fu + 0x9);
                                const uint64_t sess = ld<uint64_t>(fu + 0x38);
                                const uint8_t b6a = ld<uint8_t>(fu + 0x6a);
                                std::string uid_s;
                                if (uid_p >= 0x1000000) {
                                    const char* p = reinterpret_cast<const char*>(uid_p);
                                    for (int j = 0; j < 40 && p[j] >= 0x20 && p[j] < 0x7f; ++j) uid_s.push_back(p[j]);
                                }
                                log("friend_diag: npln_fu[%d] uid=\"%s\" sess=%llx b6a=%u", k, uid_s.c_str(),
                                    (unsigned long long)sess, b6a);
                            }
                        }
                    }
                }
            }
            const uint64_t g2 = ld<uint64_t>(main_base + 0x37bd758);
            const uint64_t flm = (g2 >= 0x1000000) ? ld<uint64_t>(g2 + 0x88) : 0;
            const int32_t flm_cnt = (flm >= 0x1000000) ? ld<int32_t>(flm + 0x40) : -1;
            log("friend_diag: npln_fl=%llx cnt=%d flm=%llx flm_cnt=%d", (unsigned long long)npln_fl, npln_cnt,
                (unsigned long long)flm, flm_cnt);
        }
        c.push<uint32_t>(written);
        return 0u;
    };
    // CheckFriendListAvailability(uid) -> bool
    s->cmds[10120] = [](IpcCtx& c) {
        log("friend: CheckFriendListAvailability -> %d", int(net::online_enabled()));
        c.push<uint8_t>(net::online_enabled() ? 1 : 0);
        return 0u;
    };
    // EnsureFriendListAvailable(uid)
    s->cmds[10121] = [](IpcCtx&) { log("friend: EnsureFriendListAvailable"); return 0u; };
    // UpdateUserPresence
    s->cmds[10400] = [](IpcCtx&) { log("friend: UpdateUserPresence"); return 0u; };
    // GetPlayHistoryRegistrationKey
    s->cmds[10600] = [](IpcCtx& c) {
        Buffer out = c.out_buf(0);
        if (out.addr && out.size) std::memset(reinterpret_cast<void*>(out.addr), 0, out.size);
        return 0u;
    };
    // AddPlayHistory
    s->cmds[10601] = [](IpcCtx&) { return 0u; };
    return s;
}

void register_friend_creator(Service& s) {
    s.cmds[0] = [](IpcCtx& c) {  // CreateFriendService
        c.push_object(make_friend_service());
        return 0u;
    };
}

ServiceRegistrar r_friend_u("friend:u", [] {
    auto s = std::make_shared<Service>("friend:u");
    register_friend_creator(*s);
    return s;
});

ServiceRegistrar r_friend_a("friend:a", [] {
    auto s = std::make_shared<Service>("friend:a");
    register_friend_creator(*s);
    return s;
});

std::shared_ptr<Service> make_ntc_task() {
    auto s = std::make_shared<Service>("IEnsureNetworkClockAvailabilityService");
    auto ev = std::make_shared<KEvent>();
    ev->state = std::make_shared<EventState>();
    ev->state->signaled = true;
    // StartTask
    s->cmds[0] = [](IpcCtx&) { return 0u; };
    // GetFinishNotificationEvent
    s->cmds[1] = [ev](IpcCtx& c) {
        c.out_copy.push_back(handle_add(ev));
        return 0u;
    };
    // GetResult
    s->cmds[2] = [](IpcCtx&) { return 0u; };
    // Cancel
    s->cmds[3] = [](IpcCtx&) { return 0u; };
    // IsProcessing -> false
    s->cmds[4] = [](IpcCtx& c) {
        c.push<uint8_t>(0);
        return 0u;
    };
    // GetServerTime -> current Unix time
    s->cmds[5] = [](IpcCtx& c) {
        c.push<int64_t>(int64_t(std::time(nullptr)));
        return 0u;
    };
    return s;
}

ServiceRegistrar r_ntc("ntc", [] {
    auto s = std::make_shared<Service>("ntc");
    // OpenEnsureNetworkClockAvailabilityService
    s->cmds[0] = [](IpcCtx& c) {
        c.push_object(make_ntc_task());
        return 0u;
    };
    // SuspendAutonomicTimeCorrection / ResumeAutonomicTimeCorrection
    s->cmds[100] = [](IpcCtx&) { return 0u; };
    s->cmds[101] = [](IpcCtx&) { return 0u; };
    return s;
});

ServiceRegistrar r_ectx_aw("ectx:aw", [] {
    auto s = std::make_shared<Service>("ectx:aw");
    // CreateContextRegistrar -> IContextRegistrar
    s->cmds[0] = [](IpcCtx& c) {
        auto reg = std::make_shared<Service>("IContextRegistrar");
        // Complete(InBuffer, ResultHolder) -> ContextDescriptor (u32)
        reg->cmds[0] = [](IpcCtx& rc) {
            const Buffer in = rc.in_buf(0);
            std::string ascii;
            if (in.addr && in.size) {
                const auto* p = reinterpret_cast<const uint8_t*>(in.addr);
                const size_t n = std::min<size_t>(in.size, 256);
                for (size_t i = 0; i < n; ++i) {
                    ascii.push_back((p[i] >= 32 && p[i] < 127) ? char(p[i]) : '.');
                }
            }
            log("ectx: Complete res=%#x in_size=%zu ascii=\"%s\"", rc.arg<uint32_t>(0), in.size, ascii.c_str());
            rc.push<uint32_t>(1);
            return 0u;
        };
        c.push_object(reg);
        return 0u;
    };
    // CommitContext
    s->cmds[1] = [](IpcCtx&) { return 0u; };
    return s;
});

ServiceRegistrar r_csrng("csrng", [] {
    auto s = std::make_shared<Service>("csrng");
    // GetRandomBytes(OutBuffer<u8>)
    s->cmds[0] = [](IpcCtx& c) {
        const Buffer out = c.out_buf(0);
        if (out.addr && out.size) {
            std::random_device rd;
            uint8_t* p = reinterpret_cast<uint8_t*>(out.addr);
            size_t i = 0;
            while (i < out.size) {
                const uint32_t v = rd();
                const size_t n = std::min<size_t>(4, out.size - i);
                std::memcpy(p + i, &v, n);
                i += n;
            }
        }
        static bool logged = false;
        if (!logged) {
            logged = true;
            log("csrng: GetRandomBytes(%zu)", out.size);
        }
        return 0u;
    };
    return s;
});

}  // namespace

void hle_friend_get_account_id(CPU& c) {
    uint64_t id = ld<uint64_t>(c.x[0] + 0x10);
    static int logs = 0;
    if (logs < 20) {
        ++logs;
        constexpr uint64_t main_base = 0x4000000000ull;
        const uint64_t g2 = ld<uint64_t>(main_base + 0x37bd758);
        const uint64_t g2_80 = (g2 >= 0x1000000) ? ld<uint64_t>(g2 + 0x80) : 0;
        const uint64_t flm = (g2 >= 0x1000000) ? ld<uint64_t>(g2 + 0x88) : 0;
        const int32_t flm_cnt = (flm >= 0x1000000) ? ld<int32_t>(flm + 0x40) : -1;
        const uint32_t flm_28 = (flm >= 0x1000000) ? ld<uint32_t>(flm + 0x28) : 0;
        const uint64_t npln_cli = (flm >= 0x1000000) ? ld<uint64_t>(flm + 0x8) : 0;
        if (npln_cli >= 0x1000000) {
            const uint64_t beg = ld<uint64_t>(npln_cli + 0x90);
            const uint64_t end = ld<uint64_t>(npln_cli + 0x98);
            const size_t nfu = (end >= beg && beg >= 0x1000000) ? (end - beg) / 0x28 : 0;
            for (size_t k = 0; k < nfu && k < 8; ++k) {
                const uint64_t e = beg + k * 0x28;
                const uint8_t sso = ld<uint8_t>(e);
                const uint64_t p = (sso & 1) ? ld<uint64_t>(e + 0x10) : (e + 1);
                const uint64_t aid = ld<uint64_t>(e + 0x20);
                std::string s;
                if (p >= 0x1000000) {
                    const char* cp = reinterpret_cast<const char*>(p);
                    for (int j = 0; j < 40 && cp[j] >= 0x20 && cp[j] < 0x7f; ++j) s.push_back(cp[j]);
                }
                log("friend_diag: npln_cli[%zu] uid=\"%s\" aid=%llu", k, s.c_str(), (unsigned long long)aid);
            }
        }
        const uint32_t st = ld<uint32_t>(c.x[0] + 0x58);
        log("friend_hle: GetAccountId -> %llu st=%u g2_80=%llx flm_cnt=%d flm_28=%u from %s", (unsigned long long)id,
            st, (unsigned long long)g2_80, flm_cnt, flm_28, describe(c.x[30]).c_str());
    }
    c.x[0] = id;
}

void hle_friend_presence_get_status(CPU& c) {
    uint32_t st = ld<uint32_t>(c.x[0] + 0x18);
    static int logs = 0;
    if (logs < 20) {
        ++logs;
        log("friend_hle: GetStatus -> %u from %s", st, describe(c.x[30]).c_str());
    }
    c.x[0] = st;
}

NX_HLE("_ZNK2nn7friends6Friend12GetAccountIdEv", hle_friend_get_account_id);
NX_HLE("_ZNK2nn7friends14FriendPresence9GetStatusEv", hle_friend_presence_get_status);

}  // namespace nx
