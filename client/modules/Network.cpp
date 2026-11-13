#include <winsock2.h>
#include <ws2tcpip.h>
#include "Network.h"
#include "core/Logger.h"
#include <windows.h>
#include <MinHook.h>
#include <mutex>
#include <map>
#include <set>
#include <vector>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <iterator>

namespace {
    // ---- Types des fonctions hookees ------------------------------------------------
    using sendto_t       = int (WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
    using WSASendTo_t    = int (WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int,
                                         LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
    using connect_t      = int (WSAAPI*)(SOCKET, const sockaddr*, int);
    using send_t         = int (WSAAPI*)(SOCKET, const char*, int, int);
    using WSASend_t      = int (WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, LPWSAOVERLAPPED,
                                         LPWSAOVERLAPPED_COMPLETION_ROUTINE);
    using bind_t         = int (WSAAPI*)(SOCKET, const sockaddr*, int);
    using closesocket_t  = int (WSAAPI*)(SOCKET);
    using getaddrinfo_t  = int (WSAAPI*)(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA*);
    using GetAddrInfoW_t = int (WSAAPI*)(PCWSTR, PCWSTR, const ADDRINFOW*, PADDRINFOW*);

    sendto_t o_sendto = nullptr;  WSASendTo_t o_WSASendTo = nullptr;
    connect_t o_connect = nullptr; send_t o_send = nullptr; WSASend_t o_WSASend = nullptr;
    bind_t o_bind = nullptr; closesocket_t o_closesocket = nullptr;
    getaddrinfo_t o_getaddrinfo = nullptr; GetAddrInfoW_t o_GetAddrInfoW = nullptr;

    // ---- Etat ---------------------------------------------------------------------
    struct Dest {
        sockaddr_storage addr{};
        int addrLen = 0;
        ULONGLONG lastSeen = 0;
        ULONGLONG windowStart = 0;
        unsigned windowCount = 0, lastWindowCount = 0;
    };

    std::mutex g_mutex;
    std::map<std::string, Dest> g_dests;            // "ip:port" -> trafic
    std::map<std::string, std::string> g_dns;       // ip -> nom tape
    std::map<SOCKET, sockaddr_storage> g_connected; // sockets UDP "connectes"
    std::set<SOCKET> g_hostSockets;                 // sockets du monde local (19132/19133)
    SOCKET g_pingSocket = INVALID_SOCKET;

    Network::ServerInfo g_server;                   // serveur actuel
    ULONGLONG g_lastPing = 0;

    // ---- Utilitaires -------------------------------------------------------------
    bool FormatAddr(const sockaddr* sa, std::string& ip, uint16_t& port) {
        char buf[INET6_ADDRSTRLEN] = "";
        if (sa->sa_family == AF_INET) {
            auto* in = reinterpret_cast<const sockaddr_in*>(sa);
            inet_ntop(AF_INET, &in->sin_addr, buf, sizeof(buf));
            port = ntohs(in->sin_port);
        } else if (sa->sa_family == AF_INET6) {
            auto* in = reinterpret_cast<const sockaddr_in6*>(sa);
            if (IN6_IS_ADDR_V4MAPPED(&in->sin6_addr)) inet_ntop(AF_INET, &in->sin6_addr.u.Byte[12], buf, sizeof(buf));
            else inet_ntop(AF_INET6, &in->sin6_addr, buf, sizeof(buf));
            port = ntohs(in->sin6_port);
        } else return false;
        ip = buf;
        return !ip.empty();
    }

    // Destinations qui ne sont pas un serveur de jeu
    bool Ignored(const sockaddr* sa, const std::string& ip, uint16_t port) {
        static const uint16_t ports[] = { 53, 67, 68, 123, 1900, 3478, 3479, 3480, 3481, 5353, 19302 };
        for (uint16_t p : ports) if (port == p) return true;
        if (sa->sa_family == AF_INET) {
            const auto a = ntohl(reinterpret_cast<const sockaddr_in*>(sa)->sin_addr.s_addr);
            if ((a >> 24) == 127 || a == 0 || a == 0xFFFFFFFF || (a >> 28) == 0xE) return true; // loopback, any, broadcast, multicast
            if ((a & 0xFF) == 0xFF) return true;                                               // broadcast de sous-reseau (LAN)
        } else if (ip == "::1" || ip.rfind("ff", 0) == 0) return true;
        return false;
    }

    void Record(SOCKET s, const sockaddr* to, int len) {
        if (!to || len <= 0 || s == g_pingSocket) return;
        std::string ip; uint16_t port = 0;
        if (!FormatAddr(to, ip, port) || Ignored(to, ip, port)) return;

        char key[80];
        snprintf(key, sizeof(key), "%s:%u", ip.c_str(), port);
        const ULONGLONG now = GetTickCount64();
        std::lock_guard lock(g_mutex);
        Dest& d = g_dests[key];
        if (!d.addrLen) { memcpy(&d.addr, to, std::min<size_t>(len, sizeof(d.addr))); d.addrLen = len; }
        d.lastSeen = now;
        if (now - d.windowStart > 5000) { d.lastWindowCount = d.windowCount; d.windowCount = 0; d.windowStart = now; }
        ++d.windowCount;
    }

    bool IsUdp(SOCKET s) {
        int type = 0, len = sizeof(type);
        return getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &len) == 0 && type == SOCK_DGRAM;
    }

    void RememberDns(const std::string& name, const sockaddr* sa) {
        std::string ip; uint16_t port;
        if (name.empty() || !FormatAddr(sa, ip, port)) return;
        std::lock_guard lock(g_mutex);
        g_dns[ip] = name;
    }

    // ---- Hooks ----------------------------------------------------------------------
    int WSAAPI Hk_sendto(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen) {
        Record(s, to, tolen);
        return o_sendto(s, buf, len, flags, to, tolen);
    }
    int WSAAPI Hk_WSASendTo(SOCKET s, LPWSABUF b, DWORD n, LPDWORD sent, DWORD f, const sockaddr* to, int tolen,
                            LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
        Record(s, to, tolen);
        return o_WSASendTo(s, b, n, sent, f, to, tolen, ov, cr);
    }
    int WSAAPI Hk_connect(SOCKET s, const sockaddr* name, int len) {
        if (name && len > 0 && IsUdp(s)) {
            sockaddr_storage st{};
            memcpy(&st, name, std::min<size_t>(len, sizeof(st)));
            std::lock_guard lock(g_mutex);
            g_connected[s] = st;
        }
        return o_connect(s, name, len);
    }
    void RecordConnected(SOCKET s) {
        sockaddr_storage st{};
        {
            std::lock_guard lock(g_mutex);
            auto it = g_connected.find(s);
            if (it == g_connected.end()) return;
            st = it->second;
        }
        Record(s, reinterpret_cast<sockaddr*>(&st), sizeof(st));
    }
    int WSAAPI Hk_send(SOCKET s, const char* buf, int len, int flags) {
        RecordConnected(s);
        return o_send(s, buf, len, flags);
    }
    int WSAAPI Hk_WSASend(SOCKET s, LPWSABUF b, DWORD n, LPDWORD sent, DWORD f, LPWSAOVERLAPPED ov,
                          LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
        RecordConnected(s);
        return o_WSASend(s, b, n, sent, f, ov, cr);
    }
    int WSAAPI Hk_bind(SOCKET s, const sockaddr* name, int len) {
        const int r = o_bind(s, name, len);
        std::string ip; uint16_t port = 0;
        if (r == 0 && name && FormatAddr(name, ip, port) && (port == 19132 || port == 19133) && IsUdp(s)) {
            std::lock_guard lock(g_mutex);
            g_hostSockets.insert(s);
        }
        return r;
    }
    int WSAAPI Hk_closesocket(SOCKET s) {
        {
            std::lock_guard lock(g_mutex);
            g_hostSockets.erase(s);
            g_connected.erase(s);
        }
        return o_closesocket(s);
    }
    int WSAAPI Hk_getaddrinfo(PCSTR node, PCSTR service, const ADDRINFOA* hints, PADDRINFOA* res) {
        const int r = o_getaddrinfo(node, service, hints, res);
        if (r == 0 && node && res && *res)
            for (auto* p = *res; p; p = p->ai_next) if (p->ai_addr) RememberDns(node, p->ai_addr);
        return r;
    }
    int WSAAPI Hk_GetAddrInfoW(PCWSTR node, PCWSTR service, const ADDRINFOW* hints, PADDRINFOW* res) {
        const int r = o_GetAddrInfoW(node, service, hints, res);
        if (r == 0 && node && res && *res) {
            char name[256];
            WideCharToMultiByte(CP_UTF8, 0, node, -1, name, sizeof(name), nullptr, nullptr);
            for (auto* p = *res; p; p = p->ai_next) if (p->ai_addr) RememberDns(name, p->ai_addr);
        }
        return r;
    }

    template <class T>
    void HookWs(const char* name, T detour, T* original) {
        void* target = nullptr;
        if (MH_CreateHookApiEx(L"ws2_32", name, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original),
                               &target) == MH_OK && MH_EnableHook(target) == MH_OK)
            LOG_INFO("Hook reseau %s installe", name);
        else
            LOG_WARN("Hook reseau %s impossible", name);
    }

    // ---- Ping RakNet ----------------------------------------------------------------
    std::string StripColors(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if ((unsigned char)s[i] == 0xC2 && i + 1 < s.size() && (unsigned char)s[i + 1] == 0xA7) { i += 2; continue; } // "§x"
            out += s[i];
        }
        return out;
    }

    void PingServer(const sockaddr_storage& addrIn, int addrLen) {
        // Le jeu utilise souvent un socket IPv6 "double pile" : l'adresse du serveur IPv4 est alors
        // au format ::ffff:a.b.c.d. Notre socket de ping ne l'accepte pas : on la reconvertit en IPv4.
        sockaddr_storage addr = addrIn;
        if (addr.ss_family == AF_INET6) {
            auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addrIn);
            if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
                sockaddr_in in4{};
                in4.sin_family = AF_INET;
                in4.sin_port = in6->sin6_port;
                memcpy(&in4.sin_addr, &in6->sin6_addr.u.Byte[12], 4);
                memset(&addr, 0, sizeof(addr));
                memcpy(&addr, &in4, sizeof(in4));
                addrLen = sizeof(in4);
            }
        }
        // Socket recree si la famille d'adresse change (IPv4 <-> IPv6)
        static int pingFamily = AF_UNSPEC;
        if (g_pingSocket != INVALID_SOCKET && pingFamily != addr.ss_family) {
            closesocket(g_pingSocket);
            g_pingSocket = INVALID_SOCKET;
        }
        pingFamily = addr.ss_family;

        static const unsigned char magic[16] = { 0x00, 0xff, 0xff, 0x00, 0xfe, 0xfe, 0xfe, 0xfe,
                                                 0xfd, 0xfd, 0xfd, 0xfd, 0x12, 0x34, 0x56, 0x78 };
        if (g_pingSocket == INVALID_SOCKET) {
            g_pingSocket = socket(addr.ss_family, SOCK_DGRAM, IPPROTO_UDP);
            if (g_pingSocket == INVALID_SOCKET) { LOG_WARN("Ping serveur : socket impossible (%d)", WSAGetLastError()); return; }
            DWORD timeout = 2000;
            setsockopt(g_pingSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
        }

        unsigned char pkt[33] = { 0x01 };
        const unsigned long long t = GetTickCount64();
        for (int i = 0; i < 8; ++i) pkt[1 + i] = static_cast<unsigned char>(t >> (56 - 8 * i));
        memcpy(pkt + 9, magic, 16);
        for (int i = 0; i < 8; ++i) pkt[25 + i] = static_cast<unsigned char>(0x4E + i); // GUID client arbitraire

        LARGE_INTEGER freq, t0, t1;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        auto sendFn = o_sendto ? o_sendto : &sendto;    // pas de passage par notre hook
        if (sendFn(g_pingSocket, reinterpret_cast<char*>(pkt), sizeof(pkt), 0,
                   reinterpret_cast<const sockaddr*>(&addr), addrLen) == SOCKET_ERROR) {
            LOG_WARN("Ping serveur : envoi impossible (erreur %d)", WSAGetLastError());
            closesocket(g_pingSocket);
            g_pingSocket = INVALID_SOCKET;
            return;
        }

        char buf[1500];
        const int n = recvfrom(g_pingSocket, buf, sizeof(buf), 0, nullptr, nullptr);
        QueryPerformanceCounter(&t1);
        if (n < 35 || (unsigned char)buf[0] != 0x1C) {           // pas de reponse / pas un Pong
            { std::lock_guard lock(g_mutex); g_server.pingMs = -1; }
            LOG_WARN("Ping serveur : pas de reponse (le serveur ne communique pas son nombre de joueurs)");
            return;
        }
        const int len = ((unsigned char)buf[33] << 8) | (unsigned char)buf[34];
        if (35 + len > n) return;

        // "MCPE;MOTD;protocole;version;connectes;max;id;sous-motd;mode;..."
        std::vector<std::string> f;
        std::string cur;
        for (int i = 0; i < len; ++i) {
            const char c = buf[35 + i];
            if (c == ';') { f.push_back(cur); cur.clear(); } else cur += c;
        }
        f.push_back(cur);
        if (f.size() < 6) return;

        std::lock_guard lock(g_mutex);
        g_server.pingMs = static_cast<int>((t1.QuadPart - t0.QuadPart) * 1000 / freq.QuadPart);
        g_server.motd = StripColors(f[1]);
        try { g_server.online = std::stoi(f[4]); g_server.maxPlayers = std::stoi(f[5]); }
        catch (...) {}
        LOG_INFO("Ping serveur : %d/%d joueurs connectes, %d ms", g_server.online, g_server.maxPlayers, g_server.pingMs);
    }
}

void Network::Init() {
    HookWs("sendto", &Hk_sendto, &o_sendto);
    HookWs("WSASendTo", &Hk_WSASendTo, &o_WSASendTo);
    HookWs("connect", &Hk_connect, &o_connect);
    HookWs("send", &Hk_send, &o_send);
    HookWs("WSASend", &Hk_WSASend, &o_WSASend);
    HookWs("bind", &Hk_bind, &o_bind);
    HookWs("closesocket", &Hk_closesocket, &o_closesocket);
    HookWs("getaddrinfo", &Hk_getaddrinfo, &o_getaddrinfo);
    HookWs("GetAddrInfoW", &Hk_GetAddrInfoW, &o_GetAddrInfoW);
}

void Network::Tick() {
    const ULONGLONG now = GetTickCount64();
    sockaddr_storage pingAddr{};
    int pingLen = 0;
    {
        std::lock_guard lock(g_mutex);

        // Serveur = destination UDP la plus active, vue dans les 4 dernieres secondes
        const Dest* best = nullptr;
        std::string bestKey;
        unsigned bestRate = 0;
        for (auto& [key, d] : g_dests) {
            if (now - d.lastSeen > 4000) continue;
            const unsigned rate = std::max(d.windowCount, d.lastWindowCount);
            if (rate >= 6 && rate > bestRate) { best = &d; bestKey = key; bestRate = rate; }
        }
        // Menage des destinations anciennes
        for (auto it = g_dests.begin(); it != g_dests.end();)
            it = now - it->second.lastSeen > 60000 ? g_dests.erase(it) : std::next(it);

        if (!best) {
            if (g_server.active) LOG_INFO("Serveur quitte");
            g_server = {};
            return;
        }

        std::string ip; uint16_t port = 0;
        FormatAddr(reinterpret_cast<const sockaddr*>(&best->addr), ip, port);
        if (!g_server.active || g_server.ip != ip || g_server.port != port) {
            g_server = {};
            g_server.active = true;
            g_server.ip = ip;
            g_server.port = port;
            g_server.since = now;
            g_lastPing = 0;
            LOG_INFO("Serveur detecte : %s:%u", ip.c_str(), port);
        }
        auto dns = g_dns.find(ip);
        g_server.host = dns != g_dns.end() ? dns->second : ip;

        if (now - g_lastPing > 10000 && now - g_server.since > 3000) {   // 1er ping ~3 s apres l'arrivee, puis toutes les 10 s
            g_lastPing = now;
            pingAddr = best->addr;
            pingLen = best->addrLen;
        }
    }
    if (pingLen) PingServer(pingAddr, pingLen);   // hors verrou : peut attendre 1,5 s
}

Network::ServerInfo Network::GetServer() {
    std::lock_guard lock(g_mutex);
    return g_server;
}

bool Network::IsHostingWorld() {
    std::lock_guard lock(g_mutex);
    return !g_hostSockets.empty();
}

std::string Network::DebugText() {
    std::lock_guard lock(g_mutex);
    char buf[256];
    if (g_server.active)
        snprintf(buf, sizeof(buf), "Serveur : %s (%s:%u), joueurs %d/%d", g_server.host.c_str(), g_server.ip.c_str(),
                 g_server.port, g_server.online, g_server.maxPlayers);
    else
        snprintf(buf, sizeof(buf), "Pas de serveur detecte%s", g_hostSockets.empty() ? "" : " (monde local ouvert)");
    return buf;
}
