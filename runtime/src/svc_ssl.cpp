// Horizon "ssl" IPC service backed by Windows Schannel (with ALPN h2 support for Nextendo NPLN).
#include "ipc.h"
#include "hle_net.h"

#include <nx/runtime.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>
#else
#include <dlfcn.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#endif

namespace nx {

namespace {

constexpr uint32_t kSslModule = 123u;
constexpr uint32_t kSslWouldBlock = (kSslModule << 9) | 204u;
constexpr uint32_t kSslIoError = (kSslModule << 9) | 205u;
constexpr uint32_t kSslInvalidState = (kSslModule << 9) | 103u;

#if defined(_WIN32)
// Build SECBUFFER_APPLICATION_PROTOCOLS buffer for Schannel ALPN negotiation.
std::vector<uint8_t> build_alpn_buffer(const std::string& proto) {
    // SEC_APPLICATION_PROTOCOL_LIST: ProtocolListId (u32), ProtocolListSize (u16), byte[len+1]
    const uint16_t list_bytes = uint16_t(1 + proto.size());
    std::vector<uint8_t> buf(sizeof(uint32_t) + sizeof(uint16_t) + list_bytes, 0);
    // ExtensionType = SecApplicationProtocolNegotiationExt_ALPN (2)
    uint32_t ext_id = SecApplicationProtocolNegotiationExt_ALPN;
    uint16_t list_size = list_bytes;
    std::memcpy(buf.data(), &ext_id, 4);
    std::memcpy(buf.data() + 4, &list_size, 2);
    buf[6] = uint8_t(proto.size());
    std::memcpy(buf.data() + 7, proto.data(), proto.size());
    return buf;
}
#else
struct SSL_CTX;
struct SSL;
struct SSL_METHOD;

struct OpenSslApi {
    void* handle = nullptr;
    const SSL_METHOD* (*TLS_client_method)() = nullptr;
    SSL_CTX* (*SSL_CTX_new)(const SSL_METHOD*) = nullptr;
    void (*SSL_CTX_free)(SSL_CTX*) = nullptr;
    int (*SSL_CTX_set_default_verify_paths)(SSL_CTX*) = nullptr;
    void (*SSL_CTX_set_verify)(SSL_CTX*, int, void*) = nullptr;
    int (*SSL_CTX_set_alpn_protos)(SSL_CTX*, const unsigned char*, unsigned int) = nullptr;
    SSL* (*SSL_new)(SSL_CTX*) = nullptr;
    void (*SSL_free)(SSL*) = nullptr;
    int (*SSL_set_fd)(SSL*, int) = nullptr;
    long (*SSL_ctrl)(SSL*, int, long, void*) = nullptr;
    int (*SSL_connect)(SSL*) = nullptr;
    int (*SSL_read)(SSL*, void*, int) = nullptr;
    int (*SSL_peek)(SSL*, void*, int) = nullptr;
    int (*SSL_write)(SSL*, const void*, int) = nullptr;
    int (*SSL_pending)(const SSL*) = nullptr;
    int (*SSL_get_error)(const SSL*, int) = nullptr;
    void (*SSL_get0_alpn_selected)(const SSL*, const unsigned char**, unsigned int*) = nullptr;
    bool ok = false;
};

const OpenSslApi& openssl() {
    static const OpenSslApi api = []() {
        OpenSslApi a;
        a.handle = dlopen("libssl.so.3", RTLD_NOW | RTLD_LOCAL);
        if (!a.handle) a.handle = dlopen("libssl.so", RTLD_NOW | RTLD_LOCAL);
        if (!a.handle) {
            log("ssl: dlopen(libssl) failed: %s", dlerror());
            return a;
        }
        auto load = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::decay_t<decltype(fn)>>(dlsym(a.handle, name));
            return fn != nullptr;
        };
        a.ok = load(a.TLS_client_method, "TLS_client_method") &&
               load(a.SSL_CTX_new, "SSL_CTX_new") &&
               load(a.SSL_CTX_free, "SSL_CTX_free") &&
               load(a.SSL_CTX_set_default_verify_paths, "SSL_CTX_set_default_verify_paths") &&
               load(a.SSL_CTX_set_verify, "SSL_CTX_set_verify") &&
               load(a.SSL_CTX_set_alpn_protos, "SSL_CTX_set_alpn_protos") &&
               load(a.SSL_new, "SSL_new") &&
               load(a.SSL_free, "SSL_free") &&
               load(a.SSL_set_fd, "SSL_set_fd") &&
               load(a.SSL_ctrl, "SSL_ctrl") &&
               load(a.SSL_connect, "SSL_connect") &&
               load(a.SSL_read, "SSL_read") &&
               load(a.SSL_peek, "SSL_peek") &&
               load(a.SSL_write, "SSL_write") &&
               load(a.SSL_pending, "SSL_pending") &&
               load(a.SSL_get_error, "SSL_get_error") &&
               load(a.SSL_get0_alpn_selected, "SSL_get0_alpn_selected");
        if (!a.ok) log("ssl: missing required OpenSSL symbols in libssl");
        return a;
    }();
    return api;
}
#endif

struct SslConnectionState {
    int32_t fd = -1;
    std::string host;
    uint32_t verify_opt = 1;
    uint32_t io_mode = 2;  // 1 = NonBlocking, 2 = Blocking
    uint32_t session_cache_mode = 0;
    uint32_t renegotiation_mode = 0;
    std::string alpn_proto = "h2";
    std::string negotiated_alpn;
    bool handshake_done = false;

#if defined(_WIN32)
    CredHandle cred{};
    CtxtHandle ctx{};
    bool has_cred = false;
    bool has_ctx = false;
    SecPkgContext_StreamSizes sizes{};
    std::vector<uint8_t> enc_in;
    std::vector<uint8_t> dec_in;

    ~SslConnectionState() { cleanup(); }

    void cleanup() {
        if (has_ctx) {
            DeleteSecurityContext(&ctx);
            has_ctx = false;
        }
        if (has_cred) {
            FreeCredentialsHandle(&cred);
            has_cred = false;
        }
    }

    SOCKET native_sock() const {
        const intptr_t s = net::get_host_socket(fd);
        return s < 0 ? INVALID_SOCKET : SOCKET(s);
    }

    std::string effective_sni() const {
        if (!host.empty()) return host;
        SOCKET s = native_sock();
        if (s == INVALID_SOCKET) return {};
        sockaddr_in peer{};
        int len = sizeof(peer);
        if (getpeername(s, reinterpret_cast<sockaddr*>(&peer), &len) == 0 && peer.sin_family == AF_INET)
            return net::lookup_sni_host(peer.sin_addr.s_addr);
        return {};
    }

    std::string target_alpn(const std::string& sni) const {
        if (sni.find("npln") != std::string::npos || sni.find("gs.nintendo.net") != std::string::npos)
            return "h2";
        return alpn_proto.empty() ? "http/1.1" : alpn_proto;
    }

    uint32_t do_handshake() {
        if (handshake_done) return 0;
        SOCKET s = native_sock();
        if (s == INVALID_SOCKET) return kSslInvalidState;

        if (!has_cred) {
            SCHANNEL_CRED sc{};
            sc.dwVersion = SCHANNEL_CRED_VERSION;
            sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT;
            sc.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_MANUAL_CRED_VALIDATION | SCH_USE_STRONG_CRYPTO;
            TimeStamp ts{};
            if (AcquireCredentialsHandleW(nullptr, const_cast<SEC_WCHAR*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND,
                                          nullptr, &sc, nullptr, nullptr, &cred, &ts) != SEC_E_OK) {
                return kSslIoError;
            }
            has_cred = true;
        }

        const std::string sni = effective_sni();
        std::wstring wsni(sni.begin(), sni.end());
        const DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                            ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM |
                            ISC_REQ_MANUAL_CRED_VALIDATION;

        for (;;) {
            if (!has_ctx) {
                std::vector<uint8_t> alpn_buf = build_alpn_buffer(target_alpn(sni));
                SecBuffer in_buf{};
                in_buf.BufferType = SECBUFFER_APPLICATION_PROTOCOLS;
                in_buf.cbBuffer = DWORD(alpn_buf.size());
                in_buf.pvBuffer = alpn_buf.data();
                SecBufferDesc in_desc{SECBUFFER_VERSION, 1, &in_buf};

                SecBuffer out_buf{0, SECBUFFER_TOKEN, nullptr};
                SecBufferDesc out_desc{SECBUFFER_VERSION, 1, &out_buf};
                DWORD out_flags = 0;
                TimeStamp ts{};
                SECURITY_STATUS st = InitializeSecurityContextW(
                    &cred, nullptr, wsni.empty() ? nullptr : const_cast<SEC_WCHAR*>(wsni.c_str()),
                    flags, 0, SECURITY_NATIVE_DREP, &in_desc, 0, &ctx, &out_desc, &out_flags, &ts);
                has_ctx = (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED);
                if (out_buf.cbBuffer && out_buf.pvBuffer) {
                    const char* p = static_cast<const char*>(out_buf.pvBuffer);
                    int rem = int(out_buf.cbBuffer);
                    while (rem > 0) {
                        int n = ::send(s, p, rem, 0);
                        if (n <= 0) {
                            FreeContextBuffer(out_buf.pvBuffer);
                            return kSslIoError;
                        }
                        p += n;
                        rem -= n;
                    }
                    FreeContextBuffer(out_buf.pvBuffer);
                }
                if (st == SEC_E_OK) {
                    handshake_done = true;
                    break;
                }
                if (st != SEC_I_CONTINUE_NEEDED) return kSslIoError;
            }

            if (enc_in.empty()) {
                uint8_t tmp[16384];
                int n = ::recv(s, reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
                if (n < 0) {
                    if (WSAGetLastError() == WSAEWOULDBLOCK) return kSslWouldBlock;
                    return kSslIoError;
                }
                if (n == 0) return kSslIoError;
                enc_in.insert(enc_in.end(), tmp, tmp + n);
            }

            SecBuffer in_bufs[2] = {
                {DWORD(enc_in.size()), SECBUFFER_TOKEN, enc_in.data()},
                {0, SECBUFFER_EMPTY, nullptr},
            };
            SecBufferDesc in_desc{SECBUFFER_VERSION, 2, in_bufs};
            SecBuffer out_buf{0, SECBUFFER_TOKEN, nullptr};
            SecBufferDesc out_desc{SECBUFFER_VERSION, 1, &out_buf};
            DWORD out_flags = 0;
            TimeStamp ts{};
            SECURITY_STATUS st = InitializeSecurityContextW(
                &cred, &ctx, wsni.empty() ? nullptr : const_cast<SEC_WCHAR*>(wsni.c_str()),
                flags, 0, SECURITY_NATIVE_DREP, &in_desc, 0, nullptr, &out_desc, &out_flags, &ts);

            if (out_buf.cbBuffer && out_buf.pvBuffer) {
                const char* p = static_cast<const char*>(out_buf.pvBuffer);
                int rem = int(out_buf.cbBuffer);
                while (rem > 0) {
                    int n = ::send(s, p, rem, 0);
                    if (n <= 0) {
                        FreeContextBuffer(out_buf.pvBuffer);
                        return kSslIoError;
                    }
                    p += n;
                    rem -= n;
                }
                FreeContextBuffer(out_buf.pvBuffer);
            }

            if (st == SEC_E_INCOMPLETE_MESSAGE) {
                uint8_t tmp[16384];
                int n = ::recv(s, reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
                if (n < 0) {
                    if (WSAGetLastError() == WSAEWOULDBLOCK) return kSslWouldBlock;
                    return kSslIoError;
                }
                if (n == 0) return kSslIoError;
                enc_in.insert(enc_in.end(), tmp, tmp + n);
                continue;
            }

            if (in_bufs[1].BufferType == SECBUFFER_EXTRA && in_bufs[1].cbBuffer > 0) {
                const size_t extra = in_bufs[1].cbBuffer;
                std::memmove(enc_in.data(), enc_in.data() + enc_in.size() - extra, extra);
                enc_in.resize(extra);
            } else {
                enc_in.clear();
            }

            if (st == SEC_E_OK) {
                handshake_done = true;
                break;
            }
            if (st != SEC_I_CONTINUE_NEEDED) return kSslIoError;
        }

        QueryContextAttributesW(&ctx, SECPKG_ATTR_STREAM_SIZES, &sizes);
        SecPkgContext_ApplicationProtocol alpn_res{};
        if (QueryContextAttributesW(&ctx, SECPKG_ATTR_APPLICATION_PROTOCOL, &alpn_res) == SEC_E_OK &&
            alpn_res.ProtoNegoStatus == SecApplicationProtocolNegotiationStatus_Success &&
            alpn_res.ProtocolIdSize > 0) {
            negotiated_alpn.assign(reinterpret_cast<const char*>(alpn_res.ProtocolId), alpn_res.ProtocolIdSize);
        } else {
            negotiated_alpn = target_alpn(sni);
        }
        log("ssl: TLS handshake OK sni=%s alpn=%s", sni.c_str(), negotiated_alpn.c_str());
        return 0;
    }

    int32_t read_tls(uint8_t* out, uint32_t cap, bool peek, uint32_t& out_res) {
        out_res = 0;
        if (!handshake_done) {
            out_res = kSslInvalidState;
            return -1;
        }
        SOCKET s = native_sock();
        if (s == INVALID_SOCKET) {
            out_res = kSslInvalidState;
            return -1;
        }
        while (dec_in.empty()) {
            if (enc_in.empty()) {
                uint8_t tmp[16384];
                int n = ::recv(s, reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
                if (n < 0) {
                    if (WSAGetLastError() == WSAEWOULDBLOCK) {
                        out_res = kSslWouldBlock;
                        return -1;
                    }
                    out_res = kSslIoError;
                    return -1;
                }
                if (n == 0) return 0;
                enc_in.insert(enc_in.end(), tmp, tmp + n);
            }
            SecBuffer bufs[4] = {
                {DWORD(enc_in.size()), SECBUFFER_DATA, enc_in.data()},
                {0, SECBUFFER_EMPTY, nullptr},
                {0, SECBUFFER_EMPTY, nullptr},
                {0, SECBUFFER_EMPTY, nullptr},
            };
            SecBufferDesc desc{SECBUFFER_VERSION, 4, bufs};
            SECURITY_STATUS st = DecryptMessage(&ctx, &desc, 0, nullptr);
            if (st == SEC_E_INCOMPLETE_MESSAGE) {
                uint8_t tmp[16384];
                int n = ::recv(s, reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
                if (n < 0) {
                    if (WSAGetLastError() == WSAEWOULDBLOCK) {
                        out_res = kSslWouldBlock;
                        return -1;
                    }
                    out_res = kSslIoError;
                    return -1;
                }
                if (n == 0) return 0;
                enc_in.insert(enc_in.end(), tmp, tmp + n);
                continue;
            }
            if (st == SEC_I_CONTEXT_EXPIRED) return 0;
            if (st != SEC_E_OK && st != SEC_I_RENEGOTIATE) {
                out_res = kSslIoError;
                return -1;
            }
            std::vector<uint8_t> extra;
            for (int i = 1; i < 4; ++i) {
                if (bufs[i].BufferType == SECBUFFER_DATA && bufs[i].cbBuffer > 0) {
                    const uint8_t* p = static_cast<const uint8_t*>(bufs[i].pvBuffer);
                    dec_in.insert(dec_in.end(), p, p + bufs[i].cbBuffer);
                } else if (bufs[i].BufferType == SECBUFFER_EXTRA && bufs[i].cbBuffer > 0) {
                    const uint8_t* p = static_cast<const uint8_t*>(bufs[i].pvBuffer);
                    extra.assign(p, p + bufs[i].cbBuffer);
                }
            }
            enc_in = std::move(extra);
        }
        const uint32_t take = std::min<uint32_t>(cap, uint32_t(dec_in.size()));
        if (take > 0 && out) {
            std::memcpy(out, dec_in.data(), take);
            if (!peek) dec_in.erase(dec_in.begin(), dec_in.begin() + take);
        }
        return int32_t(take);
    }

    int32_t write_tls(const uint8_t* data, uint32_t len, uint32_t& out_res) {
        out_res = 0;
        if (!handshake_done) {
            out_res = kSslInvalidState;
            return -1;
        }
        SOCKET s = native_sock();
        if (s == INVALID_SOCKET) {
            out_res = kSslInvalidState;
            return -1;
        }
        uint32_t total = 0;
        const uint32_t max_chunk = sizes.cbMaximumMessage ? sizes.cbMaximumMessage : 16384u;
        while (total < len) {
            const uint32_t chunk = std::min<uint32_t>(len - total, max_chunk);
            std::vector<uint8_t> packet(sizes.cbHeader + chunk + sizes.cbTrailer);
            std::memcpy(packet.data() + sizes.cbHeader, data + total, chunk);
            SecBuffer bufs[4] = {
                {sizes.cbHeader, SECBUFFER_STREAM_HEADER, packet.data()},
                {chunk, SECBUFFER_DATA, packet.data() + sizes.cbHeader},
                {sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, packet.data() + sizes.cbHeader + chunk},
                {0, SECBUFFER_EMPTY, nullptr},
            };
            SecBufferDesc desc{SECBUFFER_VERSION, 4, bufs};
            if (EncryptMessage(&ctx, 0, &desc, 0) != SEC_E_OK) {
                out_res = kSslIoError;
                return total ? int32_t(total) : -1;
            }
            const int pkt_len = int(bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer);
            const char* p = reinterpret_cast<const char*>(packet.data());
            int rem = pkt_len;
            while (rem > 0) {
                int n = ::send(s, p, rem, 0);
                if (n <= 0) {
                    out_res = (WSAGetLastError() == WSAEWOULDBLOCK) ? kSslWouldBlock : kSslIoError;
                    return total ? int32_t(total) : -1;
                }
                p += n;
                rem -= n;
            }
            total += chunk;
        }
        return int32_t(total);
    }

    int32_t pending_bytes() const { return int32_t(dec_in.size()); }

    uint32_t poll_tls(uint32_t want, uint32_t timeout_ms) const {
        uint32_t out_ev = 0;
        if ((want & 1u) && !dec_in.empty()) out_ev |= 1u;
        SOCKET sock = native_sock();
        if (sock != INVALID_SOCKET && out_ev == 0) {
            pollfd pfd{sock, short(((want & 1u) ? POLLIN : 0) | ((want & 2u) ? POLLOUT : 0)), 0};
            if (WSAPoll(&pfd, 1, int(timeout_ms)) > 0) {
                if (pfd.revents & POLLIN) out_ev |= 1u;
                if (pfd.revents & POLLOUT) out_ev |= 2u;
                if (pfd.revents & (POLLERR | POLLHUP)) out_ev |= 4u;
            }
        }
        return out_ev;
    }
#else
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;

    ~SslConnectionState() { cleanup(); }

    void cleanup() {
        const auto& ossl = openssl();
        if (ssl && ossl.SSL_free) {
            ossl.SSL_free(ssl);
            ssl = nullptr;
        }
        if (ctx && ossl.SSL_CTX_free) {
            ossl.SSL_CTX_free(ctx);
            ctx = nullptr;
        }
    }

    int native_sock() const {
        const intptr_t s = net::get_host_socket(fd);
        return s < 0 ? -1 : int(s);
    }

    std::string effective_sni() const {
        if (!host.empty()) return host;
        const int s = native_sock();
        if (s < 0) return {};
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        if (getpeername(s, reinterpret_cast<sockaddr*>(&peer), &len) == 0 && peer.sin_family == AF_INET)
            return net::lookup_sni_host(peer.sin_addr.s_addr);
        return {};
    }

    std::string target_alpn(const std::string& sni) const {
        if (sni.find("npln") != std::string::npos || sni.find("gs.nintendo.net") != std::string::npos)
            return "h2";
        return alpn_proto.empty() ? "http/1.1" : alpn_proto;
    }

    uint32_t do_handshake() {
        const auto& ossl = openssl();
        if (!ossl.ok) return kSslIoError;
        const int s = native_sock();
        if (s < 0) return kSslInvalidState;
        if (handshake_done) return 0;

        const std::string sni = effective_sni();
        const std::string want_alpn = target_alpn(sni);
        if (!ssl) {
            ctx = ossl.SSL_CTX_new(ossl.TLS_client_method());
            if (!ctx) return kSslIoError;
            // Match Windows SCH_CRED_MANUAL_CRED_VALIDATION: DNS redirect maps *.nintendo.net -> nex.nextendo.network.
            ossl.SSL_CTX_set_verify(ctx, 0 /* SSL_VERIFY_NONE */, nullptr);
            if (!want_alpn.empty() && want_alpn.size() < 255) {
                std::vector<unsigned char> wire;
                wire.push_back(static_cast<unsigned char>(want_alpn.size()));
                wire.insert(wire.end(), want_alpn.begin(), want_alpn.end());
                ossl.SSL_CTX_set_alpn_protos(ctx, wire.data(), static_cast<unsigned int>(wire.size()));
            }
            ssl = ossl.SSL_new(ctx);
            if (!ssl) return kSslIoError;
            ossl.SSL_set_fd(ssl, s);
            if (!sni.empty()) {
                ossl.SSL_ctrl(ssl, 55 /* SSL_CTRL_SET_TLSEXT_HOSTNAME */, 0 /* TLSEXT_NAMETYPE_host_name */,
                              const_cast<char*>(sni.c_str()));
            }
        }

        const int rc = ossl.SSL_connect(ssl);
        if (rc == 1) {
            handshake_done = true;
            const unsigned char* sel = nullptr;
            unsigned int sel_len = 0;
            ossl.SSL_get0_alpn_selected(ssl, &sel, &sel_len);
            if (sel && sel_len > 0) negotiated_alpn.assign(reinterpret_cast<const char*>(sel), sel_len);
            else negotiated_alpn = want_alpn;
            log("ssl: TLS handshake OK sni=%s alpn=%s", sni.c_str(), negotiated_alpn.c_str());
            return 0;
        }
        const int err = ossl.SSL_get_error(ssl, rc);
        if (err == 2 /* SSL_ERROR_WANT_READ */ || err == 3 /* SSL_ERROR_WANT_WRITE */) {
            return kSslWouldBlock;
        }
        log("ssl: SSL_connect failed sni=%s rc=%d err=%d", sni.c_str(), rc, err);
        return kSslIoError;
    }

    int32_t read_tls(uint8_t* out, uint32_t cap, bool peek, uint32_t& out_res) {
        out_res = 0;
        const auto& ossl = openssl();
        if (!ossl.ok || !handshake_done || !ssl) {
            out_res = kSslInvalidState;
            return -1;
        }
        const int n = peek ? ossl.SSL_peek(ssl, out, int(cap)) : ossl.SSL_read(ssl, out, int(cap));
        if (n > 0) return n;
        const int err = ossl.SSL_get_error(ssl, n);
        if (err == 6 /* SSL_ERROR_ZERO_RETURN */) return 0;
        if (err == 2 /* SSL_ERROR_WANT_READ */ || err == 3 /* SSL_ERROR_WANT_WRITE */) {
            out_res = kSslWouldBlock;
            return -1;
        }
        out_res = kSslIoError;
        return -1;
    }

    int32_t write_tls(const uint8_t* data, uint32_t len, uint32_t& out_res) {
        out_res = 0;
        const auto& ossl = openssl();
        if (!ossl.ok || !handshake_done || !ssl) {
            out_res = kSslInvalidState;
            return -1;
        }
        const int n = ossl.SSL_write(ssl, data, int(len));
        if (n > 0) return n;
        const int err = ossl.SSL_get_error(ssl, n);
        if (err == 2 /* SSL_ERROR_WANT_READ */ || err == 3 /* SSL_ERROR_WANT_WRITE */) {
            out_res = kSslWouldBlock;
            return -1;
        }
        out_res = kSslIoError;
        return -1;
    }

    int32_t pending_bytes() const {
        const auto& ossl = openssl();
        return (ossl.ok && ssl) ? ossl.SSL_pending(ssl) : 0;
    }

    uint32_t poll_tls(uint32_t want, uint32_t timeout_ms) const {
        uint32_t out_ev = 0;
        if ((want & 1u) && pending_bytes() > 0) out_ev |= 1u;
        const int sock = native_sock();
        if (sock >= 0 && out_ev == 0) {
            pollfd pfd{sock, short(((want & 1u) ? POLLIN : 0) | ((want & 2u) ? POLLOUT : 0)), 0};
            if (::poll(&pfd, 1, int(timeout_ms)) > 0) {
                if (pfd.revents & POLLIN) out_ev |= 1u;
                if (pfd.revents & POLLOUT) out_ev |= 2u;
                if (pfd.revents & (POLLERR | POLLHUP)) out_ev |= 4u;
            }
        }
        return out_ev;
    }
#endif
};

std::shared_ptr<Service> make_ssl_connection() {
    auto conn = std::make_shared<SslConnectionState>();
    auto s = std::make_shared<Service>("ISslConnection");
    s->cmds[0] = [conn](IpcCtx& c) {  // SetSocketDescriptor(s32 fd) -> s32 old_fd
        const int32_t prev = conn->fd;
        conn->fd = c.arg<int32_t>(0);
        c.push<int32_t>(prev);
        return 0u;
    };
    s->cmds[1] = [conn](IpcCtx& c) {  // SetHostName(buffer)
        const Buffer b = c.in_buf(0);
        if (b.addr && b.size) {
            const char* p = reinterpret_cast<const char*>(b.addr);
            conn->host.assign(p, strnlen(p, b.size));
        }
        return 0u;
    };
    s->cmds[2] = [conn](IpcCtx& c) { conn->verify_opt = c.arg<uint32_t>(0); return 0u; };
    s->cmds[3] = [conn](IpcCtx& c) { conn->io_mode = c.arg<uint32_t>(0); return 0u; };
    s->cmds[4] = [conn](IpcCtx& c) { c.push<int32_t>(conn->fd); return 0u; };
    s->cmds[5] = [conn](IpcCtx& c) {
        Buffer out = c.out_buf(0);
        const uint32_t n = out.addr ? uint32_t(std::min<size_t>(out.size, conn->host.size())) : 0;
        if (n) std::memcpy(reinterpret_cast<void*>(out.addr), conn->host.data(), n);
        c.push<uint32_t>(n);
        return 0u;
    };
    s->cmds[6] = [conn](IpcCtx& c) { c.push<uint32_t>(conn->verify_opt); return 0u; };
    s->cmds[7] = [conn](IpcCtx& c) { c.push<uint32_t>(conn->io_mode); return 0u; };
    s->cmds[8] = [conn](IpcCtx&) {  // DoHandshake
        if (!net::online_enabled()) return kSslIoError;
        return conn->do_handshake();
    };
    s->cmds[9] = [conn](IpcCtx& c) {  // DoHandshakeGetServerCert -> (u32 size, u32 count)
        if (!net::online_enabled()) return kSslIoError;
        const uint32_t res = conn->do_handshake();
        c.push<uint32_t>(0);
        c.push<uint32_t>(0);
        return res;
    };
    s->cmds[10] = [conn](IpcCtx& c) {  // Read(out buffer) -> s32 bytes
        Buffer out = c.out_buf(0);
        uint32_t res = 0;
        const int32_t n = conn->read_tls(reinterpret_cast<uint8_t*>(out.addr), uint32_t(out.size), false, res);
        c.push<int32_t>(n);
        return res;
    };
    s->cmds[11] = [conn](IpcCtx& c) {  // Write(in buffer) -> s32 bytes
        Buffer in = c.in_buf(0);
        uint32_t res = 0;
        const int32_t n = conn->write_tls(reinterpret_cast<const uint8_t*>(in.addr), uint32_t(in.size), res);
        c.push<int32_t>(n);
        return res;
    };
    s->cmds[12] = [conn](IpcCtx& c) {  // Pending -> s32
        c.push<int32_t>(conn->pending_bytes());
        return 0u;
    };
    s->cmds[13] = [conn](IpcCtx& c) {  // Peek(out buffer) -> s32
        Buffer out = c.out_buf(0);
        uint32_t res = 0;
        const int32_t n = conn->read_tls(reinterpret_cast<uint8_t*>(out.addr), uint32_t(out.size), true, res);
        c.push<int32_t>(n);
        return res;
    };
    s->cmds[14] = [conn](IpcCtx& c) {  // Poll(u32 events, u32 timeout_ms) -> u32 out_events
        const uint32_t want = c.arg<uint32_t>(0);
        const uint32_t timeout_ms = c.arg<uint32_t>(4);
        c.push<uint32_t>(conn->poll_tls(want, timeout_ms));
        return 0u;
    };
    s->cmds[15] = [conn](IpcCtx& c) { conn->session_cache_mode = c.arg<uint32_t>(0); return 0u; };
    s->cmds[16] = [conn](IpcCtx& c) { c.push<uint32_t>(conn->session_cache_mode); return 0u; };
    s->cmds[17] = [](IpcCtx&) { return 0u; };
    s->cmds[18] = [conn](IpcCtx& c) { conn->renegotiation_mode = c.arg<uint32_t>(0); return 0u; };
    s->cmds[25] = [conn](IpcCtx& c) {  // SetNextAlpnProto(wire buffer)
        const Buffer b = c.in_buf(0);
        if (b.addr && b.size >= 2) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(b.addr);
            const uint8_t plen = p[0];
            if (plen > 0 && size_t(1 + plen) <= b.size)
                conn->alpn_proto.assign(reinterpret_cast<const char*>(p + 1), plen);
        }
        return 0u;
    };
    s->cmds[26] = [conn](IpcCtx& c) {  // GetNextAlpnProto(out buffer) -> (u32 state, u32 len)
        const std::string& p = conn->negotiated_alpn.empty() ? conn->alpn_proto : conn->negotiated_alpn;
        Buffer out = c.out_buf(0);
        const uint32_t n = out.addr ? uint32_t(std::min<size_t>(out.size, p.size())) : 0;
        if (n) std::memcpy(reinterpret_cast<void*>(out.addr), p.data(), n);
        c.push<uint32_t>(conn->handshake_done ? 2u : 0u);  // 2 = Negotiated
        c.push<uint32_t>(n);
        return 0u;
    };
    return s;
}

std::shared_ptr<Service> make_ssl_context() {
    auto s = std::make_shared<Service>("ISslContext");
    s->cmds[0] = [](IpcCtx&) { return 0u; };  // SetOption
    s->cmds[1] = [](IpcCtx& c) { c.push<int32_t>(0); return 0u; };
    s->cmds[2] = [](IpcCtx& c) { c.push_object(make_ssl_connection()); return 0u; };  // CreateConnection
    s->cmds[3] = [](IpcCtx& c) { c.push<uint32_t>(0); return 0u; };
    s->cmds[4] = [](IpcCtx& c) { c.push<uint64_t>(1); return 0u; };  // ImportServerPki
    s->cmds[5] = [](IpcCtx& c) { c.push<uint64_t>(1); return 0u; };  // ImportClientPki
    s->cmds[10] = [](IpcCtx& c) { c.push<uint64_t>(1); return 0u; };  // ImportCrl
    return s;
}

const std::vector<std::vector<uint8_t>>& builtin_root_certs() {
    static const std::vector<std::vector<uint8_t>> certs = []() {
        std::vector<std::vector<uint8_t>> out;
#if defined(_WIN32)
        HCERTSTORE store = CertOpenSystemStoreA(0, "ROOT");
        if (store) {
            PCCERT_CONTEXT ctx = nullptr;
            while ((ctx = CertEnumCertificatesInStore(store, ctx)) != nullptr) {
                if (ctx->pbCertEncoded && ctx->cbCertEncoded > 0 && ctx->cbCertEncoded <= 4096) {
                    out.emplace_back(ctx->pbCertEncoded, ctx->pbCertEncoded + ctx->cbCertEncoded);
                    if (out.size() >= 32) break;
                }
            }
            if (ctx) CertFreeCertificateContext(ctx);
            CertCloseStore(store, 0);
        }
#else
        auto b64_decode = [](const std::string& b64) -> std::vector<uint8_t> {
            static const int8_t dec[256] = {
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
                52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
                -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
                15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
                -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
                41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            };
            std::vector<uint8_t> der;
            uint32_t val = 0;
            int bits = -8;
            for (unsigned char c : b64) {
                int8_t d = dec[c];
                if (d == -1) continue;
                val = (val << 6) | uint32_t(d);
                bits += 6;
                if (bits >= 0) {
                    der.push_back(uint8_t((val >> bits) & 0xffu));
                    bits -= 8;
                }
            }
            return der;
        };
        const char* env_cert = std::getenv("SSL_CERT_FILE");
        const char* candidates[] = {
            env_cert,
            "/etc/ssl/certs/ca-certificates.crt",
            "/etc/pki/tls/certs/ca-bundle.crt",
            "/etc/ssl/ca-bundle.pem",
            "/etc/ssl/cert.pem",
        };
        size_t total_bytes = 0x18;
        for (const char* path : candidates) {
            if (!path || !*path) continue;
            std::ifstream f(path);
            if (!f.is_open()) continue;
            std::string line, b64;
            bool in_cert = false;
            while (std::getline(f, line)) {
                if (line.find("-----BEGIN CERTIFICATE-----") != std::string::npos) {
                    in_cert = true;
                    b64.clear();
                } else if (line.find("-----END CERTIFICATE-----") != std::string::npos) {
                    if (in_cert) {
                        auto der = b64_decode(b64);
                        if (!der.empty() && der.size() <= 4096 && total_bytes + 0x18 + der.size() <= 48000) {
                            total_bytes += 0x18 + der.size();
                            out.push_back(std::move(der));
                            if (out.size() >= 32) break;
                        }
                    }
                    in_cert = false;
                } else if (in_cert) {
                    b64 += line;
                }
            }
            if (!out.empty()) break;
        }
#endif
        return out;
    }();
    return certs;
}

ServiceRegistrar r_ssl("ssl", [] {
    auto s = std::make_shared<Service>("ssl");
    s->cmds[0] = [](IpcCtx& c) {  // CreateContext
        log("ssl: CreateContext (online=%d)", int(net::online_enabled()));
        if (!net::online_enabled()) return kSslIoError;
        c.push_object(make_ssl_context());
        return 0u;
    };
    s->cmds[1] = [](IpcCtx& c) { log("ssl: GetContextCount"); c.push<uint32_t>(1); return 0u; };  // GetContextCount
    s->cmds[2] = [](IpcCtx& c) {  // GetCertificates(OutBuffer out, InBuffer ids) -> u32 count
        const auto& certs = builtin_root_certs();
        const Buffer in = c.in_buf(0);
        const Buffer out = c.out_buf(0);
        const size_t n_req = in.addr ? (in.size / 4) : 0;
        const uint32_t* req_ids = reinterpret_cast<const uint32_t*>(in.addr);
        const uint32_t count = uint32_t(certs.size());
        const uint64_t hdr_bytes = uint64_t(count) * 0x18ull;
        uint64_t data_off = hdr_bytes;
        uint64_t needed = hdr_bytes;
        for (const auto& der : certs) needed += der.size();
        if (!out.addr || out.size < needed) {
            log("ssl: GetCertificates -> buf too small (%zu < %llu)", out.size, (unsigned long long)needed);
            c.push<uint32_t>(0);
            return kSslInvalidState;
        }
        std::memset(reinterpret_cast<void*>(out.addr), 0, out.size);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t id = (req_ids && i < n_req && req_ids[i] != 0xFFFFFFFFu) ? req_ids[i] : (1000u + i);
            const uint64_t entry = out.addr + uint64_t(i) * 0x18ull;
            st<uint32_t>(entry + 0x00, id);
            st<uint32_t>(entry + 0x04, 1);  // CertificateStatus_Trusted
            st<uint64_t>(entry + 0x08, certs[i].size());
            st<uint64_t>(entry + 0x10, data_off);
            std::memcpy(reinterpret_cast<void*>(out.addr + data_off), certs[i].data(), certs[i].size());
            data_off += certs[i].size();
        }
        log("ssl: GetCertificates -> %u certs (%llu bytes)", count, (unsigned long long)data_off);
        c.push<uint32_t>(count);
        return 0u;
    };
    s->cmds[3] = [](IpcCtx& c) {  // GetCertificateBufSize(InBuffer ids) -> u32 size
        const auto& certs = builtin_root_certs();
        uint64_t total = uint64_t(certs.size()) * 0x18ull + 0x18ull;
        for (const auto& der : certs) total += der.size();
        log("ssl: GetCertificateBufSize -> %llu bytes (%zu certs)", (unsigned long long)total, certs.size());
        c.push<uint32_t>(uint32_t(total));
        return 0u;
    };
    s->cmds[5] = [](IpcCtx&) { log("ssl: SetInterfaceVersion"); return 0u; };  // SetInterfaceVersion
    return s;
});

}  // namespace

}  // namespace nx
