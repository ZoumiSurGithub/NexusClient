#include "Updater.h"
#include "version.h"
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <cstdio>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>

namespace fs = std::filesystem;

namespace {
    void Say(const wchar_t* fmt, ...) {
        va_list a; va_start(a, fmt);
        wprintf(L"[*] "); vwprintf(fmt, a); wprintf(L"\n");
        va_end(a);
    }

    std::wstring Widen(const std::string& s) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n > 0 ? n - 1 : 0, L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    }

    // ---- HTTP GET (suit les redirections GitHub -> serveur de fichiers) -------------
    bool HttpGet(const std::wstring& url, std::string& out, DWORD& status) {
        out.clear();
        status = 0;
        wchar_t host[256], path[2048];
        URL_COMPONENTS uc{ sizeof(uc) };
        uc.lpszHostName = host; uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path;  uc.dwUrlPathLength = 2048;
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;

        const std::wstring agent = std::wstring(L"NexusLauncher/") + NEXUS_VERSION_W;
        HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return false;
        WinHttpSetTimeouts(session, 10000, 10000, 15000, 60000);

        bool ok = false;
        HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
        HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                    : nullptr;
        if (request && WinHttpSendRequest(request, L"Cache-Control: no-cache\r\n", (DWORD)-1L,
                                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD len = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
            ok = true;
            for (DWORD avail = 0; WinHttpQueryDataAvailable(request, &avail) && avail > 0;) {
                std::string chunk(avail, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), avail, &read)) { ok = false; break; }
                out.append(chunk.data(), read);
                if (out.size() > 200u * 1024 * 1024) { ok = false; break; }   // garde-fou : 200 Mo
            }
        }
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return ok;
    }

    std::string Sha256Hex(const std::string& data) {
        BCRYPT_ALG_HANDLE alg = nullptr;
        unsigned char hash[32] = {};
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
        const NTSTATUS st = BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                                       static_cast<ULONG>(data.size()), hash, sizeof(hash));
        BCryptCloseAlgorithmProvider(alg, 0);
        if (st != 0) return {};
        static const char* hex = "0123456789abcdef";
        std::string out;
        for (unsigned char b : hash) { out += hex[b >> 4]; out += hex[b & 15]; }
        return out;
    }

    // "1.10.2" -> {1, 10, 2} ; le suffixe ("-dev", "-beta") est ignore
    std::vector<int> ParseVersion(const std::string& v) {
        std::vector<int> parts;
        std::stringstream ss(v.substr(0, v.find('-')));
        for (std::string p; std::getline(ss, p, '.');) {
            try { parts.push_back(std::stoi(p)); } catch (...) { parts.push_back(0); }
        }
        while (parts.size() < 3) parts.push_back(0);
        return parts;
    }
    bool IsNewer(const std::string& remote, const std::string& local) {
        return ParseVersion(remote) > ParseVersion(local);
    }

    struct Manifest {
        std::string version;
        std::vector<std::string> notes;
        struct File { std::string name, sha256; };
        std::vector<File> files;
    };

    // Seuls des noms de fichiers simples sont acceptes (pas de chemin, pas de "..")
    bool SafeName(const std::string& n) {
        if (n.empty() || n.size() > 100 || n.find("..") != std::string::npos) return false;
        for (char c : n) if (c == '/' || c == '\\' || c == ':') return false;
        const std::string lower = [&] { std::string l = n; for (auto& c : l) c = (char)tolower((unsigned char)c); return l; }();
        return lower != "client.ini" && lower != "commands.txt" && lower != "nexus.log";   // fichiers du joueur
    }

    bool ParseManifest(std::string text, Manifest& m) {
        if (text.size() >= 3 && (unsigned char)text[0] == 0xEF) text.erase(0, 3);   // BOM
        std::stringstream ss(text);
        for (std::string line; std::getline(ss, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
            if (key == "version") m.version = val;
            else if (key == "note") m.notes.push_back(val);
            else if (key == "file") {
                const size_t bar = val.find('|');
                if (bar == std::string::npos) return false;
                Manifest::File f{ val.substr(0, bar), val.substr(bar + 1) };
                for (auto& c : f.sha256) c = (char)tolower((unsigned char)c);
                if (!SafeName(f.name) || f.sha256.size() != 64) return false;
                m.files.push_back(f);
            }
        }
        return !m.version.empty() && !m.files.empty();
    }

    void ShowManualHelp() {
        wprintf(L"\n    Telecharge la derniere version a la main :\n    https://github.com/%s/releases/latest\n\n",
                NEXUS_UPDATE_REPO);
    }
}

void Updater::CleanupOldFiles(const fs::path& dir) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == L".old") fs::remove(e.path(), ec);   // echoue si encore utilise : pas grave
    fs::remove_all(dir / L"update", ec);
}

Updater::Result Updater::Run(const fs::path& dir, const fs::path& selfExe) {
    const std::string local = NEXUS_VERSION;
    if (local.find("-dev") != std::string::npos) {
        Say(L"Version de developpement (%s) : pas de verification des mises a jour.", NEXUS_VERSION_W);
        return Result::DevBuild;
    }

    Say(L"Recherche de mises a jour...");
    const std::wstring base = std::wstring(L"https://github.com/") + NEXUS_UPDATE_REPO;
    std::string text;
    DWORD status = 0;
    if (!HttpGet(base + L"/releases/latest/download/nexus-update.txt", text, status) || status != 200) {
        Say(L"Impossible de verifier les mises a jour (connexion ou GitHub indisponible, code %lu).", status);
        return Result::Offline;
    }

    Manifest m;
    if (!ParseManifest(text, m)) {
        Say(L"Manifeste de mise a jour invalide : verification ignoree.");
        return Result::Offline;
    }
    if (!IsNewer(m.version, local)) {
        Say(L"Nexus est a jour (version %s).", NEXUS_VERSION_W);
        return Result::UpToDate;
    }

    // ---- Mise a jour obligatoire ----------------------------------------------------
    wprintf(L"\n  ==============================================\n");
    wprintf(L"   Mise a jour obligatoire : %s  ->  %s\n", NEXUS_VERSION_W, Widen(m.version).c_str());
    wprintf(L"  ==============================================\n");
    for (const auto& n : m.notes) wprintf(L"   %s\n", Widen(n).c_str());
    wprintf(L"\n");

    std::error_code ec;
    const fs::path staging = dir / L"update";
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);

    // 1. Telechargement + verification de chaque fichier
    const std::wstring tagBase = base + L"/releases/download/v" + Widen(m.version) + L"/";
    for (const auto& f : m.files) {
        Say(L"Telechargement de %s...", Widen(f.name).c_str());
        std::string data;
        if (!HttpGet(tagBase + Widen(f.name), data, status) || status != 200) {
            Say(L"Echec du telechargement de %s (code %lu).", Widen(f.name).c_str(), status);
            ShowManualHelp();
            return Result::Failed;
        }
        if (Sha256Hex(data) != f.sha256) {
            Say(L"%s est corrompu (empreinte SHA-256 differente) : mise a jour annulee.", Widen(f.name).c_str());
            ShowManualHelp();
            return Result::Failed;
        }
        std::ofstream out(staging / Widen(f.name), std::ios::binary);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) { Say(L"Ecriture impossible dans %s.", staging.c_str()); return Result::Failed; }
    }

    // 2. Remplacement. Un .exe en cours d'execution ou une DLL chargee ne peuvent pas etre
    //    supprimes, mais peuvent etre renommes : l'ancien fichier devient *.old.
    std::vector<std::pair<fs::path, fs::path>> done;   // (cible, sauvegarde) pour annuler en cas d'echec
    for (const auto& f : m.files) {
        const fs::path target = dir / Widen(f.name);
        const fs::path backup = fs::path(target).concat(L".old");
        const fs::path fresh = staging / Widen(f.name);

        if (fs::exists(target)) {
            DeleteFileW(backup.c_str());
            if (!MoveFileExW(target.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                Say(L"Impossible de remplacer %s (erreur %lu). Ferme Minecraft et reessaie.", target.filename().c_str(), GetLastError());
                for (auto it = done.rbegin(); it != done.rend(); ++it) {   // annulation
                    DeleteFileW(it->first.c_str());
                    MoveFileExW(it->second.c_str(), it->first.c_str(), MOVEFILE_REPLACE_EXISTING);
                }
                return Result::Failed;
            }
        }
        if (!MoveFileExW(fresh.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            Say(L"Installation de %s impossible (erreur %lu).", target.filename().c_str(), GetLastError());
            MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
            return Result::Failed;
        }
        done.emplace_back(target, backup);
    }
    fs::remove_all(staging, ec);

    // 3. Relance du nouveau launcher
    Say(L"Mise a jour %s installee ! Redemarrage du launcher...", Widen(m.version).c_str());
    std::wstring cmd = L"\"" + selfExe.wstring() + L"\" --updated";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(selfExe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        Say(L"Relance impossible : ouvre NexusLauncher.exe a nouveau.");
        Sleep(4000);
    }
    return Result::Updated;
}
