// Nextendo Network OAuth 2.0 PKCE client, nex-token manager, BAAS id_token RS256 signer and session heartbeat.
#include "nextendo_client.h"

#include <nx/runtime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string_view>
static inline std::filesystem::path nx_u8path(std::string_view s) { return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(s.data()), s.size())); }
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <random>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace nx::nextendo {

namespace {

constexpr const char* kDefaultClientId = "nxc_d91GmCAGSfSp";
constexpr uint16_t kCallbackPort = 47823;
constexpr const char* kRedirectUri = "http://127.0.0.1:47823/callback";

std::mutex g_mu;
std::string g_data_root;
bool g_enabled = false;
std::string g_client_id = kDefaultClientId;
std::string g_access_token;
std::string g_refresh_token;
std::string g_nex_token;
uint64_t g_pid = 0;
std::string g_username;
std::string g_friend_code;
int64_t g_nex_expires_at = 0;
std::string g_last_error;
std::atomic<bool> g_login_busy{false};
std::atomic<bool> g_heartbeat_stop{false};
std::thread g_heartbeat_thread;
std::string g_cached_jwt;
int64_t g_cached_jwt_exp = 0;

std::string api_base_host() {
    if (const char* ov = std::getenv("NEXTENDO_API"); ov && *ov) {
        std::string s = ov;
        if (s.rfind("https://", 0) == 0) s = s.substr(8);
        else if (s.rfind("http://", 0) == 0) s = s.substr(7);
        while (!s.empty() && s.back() == '/') s.pop_back();
        return s;
    }
    return "nextendo.network";
}

std::string base64url(const uint8_t* data, size_t len) {
    static constexpr char kTbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t a = data[i];
        const uint32_t b = (i + 1 < len) ? data[i + 1] : 0;
        const uint32_t c = (i + 2 < len) ? data[i + 2] : 0;
        const uint32_t triple = (a << 16) | (b << 8) | c;
        out.push_back(kTbl[(triple >> 18) & 0x3F]);
        out.push_back(kTbl[(triple >> 12) & 0x3F]);
        if (i + 1 < len) out.push_back(kTbl[(triple >> 6) & 0x3F]);
        if (i + 2 < len) out.push_back(kTbl[triple & 0x3F]);
    }
    return out;
}

std::string base64url_str(const std::string& s) {
    return base64url(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

std::string url_encode(const std::string& s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : s) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out.push_back(char(ch));
        } else {
            out.push_back('%');
            out.push_back(kHex[(ch >> 4) & 0xF]);
            out.push_back(kHex[ch & 0xF]);
        }
    }
    return out;
}

std::string json_get_string(const std::string& json, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return {};
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) return {};
    p = json.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos || json[p] != '"') return {};
    ++p;
    std::string out;
    while (p < json.size() && json[p] != '"') {
        if (json[p] == '\\' && p + 1 < json.size()) {
            ++p;
        }
        out.push_back(json[p++]);
    }
    return out;
}

int64_t json_get_int(const std::string& json, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return 0;
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) return 0;
    p = json.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos) return 0;
    return std::strtoll(json.c_str() + p, nullptr, 10);
}

void save_locked() {
    if (g_data_root.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(nx_u8path(g_data_root), ec);
    const auto p = nx_u8path(g_data_root) / "nextendo.ini";
    std::ofstream f(p, std::ios::trunc);
    if (!f) return;
    f << "enabled=" << (g_enabled ? 1 : 0) << "\n";
    f << "client_id=" << g_client_id << "\n";
    f << "access_token=" << g_access_token << "\n";
    f << "refresh_token=" << g_refresh_token << "\n";
    f << "nex_token=" << g_nex_token << "\n";
    f << "pid=" << g_pid << "\n";
    f << "username=" << g_username << "\n";
    f << "friend_code=" << g_friend_code << "\n";
    f << "nex_expires_at=" << g_nex_expires_at << "\n";
}

void load_locked() {
    if (g_data_root.empty()) return;
    const auto p = nx_u8path(g_data_root) / "nextendo.ini";
    std::ifstream f(p);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);
        if (k == "enabled") g_enabled = (v == "1");
        else if (k == "client_id" && !v.empty()) g_client_id = v;
        else if (k == "access_token") g_access_token = v;
        else if (k == "refresh_token") g_refresh_token = v;
        else if (k == "nex_token") g_nex_token = v;
        else if (k == "pid") g_pid = std::strtoull(v.c_str(), nullptr, 10);
        else if (k == "username") g_username = v;
        else if (k == "friend_code") g_friend_code = v;
        else if (k == "nex_expires_at") g_nex_expires_at = std::strtoll(v.c_str(), nullptr, 10);
    }
    if (const char* env_cid = std::getenv("NX_NEXTENDO_CLIENT_ID"); env_cid && *env_cid) g_client_id = env_cid;
}

#if defined(_WIN32)
bool http_request(const std::string& method, const std::string& path, const std::string& headers,
                  const std::string& body, uint32_t& out_status, std::string& out_body) {
    out_status = 0;
    out_body.clear();
    const std::string host = api_base_host();
    std::wstring whost(host.begin(), host.end());
    std::wstring wmethod(method.begin(), method.end());
    std::wstring wpath(path.begin(), path.end());
    std::wstring wheaders(headers.begin(), headers.end());

    HINTERNET ses = WinHttpOpen(L"SuperWonders/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    HINTERNET con = WinHttpConnect(ses, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, wmethod.c_str(), wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                        : nullptr;
    bool ok = false;
    if (req && WinHttpSendRequest(req, wheaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wheaders.c_str(),
                                  wheaders.empty() ? 0 : DWORD(wheaders.size()),
                                  body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                  DWORD(body.size()), DWORD(body.size()), 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD st = 0;
        DWORD sz = sizeof(st);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &st, &sz, WINHTTP_NO_HEADER_INDEX);
        out_status = uint32_t(st);
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(req, chunk.data(), avail, &read) || read == 0) break;
            chunk.resize(read);
            out_body += chunk;
        }
        ok = true;
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

void random_bytes(uint8_t* dst, size_t n) {
    BCryptGenRandom(nullptr, dst, ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

bool sha256_bytes(const uint8_t* in, size_t len, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
    bool ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
              BCryptHashData(hash, const_cast<PUCHAR>(in), ULONG(len), 0) == 0 &&
              BCryptFinishHash(hash, out, 32, 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

std::string sign_rs256_jwt(const std::string& header_and_payload) {
    BCRYPT_ALG_HANDLE rsa_alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptOpenAlgorithmProvider(&rsa_alg, BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0) return {};

    const auto key_path = nx_u8path(g_data_root) / "nextendo-rsa.bin";
    std::vector<uint8_t> blob;
    {
        std::ifstream kf(key_path, std::ios::binary | std::ios::ate);
        if (kf && kf.tellg() > 0) {
            blob.resize(size_t(kf.tellg()));
            kf.seekg(0);
            kf.read(reinterpret_cast<char*>(blob.data()), std::streamsize(blob.size()));
        }
    }
    bool loaded = !blob.empty() &&
                  BCryptImportKeyPair(rsa_alg, nullptr, BCRYPT_RSAPRIVATE_BLOB, &key, blob.data(), ULONG(blob.size()),
                                      0) == 0;
    if (!loaded) {
        if (BCryptGenerateKeyPair(rsa_alg, &key, 2048, 0) == 0 && BCryptFinalizeKeyPair(key, 0) == 0) {
            ULONG sz = 0;
            if (BCryptExportKey(key, nullptr, BCRYPT_RSAPRIVATE_BLOB, nullptr, 0, &sz, 0) == 0 && sz > 0) {
                blob.resize(sz);
                if (BCryptExportKey(key, nullptr, BCRYPT_RSAPRIVATE_BLOB, blob.data(), sz, &sz, 0) == 0) {
                    std::ofstream kf(key_path, std::ios::binary | std::ios::trunc);
                    if (kf) kf.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(blob.size()));
                }
            }
            loaded = true;
        }
    }
    std::string sig_b64;
    uint8_t digest[32]{};
    if (loaded &&
        sha256_bytes(reinterpret_cast<const uint8_t*>(header_and_payload.data()), header_and_payload.size(), digest)) {
        BCRYPT_PKCS1_PADDING_INFO pad{BCRYPT_SHA256_ALGORITHM};
        ULONG sig_len = 0;
        if (BCryptSignHash(key, &pad, digest, sizeof(digest), nullptr, 0, &sig_len, BCRYPT_PAD_PKCS1) == 0 &&
            sig_len > 0) {
            std::vector<uint8_t> sig(sig_len);
            if (BCryptSignHash(key, &pad, digest, sizeof(digest), sig.data(), sig_len, &sig_len, BCRYPT_PAD_PKCS1) == 0)
                sig_b64 = base64url(sig.data(), sig_len);
        }
    }
    if (key) BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(rsa_alg, 0);
    return sig_b64;
}

void open_browser_url(const std::string& url) {
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
#else
bool run_exec_capture(const std::vector<std::string>& args, const std::string& stdin_data,
                      std::string& stdout_data, int& exit_code) {
    stdout_data.clear();
    exit_code = -1;
    int in_pipe[2]{-1, -1};
    int out_pipe[2]{-1, -1};
    if (pipe(in_pipe) != 0) return false;
    if (pipe(out_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return false;
    }
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    size_t written = 0;
    while (written < stdin_data.size()) {
        ssize_t w = write(in_pipe[1], stdin_data.data() + written, stdin_data.size() - written);
        if (w <= 0) break;
        written += size_t(w);
    }
    close(in_pipe[1]);
    char buf[4096];
    for (;;) {
        ssize_t r = read(out_pipe[0], buf, sizeof(buf));
        if (r <= 0) break;
        stdout_data.append(buf, size_t(r));
    }
    close(out_pipe[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
    return exit_code == 0;
}

bool http_request(const std::string& method, const std::string& path, const std::string& headers,
                  const std::string& body, uint32_t& out_status, std::string& out_body) {
    out_status = 0;
    out_body.clear();
    const std::string url = "https://" + api_base_host() + path;
    std::vector<std::string> args = {
        "curl", "-sS", "--max-time", "15", "-X", method, "-w", "\n%{http_code}", url,
    };
    std::istringstream hs(headers);
    std::string line;
    while (std::getline(hs, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (!line.empty()) {
            args.push_back("-H");
            args.push_back(line);
        }
    }
    if (!body.empty()) {
        args.push_back("--data-binary");
        args.push_back("@-");
    }
    std::string raw;
    int rc = -1;
    if (!run_exec_capture(args, body, raw, rc)) return false;
    const size_t nl = raw.rfind('\n');
    if (nl == std::string::npos) return false;
    out_status = uint32_t(std::strtoul(raw.substr(nl + 1).c_str(), nullptr, 10));
    out_body = raw.substr(0, nl);
    return out_status > 0;
}

void random_bytes(uint8_t* dst, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t got = 0;
        while (got < n) {
            ssize_t r = read(fd, dst + got, n - got);
            if (r <= 0) break;
            got += size_t(r);
        }
        close(fd);
        if (got == n) return;
    }
    std::random_device rd;
    for (size_t i = 0; i < n; ++i) dst[i] = uint8_t(rd());
}

bool sha256_bytes(const uint8_t* in, size_t len, uint8_t out[32]) {
    static constexpr uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rotr = [](uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); };
    std::vector<uint8_t> msg(in, in + len);
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) msg.push_back(0);
    const uint64_t bits = uint64_t(len) * 8;
    for (int i = 7; i >= 0; --i) msg.push_back(uint8_t((bits >> (i * 8)) & 0xFF));
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64]{};
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(msg[off + i * 4]) << 24) | (uint32_t(msg[off + i * 4 + 1]) << 16) |
                   (uint32_t(msg[off + i * 4 + 2]) << 8) | uint32_t(msg[off + i * 4 + 3]);
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    for (int i = 0; i < 8; ++i) {
        out[i * 4 + 0] = uint8_t((h[i] >> 24) & 0xFF);
        out[i * 4 + 1] = uint8_t((h[i] >> 16) & 0xFF);
        out[i * 4 + 2] = uint8_t((h[i] >> 8) & 0xFF);
        out[i * 4 + 3] = uint8_t(h[i] & 0xFF);
    }
    return true;
}

std::string sign_rs256_jwt(const std::string& header_and_payload) {
    if (g_data_root.empty()) return {};
    const auto key_path = nx_u8path(g_data_root) / "nextendo-rsa.pem";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(key_path, ec)) {
        std::string out;
        int rc = -1;
        run_exec_capture({"openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048",
                          "-out", key_path.string()},
                         "", out, rc);
    }
    std::string sig;
    int rc = -1;
    if (!run_exec_capture({"openssl", "dgst", "-sha256", "-sign", key_path.string()},
                          header_and_payload, sig, rc) ||
        sig.empty())
        return {};
    return base64url(reinterpret_cast<const uint8_t*>(sig.data()), sig.size());
}

void open_browser_url(const std::string& url) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("xdg-open", "xdg-open", url.c_str(), nullptr);
        _exit(127);
    }
}
#endif

std::string random_hex(size_t bytes_count) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::vector<uint8_t> buf(bytes_count, 0);
    random_bytes(buf.data(), buf.size());
    std::string out;
    out.reserve(bytes_count * 2);
    for (uint8_t b : buf) {
        out.push_back(kHex[(b >> 4) & 0xF]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

bool refresh_oauth_token_locked() {
    if (g_refresh_token.empty() || g_client_id.empty()) return false;
    const std::string form =
        "grant_type=refresh_token&refresh_token=" + url_encode(g_refresh_token) +
        "&client_id=" + url_encode(g_client_id);
    uint32_t st = 0;
    std::string body;
    const std::string hdrs =
        "Content-Type: application/x-www-form-urlencoded\r\nX-Nextendo-Client-Id: " + g_client_id + "\r\n";
    if (http_request("POST", "/api/oauth/token", hdrs, form, st, body) && st == 200) {
        const std::string at = json_get_string(body, "access_token");
        const std::string rt = json_get_string(body, "refresh_token");
        if (!at.empty()) g_access_token = at;
        if (!rt.empty()) g_refresh_token = rt;
        save_locked();
        log("nextendo: refreshed OAuth access_token");
        return !at.empty();
    }
    log("nextendo: OAuth refresh failed (HTTP %lu)", (unsigned long)st);
    return false;
}

bool fetch_nex_token_locked() {
    if (g_access_token.empty() || g_client_id.empty()) return false;
    uint32_t st = 0;
    std::string body;
    const std::string hdrs =
        "Authorization: Bearer " + g_access_token + "\r\nX-Nextendo-Client-Id: " + g_client_id + "\r\n";
    if (!http_request("GET", "/api/nex-token", hdrs, "", st, body)) {
        g_last_error = "Network error contacting Nextendo /api/nex-token";
        return false;
    }
    if (st == 200) {
        const std::string tok = json_get_string(body, "nex_token");
        const int64_t pid = json_get_int(body, "pid");
        if (!tok.empty() && pid > 0) {
            g_nex_token = tok;
            g_pid = uint64_t(pid);
            g_username = json_get_string(body, "username");
            g_friend_code = json_get_string(body, "friend_code");
            const int64_t exp_in = json_get_int(body, "expires_in");
            g_nex_expires_at = std::time(nullptr) + (exp_in > 0 ? exp_in : 86400);
            g_cached_jwt.clear();
            g_last_error.clear();
            save_locked();
            log("nextendo: signed in as %s (pid=%llu, friend_code=%s)",
                g_username.c_str(), (unsigned long long)g_pid, g_friend_code.c_str());
            return true;
        }
    }
    g_last_error = "Nextendo /api/nex-token failed (HTTP " + std::to_string(st) + ")";
    return false;
}

void set_env_var(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

}  // namespace

void init(const std::string& data_root) {
    std::lock_guard lk(g_mu);
    g_data_root = data_root;
    load_locked();
    if (g_enabled) set_env_var("NX_ONLINE_MODE", "1");
}

void shutdown() {
    g_heartbeat_stop = true;
    if (g_heartbeat_thread.joinable()) g_heartbeat_thread.join();
}

Status status() {
    std::lock_guard lk(g_mu);
    return {g_enabled, !g_nex_token.empty() && g_pid != 0, g_login_busy.load(),
            g_client_id, g_pid, g_username, g_friend_code, g_last_error};
}

void set_enabled(bool enabled) {
    std::lock_guard lk(g_mu);
    g_enabled = enabled;
    set_env_var("NX_ONLINE_MODE", enabled ? "1" : "0");
    save_locked();
}

void set_client_id(const std::string& client_id) {
    std::lock_guard lk(g_mu);
    g_client_id = client_id.empty() ? kDefaultClientId : client_id;
    save_locked();
}

void sign_out() {
    std::lock_guard lk(g_mu);
    g_access_token.clear();
    g_refresh_token.clear();
    g_nex_token.clear();
    g_pid = 0;
    g_username.clear();
    g_friend_code.clear();
    g_nex_expires_at = 0;
    g_cached_jwt.clear();
    g_last_error.clear();
    save_locked();
}

bool begin_oauth_login() {
    if (g_login_busy.exchange(true)) return false;
    std::string client_id;
    {
        std::lock_guard lk(g_mu);
        client_id = g_client_id;
    }
    std::thread([client_id]() {
        uint8_t verifier_raw[32]{};
        random_bytes(verifier_raw, sizeof(verifier_raw));
        const std::string verifier = base64url(verifier_raw, sizeof(verifier_raw));
        uint8_t challenge_raw[32]{};
        sha256_bytes(reinterpret_cast<const uint8_t*>(verifier.data()), verifier.size(), challenge_raw);
        const std::string challenge = base64url(challenge_raw, sizeof(challenge_raw));

#if defined(_WIN32)
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
        using sock_t = SOCKET;
        constexpr sock_t kBadSock = INVALID_SOCKET;
        auto close_sock = [](sock_t s) { closesocket(s); };
#else
        using sock_t = int;
        constexpr sock_t kBadSock = -1;
        auto close_sock = [](sock_t s) { close(s); };
#endif
        sock_t srv = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        int yes = 1;
        if (srv != kBadSock) setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(kCallbackPort);
        if (srv == kBadSock || ::bind(srv, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(srv, 1) != 0) {
            if (srv != kBadSock) close_sock(srv);
            std::lock_guard lk(g_mu);
            g_last_error = "Could not open local OAuth callback listener on 127.0.0.1:47823";
            g_login_busy = false;
            return;
        }

        const std::string auth_url = "https://" + api_base_host() + "/oauth/authorize?client_id=" +
                                     url_encode(client_id) + "&redirect_uri=" + url_encode(kRedirectUri) +
                                     "&response_type=code&scope=" + url_encode("identity friends game.matchmaking") +
                                     "&code_challenge=" + url_encode(challenge) + "&code_challenge_method=S256";
        open_browser_url(auth_url);

#if defined(_WIN32)
        DWORD timeout_ms = 180000;
        setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
        timeval tv{180, 0};
        setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        sock_t cli = ::accept(srv, nullptr, nullptr);
        close_sock(srv);
        std::string code;
        if (cli != kBadSock) {
            char req_buf[4096]{};
            int n = ::recv(cli, req_buf, sizeof(req_buf) - 1, 0);
            if (n > 0) {
                std::string req(req_buf, n);
                const size_t cpos = req.find("code=");
                if (cpos != std::string::npos) {
                    size_t end = req.find_first_of(" &\r\n", cpos + 5);
                    code = req.substr(cpos + 5, end == std::string::npos ? std::string::npos : end - (cpos + 5));
                }
            }
            const char* html =
                "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
                "<html><body style='font-family:sans-serif;background:#111;color:#eee;padding:40px'>"
                "<h2>Nextendo login complete</h2><p>You can close this tab and return to SuperWonders.</p>"
                "</body></html>";
            ::send(cli, html, int(std::strlen(html)), 0);
            close_sock(cli);
        }
        if (code.empty()) {
            std::lock_guard lk(g_mu);
            g_last_error = "OAuth login cancelled or timed out";
            g_login_busy = false;
            return;
        }

        const std::string form =
            "grant_type=authorization_code&code=" + url_encode(code) + "&redirect_uri=" + url_encode(kRedirectUri) +
            "&client_id=" + url_encode(client_id) + "&code_verifier=" + url_encode(verifier);
        uint32_t st = 0;
        std::string body;
        const std::string hdrs =
            "Content-Type: application/x-www-form-urlencoded\r\nX-Nextendo-Client-Id: " + client_id + "\r\n";
        if (http_request("POST", "/api/oauth/token", hdrs, form, st, body) && st == 200) {
            std::lock_guard lk(g_mu);
            g_access_token = json_get_string(body, "access_token");
            g_refresh_token = json_get_string(body, "refresh_token");
            fetch_nex_token_locked();
        } else {
            std::lock_guard lk(g_mu);
            g_last_error = "OAuth token exchange failed (HTTP " + std::to_string(st) + ")";
        }
        g_login_busy = false;
    }).detach();
    return true;
}

std::string get_baas_id_token(const std::string& game_version) {
    std::lock_guard lk(g_mu);
    const int64_t now = std::time(nullptr);
    if (g_nex_token.empty() || now + 60 >= g_nex_expires_at) {
        if (!g_access_token.empty()) fetch_nex_token_locked();
    }
    if (g_nex_token.empty() || g_pid == 0) return {};
    if (!g_cached_jwt.empty() && now + 300 < g_cached_jwt_exp) return g_cached_jwt;

    const int64_t exp = now + 3 * 3600;
    const std::string jti = random_hex(16);
    const std::string di = random_hex(16);
    std::ostringstream payload;
    payload << "{"
            << "\"iss\":\"e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com\","
            << "\"aud\":\"ed9e2f05d286f7b8\","
            << "\"sub\":\"" << g_pid << "\","
            << "\"iat\":" << now << ","
            << "\"exp\":" << exp << ","
            << "\"jti\":\"" << jti << "\","
            << "\"di\":\"" << di << "\","
            << "\"bs:did\":\"" << di << "\","
            << "\"sn\":\"XAW10000000000\","
            << "\"hm\":true,"
            << "\"tv\":\"" << game_version << "\","
            << "\"nnex\":\"" << g_nex_token << "\""
            << "}";
    const std::string hdr = "{\"alg\":\"RS256\",\"typ\":\"id_token\"}";
    const std::string signing_input = base64url_str(hdr) + "." + base64url_str(payload.str());
    const std::string sig = sign_rs256_jwt(signing_input);
    if (sig.empty()) return {};
    g_cached_jwt = signing_input + "." + sig;
    g_cached_jwt_exp = exp;
    return g_cached_jwt;
}

uint64_t current_pid() {
    std::lock_guard lk(g_mu);
    return g_pid;
}

void start_session_heartbeat() {
    std::lock_guard lk(g_mu);
    if (!g_enabled || g_heartbeat_thread.joinable()) return;
    g_heartbeat_stop = false;
    g_heartbeat_thread = std::thread([]() {
        while (!g_heartbeat_stop.load()) {
            std::string tok, cid;
            {
                std::lock_guard lock(g_mu);
                tok = g_nex_token;
                cid = g_client_id;
            }
            if (!tok.empty()) {
                uint32_t st = 0;
                std::string body;
                const std::string hdrs = "Authorization: Bearer " + tok + "\r\nX-Nextendo-Client-Id: " + cid +
                                         "\r\nContent-Type: application/json\r\n";
                http_request("POST", "/api/nex-session", hdrs, "{}", st, body);
            }
            for (int i = 0; i < 60 && !g_heartbeat_stop.load(); ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });
}

std::vector<FriendInfo> get_friends() {
    static std::mutex cache_mu;
    static std::vector<FriendInfo> cached;
    static int64_t cached_at = 0;
    std::lock_guard clk(cache_mu);
    const int64_t now = std::time(nullptr);
    if (now - cached_at < 10 && !cached.empty()) return cached;

    std::vector<FriendInfo> out;
    std::string root, tok, cid;
    {
        std::lock_guard lk(g_mu);
        root = g_data_root;
        tok = g_access_token;
        cid = g_client_id;
    }
    if (!root.empty()) {
        std::ifstream ff(nx_u8path(root) / "friends.txt");
        std::string line;
        while (std::getline(ff, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            size_t sp = line.find_first_of(" \t,");
            std::string pid_s = (sp == std::string::npos) ? line : line.substr(0, sp);
            std::string nick = (sp == std::string::npos) ? "Friend" : line.substr(sp + 1);
            while (!nick.empty() && (nick.front() == ' ' || nick.front() == '\t')) nick.erase(nick.begin());
            uint64_t pid = std::strtoull(pid_s.c_str(), nullptr, 10);
            if (pid != 0) out.push_back({pid, nick.empty() ? "Friend" : nick, 2, true});
        }
    }
    if (!tok.empty() && !cid.empty()) {
        uint32_t st = 0;
        std::string body;
        auto fetch_friends = [&](const std::string& t) {
            const std::string hdrs = "Authorization: Bearer " + t + "\r\nX-Nextendo-Client-Id: " + cid + "\r\n";
            return http_request("GET", "/api/friends", hdrs, "", st, body);
        };
        bool ok = fetch_friends(tok);
        if (ok && st == 401) {
            std::lock_guard lk(g_mu);
            if (refresh_oauth_token_locked()) {
                tok = g_access_token;
                ok = fetch_friends(tok);
            }
        }
        static bool logged_once = false;
        if (!logged_once) {
            logged_once = true;
            log("nextendo: GET /api/friends ok=%d http=%lu %.300s", int(ok), (unsigned long)st, body.c_str());
        }
        if (ok && st == 200) {
            const size_t arr_pos = body.find("\"friends\"");
            size_t pos = (arr_pos != std::string::npos) ? body.find('[', arr_pos) : 0;
            if (pos == std::string::npos) pos = 0;
            while ((pos = body.find('{', pos)) != std::string::npos) {
                int depth = 0;
                bool in_str = false;
                bool esc = false;
                size_t end = pos;
                for (; end < body.size(); ++end) {
                    const char ch = body[end];
                    if (esc) {
                        esc = false;
                        continue;
                    }
                    if (in_str) {
                        if (ch == '\\') esc = true;
                        else if (ch == '"') in_str = false;
                        continue;
                    }
                    if (ch == '"') in_str = true;
                    else if (ch == '{') ++depth;
                    else if (ch == '}' && --depth == 0) break;
                }
                if (end >= body.size()) break;
                std::string obj = body.substr(pos, end - pos + 1);
                const std::string nsa_s = json_get_string(obj, "nsa");
                uint64_t aid = !nsa_s.empty() ? std::strtoull(nsa_s.c_str(), nullptr, 16) : 0;
                if (aid == 0) {
                    int64_t pid = json_get_int(obj, "pid");
                    if (pid <= 0) pid = json_get_int(obj, "baasUserId");
                    if (pid <= 0) pid = std::strtoll(json_get_string(obj, "baasUserId").c_str(), nullptr, 10);
                    if (pid > 0) aid = uint64_t(pid);
                }
                std::string uname = json_get_string(obj, "name");
                if (uname.empty()) uname = json_get_string(obj, "username");
                if (uname.empty()) uname = json_get_string(obj, "nickname");
                if (aid != 0 && std::none_of(out.begin(), out.end(), [&](const FriendInfo& f) { return f.pid == aid; })) {
                    out.push_back({aid, uname.empty() ? "Friend" : uname, 2, true});
                }
                pos = end + 1;
            }
        }
    }
    cached = out;
    cached_at = now;
    return out;
}

}  // namespace nx::nextendo
