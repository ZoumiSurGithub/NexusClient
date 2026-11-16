#include <winsock2.h>
#include "DiscordRPC.h"
#include "Network.h"
#include "core/Config.h"
#include "Credits.h"   // version du client
#include "core/Logger.h"
#include "game/Game.h"
#include <windows.h>
#include <atomic>
#include <mutex>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>

namespace {
    HANDLE g_thread = nullptr;
    HANDLE g_stopEvent = nullptr;
    HANDLE g_pipe = INVALID_HANDLE_VALUE;

    std::mutex  g_mutex;
    std::string g_status = "Désactivé";
    std::string g_details, g_state, g_hover;

    void SetStatus(const std::string& s) { std::lock_guard lock(g_mutex); g_status = s; }

    // ---- Protocole IPC : [opcode u32][longueur u32][JSON UTF-8] ----------------------
    enum Op : uint32_t { Handshake = 0, Frame = 1, Close = 2 };

    bool WriteFrame(uint32_t op, const std::string& json) {
        if (g_pipe == INVALID_HANDLE_VALUE) return false;
        std::string buf(8 + json.size(), '\0');
        const uint32_t len = static_cast<uint32_t>(json.size());
        memcpy(buf.data(), &op, 4);
        memcpy(buf.data() + 4, &len, 4);
        memcpy(buf.data() + 8, json.data(), json.size());
        DWORD written = 0;
        return WriteFile(g_pipe, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr) && written == buf.size();
    }

    bool ReadFrame(uint32_t& op, std::string& json) {
        uint32_t header[2];
        DWORD read = 0;
        if (!ReadFile(g_pipe, header, 8, &read, nullptr) || read != 8) return false;
        op = header[0];
        json.assign(header[1], '\0');
        return header[1] == 0 || (ReadFile(g_pipe, json.data(), header[1], &read, nullptr) && read == header[1]);
    }

    // Lit les reponses en attente (sinon le tube finit par se remplir)
    bool Drain() {
        DWORD avail = 0;
        while (PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr) && avail >= 8) {
            uint32_t op; std::string json;
            if (!ReadFrame(op, json)) return false;
            if (op == Close) return false;
            if (json.find("\"evt\":\"ERROR\"") != std::string::npos) LOG_WARN("Discord : %s", json.c_str());
        }
        return true;
    }

    void Disconnect() {
        if (g_pipe != INVALID_HANDLE_VALUE) { CloseHandle(g_pipe); g_pipe = INVALID_HANDLE_VALUE; }
    }

    bool Connect(const char* clientId) {
        for (int i = 0; i < 10 && g_pipe == INVALID_HANDLE_VALUE; ++i) {
            wchar_t name[64];
            swprintf_s(name, L"\\\\.\\pipe\\discord-ipc-%d", i);
            g_pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        }
        if (g_pipe == INVALID_HANDLE_VALUE) { SetStatus("Discord n'est pas ouvert"); return false; }

        char hs[96];
        snprintf(hs, sizeof(hs), "{\"v\":1,\"client_id\":\"%s\"}", clientId);
        uint32_t op = 0; std::string reply;
        if (!WriteFrame(Handshake, hs) || !ReadFrame(op, reply) || op != Frame ||
            reply.find("\"READY\"") == std::string::npos) {
            Disconnect();
            SetStatus("Connexion refusée par Discord, nouvel essai dans 15 s");
            LOG_WARN("Discord : handshake refuse (%s)", reply.c_str());
            return false;
        }
        SetStatus("Connecté à Discord");
        LOG_INFO("Discord : connecte");
        return true;
    }

    std::string Escape(const std::string& s) {
        std::string out;
        for (unsigned char c : s) {
            if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
            else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
            else out += static_cast<char>(c);
        }
        return out;
    }

    // ---- Construction de la presence ---------------------------------------------
    struct Activity {
        std::string details, state, largeText, partyId, image;
        long long start = 0;
        int partySize = -1, partyMax = -1;
        bool operator==(const Activity& o) const {
            return details == o.details && state == o.state && largeText == o.largeText && image == o.image &&
                   start == o.start && partySize == o.partySize && partyMax == o.partyMax;
        }
    };

    // Coupe une chaine UTF-8 sans casser un caractere accentue (Discord refuse l'UTF-8 invalide)
    void Truncate(std::string& s, size_t max) {
        if (s.size() <= max) return;
        size_t n = max;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
        s.resize(n);
    }

    std::string g_lastMode;
    long long   g_modeStart = 0;

    Activity BuildActivity() {
        const Settings& s = Config::GetSettings();
        const Network::ServerInfo srv = Network::GetServer();
        Activity a;
        const std::string ver = std::string("Nexus v") + Credits::kVersion;
        a.largeText = ver + " · Minecraft Bedrock";
        a.image = kDiscordImage;   // logo du client (Art Asset "discord_nexus")

        std::string mode;
        if (srv.active) {
            mode = "server:" + srv.ip + ":" + std::to_string(srv.port);
            // Ligne 1 : "Sur un serveur" ; ligne 2 : adresse (ou "Multijoueur" si masquee)
            a.details = "Sur un serveur";
            a.state = s.discordShowIp ? srv.host + ":" + std::to_string(srv.port) : std::string("Multijoueur");
            Truncate(a.state, 120);

            // Survol de l'image : joueurs connectes et ping, ex. "12/100 connectés · 45 ms"
            std::string info;
            if (s.discordShowPlayers && srv.online >= 0) {
                info = std::to_string(srv.online);
                if (srv.maxPlayers > 0) info += "/" + std::to_string(srv.maxPlayers);
                info += srv.online > 1 ? " connectés" : " connecté";
            }
            if (s.discordShowPing && srv.pingMs >= 0) {
                if (!info.empty()) info += " · ";
                info += std::to_string(srv.pingMs) + " ms";
            }
            if (s.discordShowMotd && !srv.motd.empty())
                info = info.empty() ? srv.motd : info + " — " + srv.motd;   // les infos d'abord, le MOTD ensuite
            a.largeText = info.empty() ? ver : ver + " · " + info;
            Truncate(a.largeText, 120);
        } else if (Network::IsHostingWorld() || Game::LocalPlayer()) {
            mode = "solo";
            a.details = "En solo";
            a.state = std::string("Dans un monde · ") + ver;
        } else {
            mode = "menu";
            a.details = "Dans les menus";
            a.state = ver;
        }

        // Le chrono repart a chaque changement (menu -> solo -> serveur...)
        if (mode != g_lastMode) { g_lastMode = mode; g_modeStart = static_cast<long long>(_time64(nullptr)); }
        a.start = g_modeStart;
        return a;
    }

    std::string ActivityJson(const Activity& a) {
        std::string j = "{";
        j += "\"details\":\"" + Escape(a.details) + "\"";
        if (!a.state.empty()) j += ",\"state\":\"" + Escape(a.state) + "\"";
        j += ",\"timestamps\":{\"start\":" + std::to_string(a.start) + "}";
        j += ",\"assets\":{\"large_image\":\"" + Escape(a.image) + "\",\"large_text\":\"" + Escape(a.largeText) + "\"}";
        if (a.partySize > 0)
            j += ",\"party\":{\"id\":\"" + Escape(a.partyId) + "\",\"size\":[" + std::to_string(a.partySize) + "," +
                 std::to_string(a.partyMax) + "]}";
        j += ",\"instance\":false}";
        return j;
    }

    bool SendActivity(const std::string& activityJson) {
        static unsigned nonce = 0;
        char head[160];
        snprintf(head, sizeof(head), "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"nexus-%u\",\"args\":{\"pid\":%lu,\"activity\":",
                 ++nonce, GetCurrentProcessId());
        return WriteFrame(Frame, std::string(head) + activityJson + "}}") && Drain();
    }

    DWORD WINAPI Worker(LPVOID) {
        Activity last;
        bool sentOnce = false;
        ULONGLONG lastSend = 0, lastConnectTry = 0;

        while (WaitForSingleObject(g_stopEvent, 1000) == WAIT_TIMEOUT) {
            Network::Tick();
            const Settings& s = Config::GetSettings();
            const Activity a = BuildActivity();
            {
                std::lock_guard lock(g_mutex);
                g_details = a.details;
                g_state = a.state;
                g_hover = a.largeText;
            }

            if (!s.discordEnabled) {
                if (g_pipe != INVALID_HANDLE_VALUE) { SendActivity("null"); Disconnect(); }
                SetStatus("Désactivé");
                sentOnce = false;
                continue;
            }

            const ULONGLONG now = GetTickCount64();
            if (g_pipe == INVALID_HANDLE_VALUE) {
                if (now - lastConnectTry < 15000 && lastConnectTry) continue;
                lastConnectTry = now;
                if (!Connect(kDiscordClientId)) continue;
                sentOnce = false;
            }

            // Discord limite a ~5 mises a jour / 20 s : on n'envoie que si ca change (et 1 fois / 5 s max)
            if ((!sentOnce || !(a == last)) && now - lastSend >= 5000) {
                if (!SendActivity(ActivityJson(a))) {
                    Disconnect();
                    SetStatus("Connexion à Discord perdue, nouvel essai...");
                    continue;
                }
                last = a;
                sentOnce = true;
                lastSend = now;
            } else if (!Drain()) {
                Disconnect();
            }
        }

        if (g_pipe != INVALID_HANDLE_VALUE) { SendActivity("null"); Disconnect(); }
        return 0;
    }
}

void DiscordRPC::Start() {
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
}

void DiscordRPC::Stop() {
    if (!g_thread) return;
    SetEvent(g_stopEvent);
    WaitForSingleObject(g_thread, 5000);
    CloseHandle(g_thread);
    CloseHandle(g_stopEvent);
    g_thread = nullptr;
}

std::string DiscordRPC::Status() {
    std::lock_guard lock(g_mutex);
    return g_status;
}

void DiscordRPC::Preview(std::string& details, std::string& state, std::string& hover) {
    std::lock_guard lock(g_mutex);
    details = g_details;
    state = g_state;
    hover = g_hover;
}
