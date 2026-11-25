// Networking HLE: offline by default, or real Winsock2/POSIX sockets + Nextendo DNS redirect when online mode is on.
#include "hle_net.h"

#include <nx/runtime.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#endif
#endif

namespace nx::net {

namespace {

int host_last_sock_error() {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool is_connect_in_progress(int err) {
#if defined(_WIN32)
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
    return err == EWOULDBLOCK || err == EINPROGRESS;
#endif
}

}  // namespace

bool online_enabled() {
    static const bool env_on = [] {
        const char* e = std::getenv("NX_ONLINE_MODE");
        return e && std::strcmp(e, "1") == 0;
    }();
    return env_on;
}

namespace {

constexpr uint32_t kNifmNoNetwork = (3400u << 9) | 110u;
constexpr uint32_t kGenericFail = (1u << 9) | 110u;

// Horizon / FreeBSD socket constants.
constexpr int kNxAfUnspec = 0;
constexpr int kNxAfInet = 2;
constexpr int kNxSockStream = 1;
constexpr int kNxSockDgram = 2;
constexpr int kNxSolSocket = 0xFFFF;
constexpr int kNxSoReuseAddr = 0x0004;
constexpr int kNxSoBroadcast = 0x0020;
constexpr int kNxSoSndBuf = 0x1001;
constexpr int kNxSoRcvBuf = 0x1002;
constexpr int kNxSoSndTimeo = 0x1005;
constexpr int kNxSoRcvTimeo = 0x1006;
constexpr int kNxSoError = 0x1007;
constexpr int kNxSoNonBlock = 0x2000;  // nn::socket::Option::NonBlock
constexpr int kNxMsgDontWait = 0x0080;

#if defined(_WIN32)
using HostSocket = SOCKET;
constexpr HostSocket kBadHostSocket = INVALID_SOCKET;
#else
using HostSocket = int;
constexpr HostSocket kBadHostSocket = -1;
#endif

struct FdEntry {
    bool is_event = false;
    HostSocket sock = kBadHostSocket;
    HostSocket event_peer = kBadHostSocket;  // loopback sender for emulated eventfd on Windows/macOS
    uint64_t event_counter = 0;
};

struct AddrBlock {
    uint64_t base = 0;
    uint64_t size = 0;
    bool used = false;
};

std::mutex g_net_mu;
bool g_wsa_ready = false;
int g_next_fd = 100;
std::unordered_map<int, FdEntry> g_fds;
thread_local int g_last_errno = 0;

// Dedicated guest-visible pool for GetAddrInfo / FreeAddrInfo nodes.
constexpr uint64_t kAddrPoolSize = 512 * 1024;
uint64_t g_addr_pool = 0;
uint64_t g_addr_pool_used = 0;
std::vector<AddrBlock> g_addr_blocks;

// DNS redirect cache (10-minute TTL) + IPv4 -> original SNI hostname map.
struct DnsCacheEntry {
    uint32_t ipv4_be = 0;
    std::chrono::steady_clock::time_point expires;
};
std::unordered_map<std::string, DnsCacheEntry> g_dns_cache;
std::unordered_map<uint32_t, std::string> g_sni_map;

struct NetOp {
    char op[16]{};
    int fd = -1;
    int64_t res = 0;
    int extra = 0;
};
std::mutex g_ring_mu;
NetOp g_op_ring[16]{};
uint32_t g_op_head = 0;
char g_active_poll[160]{};

void record_op(const char* name, int fd, int64_t res, int extra = 0) {
    std::lock_guard lk(g_ring_mu);
    NetOp& e = g_op_ring[g_op_head++ & 15u];
    std::snprintf(e.op, sizeof(e.op), "%s", name);
    e.fd = fd;
    e.res = res;
    e.extra = extra;
}

void ensure_wsa() {
#if defined(_WIN32)
    if (!g_wsa_ready) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) g_wsa_ready = true;
    }
#else
    g_wsa_ready = true;
#endif
}

std::string lower_ascii(std::string s) {
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int map_host_errno(int err) {
#if defined(_WIN32)
    switch (err) {
    case 0: return 0;
    case WSAEWOULDBLOCK: return 11;   // EAGAIN / EWOULDBLOCK
    case WSAEINPROGRESS: return 115;  // EINPROGRESS
    case WSAEALREADY: return 114;
    case WSAECONNREFUSED: return 111;
    case WSAETIMEDOUT: return 110;
    case WSAECONNRESET: return 104;
    case WSAENOTCONN: return 107;
    case WSAEISCONN: return 106;
    case WSAEADDRINUSE: return 98;
    case WSAEADDRNOTAVAIL: return 99;
    case WSAENETDOWN: return 50;
    case WSAEBADF: return 9;
    default: return 22;  // EINVAL
    }
#else
    switch (err) {
    case 0: return 0;
    case EAGAIN: return 11;
    case EINPROGRESS: return 115;
    case ECONNREFUSED: return 111;
    case ETIMEDOUT: return 110;
    case ECONNRESET: return 104;
    default: return err;
    }
#endif
}

static std::mutex g_errno_slab_mu;
static uint64_t g_errno_slab = 0;
static uint32_t g_errno_next = 0;

uint64_t guest_errno_addr() {
    thread_local uint64_t tl_addr = 0;
    if (!tl_addr) {
        std::lock_guard lk(g_errno_slab_mu);
        if (!g_errno_slab || g_errno_next + 4 > 4096) {
            g_errno_slab = kernel_alloc(4096, 16);
            std::memset(reinterpret_cast<void*>(g_errno_slab), 0, 4096);
            g_errno_next = 0;
        }
        tl_addr = g_errno_slab + g_errno_next;
        g_errno_next += 4;
    }
    return tl_addr;
}

void set_guest_errno(int err) {
    g_last_errno = err;
    st<int32_t>(guest_errno_addr(), err);
}

int capture_host_error() {
#if defined(_WIN32)
    set_guest_errno(map_host_errno(WSAGetLastError()));
#else
    set_guest_errno(map_host_errno(errno));
#endif
    return -1;
}

void close_host_socket(HostSocket s) {
    if (s == kBadHostSocket) return;
#if defined(_WIN32)
    closesocket(s);
#else
    ::close(s);
#endif
}

bool set_nonblocking(HostSocket s, bool nonblock) {
#if defined(_WIN32)
    u_long mode = nonblock ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(s, F_SETFL, nonblock ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
}

bool wait_ready_nowait(HostSocket s, short events) {
#if defined(_WIN32)
    pollfd pfd{s, events, 0};
    const int ready = WSAPoll(&pfd, 1, 0);
    if (ready == 0) {
        set_guest_errno(11);  // EAGAIN / EWOULDBLOCK
        return false;
    }
    return true;
#else
    short ev = events;
    if (ev & POLLRDNORM) ev = short((ev & ~POLLRDNORM) | POLLIN);
    pollfd pfd{s, ev, 0};
    const int ready = ::poll(&pfd, 1, 0);
    if (ready == 0) {
        set_guest_errno(11);  // EAGAIN / EWOULDBLOCK
        return false;
    }
    return true;
#endif
}

// Convert Horizon SockAddrIn (sin_len, sin_family, port, addr, zero[8]) -> host sockaddr_in.
bool guest_to_host_addr(uint64_t addr, uint32_t len, sockaddr_in& out) {
    if (!addr || len < 8) return false;
    std::memset(&out, 0, sizeof(out));
    const uint8_t fam = ld<uint8_t>(addr + 1);
    if (fam != kNxAfInet) return false;
    out.sin_family = AF_INET;
    out.sin_port = ld<uint16_t>(addr + 2);
    out.sin_addr.s_addr = ld<uint32_t>(addr + 4);
    return true;
}

void host_to_guest_addr(const sockaddr_in& in, uint64_t addr, uint64_t len_ptr) {
    if (addr) {
        std::memset(reinterpret_cast<void*>(addr), 0, 16);
        st<uint8_t>(addr + 0, 16);
        st<uint8_t>(addr + 1, uint8_t(kNxAfInet));
        st<uint16_t>(addr + 2, in.sin_port);
        st<uint32_t>(addr + 4, in.sin_addr.s_addr);
    }
    if (len_ptr) st<uint32_t>(len_ptr, 16);
}

uint64_t alloc_addr_mem(uint64_t bytes) {
    bytes = (bytes + 15ull) & ~15ull;
    if (!g_addr_pool) g_addr_pool = kernel_alloc(kAddrPoolSize, 0x1000);
    for (AddrBlock& b : g_addr_blocks) {
        if (!b.used && b.size >= bytes) {
            b.used = true;
            return b.base;
        }
    }
    if (g_addr_pool_used + bytes > kAddrPoolSize) return 0;
    uint64_t p = g_addr_pool + g_addr_pool_used;
    g_addr_pool_used += bytes;
    g_addr_blocks.push_back({p, bytes, true});
    return p;
}

void free_addr_mem(uint64_t ptr) {
    for (AddrBlock& b : g_addr_blocks) {
        if (b.base == ptr) {
            b.used = false;
            return;
        }
    }
}

bool resolve_host_ipv4(const std::string& host, uint32_t& out_ipv4_be) {
    struct in_addr literal{};
    if (inet_pton(AF_INET, host.c_str(), &literal) == 1) {
        out_ipv4_be = literal.s_addr;
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lk(g_net_mu);
        auto it = g_dns_cache.find(host);
        if (it != g_dns_cache.end() && it->second.expires > now) {
            out_ipv4_be = it->second.ipv4_be;
            return true;
        }
    }
    ensure_wsa();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res) {
        auto* sin = reinterpret_cast<sockaddr_in*>(res->ai_addr);
        out_ipv4_be = sin->sin_addr.s_addr;
        ::freeaddrinfo(res);
        std::lock_guard lk(g_net_mu);
        g_dns_cache[host] = {out_ipv4_be, now + std::chrono::minutes(10)};
        return true;
    }
    if (res) ::freeaddrinfo(res);
    // Keep last known good address on transient DNS failure.
    std::lock_guard lk(g_net_mu);
    auto it = g_dns_cache.find(host);
    if (it != g_dns_cache.end()) {
        out_ipv4_be = it->second.ipv4_be;
        return true;
    }
    return false;
}

}  // namespace

std::string redirect_target_host(const std::string& host, bool& is_nat) {
    const std::string low = lower_ascii(host);
    is_nat = false;
    const bool is_nintendo =
        ends_with(low, ".nintendo.net") || ends_with(low, ".nintendo.com") ||
        ends_with(low, ".nintendowifi.net") || ends_with(low, ".nintendo.co.jp") ||
        low == "nintendo.net" || low == "nintendo.com";
    if (!is_nintendo) return {};
    if (low.find("nncs") != std::string::npos) {
        is_nat = true;
        if (const char* ov = std::getenv("NEXTENDO_NAT_IP"); ov && *ov) return ov;
        return "nat.nextendo.network";
    }
    if (const char* ov = std::getenv("NEXTENDO_SERVER_IP"); ov && *ov) return ov;
    return "nex.nextendo.network";
}

std::string lookup_sni_host(uint32_t ipv4_be) {
    std::lock_guard lk(g_net_mu);
    auto it = g_sni_map.find(ipv4_be);
    return it != g_sni_map.end() ? it->second : std::string{};
}

intptr_t get_host_socket(int fd) {
    std::lock_guard lk(g_net_mu);
    auto it = g_fds.find(fd);
    if (it == g_fds.end() || it->second.is_event) return -1;
    return intptr_t(it->second.sock);
}

}  // namespace nx::net

namespace nx {

void request_thread_dump_in_ms(uint64_t ms);
void backtrace(CPU& c);

namespace {

void ok(CPU& c) { c.x[0] = 0; }
void no(CPU& c) { c.x[0] = 0; }
void yes_if_online(CPU& c) {
    const bool on = net::online_enabled();
    static bool logged = false;
    if (!logged) {
        logged = true;
        log("nifm: IsNetworkAvailable -> %d", int(on));
    }
    c.x[0] = on ? 1 : 0;
}
void ok_if_online_else_nifm(CPU& c) {
    const uint32_t res = net::online_enabled() ? 0 : net::kNifmNoNetwork;
    static bool logged = false;
    if (!logged) {
        logged = true;
        log("nifm: %s -> %#x", hle_current_name(), res);
    }
    c.x[0] = res;
}
void minus_one(CPU& c) { c.x[0] = uint64_t(-1); }
void result_fail(CPU& c) { c.x[0] = net::kGenericFail; }
void htonl_(CPU& c) { c.x[0] = bswap(uint32_t(c.x[0])); }
void htons_(CPU& c) { c.x[0] = bswap(uint16_t(c.x[0])); }
void show_error(CPU& c) {
    const uint32_t r = uint32_t(c.x[0]);
    log("nn::err::%s(%#x mod=%u desc=%u) lr=%llx", hle_current_name(), r, r & 0x1ffu, (r >> 9) & 0x1fffu,
        (unsigned long long)c.x[30]);
}
void show_error_code(CPU& c) {
    const uint64_t code = c.x[0];
    log("nn::err::%s(cat=%u num=%u x1=%#llx) lr=%llx", hle_current_name(), uint32_t(code), uint32_t(code >> 32),
        (unsigned long long)c.x[1], (unsigned long long)c.x[30]);
}
void show_error_variant(CPU& c) {
    const uint64_t p = c.x[0];
    uint32_t w[4]{};
    if (p >= 0x100000 && (p & 3) == 0) {
        for (int i = 0; i < 4; ++i) w[i] = ld<uint32_t>(p + i * 4);
    }
    log("nn::err::%s(ptr=%#llx w=[%#x, %#x, %#x, %#x]) lr=%llx", hle_current_name(), (unsigned long long)p,
        w[0], w[1], w[2], w[3], (unsigned long long)c.x[30]);
}

uint32_t detect_local_ipv4_be() {
    static const uint32_t cached = []() -> uint32_t {
        net::ensure_wsa();
        net::HostSocket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s != net::kBadHostSocket) {
            sockaddr_in dst{};
            dst.sin_family = AF_INET;
            dst.sin_port = htons(53);
            dst.sin_addr.s_addr = htonl(0x01010101u);  // 1.1.1.1
            if (::connect(s, reinterpret_cast<const sockaddr*>(&dst), sizeof(dst)) == 0) {
                sockaddr_in local{};
                socklen_t len = sizeof(local);
                if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0 &&
                    local.sin_addr.s_addr != 0) {
                    net::close_host_socket(s);
                    const uint32_t ip = local.sin_addr.s_addr;
                    log("nifm: detected local IPv4 %u.%u.%u.%u",
                        ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
                    return ip;
                }
            }
            net::close_host_socket(s);
        }
        return (192u) | (168u << 8) | (1u << 16) | (100u << 24);
    }();
    return cached;
}

void nifm_get_primary_ip(CPU& c) {
    if (!net::online_enabled()) {
        c.x[0] = net::kNifmNoNetwork;
        return;
    }
    if (c.x[0]) {
        st<uint32_t>(c.x[0], detect_local_ipv4_be());
    }
    c.x[0] = 0;
}

void nifm_get_internet_status(CPU& c) {
    if (!net::online_enabled()) {
        c.x[0] = net::kNifmNoNetwork;
        return;
    }
    if (c.x[0]) {
        st<uint8_t>(c.x[0] + 0, 1);  // InternetConnectionType_WiFi
        st<uint8_t>(c.x[0] + 1, 3);  // wifiStrength = 3
        st<uint8_t>(c.x[0] + 2, 2);  // InternetConnectionState_Connected
    }
    c.x[0] = 0;
}

void nifm_get_ip_config(CPU& c) {
    if (!net::online_enabled()) {
        c.x[0] = net::kNifmNoNetwork;
        return;
    }
    auto write_ip = [](uint64_t ptr, uint8_t a, uint8_t b, uint8_t d1, uint8_t d2) {
        if (!ptr) return;
        const uint32_t val = (uint32_t(a)) | (uint32_t(b) << 8) | (uint32_t(d1) << 16) | (uint32_t(d2) << 24);
        st<uint32_t>(ptr, val);
    };
    const uint32_t local_ip = detect_local_ipv4_be();
    if (c.x[0]) st<uint32_t>(c.x[0], local_ip);
    write_ip(c.x[1], 255, 255, 255, 0);
    if (c.x[2]) st<uint32_t>(c.x[2], (local_ip & 0x00FFFFFFu) | (1u << 24));
    write_ip(c.x[3], 1, 1, 1, 1);
    write_ip(c.x[4], 8, 8, 8, 8);
    c.x[0] = 0;
}

void nifm_get_proxy(CPU& c) {
    if (!net::online_enabled()) {
        c.x[0] = net::kNifmNoNetwork;
        return;
    }
    log("nifm: GetCurrentProxySetting -> disabled");
    if (c.x[0]) std::memset(reinterpret_cast<void*>(c.x[0]), 0, 0x70);
    c.x[0] = 0;
}

void nsd_get_env(CPU& c) {
    log("nsd: GetEnvironmentIdentifier -> lp1");
    if (c.x[0]) {
        std::memset(reinterpret_cast<void*>(c.x[0]), 0, 8);
        std::memcpy(reinterpret_cast<void*>(c.x[0]), "lp1", 4);
    }
    c.x[0] = 0;
}

void nsd_resolve_ex(CPU& c) {
    if (!net::online_enabled()) {
        c.x[0] = net::kGenericFail;
        return;
    }
    const uint64_t out_fqdn = c.x[0];
    const uint64_t in_fqdn = c.x[1];
    if (!out_fqdn || !in_fqdn) {
        c.x[0] = net::kGenericFail;
        return;
    }
    char buf[256] = {};
    std::memcpy(buf, reinterpret_cast<const void*>(in_fqdn), 255);
    std::string s(buf);
    for (size_t pos = s.find('%'); pos != std::string::npos; pos = s.find('%', pos))
        s.replace(pos, 1, "lp1");
    log("nsd: ResolveEx %s -> %s", buf, s.c_str());
    std::memset(reinterpret_cast<void*>(out_fqdn), 0, 256);
    std::memcpy(reinterpret_cast<void*>(out_fqdn), s.data(), std::min<size_t>(s.size(), 255));
    c.x[0] = 0;
}

void sock_init(CPU& c) {
    log("socket: Initialize (online=%d)", int(net::online_enabled()));
    if (net::online_enabled()) {
        std::lock_guard lk(net::g_net_mu);
        net::ensure_wsa();
    }
    c.x[0] = 0;
}

void sock_socket(CPU& c) {
    if (!net::online_enabled()) {
        net::g_last_errno = 50;
        c.x[0] = uint64_t(-1);
        return;
    }
    net::ensure_wsa();
    const int family = int(int32_t(c.x[0]));
    const int type = int(int32_t(c.x[1]));
    const int proto = int(int32_t(c.x[2]));
    const int host_fam = (family == net::kNxAfInet) ? AF_INET : AF_UNSPEC;
    const int host_type = (type == net::kNxSockStream) ? SOCK_STREAM : (type == net::kNxSockDgram ? SOCK_DGRAM : type);
    const net::HostSocket s = ::socket(host_fam, host_type, proto);
    if (s == net::kBadHostSocket) {
        const int err = net::capture_host_error();
        log("socket: Socket(fam=%d, type=%d, proto=%d) FAILED err=%d", family, type, proto, err);
        c.x[0] = uint64_t(int64_t(err));
        return;
    }
#ifdef _WIN32
    if (host_type == SOCK_DGRAM) {
        BOOL connreset = FALSE;
        DWORD bytes_ret = 0;
        // Horizon/BSD UDP sockets never surface ICMP Port Unreachable as ECONNRESET on recvfrom.
        WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &connreset, sizeof(connreset), nullptr, 0, &bytes_ret, nullptr, nullptr);
    }
#endif
    std::lock_guard lk(net::g_net_mu);
    const int fd = net::g_next_fd++;
    net::g_fds[fd] = {false, s, net::kBadHostSocket, 0};
    log("socket: Socket(fam=%d, type=%d, proto=%d) -> fd=%d", family, type, proto, fd);
    c.x[0] = uint64_t(int64_t(fd));
}

void sock_connect(CPU& c) {
    const int fd = int(int32_t(c.x[0]));
    const uint64_t addr_ptr = c.x[1];
    const uint32_t addr_len = uint32_t(c.x[2]);
    sockaddr_in sin{};
    if (!net::guest_to_host_addr(addr_ptr, addr_len, sin)) {
        static int bad_conn_logs = 0;
        if (bad_conn_logs < 10) {
            ++bad_conn_logs;
            const uint8_t b0 = addr_ptr ? ld<uint8_t>(addr_ptr) : 0;
            const uint8_t b1 = addr_ptr ? ld<uint8_t>(addr_ptr + 1) : 0;
            log("socket: Connect BAD_ADDR fd=%d len=%u b0=%#x b1=%#x", fd, addr_len, b0, b1);
        }
        net::g_last_errno = 22;
        c.x[0] = uint64_t(-1);
        return;
    }
    net::HostSocket s = net::kBadHostSocket;
    {
        std::lock_guard lk(net::g_net_mu);
        auto it = net::g_fds.find(fd);
        if (it == net::g_fds.end() || it->second.is_event) {
            net::g_last_errno = 9;
            c.x[0] = uint64_t(-1);
            return;
        }
        s = it->second.sock;
    }
    const int rc = ::connect(s, reinterpret_cast<const sockaddr*>(&sin), sizeof(sin));
    static int conn_logs = 0;
    if (conn_logs < 64) {
        ++conn_logs;
        const int werr = rc == 0 ? 0 : net::host_last_sock_error();
        log("socket: Connect(fd=%d, ip=%#x, port=%u) -> rc=%d werr=%d", fd, sin.sin_addr.s_addr,
            ntohs(sin.sin_port), rc, werr);
    }
    if (rc != 0) {
        const int werr = net::host_last_sock_error();
        if (net::is_connect_in_progress(werr)) {
            net::set_guest_errno(115);  // EINPROGRESS for non-blocking connect
            c.x[0] = uint64_t(-1);
            return;
        }
        c.x[0] = uint64_t(int64_t(net::capture_host_error()));
        return;
    }
    c.x[0] = 0;
}

void sock_set_last_errno(CPU& c) {
    net::set_guest_errno(int(int32_t(c.x[0])));
    c.x[0] = 0;
}

void hle_errno_location(CPU& c) { c.x[0] = net::guest_errno_addr(); }

void sock_getsockopt(CPU& c) {
    const int fd = int(int32_t(c.x[0]));
    const int level = int(int32_t(c.x[1]));
    const int opt = int(int32_t(c.x[2]));
    const uint64_t val_ptr = c.x[3];
    const uint64_t len_ptr = c.x[4];
    net::HostSocket s = net::kBadHostSocket;
    {
        std::lock_guard lk(net::g_net_mu);
        auto it = net::g_fds.find(fd);
        if (it == net::g_fds.end() || it->second.is_event) {
            net::g_last_errno = 9;
            c.x[0] = uint64_t(-1);
            return;
        }
        s = it->second.sock;
    }
    int host_level = level == net::kNxSolSocket ? SOL_SOCKET : level;
    int host_opt = opt;
    if (level == net::kNxSolSocket) {
        if (opt == net::kNxSoReuseAddr) host_opt = SO_REUSEADDR;
        else if (opt == 0x0008) host_opt = SO_KEEPALIVE;
        else if (opt == net::kNxSoBroadcast) host_opt = SO_BROADCAST;
        else if (opt == 0x0080) host_opt = SO_LINGER;
        else if (opt == net::kNxSoSndBuf) host_opt = SO_SNDBUF;
        else if (opt == net::kNxSoRcvBuf) host_opt = SO_RCVBUF;
        else if (opt == net::kNxSoSndTimeo) host_opt = SO_SNDTIMEO;
        else if (opt == net::kNxSoRcvTimeo) host_opt = SO_RCVTIMEO;
        else if (opt == 0x1007) host_opt = SO_ERROR;
    }
    if (val_ptr && len_ptr) {
        socklen_t hlen = socklen_t(ld<uint32_t>(len_ptr));
        if (hlen >= 4) st<int32_t>(val_ptr, 0);
        int rc = ::getsockopt(s, host_level, host_opt, reinterpret_cast<char*>(val_ptr), &hlen);
        if (rc == 0) {
            if (level == net::kNxSolSocket && opt == 0x1007 && hlen >= 4) {
                const int herr = ld<int32_t>(val_ptr);
                st<int32_t>(val_ptr, herr ? net::map_host_errno(herr) : 0);
            }
            st<uint32_t>(len_ptr, uint32_t(hlen));
        } else {
            if (hlen >= 4) st<int32_t>(val_ptr, 0);
        }
    }
    static int gso_logs = 0;
    if (gso_logs < 15) {
        ++gso_logs;
        log("socket: GetSockOpt(fd=%d, lvl=%#x, opt=%#x) -> val=%d", fd, level, opt,
            val_ptr ? ld<int32_t>(val_ptr) : -1);
    }
    c.x[0] = 0;
}

void sock_getname_impl(CPU& c, bool peer) {
    const int fd = int(int32_t(c.x[0]));
    const uint64_t sa_ptr = c.x[1];
    const uint64_t len_ptr = c.x[2];
    net::HostSocket s = net::kBadHostSocket;
    {
        std::lock_guard lk(net::g_net_mu);
        auto it = net::g_fds.find(fd);
        if (it == net::g_fds.end() || it->second.is_event) {
            net::g_last_errno = 9;
            c.x[0] = uint64_t(-1);
            return;
        }
        s = it->second.sock;
    }
    sockaddr_in sin{};
    socklen_t slen = sizeof(sin);
    const int rc = peer ? ::getpeername(s, reinterpret_cast<sockaddr*>(&sin), &slen)
                        : ::getsockname(s, reinterpret_cast<sockaddr*>(&sin), &slen);
    if (rc != 0) {
        const int werr = net::host_last_sock_error();
        if (net::is_connect_in_progress(werr)) {
            net::set_guest_errno(115);  // EINPROGRESS for non-blocking connect
            c.x[0] = uint64_t(-1);
            return;
        }
        c.x[0] = uint64_t(int64_t(net::capture_host_error()));
        return;
    }
    if (sa_ptr && len_ptr) {
        const uint32_t avail = ld<uint32_t>(len_ptr);
        const uint32_t w = std::min<uint32_t>(avail, 16);
        uint8_t buf[16]{};
        buf[0] = 16;
        buf[1] = uint8_t(net::kNxAfInet);
        std::memcpy(buf + 2, &sin.sin_port, 2);
        std::memcpy(buf + 4, &sin.sin_addr.s_addr, 4);
        if (w) std::memcpy(reinterpret_cast<void*>(sa_ptr), buf, w);
        st<uint32_t>(len_ptr, 16);
    }
    c.x[0] = 0;
}

void sock_getpeername(CPU& c) { sock_getname_impl(c, true); }
void sock_getsockname(CPU& c) { sock_getname_impl(c, false); }

void sock_sendmsg(CPU& c) {
    const int fd = int(int32_t(c.x[0]));
    const uint64_t msg_ptr = c.x[1];
    const int flags = int(int32_t(c.x[2]));
    net::HostSocket s = net::kBadHostSocket;
    {
        std::lock_guard lk(net::g_net_mu);
        auto it = net::g_fds.find(fd);
        if (it == net::g_fds.end() || it->second.is_event) {
            net::g_last_errno = 9;
            c.x[0] = uint64_t(-1);
            return;
        }
        s = it->second.sock;
    }
    if (!msg_ptr) {
        net::g_last_errno = 14;
        c.x[0] = uint64_t(-1);
        return;
    }
    const uint64_t iov_ptr = ld<uint64_t>(msg_ptr + 0x10);
    const int iov_len = ld<int32_t>(msg_ptr + 0x18);
    int64_t total = 0;
    for (int i = 0; i < iov_len; ++i) {
        const uint64_t base = ld<uint64_t>(iov_ptr + size_t(i) * 16 + 0);
        const size_t len = size_t(ld<uint64_t>(iov_ptr + size_t(i) * 16 + 8));
        if (!len) continue;
        const int sent = ::send(s, reinterpret_cast<const char*>(base), int(len), flags & ~net::kNxMsgDontWait);
        if (sent < 0) {
            c.x[0] = total > 0 ? uint64_t(total) : uint64_t(int64_t(net::capture_host_error()));
            return;
        }
        total += sent;
        if (size_t(sent) < len) break;
    }
    static int sm_logs = 0;
    if (total > 0 && (sm_logs < 120 || fd == 102 || fd == 106)) {
        ++sm_logs;
        log("socket: SendMsg(fd=%d, iov=%d) -> %lld", fd, iov_len, (long long)total);
    }
    net::record_op("SendMsg", fd, total);
    c.x[0] = uint64_t(total);
}

void sock_recvmsg(CPU& c) {
    const int fd = int(int32_t(c.x[0]));
    const uint64_t msg_ptr = c.x[1];
    const int flags = int(int32_t(c.x[2]));
    net::HostSocket s = net::kBadHostSocket;
    {
        std::lock_guard lk(net::g_net_mu);
        auto it = net::g_fds.find(fd);
        if (it == net::g_fds.end() || it->second.is_event) {
            net::g_last_errno = 9;
            c.x[0] = uint64_t(-1);
            return;
        }
        s = it->second.sock;
    }
    if (!msg_ptr) {
        net::g_last_errno = 14;
        c.x[0] = uint64_t(-1);
        return;
    }
    const uint64_t iov_ptr = ld<uint64_t>(msg_ptr + 0x10);
    const int iov_len = ld<int32_t>(msg_ptr + 0x18);
    int64_t total = 0;
    for (int i = 0; i < iov_len; ++i) {
        const uint64_t base = ld<uint64_t>(iov_ptr + size_t(i) * 16 + 0);
        const size_t len = size_t(ld<uint64_t>(iov_ptr + size_t(i) * 16 + 8));
        if (!len) continue;
        if ((flags & net::kNxMsgDontWait) != 0 && !net::wait_ready_nowait(s, POLLRDNORM)) {
            if (total > 0) {
                c.x[0] = uint64_t(total);
            } else {
                net::set_guest_errno(11);  // EAGAIN
                c.x[0] = uint64_t(-1);
            }
            return;
        }
#if defined(_WIN32)
        const int recv_flags = flags & ~net::kNxMsgDontWait;
#else
        const int recv_flags = (flags & ~net::kNxMsgDontWait) | ((flags & net::kNxMsgDontWait) ? MSG_DONTWAIT : 0);
#endif
        const int recvd = ::recv(s, reinterpret_cast<char*>(base), int(len), recv_flags);
        if (recvd < 0) {
            c.x[0] = total > 0 ? uint64_t(total) : uint64_t(int64_t(net::capture_host_error()));
            return;
        }
        total += recvd;
        if (recvd == 0 || size_t(recvd) < len) break;
    }
    st<int32_t>(msg_ptr + 0x2C, 0);
    static int rm_logs = 0;
    if ((total > 60 && rm_logs < 80) || (total > 0 && (fd == 102 || fd == 106))) {
        ++rm_logs;
        log("socket: RecvMsg(fd=%d, iov=%d) -> %lld", fd, iov_len, (long long)total);
    }
    net::record_op("RecvMsg", fd, total);
    c.x[0] = uint64_t(total);
}

void sock_send(CPU& c) {
    const int fd = int(int32_t(c.x[0]));
    const void* buf = reinterpret_cast<const void*>(c.x[1]);
    const size_t len = size_t(c.x[2]);
    const int flags = int(int32_t(c.x[3]));
    net::HostSocket s = net::kBadHostSocket;
    {
        std::lock_guard lk(net::g_net_mu);
        auto it = net::g_fds.find(fd);
        if (it == net::g_fds.end() || it->second.is_event) {
            net::g_last_errno = 9;
            c.x[0] = uint64_t(-1);
            return;
        }
        s = it->second.sock;
    }
    const int sent = ::send(s, reinterpret_cast<const char*>(buf), int(len), flags & ~net::kNxMsgDontWait);
    if (sent < 0) {
        c.x[0] = uint64_t(int64_t(net::capture_host_error()));
        return;
    }
    net::record_op("Send", fd, sent);
    c.x[0] = uint64_t(int64_t(sent));
}

void sock_recv(CPU& c) {
    const int fd = int(int32_t(c.x[0]));
    void* buf = reinterpret_cast<void*>(c.x[1]);
    const size_t len = size_t(c.x[2]);
    const int flags = int(int32_t(c.x[3]));
    net::HostSocket s = net::kBadHostSocket;
    {
        std::lock_guard lk(net::g_net_mu);
        auto it = net::g_fds.find(fd);
        if (it == net::g_fds.end() || it->second.is_event) {
            net::g_last_errno = 9;
            c.x[0] = uint64_t(-1);
            return;
        }
        s = it->second.sock;
    }
    if ((flags & net::kNxMsgDontWait) != 0 && !net::wait_ready_nowait(s, POLLRDNORM)) {
        static int eagain_hi = 0;
        if (fd >= 108 && eagain_hi < 5) {
            ++eagain_hi;
            log("socket: RecvFrom(fd=%d, len=%zu, flags=%#x) -> EAGAIN", fd, len, flags);
        }
        net::set_guest_errno(11);
        c.x[0] = uint64_t(-1);
        return;
    }
#if defined(_WIN32)
    const int recv_flags = flags & ~net::kNxMsgDontWait;
#else
    const int recv_flags = (flags & ~net::kNxMsgDontWait) | ((flags & net::kNxMsgDontWait) ? MSG_DONTWAIT : 0);
#endif
    const int recvd = ::recv(s, reinterpret_cast<char*>(buf), int(len), recv_flags);
    if (recvd < 0) {
        c.x[0] = uint64_t(int64_t(net::capture_host_error()));
        return;
    }
    c.x[0] = uint64_t(int64_t(recvd));
}

void sock_fcntl(CPU& c) {
    const int fd = int(int32_t(c.x[0]));
    const int cmd = int(int32_t(c.x[1]));
    const int arg = int(int32_t(c.x[2]));
    static int fcntl_logs = 0;
    if (fcntl_logs < 15) {
        ++fcntl_logs;
        log("socket: Fcntl(fd=%d, cmd=%d, arg=%#x)", fd, cmd, arg);
    }
    net::HostSocket s = net::kBadHostSocket;
    {
        std::lock_guard lk(net::g_net_mu);
        auto it = net::g_fds.find(fd);
        if (it == net::g_fds.end()) {
            net::g_last_errno = 9;
            c.x[0] = uint64_t(-1);
            return;
        }
        s = it->second.sock;
    }
    if (cmd == 3) {  // F_GETFL
        c.x[0] = 0;
        return;
    }
    if (cmd == 4) {  // F_SETFL
        const bool nb = (arg & 0x4) != 0 || (arg & 0x800) != 0;
        c.x[0] = net::set_nonblocking(s, nb) ? 0ull : uint64_t(-1);
        return;
    }
    c.x[0] = 0;
}

void sock_close(CPU& c) {
    const int fd = int(int32_t(c.x[0]));
    static int close_logs = 0;
    if (close_logs < 64) {
        ++close_logs;
        log("socket: Close(fd=%d)", fd);
    }
    std::lock_guard lk(net::g_net_mu);
    auto it = net::g_fds.find(fd);
    if (it == net::g_fds.end()) {
        net::g_last_errno = 9;
        c.x[0] = uint64_t(-1);
        return;
    }
    net::close_host_socket(it->second.sock);
    net::close_host_socket(it->second.event_peer);
    net::g_fds.erase(it);
    c.x[0] = 0;
}

void sock_shutdown(CPU& c) {
    const int fd = int(int32_t(c.x[0]));
    const int how = int(int32_t(c.x[1]));
    net::HostSocket s = net::kBadHostSocket;
    {
        std::lock_guard lk(net::g_net_mu);
        auto it = net::g_fds.find(fd);
        if (it == net::g_fds.end() || it->second.is_event) {
            net::g_last_errno = 9;
            c.x[0] = uint64_t(-1);
            return;
        }
        s = it->second.sock;
    }
    if (::shutdown(s, how) != 0) {
        c.x[0] = uint64_t(int64_t(net::capture_host_error()));
        return;
    }
    c.x[0] = 0;
}

void sock_bind(CPU& c) {
    const int fd = int(int32_t(c.x[0]));
    sockaddr_in sin{};
    if (!net::guest_to_host_addr(c.x[1], uint32_t(c.x[2]), sin)) {
        net::g_last_errno = 22;
        c.x[0] = uint64_t(-1);
        return;
    }
    net::HostSocket s = net::kBadHostSocket;
    {
        std::lock_guard lk(net::g_net_mu);
        auto it = net::g_fds.find(fd);
        if (it == net::g_fds.end() || it->second.is_event) {
            net::g_last_errno = 9;
            c.x[0] = uint64_t(-1);
            return;
        }
        s = it->second.sock;
    }
    const int brc = ::bind(s, reinterpret_cast<const sockaddr*>(&sin), sizeof(sin));
    static int bind_logs = 0;
    if (bind_logs < 10) {
        ++bind_logs;
        log("socket: Bind(fd=%d, ip=%#x, port=%u) -> rc=%d", fd, sin.sin_addr.s_addr, ntohs(sin.sin_port), brc);
    }
    if (brc != 0) {
        c.x[0] = uint64_t(int64_t(net::capture_host_error()));
        return;
    }
    c.x[0] = 0;
}

void sock_sendto(CPU& c) {
    const int fd = int(int32_t(c.x[0]));
    const void* buf = reinterpret_cast<const void*>(c.x[1]);
    const size_t len = size_t(c.x[2]);
    const int flags = int(int32_t(c.x[3]));
    const uint64_t addr_ptr = c.x[4];
    const uint32_t addr_len = uint32_t(c.x[5]);
    net::HostSocket s = net::kBadHostSocket;
    {
        std::lock_guard lk(net::g_net_mu);
        auto it = net::g_fds.find(fd);
        if (it == net::g_fds.end() || it->second.is_event) {
            net::g_last_errno = 9;
            c.x[0] = uint64_t(-1);
            return;
        }
        s = it->second.sock;
    }
    sockaddr_in sin{};
    const sockaddr* sa = nullptr;
    socklen_t salen = 0;
    if (addr_ptr) {
        if (!net::guest_to_host_addr(addr_ptr, addr_len, sin)) {
            net::g_last_errno = 22;
            c.x[0] = uint64_t(-1);
            return;
        }
        sa = reinterpret_cast<const sockaddr*>(&sin);
        salen = sizeof(sin);
    }
    const int host_flags = (flags & ~net::kNxMsgDontWait);
    const int sent = ::sendto(s, reinterpret_cast<const char*>(buf), int(len), host_flags, sa, salen);
    static int st_logs = 0;
    static int st_err_logs = 0;
    if (st_logs < 64 || (sent < 0 && st_err_logs < 20)) {
        if (sent < 0) ++st_err_logs;
        else ++st_logs;
        const int werr = sent < 0 ? net::host_last_sock_error() : 0;
        log("socket: SendTo(fd=%d, len=%zu, ip=%#x, port=%u) -> %d (werr=%d)", fd, len,
            addr_ptr ? sin.sin_addr.s_addr : 0u, addr_ptr ? ntohs(sin.sin_port) : 0u, sent, werr);
    }
    if (sent < 0) {
        c.x[0] = uint64_t(int64_t(net::capture_host_error()));
        return;
    }
    net::record_op("SendTo", fd, sent, addr_ptr ? int(ntohs(sin.sin_port)) : 0);
    c.x[0] = uint64_t(int64_t(sent));
}

void sock_recvfrom(CPU& c) {
    const int fd = int(int32_t(c.x[0]));
    void* buf = reinterpret_cast<void*>(c.x[1]);
    const size_t len = size_t(c.x[2]);
    const int flags = int(int32_t(c.x[3]));
    const uint64_t addr_ptr = c.x[4];
    const uint64_t len_ptr = c.x[5];
    net::HostSocket s = net::kBadHostSocket;
    {
        std::lock_guard lk(net::g_net_mu);
        auto it = net::g_fds.find(fd);
        if (it == net::g_fds.end() || it->second.is_event) {
            net::g_last_errno = 9;
            c.x[0] = uint64_t(-1);
            return;
        }
        s = it->second.sock;
    }
    sockaddr_in sin{};
    socklen_t salen = sizeof(sin);
    if ((flags & net::kNxMsgDontWait) != 0 && !net::wait_ready_nowait(s, POLLRDNORM)) {
        static int eagain_hi = 0;
        if (fd >= 108 && eagain_hi < 5) {
            ++eagain_hi;
            log("socket: RecvFrom(fd=%d, len=%zu, flags=%#x) -> EAGAIN", fd, len, flags);
        }
        net::set_guest_errno(11);
        c.x[0] = uint64_t(-1);
        return;
    }
#if defined(_WIN32)
    const int host_flags = (flags & ~net::kNxMsgDontWait);
#else
    const int host_flags = (flags & ~net::kNxMsgDontWait) | ((flags & net::kNxMsgDontWait) ? MSG_DONTWAIT : 0);
#endif
    const int recvd = ::recvfrom(s, reinterpret_cast<char*>(buf), int(len), host_flags,
                                 addr_ptr ? reinterpret_cast<sockaddr*>(&sin) : nullptr,
                                 addr_ptr ? &salen : nullptr);
    const int werr = recvd < 0 ? net::host_last_sock_error() : 0;
    if (recvd < 0) {
        c.x[0] = uint64_t(int64_t(net::capture_host_error()));
    }
    static int rf_logs = 0;
    static int rf_err_logs = 0;
    if (rf_logs < 64 || (recvd < 0 && werr != 11 && rf_err_logs < 20)) {
        if (recvd < 0 && werr != 11) ++rf_err_logs;
        else ++rf_logs;
        log("socket: RecvFrom(fd=%d, len=%zu, flags=%#x) -> %d (werr=%d)", fd, len, flags, recvd, werr);
    }
    if (recvd < 0) {
        return;
    }
    if (addr_ptr) net::host_to_guest_addr(sin, addr_ptr, len_ptr);
    net::record_op("RecvFrom", fd, recvd);
    c.x[0] = uint64_t(int64_t(recvd));
}

void sock_setsockopt(CPU& c) {
    const int fd = int(int32_t(c.x[0]));
    const int level = int(int32_t(c.x[1]));
    const int opt = int(int32_t(c.x[2]));
    const uint64_t val_ptr = c.x[3];
    const uint32_t val_len = uint32_t(c.x[4]);
    net::HostSocket s = net::kBadHostSocket;
    {
        std::lock_guard lk(net::g_net_mu);
        auto it = net::g_fds.find(fd);
        if (it == net::g_fds.end() || it->second.is_event) {
            net::g_last_errno = 9;
            c.x[0] = uint64_t(-1);
            return;
        }
        s = it->second.sock;
    }
    if (level == net::kNxSolSocket && opt == net::kNxSoNonBlock) {
        const bool nb = val_ptr && val_len >= 4 && ld<uint32_t>(val_ptr) != 0;
        c.x[0] = net::set_nonblocking(s, nb) ? 0ull : uint64_t(int64_t(net::capture_host_error()));
        return;
    }
    int host_level = level == net::kNxSolSocket ? SOL_SOCKET : level;
    int host_opt = opt;
    if (level == net::kNxSolSocket) {
        if (opt == net::kNxSoReuseAddr) host_opt = SO_REUSEADDR;
        else if (opt == 0x0008) host_opt = SO_KEEPALIVE;
        else if (opt == net::kNxSoBroadcast) host_opt = SO_BROADCAST;
        else if (opt == 0x0080) host_opt = SO_LINGER;
        else if (opt == net::kNxSoSndBuf) host_opt = SO_SNDBUF;
        else if (opt == net::kNxSoRcvBuf) host_opt = SO_RCVBUF;
        else if (opt == net::kNxSoSndTimeo) host_opt = SO_SNDTIMEO;
        else if (opt == net::kNxSoRcvTimeo) host_opt = SO_RCVTIMEO;
        else if (opt == net::kNxSoError) host_opt = SO_ERROR;
        else {
            static int unk_opt_logs = 0;
            if (unk_opt_logs < 15) {
                ++unk_opt_logs;
                log("socket: SetSockOpt(fd=%d, SOL_SOCKET, opt=%#x) -> ignored ok", fd, opt);
            }
            c.x[0] = 0;
            return;
        }
    }
    if (::setsockopt(s, host_level, host_opt, reinterpret_cast<const char*>(val_ptr), int(val_len)) != 0) {
        const int err = net::capture_host_error();
        static int err_opt_logs = 0;
        if (err_opt_logs < 15) {
            ++err_opt_logs;
            log("socket: SetSockOpt(fd=%d, lvl=%#x, opt=%#x) -> host err=%d (ignored ok)", fd, level, opt, err);
        }
        // gRPC sets FreeBSD/Linux-only options (e.g. SO_NOSIGPIPE, TCP_USER_TIMEOUT) and aborts if they fail.
        c.x[0] = 0;
        return;
    }
    c.x[0] = 0;
}

void sock_poll(CPU& c) {
    if (!net::online_enabled()) {
        net::g_last_errno = 50;
        c.x[0] = uint64_t(-1);
        return;
    }
    const uint64_t fds_ptr = c.x[0];
    const size_t nfds = size_t(c.x[1]);
    const int timeout_ms = int(int32_t(c.x[2]));
    std::vector<pollfd> host_fds(nfds);
    {
        std::lock_guard lk(net::g_net_mu);
        for (size_t i = 0; i < nfds; ++i) {
            const uint64_t p = fds_ptr + i * 8;
            const int32_t fd = ld<int32_t>(p + 0);
            const int16_t ev = ld<int16_t>(p + 4);
            st<int16_t>(p + 6, 0);
            auto it = net::g_fds.find(fd);
            host_fds[i].fd = (it != net::g_fds.end()) ? it->second.sock : net::kBadHostSocket;
            short hev = 0;
#if defined(_WIN32)
            if (ev & 0x0001) hev |= POLLRDNORM;
            if (ev & 0x0002) hev |= POLLRDBAND;
            if (ev & 0x0004) hev |= POLLWRNORM;
#else
            hev = ev;
#endif
            host_fds[i].events = hev;
            host_fds[i].revents = 0;
        }
    }
    const int32_t fd0 = nfds ? ld<int32_t>(fds_ptr) : -1;
    const int32_t fd1 = nfds > 1 ? ld<int32_t>(fds_ptr + 8) : -1;
    const int32_t fd2 = nfds > 2 ? ld<int32_t>(fds_ptr + 16) : -1;
    const bool high_fd = (fd0 >= 103 || fd1 >= 103 || fd2 >= 103);
    static int poll_enter_logs = 0;
    static int poll_hi_logs = 0;
    const bool do_log_enter = (poll_enter_logs < 10) || (high_fd && poll_hi_logs < 25);
    if (do_log_enter) {
        if (poll_enter_logs < 10) ++poll_enter_logs;
        else ++poll_hi_logs;
        log("socket: Poll enter nfds=%zu timeout=%d fd0=%d ev0=%#x fd1=%d ev1=%#x fd2=%d ev2=%#x", nfds, timeout_ms,
            fd0, nfds ? ld<int16_t>(fds_ptr + 4) : 0,
            fd1, nfds > 1 ? ld<int16_t>(fds_ptr + 12) : 0,
            fd2, nfds > 2 ? ld<int16_t>(fds_ptr + 20) : 0);
    }
    {
        std::lock_guard lk(net::g_net_mu);
        int off = 0;
        for (size_t i = 0; i < nfds && off < int(sizeof(net::g_active_poll)) - 24; ++i) {
            const uint64_t p = fds_ptr + i * 8;
            const int32_t fd = ld<int32_t>(p + 0);
            const int16_t ev = ld<int16_t>(p + 4);
            off += std::snprintf(net::g_active_poll + off, sizeof(net::g_active_poll) - size_t(off),
                                 "%s%d:%#x", i ? "," : "", fd, unsigned(uint16_t(ev)));
        }
        if (nfds == 0) net::g_active_poll[0] = '\0';
    }
#if defined(_WIN32)
    const int wait_ms = (timeout_ms < 0) ? -1 : timeout_ms;
    const int ready = nfds ? WSAPoll(host_fds.data(), ULONG(nfds), wait_ms) : 0;
#else
    const int ready = nfds ? ::poll(host_fds.data(), nfds, timeout_ms) : 0;
#endif
    static int poll_logs = 0;
    if (ready != 0 && poll_logs < 60) {
        ++poll_logs;
        log("socket: Poll exit ready=%d fd0=%d rev0=%#x fd1=%d rev1=%#x fd2=%d rev2=%#x err=%d", ready,
            fd0, nfds ? int(host_fds[0].revents) : 0,
            fd1, nfds > 1 ? int(host_fds[1].revents) : 0,
            fd2, nfds > 2 ? int(host_fds[2].revents) : 0,
            ready < 0 ? net::g_last_errno : 0);
    }
    if (ready < 0) {
        c.x[0] = uint64_t(int64_t(net::capture_host_error()));
        return;
    }
    for (size_t i = 0; i < nfds; ++i) {
        int16_t rev = 0;
#if defined(_WIN32)
        const short hr = host_fds[i].revents;
        if (hr & (POLLRDNORM | POLLRDBAND)) rev |= 0x0001;
        if (hr & POLLWRNORM) rev |= 0x0004;
        if (hr & POLLERR) rev |= 0x0008;
        if (hr & POLLHUP) rev |= 0x0010;
        if (hr & POLLNVAL) rev |= 0x0020;
#else
        rev = host_fds[i].revents;
#endif
        st<int16_t>(fds_ptr + i * 8 + 6, rev);
    }
    c.x[0] = uint64_t(int64_t(ready));
}

void sock_eventfd(CPU& c) {
    if (!net::online_enabled()) {
        net::g_last_errno = 50;
        c.x[0] = uint64_t(-1);
        return;
    }
    const uint64_t initval = c.x[0];
    const int flags = int(int32_t(c.x[1]));
    const bool nonblock = (flags & 0x3) != 0;  // 0x1 (O_NONBLOCK/EFD) or 0x2 (nn::socket::EventFdFlags::Nonblock)
#if defined(__linux__)
    int efd = ::eventfd(unsigned(initval), nonblock ? EFD_NONBLOCK : 0);
    if (efd < 0) {
        c.x[0] = uint64_t(int64_t(net::capture_host_error()));
        return;
    }
    std::lock_guard lk(net::g_net_mu);
    const int fd = net::g_next_fd++;
    net::g_fds[fd] = {true, efd, -1, initval};
    c.x[0] = uint64_t(int64_t(fd));
#else
    net::ensure_wsa();
    net::HostSocket listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t alen = sizeof(addr);
    if (listener == net::kBadHostSocket ||
        ::bind(listener, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listener, 1) != 0 ||
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &alen) != 0) {
        net::close_host_socket(listener);
        c.x[0] = uint64_t(int64_t(net::capture_host_error()));
        return;
    }
    net::HostSocket client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == net::kBadHostSocket || ::connect(client, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        net::close_host_socket(listener);
        net::close_host_socket(client);
        c.x[0] = uint64_t(int64_t(net::capture_host_error()));
        return;
    }
    net::HostSocket server = ::accept(listener, nullptr, nullptr);
    net::close_host_socket(listener);
    if (server == net::kBadHostSocket) {
        net::close_host_socket(client);
        c.x[0] = uint64_t(int64_t(net::capture_host_error()));
        return;
    }
    if (nonblock) net::set_nonblocking(server, true);
    if (initval > 0) {
        const char b = 1;
        ::send(client, &b, 1, 0);
    }
    std::lock_guard lk(net::g_net_mu);
    const int fd = net::g_next_fd++;
    net::g_fds[fd] = {true, server, client, initval};
    log("socket: EventFd -> fd=%d (tid=%lu)", fd, (unsigned long)GetCurrentThreadId());
    if (fd >= 103 && fd <= 108) {
        backtrace(c);
        // Fallback stack scan in case frame pointers are omitted inside gRPC.
        int hits = 0;
        for (uint64_t off = 0; off < 0x1200 && hits < 16; off += 8) {
            const uint64_t v = ld<uint64_t>(c.x[31] + off);
            if (v >= 0x4000000000ULL && v < 0x4004000000ULL) {
                ++hits;
                log("socket:   sp+%#llx -> main+%#llx", (unsigned long long)off,
                    (unsigned long long)(v - 0x4000000000ULL));
            }
        }
    }
    c.x[0] = uint64_t(int64_t(fd));
#endif
}

void sock_eventfd_read(CPU& c) {
    const int fd = int(int32_t(c.x[0]));
    const uint64_t out_ptr = c.x[1];
#if defined(__linux__)
    int efd = -1;
    {
        std::lock_guard lk(net::g_net_mu);
        auto it = net::g_fds.find(fd);
        if (it == net::g_fds.end() || !it->second.is_event) {
            net::g_last_errno = 9;
            c.x[0] = uint64_t(-1);
            return;
        }
        efd = it->second.sock;
    }
    uint64_t v = 0;
    if (::eventfd_read(efd, &v) != 0) {
        c.x[0] = uint64_t(int64_t(net::capture_host_error()));
        return;
    }
    if (out_ptr) st<uint64_t>(out_ptr, v);
    c.x[0] = 0;
#else
    std::lock_guard lk(net::g_net_mu);
    auto it = net::g_fds.find(fd);
    if (it == net::g_fds.end() || !it->second.is_event) {
        net::g_last_errno = 9;
        c.x[0] = uint64_t(-1);
        return;
    }
    char buf[64];
    int total_drained = 0;
    for (;;) {
        int n = ::recv(it->second.sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        total_drained += n;
    }
    if (total_drained == 0 && it->second.event_counter == 0) {
        net::set_guest_errno(11);
        c.x[0] = uint64_t(-1);
        return;
    }
    const uint64_t val = it->second.event_counter;
    it->second.event_counter = 0;
    static int efd_r_logs = 0;
    if (efd_r_logs < 15 || (fd >= 105 && efd_r_logs < 45)) {
        ++efd_r_logs;
        log("socket: EventFdRead(fd=%d) -> val=%llu", fd, (unsigned long long)val);
    }
    if (out_ptr) st<uint64_t>(out_ptr, val);
    c.x[0] = 0;
#endif
}

void sock_eventfd_write(CPU& c) {
    const int fd = int(int32_t(c.x[0]));
    const uint64_t val = c.x[1];
#if defined(__linux__)
    int efd = -1;
    {
        std::lock_guard lk(net::g_net_mu);
        auto it = net::g_fds.find(fd);
        if (it == net::g_fds.end() || !it->second.is_event) {
            net::g_last_errno = 9;
            c.x[0] = uint64_t(-1);
            return;
        }
        efd = it->second.sock;
    }
    if (::eventfd_write(efd, val) != 0) {
        c.x[0] = uint64_t(int64_t(net::capture_host_error()));
        return;
    }
    c.x[0] = 0;
#else
    std::lock_guard lk(net::g_net_mu);
    auto it = net::g_fds.find(fd);
    if (it == net::g_fds.end() || !it->second.is_event) {
        net::g_last_errno = 9;
        c.x[0] = uint64_t(-1);
        return;
    }
    const bool was_zero = it->second.event_counter == 0;
    it->second.event_counter += val;
    static int efd_w_logs = 0;
    if (efd_w_logs < 15 || (fd >= 105 && efd_w_logs < 45)) {
        ++efd_w_logs;
        log("socket: EventFdWrite(fd=%d, val=%llu)", fd, (unsigned long long)val);
    }
    if (was_zero && it->second.event_counter > 0) {
        const char b = 1;
        ::send(it->second.event_peer, &b, 1, 0);
    }
    c.x[0] = 0;
#endif
}

void sock_getaddrinfo_impl(CPU& c, bool cpp_layout) {
    if (!net::online_enabled()) {
        c.x[0] = 4;  // EAI_FAIL
        return;
    }
    const char* node = reinterpret_cast<const char*>(c.x[0]);
    const char* service = reinterpret_cast<const char*>(c.x[1]);
    const uint64_t hints_ptr = c.x[2];
    const uint64_t res_ptr = c.x[3];
    if (!res_ptr) {
        c.x[0] = 4;
        return;
    }
    st<uint64_t>(res_ptr, 0);
    uint16_t port = 0;
    if (service && *service) port = uint16_t(std::strtoul(service, nullptr, 10));
    int socktype = net::kNxSockStream;
    int proto = IPPROTO_TCP;
    if (hints_ptr) {
        socktype = ld<int32_t>(hints_ptr + 8);
        proto = ld<int32_t>(hints_ptr + 12);
    }
    std::string target = node ? node : "";
    bool is_nat = false;
    const std::string redir = net::redirect_target_host(target, is_nat);
    const std::string query_host = redir.empty() ? target : redir;
    uint32_t ipv4_be = 0;
    if (query_host.empty()) {
        ipv4_be = htonl(INADDR_ANY);
    } else if (!net::resolve_host_ipv4(query_host, ipv4_be)) {
        log("socket: GetAddrInfo(%s, %s) -> FAIL resolve(%s)", node ? node : "", service ? service : "",
            query_host.c_str());
        c.x[0] = 4;
        return;
    }
    log("socket: GetAddrInfo(%s, %s, cpp=%d, st=%d, proto=%d) -> %s (%#x)", node ? node : "", service ? service : "",
        int(cpp_layout), socktype, proto, query_host.c_str(), ipv4_be);
    if (!redir.empty() && node && *node) {
        std::lock_guard lk(net::g_net_mu);
        net::g_sni_map[ipv4_be] = node;
        log("net: DNS redirect %s -> %s", node, query_host.c_str());
    }
    // Allocate one Horizon AddrInfo (48 bytes) + SockAddrIn (16 bytes) in guest-visible pool.
    const uint64_t mem = net::alloc_addr_mem(64);
    if (!mem) {
        c.x[0] = 10;  // EAI_MEMORY
        return;
    }
    std::memset(reinterpret_cast<void*>(mem), 0, 64);
    const uint64_t sa_ptr = mem + 48;
    st<uint8_t>(sa_ptr + 0, 16);
    st<uint8_t>(sa_ptr + 1, uint8_t(net::kNxAfInet));
    st<uint16_t>(sa_ptr + 2, htons(port));
    st<uint32_t>(sa_ptr + 4, ipv4_be);

    st<int32_t>(mem + 0x00, 0);
    st<int32_t>(mem + 0x04, net::kNxAfInet);
    st<int32_t>(mem + 0x08, socktype);
    st<int32_t>(mem + 0x0C, proto);
    st<uint32_t>(mem + 0x10, 16);
    if (cpp_layout) {
        st<uint64_t>(mem + 0x18, sa_ptr);  // nn::socket::AddrInfo::ai_addr
        st<uint64_t>(mem + 0x20, 0);       // nn::socket::AddrInfo::ai_canonname
    } else {
        st<uint64_t>(mem + 0x18, 0);       // nnsocket AddrInfo::ai_canonname (FreeBSD)
        st<uint64_t>(mem + 0x20, sa_ptr);  // nnsocket AddrInfo::ai_addr
    }
    st<uint64_t>(mem + 0x28, 0);       // ai_next
    st<uint64_t>(res_ptr, mem);
    c.x[0] = 0;
}

void sock_getaddrinfo_cpp(CPU& c) { sock_getaddrinfo_impl(c, true); }
void sock_getaddrinfo_c(CPU& c) { sock_getaddrinfo_impl(c, false); }

void sock_freeaddrinfo(CPU& c) {
    if (c.x[0]) {
        std::lock_guard lk(net::g_net_mu);
        net::free_addr_mem(c.x[0]);
    }
    c.x[0] = 0;
}

void sock_last_errno(CPU& c) { c.x[0] = uint64_t(net::g_last_errno); }

void sock_gai_strerror(CPU& c) {
    static uint64_t msg_addr = 0;
    if (!msg_addr) {
        msg_addr = kernel_alloc(64, 16);
        std::memcpy(reinterpret_cast<void*>(msg_addr), "address lookup error", 21);
    }
    c.x[0] = msg_addr;
}

void sock_inet_ntop(CPU& c) {
    const int af = int(int32_t(c.x[0]));
    const void* src = reinterpret_cast<const void*>(c.x[1]);
    char* dst = reinterpret_cast<char*>(c.x[2]);
    const uint32_t size = uint32_t(c.x[3]);
    const char* out = ::inet_ntop(af == net::kNxAfInet ? AF_INET : AF_INET6, src, dst, size);
    c.x[0] = out ? c.x[2] : 0;
}

void sock_inet_pton(CPU& c) {
    const int af = int(int32_t(c.x[0]));
    const char* src = reinterpret_cast<const char*>(c.x[1]);
    void* dst = reinterpret_cast<void*>(c.x[2]);
    c.x[0] = uint64_t(int64_t(::inet_pton(af == net::kNxAfInet ? AF_INET : AF_INET6, src, dst)));
}

}  // namespace

void dump_net_state() {
    {
        std::lock_guard lk(net::g_net_mu);
        for (const auto& [fd, e] : net::g_fds) {
#if defined(_WIN32)
            u_long avail = 0;
            ioctlsocket(e.sock, FIONREAD, &avail);
            WSAPOLLFD pfd{};
            pfd.fd = e.sock;
            pfd.events = POLLIN | POLLOUT;
            int pr = WSAPoll(&pfd, 1, 0);
            int serr = 0;
            int slen = sizeof(serr);
            getsockopt(e.sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&serr), &slen);
            log("netstate: fd=%d ev=%d avail=%lu poll=%d rev=0x%x so_err=%d",
                fd, e.is_event ? 1 : 0, (unsigned long)avail, pr, (unsigned)pfd.revents, serr);
#else
            int avail = 0;
            ::ioctl(e.sock, FIONREAD, &avail);
            pollfd pfd{};
            pfd.fd = e.sock;
            pfd.events = POLLIN | POLLOUT;
            int pr = ::poll(&pfd, 1, 0);
            int serr = 0;
            socklen_t slen = sizeof(serr);
            ::getsockopt(e.sock, SOL_SOCKET, SO_ERROR, &serr, &slen);
            log("netstate: fd=%d ev=%d avail=%d poll=%d rev=0x%x so_err=%d",
                fd, e.is_event ? 1 : 0, avail, pr, (unsigned)pfd.revents, serr);
#endif
        }
    }
    std::lock_guard lk(net::g_ring_mu);
    log("netstate: last_poll=[%s]", net::g_active_poll);
    for (uint32_t i = 0; i < 16; ++i) {
        const auto& e = net::g_op_ring[(net::g_op_head + i) & 15u];
        if (!e.op[0]) continue;
        log("netstate: op=%s fd=%d res=%lld extra=%d", e.op, e.fd, (long long)e.res, e.extra);
    }
}

// ---- nn::nifm ----
NX_HLE("_ZN2nn4nifm10InitializeEv", ok);
NX_HLE("_ZN2nn4nifm20SubmitNetworkRequestEv", ok);
NX_HLE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", ok_if_online_else_nifm);
NX_HLE("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", no);
NX_HLE("_ZN2nn4nifm20CancelNetworkRequestEv", ok);
NX_HLE("_ZN2nn4nifm18IsNetworkAvailableEv", yes_if_online);
NX_HLE("_ZN2nn4nifm28IsAnyInternetRequestAcceptedENS0_8ClientIdE", yes_if_online);
NX_HLE("_ZN2nn4nifm30IsAnyForegroundRequestAcceptedEv", yes_if_online);
NX_HLE("_ZN2nn4nifm27GetInternetConnectionStatusEPNS0_24InternetConnectionStatusE", nifm_get_internet_status);
NX_HLE("_ZN2nn4nifm22GetCurrentIpConfigInfoEPNS_6socket6InAddrES3_S3_S3_S3_", nifm_get_ip_config);
NX_HLE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", nifm_get_primary_ip);
NX_HLE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", nifm_get_primary_ip);
NX_HLE("_ZN2nn4nifm30IsWirelessCommunicationEnabledEv", yes_if_online);
NX_HLE("_ZN2nn4nifm30IsEthernetCommunicationEnabledEv", no);
NX_HLE("_ZN2nn4nifm19SetLocalNetworkModeEb", ok);
NX_HLE("_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", nifm_get_ip_config);
NX_HLE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", ok_if_online_else_nifm);
NX_HLE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", nifm_get_proxy);

// ---- nn::nsd ----
NX_HLE("_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", nsd_get_env);
NX_HLE("_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", nsd_resolve_ex);

// ---- nn::socket ----
NX_HLE("_ZN2nn6socket10InitializeERKNS0_6ConfigE", sock_init);
NX_HLE("_ZN2nn6socket10InitializeEPvmmi", sock_init);
NX_HLE("_ZN2nn6socket8FinalizeEv", ok);
NX_HLE("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", sock_socket);
NX_HLE("_ZN2nn6socket6SocketEiii", sock_socket);
NX_HLE("_ZN2nn6socket7ConnectEiPKNS0_8SockAddrEj", sock_connect);
NX_HLE("_ZN2nn6socket7ConnectEiPK8sockaddrj", sock_connect);
NX_HLE("_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", sock_bind);
NX_HLE("_ZN2nn6socket4BindEiPK8sockaddrj", sock_bind);
NX_HLE("_ZN2nn6socket6ListenEii", minus_one);
NX_HLE("_ZN2nn6socket6AcceptEiPNS0_8SockAddrEPj", minus_one);
NX_HLE("_ZN2nn6socket4RecvEiPvmNS0_7MsgFlagE", sock_recv);
NX_HLE("_ZN2nn6socket4RecvEiPvmi", sock_recv);
NX_HLE("_ZN2nn6socket4SendEiPKvmNS0_7MsgFlagE", sock_send);
NX_HLE("_ZN2nn6socket4SendEiPKvmi", sock_send);
NX_HLE("_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", sock_sendto);
NX_HLE("_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", sock_sendto);
NX_HLE("_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", sock_recvfrom);
NX_HLE("_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", sock_recvfrom);
NX_HLE("_ZN2nn6socket4PollEP6pollfdmi", sock_poll);
NX_HLE("_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", sock_eventfd);
NX_HLE("_ZN2nn6socket11EventFdReadEiPm", sock_eventfd_read);
NX_HLE("_ZN2nn6socket12EventFdWriteEim", sock_eventfd_write);
NX_HLE("_ZN2nn6socket5CloseEi", sock_close);
NX_HLE("_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", sock_fcntl);
NX_HLE("_ZN2nn6socket5FcntlEiiz", sock_fcntl);
NX_HLE("_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE", minus_one);
NX_HLE("_ZN2nn6socket8ShutdownEiNS0_14ShutdownMethodE", sock_shutdown);
NX_HLE("_ZN2nn6socket8ShutdownEii", sock_shutdown);
NX_HLE("_ZN2nn6socket10GetSockOptEiNS0_5LevelENS0_6OptionEPvPj", minus_one);
NX_HLE("_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", sock_setsockopt);
NX_HLE("_ZN2nn6socket12GetLastErrorEv", sock_last_errno);
NX_HLE("_ZN2nn6socket12GetLastErrnoEv", sock_last_errno);
NX_HLE("_ZN2nn6socket11GAIStrErrorENS0_7AiErrnoE", sock_gai_strerror);
NX_HLE("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", sock_getaddrinfo_cpp);
NX_HLE("_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", sock_getaddrinfo_cpp);
NX_HLE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", sock_freeaddrinfo);
NX_HLE("_ZN2nn6socket12FreeAddrInfoEP8addrinfo", sock_freeaddrinfo);
NX_HLE("_ZN2nn6socket8InetNtopEiPKvPcj", sock_inet_ntop);
NX_HLE("_ZN2nn6socket8InetPtonEiPKcPv", sock_inet_pton);
NX_HLE("_ZN2nn6socket9InetHtonlEj", htonl_);
NX_HLE("_ZN2nn6socket9InetHtonsEt", htons_);
NX_HLE("_ZN2nn6socket9InetNtohlEj", htonl_);
NX_HLE("_ZN2nn6socket9InetNtohsEt", htons_);

// ---- C-style nnsocket* symbols (used by gRPC inside main) ----
NX_HLE("nnsocketSocket", sock_socket);
NX_HLE("nnsocketConnect", sock_connect);
NX_HLE("nnsocketBind", sock_bind);
NX_HLE("nnsocketListen", minus_one);
NX_HLE("nnsocketAccept", minus_one);
NX_HLE("nnsocketRecv", sock_recv);
NX_HLE("nnsocketRecvMsg", sock_recvmsg);
NX_HLE("nnsocketSend", sock_send);
NX_HLE("nnsocketSendMsg", sock_sendmsg);
NX_HLE("nnsocketPoll", sock_poll);
NX_HLE("nnsocketClose", sock_close);
NX_HLE("nnsocketFcntl", sock_fcntl);
NX_HLE("nnsocketShutdown", sock_shutdown);
NX_HLE("nnsocketGetSockOpt", sock_getsockopt);
NX_HLE("nnsocketSetSockOpt", sock_setsockopt);
NX_HLE("nnsocketGetLastErrno", sock_last_errno);
NX_HLE("nnsocketSetLastErrno", sock_set_last_errno);
NX_HLE("nnsocketGAIStrError", sock_gai_strerror);
NX_HLE("nnsocketGetAddrInfo", sock_getaddrinfo_c);
NX_HLE("nnsocketFreeAddrInfo", sock_freeaddrinfo);
NX_HLE("nnsocketGetPeerName", sock_getpeername);
NX_HLE("nnsocketGetSockName", sock_getsockname);
NX_HLE("nnsocketInetHtonl", htonl_);
NX_HLE("nnsocketInetHtons", htons_);
NX_HLE("nnsocketInetNtohl", htonl_);
NX_HLE("nnsocketInetNtohs", htons_);
NX_HLE("nnsocketResolverGetOption", ok);
NX_HLE("nnsocketResolverSetOption", ok);
NX_HLE("__errno_location", hle_errno_location);

// ---- nn::err & gRPC weak TLS init ----
NX_HLE("_ZN2nn3err9ShowErrorENS_6ResultE", show_error);
NX_HLE("_ZN2nn3err9ShowErrorENS_6ResultERKNS0_12ErrorContextE", show_error);
NX_HLE("_ZN2nn3err9ShowErrorENS_6ResultERKNS0_15ResultBacktraceE", show_error);
NX_HLE("_ZN2nn3err15ShowErrorViewerENS_6ResultE", show_error);
NX_HLE("_ZN2nn3err9ShowErrorENS0_9ErrorCodeE", show_error_code);
NX_HLE("_ZN2nn3err9ShowErrorENS0_9ErrorCodeERKNS0_12ErrorContextE", show_error_code);
NX_HLE("_ZN2nn3err15ShowErrorViewerENS0_9ErrorCodeE", show_error_code);
NX_HLE("_ZN2nn3err9ShowErrorERKNS0_18ErrorResultVariantE", show_error_variant);
NX_HLE("_ZN2nn3err20ShowApplicationErrorERKNS0_19ApplicationErrorArgE", show_error_variant);
NX_HLE("_ZTHN9grpc_core7ExecCtx9exec_ctx_E", ok);
NX_HLE("_ZTHN9grpc_core26ApplicationCallbackExecCtx18callback_exec_ctx_E", ok);

}  // namespace nx
