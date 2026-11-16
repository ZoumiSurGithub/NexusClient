#include "Launcher.h"
#include "version.h"   // NEXUS_UPDATE_REPO (URL de la config distante)

#include <tlhelp32.h>
#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <winhttp.h>
#include <fstream>
#include <algorithm>
#include <map>
#include <set>
#include <thread>
#include <cwctype>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cwchar>

#pragma comment(lib, "winhttp.lib")

namespace Launcher {

static constexpr wchar_t kProcessName[] = L"Minecraft.Windows.exe";

// ============================ Journal ===========================================
void Log::Add(Level lvl, const std::wstring& text) {
    std::lock_guard<std::mutex> lk(mtx_);
    lines_.push_back({ lvl, text });
    if (lines_.size() > 500) lines_.erase(lines_.begin(), lines_.begin() + 100);
}
std::vector<Log::Line> Log::Snapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return lines_;
}
std::wstring Log::Status() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return lines_.empty() ? std::wstring() : lines_.back().text;
}
void Log::Clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    lines_.clear();
}

// ============================ Utilitaires =======================================
static std::wstring EnvVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring();
}

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    size_t b = s.find_last_not_of(L" \t\r\n");
    return (a == std::wstring::npos) ? std::wstring() : s.substr(a, b - a + 1);
}

// Lit un fichier texte UTF-8 et le convertit en wstring.
static std::wstring ReadTextUtf8(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF) bytes.erase(0, 3);  // BOM
    if (bytes.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), w.data(), n);
    return w;
}

// Decoupe un texte en lignes (gere \r\n et \n).
static std::vector<std::wstring> SplitLines(const std::wstring& text) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : text) {
        if (c == L'\n') { out.push_back(cur); cur.clear(); }
        else if (c != L'\r') cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static std::vector<std::wstring> SplitComma(const std::wstring& s) {
    std::vector<std::wstring> out; std::wstring cur;
    for (wchar_t c : s) { if (c == L',') { auto t = Trim(cur); if (!t.empty()) out.push_back(t); cur.clear(); } else cur.push_back(c); }
    auto t = Trim(cur); if (!t.empty()) out.push_back(t);
    return out;
}

// ============================ Configuration =====================================
Config LoadConfig(const fs::path& exeDir) {
    Config cfg;
    cfg.exeDir = exeDir;
    cfg.clientsDir = exeDir / L"clients";
    cfg.managedVersionsDir = exeDir / L"mc-versions";   // versions telechargees par Nexus

    // launcher.ini (facultatif) : permet de forcer les dossiers.
    const fs::path ini = exeDir / L"launcher.ini";
    wchar_t buf[MAX_PATH];
    if (fs::exists(ini)) {
        if (GetPrivateProfileStringW(L"paths", L"bedrock_versions_dir", L"", buf, MAX_PATH, ini.c_str()) > 0)
            cfg.bedrockVersionsDir = Trim(buf);
        if (GetPrivateProfileStringW(L"paths", L"clients_dir", L"", buf, MAX_PATH, ini.c_str()) > 0)
            cfg.clientsDir = Trim(buf);
        cfg.hideWhilePlaying = GetPrivateProfileIntW(L"launcher", L"hide_while_playing", 1, ini.c_str()) != 0;
        cfg.autoInject       = GetPrivateProfileIntW(L"launcher", L"auto_inject", 1, ini.c_str()) != 0;
        cfg.centerOnStart    = GetPrivateProfileIntW(L"launcher", L"center_on_start", 1, ini.c_str()) != 0;
    }

    // Auto-detection du dossier de versions de BedrockLauncher si non force.
    if (cfg.bedrockVersionsDir.empty()) {
        const std::wstring local = EnvVar(L"LOCALAPPDATA");
        const std::wstring roam  = EnvVar(L"APPDATA");
        const std::wstring prof  = EnvVar(L"USERPROFILE");
        const fs::path bases[] = {
            fs::path(local) / L"BedrockLauncher",
            fs::path(roam)  / L"BedrockLauncher",
            fs::path(local) / L"Packages" / L"BedrockLauncher",
            fs::path(prof)  / L"Documents" / L"BedrockLauncher",
            fs::path(local) / L".minecraft_bedrock",
        };
        std::error_code ec;
        for (const auto& b : bases) {
            if (b.empty()) continue;
            const fs::path v = b / L"versions";
            if (fs::exists(v, ec)) { cfg.bedrockVersionsDir = v; break; }
            if (fs::exists(b, ec)) { cfg.bedrockVersionsDir = b; break; }  // versions a la racine
        }
    }
    return cfg;
}

// Ecrit la section [launcher] de launcher.ini (reglages modifiables dans l'onglet Parametres).
void SaveLauncherSettings(const Config& cfg) {
    const fs::path ini = cfg.exeDir / L"launcher.ini";
    WritePrivateProfileStringW(L"launcher", L"hide_while_playing", cfg.hideWhilePlaying ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"launcher", L"auto_inject",       cfg.autoInject       ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"launcher", L"center_on_start",   cfg.centerOnStart    ? L"1" : L"0", ini.c_str());
}

// ============================ Versions Minecraft ================================
std::vector<McVersion> EnumerateMcVersions(const Config& cfg) {
    (void)cfg;
    std::vector<McVersion> out;
    // Entree par defaut : lancer la version Minecraft actuellement installee.
    McVersion active;
    active.label  = L"Version active (celle installée)";
    active.active = true;
    out.push_back(active);
    return out;
}

// ============================ Versions en ligne (OnixClient) ===================
// Liste des versions Bedrock servie par OnixClient (schema v2) : telechargements directs,
// hashes, type d'install (uwp/msixvc) et profils de dependances. Source independante et a jour.
static const wchar_t* kOnixVersionsUrl =
    L"https://raw.githubusercontent.com/OnixClient/onix_compatible_appx/main/versionsv2.json";

static bool HttpDownload(const std::wstring& url, const fs::path& dest, Log& log, bool quiet = false);  // defini plus bas
static std::string  ToUtf8(const std::wstring& w);   // defini plus bas
static std::wstring ToWide(const std::string& s);    // defini plus bas

// Lit un fichier en octets bruts (UTF-8), pour parsing JSON en chaine etroite.
static std::string ReadFileBytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::string s;
    if (f) { f.seekg(0, std::ios::end); s.resize((size_t)f.tellg()); f.seekg(0);
             if (!s.empty()) f.read(&s[0], (std::streamsize)s.size()); }
    return s;
}

// --- Mini-extracteur JSON (schema connu, pas un parseur complet) ---------------
// Position juste apres le ':' de "key" au niveau superieur de obj[start,end), sinon npos.
static size_t JsonKeyPos(const std::string& s, const std::string& key, size_t start, size_t end) {
    const std::string pat = "\"" + key + "\"";
    int depth = 0; bool inStr = false;
    for (size_t i = start; i < end; ++i) {
        const char c = s[i];
        if (inStr) { if (c == '\\') ++i; else if (c == '"') inStr = false; continue; }
        if (c == '"') {
            if (depth == 1 && s.compare(i, pat.size(), pat) == 0) {
                size_t j = i + pat.size();
                while (j < end && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r')) ++j;
                if (j < end && s[j] == ':') return j + 1;
            }
            inStr = true; continue;
        }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') --depth;
    }
    return std::string::npos;
}
// Valeur a la position pos : chaine deguillemetee, objet/tableau brut, ou litteral.
static std::string JsonValueAt(const std::string& s, size_t pos, size_t end) {
    while (pos < end && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) ++pos;
    if (pos >= end) return "";
    const char c = s[pos];
    if (c == '"') {
        std::string out; ++pos;
        for (; pos < end && s[pos] != '"'; ++pos) {
            if (s[pos] != '\\' || pos + 1 >= end) { out += s[pos]; continue; }
            const char e = s[++pos];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {   // \uXXXX -> UTF-8
                    if (pos + 4 < end) {
                        int cp = 0;
                        for (int k = 1; k <= 4; ++k) {
                            const char h = s[pos + k];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= h - '0';
                            else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                        }
                        pos += 4;
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                        else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                    }
                    break;
                }
                default: out += e;   // \" \\ \/ et autres : caractere tel quel
            }
        }
        return out;
    }
    if (c == '{' || c == '[') {
        const char close = (c == '{') ? '}' : ']';
        int d = 0; bool inStr = false; const size_t st = pos;
        for (; pos < end; ++pos) {
            const char ch = s[pos];
            if (inStr) { if (ch == '\\') ++pos; else if (ch == '"') inStr = false; continue; }
            if (ch == '"') inStr = true;
            else if (ch == c) ++d;
            else if (ch == close) { if (--d == 0) return s.substr(st, pos - st + 1); }
        }
        return s.substr(st);
    }
    size_t st = pos;
    while (pos < end && s[pos] != ',' && s[pos] != '}' && s[pos] != ']') ++pos;
    std::string out = s.substr(st, pos - st);
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n' || out.back() == '\r' || out.back() == '\t'))
        out.pop_back();
    return out;
}
// Valeur de "key" au niveau superieur de obj (chaine, objet brut, ou litteral).
static std::string JsonField(const std::string& obj, const std::string& key) {
    const size_t p = JsonKeyPos(obj, key, 0, obj.size());
    return (p == std::string::npos) ? std::string() : JsonValueAt(obj, p, obj.size());
}
// Decoupe les objets {...} de premier niveau contenus dans un tableau.
static std::vector<std::string> JsonObjects(const std::string& arr) {
    std::vector<std::string> out;
    int depth = 0; bool inStr = false; size_t st = std::string::npos;
    for (size_t i = 0; i < arr.size(); ++i) {
        const char c = arr[i];
        if (inStr) { if (c == '\\') ++i; else if (c == '"') inStr = false; continue; }
        if (c == '"') { inStr = true; continue; }
        if (c == '{') { if (depth == 0) st = i; ++depth; }
        else if (c == '}') { if (--depth == 0 && st != std::string::npos) { out.push_back(arr.substr(st, i - st + 1)); st = std::string::npos; } }
    }
    return out;
}

// Telecharge et parse la liste des versions OnixClient. La liste est deja de la plus
// recente a la plus ancienne. On garde aussi le JSON en cache (pour les dependances a l'install).
std::vector<McVersion> FetchOnlineReleases(const Config& cfg, int maxCount, Log& log) {
    std::vector<McVersion> out;
    const fs::path cache = cfg.clientsDir / L"versionsv2.json";
    std::error_code ec; fs::create_directories(cfg.clientsDir, ec);
    if (!HttpDownload(kOnixVersionsUrl, cache, log, true)) {
        log.Warn(L"Liste des versions indisponible (pas d'Internet ?).");
        return out;
    }
    const std::string data = ReadFileBytes(cache);
    const std::string versions = JsonField(data, "versions");
    if (versions.empty()) { log.Warn(L"Liste des versions illisible."); return out; }

    for (const std::string& vo : JsonObjects(versions)) {
        const std::string ver  = JsonField(vo, "version");
        if (ver.empty()) continue;
        const std::string pkg  = JsonField(vo, "package_version");
        const std::string it   = JsonField(vo, "install_type");
        const std::string dp   = JsonField(vo, "dependency_profile");
        const std::string urlO = JsonField(vo, "url");          // objet { type, url, ... }
        const std::string mirO = JsonField(vo, "mirror_url");   // objet ou "null"
        std::string prim = JsonField(urlO, "url");
        std::string mir  = (mirO == "null" || mirO.empty()) ? "" : JsonField(mirO, "url");
        if (mir.rfind("http", 0) != 0) mir.clear();             // on ignore les miroirs scindes (tableau)

        McVersion v;
        v.version = ToWide(ver);
        v.packageVersion = ToWide(pkg);
        v.installType = ToWide(it);
        v.depProfile = ToWide(dp);
        v.url = ToWide(prim);
        v.mirrorUrl = ToWide(mir);
        v.label = v.version + L" (à télécharger)";
        out.push_back(std::move(v));
        if (maxCount > 0 && (int)out.size() >= maxCount) break;
    }

    if (out.empty()) log.Warn(L"Aucune version trouvée dans la liste.");
    else log.Good(std::to_wstring(out.size()) + L" versions disponibles (OnixClient).");
    return out;
}

// ============================ Manifeste clients =================================
bool ClientBuild::Supports(const std::wstring& mcVersion) const {
    for (const auto& v : mcVersions) {
        if (v == L"*") return true;
        if (!mcVersion.empty() && (mcVersion.rfind(v, 0) == 0 || v.rfind(mcVersion, 0) == 0))
            return true;  // correspondance par prefixe (ex. "1.21" couvre "1.21.2.2")
    }
    return false;
}

// Format de clients.ini (une ou plusieurs sections), insensible a la casse des cles :
//   [client]
//   name=Nexus 1.3.0 (1.21)
//   mc=1.21,1.20.80          ; versions MC supportees ("*" = toutes)
//   dll=clients/Nexus-1.21.dll   ; chemin local (relatif a l'exe) OU
//   url=https://.../Nexus.dll    ; telechargement si la DLL n'est pas la
//   sha256=...                   ; facultatif
// "1.26.6301.0" -> "1.26" (major.minor), vide si inconnu.
static std::wstring MajorMinor(const std::wstring& v) {
    const size_t d1 = v.find(L'.');
    if (d1 == std::wstring::npos) return L"";
    const size_t d2 = v.find(L'.', d1 + 1);
    return (d2 == std::wstring::npos) ? v : v.substr(0, d2);
}

// Cree le fichier Nexus.mc (versions compatibles du client) avec un prefixe par defaut.
static void WriteClientVersions(const fs::path& p, const std::wstring& prefix) {
    std::ofstream f(p, std::ios::binary);
    if (!f) return;
    const std::string s =
        "; Versions de Minecraft compatibles avec le client Nexus (prefixes separes par des virgules).\n"
        "; Exemples : 1.21   ou   1.21,1.20.80   ou   * pour toutes les versions.\n"
        "; Le client n'est injecte que pour une version qui correspond.\n"
        + ToUtf8(prefix) + "\n";
    f.write(s.data(), (std::streamsize)s.size());
}

std::vector<ClientBuild> LoadClients(const Config& cfg, Log& log) {
    std::vector<ClientBuild> out;
    const fs::path ini = cfg.exeDir / L"clients.ini";

    if (fs::exists(ini)) {
        ClientBuild cur; bool inBlock = false;
        auto flush = [&] { if (inBlock && !cur.name.empty()) out.push_back(cur); cur = ClientBuild{}; };
        for (auto& raw : SplitLines(ReadTextUtf8(ini))) {
            std::wstring line = Trim(raw);
            if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
            if (line[0] == L'[') { flush(); inBlock = true; continue; }
            const size_t eq = line.find(L'=');
            if (eq == std::wstring::npos) continue;
            std::wstring key = Trim(line.substr(0, eq)), val = Trim(line.substr(eq + 1));
            std::transform(key.begin(), key.end(), key.begin(), ::towlower);
            if      (key == L"name") cur.name = val;
            else if (key == L"mc")   cur.mcVersions = SplitComma(val);
            else if (key == L"dll")  cur.dll = fs::absolute(cfg.exeDir / val);
            else if (key == L"url")  cur.url = val;
            else if (key == L"sha256") cur.sha256 = val;
        }
        flush();
    }

    // Repli : si aucun manifeste mais une Nexus.dll est presente, on l'expose comme client "actuel".
    // Les versions supportees sont decidees par le developpeur via la config distante (featured.json)
    // et appliquees apres coup ; en attendant (ou hors-ligne sans cache) on reste permissif ("*").
    if (out.empty()) {
        const fs::path dll = cfg.exeDir / L"Nexus.dll";
        if (fs::exists(dll)) {
            ClientBuild c;
            c.name = L"Nexus (client actuel)";
            c.dll = dll;
            c.mcVersions = { L"*" };
            out.push_back(c);
        } else {
            log.Warn(L"Aucun manifeste clients.ini et aucune Nexus.dll trouvée.");
        }
    }
    return out;
}

const ClientBuild* FindClientForMc(const std::vector<ClientBuild>& clients, const McVersion& mc) {
    // Numero inconnu : on ne peut pas verifier la compatibilite -> uniquement un client "*".
    if (mc.version.empty()) {
        for (const auto& c : clients) if (c.Supports(L"")) return &c;  // "*" uniquement
        return nullptr;
    }
    for (const auto& c : clients) if (c.Supports(mc.version)) return &c;
    return nullptr;
}

// URI de connexion directe Bedrock : minecraft://connect/?serverUrl=HOST&serverPort=PORT
std::wstring ConnectUri(const ServerShortcut& s) {
    return L"minecraft://connect/?serverUrl=" + s.address + L"&serverPort=" + std::to_wstring(s.port);
}

// Extrait les chaines d'un tableau JSON plat (ex. ["1.26","1.21.60"]).
static std::vector<std::string> JsonStringArray(const std::string& arr) {
    std::vector<std::string> out;
    bool inStr = false; size_t st = 0;
    for (size_t i = 0; i < arr.size(); ++i) {
        const char c = arr[i];
        if (inStr) { if (c == '\\') { ++i; } else if (c == '"') { out.push_back(arr.substr(st, i - st)); inStr = false; } }
        else if (c == '"') { inStr = true; st = i + 1; }
    }
    return out;
}

// Recupere la config distante "mise en avant" (featured.json sur le depot GitHub) : serveurs
// a afficher + versions sur lesquelles le client est utilisable. Garde un cache local pour le
// mode hors-ligne (dernier contenu connu). Les joueurs ne modifient pas ce contenu.
Featured FetchFeatured(const Config& cfg, Log& log) {
    Featured out;
    const std::wstring url = std::wstring(L"https://raw.githubusercontent.com/") + NEXUS_UPDATE_REPO + L"/main/featured.json";
    const fs::path cache = cfg.clientsDir / L"featured.json";
    std::error_code ec; fs::create_directories(cfg.clientsDir, ec);

    if (!HttpDownload(url, cache, log, true) && !fs::exists(cache)) {
        log.Warn(L"Contenu mis en avant indisponible (pas d'Internet ?).");
        return out;   // ni reseau ni cache
    }
    const std::string data = ReadFileBytes(cache);
    if (data.empty()) return out;

    // Versions supportees par le client (decidees par le developpeur).
    for (const auto& v : JsonStringArray(JsonField(data, "clientVersions")))
        if (!v.empty()) out.clientVersions.push_back(ToWide(v));

    // Serveurs mis en avant (logos telecharges depuis leur URL vers un cache local).
    int idx = 0;
    for (const std::string& so : JsonObjects(JsonField(data, "servers"))) {
        ServerShortcut sv;
        sv.name    = ToWide(JsonField(so, "name"));
        sv.address = ToWide(JsonField(so, "address"));
        const std::string port = JsonField(so, "port");
        sv.port = port.empty() ? 19132 : atoi(port.c_str());
        if (sv.port <= 0) sv.port = 19132;
        const std::string logo = JsonField(so, "logo");
        if (logo.rfind("http", 0) == 0) {
            const fs::path lf = cfg.clientsDir / (L"logo_" + std::to_wstring(idx) + L".img");
            if (HttpDownload(ToWide(logo), lf, log, true)) {
                // Verifie que le fichier telecharge est bien une image reconnue.
                const std::string head = ReadFileBytes(lf);
                auto starts = [&](const char* sig, size_t n) { return head.size() >= n && memcmp(head.data(), sig, n) == 0; };
                const bool png  = starts("\x89PNG", 4);
                const bool jpg  = head.size() >= 2 && (unsigned char)head[0] == 0xFF && (unsigned char)head[1] == 0xD8;
                const bool gif  = starts("GIF8", 4);
                const bool bmp  = starts("BM", 2);
                const bool webp = starts("RIFF", 4) && head.size() >= 12 && memcmp(head.data() + 8, "WEBP", 4) == 0;
                if (webp)
                    log.Warn(L"Logo de " + sv.name + L" : WebP non supporte, convertis-le en PNG ou JPG.");
                else if (!(png || jpg || gif || bmp))
                    log.Warn(L"Logo de " + sv.name + L" : fichier non reconnu comme image (URL correcte ? lien 'raw' GitHub ?).");
                sv.logoPath = lf.wstring();
            } else {
                log.Warn(L"Logo de " + sv.name + L" : telechargement echoue (" + ToWide(logo) + L").");
            }
        }
        if (!sv.name.empty() && !sv.address.empty()) out.servers.push_back(std::move(sv));
        ++idx;
    }
    if (!out.servers.empty() || !out.clientVersions.empty())
        log.Info(std::to_wstring(out.servers.size()) + L" serveur(s) mis en avant, "
                 + std::to_wstring(out.clientVersions.size()) + L" version(s) client.");
    return out;
}

// ============================ Notes de version ==================================
std::vector<ChangeEntry> LoadChangelog(const Config& cfg) {
    std::vector<ChangeEntry> out;
    // Cache distant (recupere depuis GitHub) en priorite ; sinon le fichier livre avec l'exe.
    std::wstring text = ReadTextUtf8(cfg.clientsDir / L"CHANGELOG.md");
    if (text.empty()) text = ReadTextUtf8(cfg.exeDir / L"CHANGELOG.md");
    if (text.empty()) return out;

    ChangeEntry cur; bool have = false;
    for (auto& raw : SplitLines(text)) {
        std::wstring line = Trim(raw);
        if (line.rfind(L"## ", 0) == 0) {           // nouvelle version : "## [1.1.0] - 2026-11-14"
            if (have) out.push_back(cur);
            cur = ChangeEntry{}; have = true;
            std::wstring h = line.substr(3);
            size_t lb = h.find(L'['), rb = h.find(L']');
            if (lb != std::wstring::npos && rb != std::wstring::npos && rb > lb)
                cur.version = h.substr(lb + 1, rb - lb - 1);
            size_t dash = h.find(L'-', rb == std::wstring::npos ? 0 : rb);
            if (dash != std::wstring::npos) cur.date = Trim(h.substr(dash + 1));
        } else if (have && (line.rfind(L"- ", 0) == 0 || line.rfind(L"* ", 0) == 0)) {
            cur.notes.push_back(Trim(line.substr(2)));
        }
    }
    if (have) out.push_back(cur);
    return out;
}

// Recupere le CHANGELOG.md directement depuis GitHub (branche main) vers le cache, puis le parse.
// Toujours a jour ; repli automatique sur le cache / fichier livre si GitHub est injoignable.
std::vector<ChangeEntry> FetchChangelog(const Config& cfg, Log& log) {
    const std::wstring url = std::wstring(L"https://raw.githubusercontent.com/") + NEXUS_UPDATE_REPO + L"/main/CHANGELOG.md";
    const fs::path cache = cfg.clientsDir / L"CHANGELOG.md";
    std::error_code ec; fs::create_directories(cfg.clientsDir, ec);
    if (!HttpDownload(url, cache, log, true))
        log.Warn(L"Changelog en ligne indisponible, affichage du dernier connu.");
    return LoadChangelog(cfg);
}

// ============================ Injection =========================================
DWORD FindProcess(const wchar_t* name) {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

bool HasModule(DWORD pid, const wchar_t* module) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{ sizeof(me) };
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
        if (_wcsicmp(me.szModule, module) == 0) { found = true; break; }
    CloseHandle(snap);
    return found;
}

// Ancienne version UWP : autorise "ALL APPLICATION PACKAGES" a lire la DLL (sandbox).
static void GrantAppContainerAccess(const fs::path& file) {
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-15-2-1", &sid)) return;
    PACL oldAcl = nullptr; PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(file.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, &oldAcl, nullptr, &sd) == ERROR_SUCCESS) {
        EXPLICIT_ACCESSW ea{};
        ea.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
        ea.grfAccessMode = GRANT_ACCESS;
        ea.grfInheritance = NO_INHERITANCE;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea.Trustee.ptstrName = static_cast<LPWSTR>(sid);
        PACL newAcl = nullptr;
        if (SetEntriesInAclW(1, &ea, oldAcl, &newAcl) == ERROR_SUCCESS) {
            SetNamedSecurityInfoW(const_cast<LPWSTR>(file.c_str()), SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION, nullptr, nullptr, newAcl, nullptr);
            LocalFree(newAcl);
        }
        LocalFree(sd);
    }
    LocalFree(sid);
}

bool InjectDll(DWORD pid, const fs::path& dll, Log& log) {
    GrantAppContainerAccess(dll);
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) { log.Error(L"Impossible d'ouvrir le processus (lance en administrateur ?)."); return false; }

    const std::wstring path = dll.wstring();
    const SIZE_T size = (path.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = false;
    if (remote && WriteProcessMemory(proc, remote, path.c_str(), size, nullptr)) {
        auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE th = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
        if (th) {
            WaitForSingleObject(th, 15000);
            DWORD exitCode = 0; GetExitCodeThread(th, &exitCode);
            ok = exitCode != 0;
            if (!ok) log.Error(L"LoadLibraryW a renvoye NULL : la DLL n'a pas pu etre chargee.");
            CloseHandle(th);
        } else log.Error(L"CreateRemoteThread a échoué.");
    } else log.Error(L"Ecriture memoire impossible.");
    if (remote) VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
}

// ============================ Telechargement (winhttp) ==========================
static bool HttpDownload(const std::wstring& url, const fs::path& dest, Log& log, bool quiet) {
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { log.Error(L"URL invalide : " + url); return false; }

    HINTERNET s = WinHttpOpen(L"NexusLauncher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    if (c) {
        DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET r = WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
              && WinHttpReceiveResponse(r, nullptr)) {
            DWORD status = 0, len = sizeof(status);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
            if (status == 200) {
                std::error_code ec; fs::create_directories(dest.parent_path(), ec);
                std::ofstream f(dest, std::ios::binary);
                DWORD avail = 0; ok = f.good();
                while (ok && WinHttpQueryDataAvailable(r, &avail) && avail) {
                    std::string buf(avail, '\0'); DWORD read = 0;
                    if (!WinHttpReadData(r, buf.data(), avail, &read) || read == 0) { ok = false; break; }
                    f.write(buf.data(), read);
                }
            } else if (!quiet) log.Error(L"Téléchargement : code HTTP " + std::to_wstring(status));
        }
        if (r) WinHttpCloseHandle(r);
        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(s);
    return ok;
}

// ============================ Telechargement independant d'une version =========
static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// POST generique ; 'headers' = en-tetes bruts (termines par \r\n). Renvoie le corps en texte.
static bool HttpPost(const std::wstring& url, const std::string& body, const wchar_t* headers,
                     std::string& out, Log& log, const wchar_t* errLabel) {
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    HINTERNET s = WinHttpOpen(L"Windows-Update-Agent/10.0.10011.16384 Client-Protocol/2.0",
                              WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    bool okr = false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    if (c) {
        DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET r = WinHttpOpenRequest(c, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (r) {
            if (WinHttpSendRequest(r, headers, (DWORD)-1, (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0)
                && WinHttpReceiveResponse(r, nullptr)) {
                DWORD status = 0, len = sizeof(status);
                WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    DWORD avail = 0; okr = true;
                    while (WinHttpQueryDataAvailable(r, &avail) && avail) {
                        std::string buf(avail, '\0'); DWORD read = 0;
                        if (!WinHttpReadData(r, buf.data(), avail, &read) || read == 0) { okr = false; break; }
                        out.append(buf.data(), read);
                    }
                } else log.Error(std::wstring(errLabel) + L" : code HTTP " + std::to_wstring(status));
            }
            WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(s);
    return okr;
}

// Horodatage ISO 8601 UTC (ex. 2026-11-16T09:00:00Z), decale de plusSeconds.
static std::wstring NowIso(int plusSeconds) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    u.QuadPart += (ULONGLONG)plusSeconds * 10000000ULL;
    ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    SYSTEMTIME st; FileTimeToSystemTime(&ft, &st);
    wchar_t b[32];
    swprintf(b, 32, L"%04d-%02d-%02dT%02d:%02d:%02dZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

// uuid pseudo-aleatoire pour le MessageID SOAP.
static std::string MakeUuid() {
    static bool seeded = false; if (!seeded) { srand((unsigned)GetTickCount64()); seeded = true; }
    char b[40]; auto h = [] { return "0123456789abcdef"[rand() & 0xF]; };
    for (int i = 0; i < 36; ++i) b[i] = (i==8||i==13||i==18||i==23) ? '-' : h();
    b[14] = '4'; b[19] = "89ab"[rand() & 3]; b[36] = 0;
    return b;
}

// Desechappe &amp; dans une URL FE3.
static std::string XmlUnescape(std::string s) {
    size_t p; while ((p = s.find("&amp;")) != std::string::npos) s.replace(p, 5, "&");
    return s;
}

// Lance une commande fenetre cachee et renvoie son code de sortie (-1 si echec de lancement).
static int RunHidden(const std::wstring& cmd, DWORD timeoutMs = 180000) {
    STARTUPINFOW si{ sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{}; std::wstring m = cmd;
    if (!CreateProcessW(nullptr, m.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return (int)code;
}

// Extrait une archive zip/appx via tar (bsdtar, integre a Windows 10+).
static bool ExtractArchive(const fs::path& archive, const fs::path& dest, Log& log) {
    std::error_code ec; fs::remove_all(dest, ec); fs::create_directories(dest, ec);
    const std::wstring cmd = L"tar -xf \"" + archive.wstring() + L"\" -C \"" + dest.wstring() + L"\"";
    if (RunHidden(cmd) != 0) { log.Error(L"Extraction échouée (tar)."); return false; }
    return true;
}

// Cherche l'appx x64 de Minecraft dans un dossier (cas d'un .appxbundle extrait).
static fs::path FindMinecraftX64Appx(const fs::path& dir) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        std::wstring n = e.path().filename().wstring(), low = n;
        std::transform(low.begin(), low.end(), low.begin(), ::towlower);
        if (low.find(L"minecraft") != std::wstring::npos && low.find(L"x64") != std::wstring::npos &&
            low.size() > 5 && low.substr(low.size() - 5) == L".appx")
            return e.path();
    }
    return {};
}

static std::wstring RunPsCapture(const std::wstring& inner);       // defini plus bas
static bool PackageInstalled(const std::wstring& name);            // defini plus bas

// Construit l'enveloppe SOAP GetExtendedUpdateInfo2 (API FE3 de Microsoft) pour un updateId.
static std::string BuildFe3Request(const std::string& updateId, const std::string& revision = "1") {
    const std::string created = ToUtf8(NowIso(0));
    const std::string expires = ToUtf8(NowIso(300));
    const std::string uuid = MakeUuid();
    const std::string dev =
        "E:App=WU_OS&amp;AppVer=10.0.19041.1&amp;OSArchitecture=AMD64&amp;DeviceFamily=Windows.Desktop"
        "&amp;InstallationType=Client&amp;OSVersion=10.0.19041.1&amp;FlightRing=Retail&amp;IsRetailOS=1"
        "&amp;AttrDataVer=61&amp;BranchReadinessLevel=CB";
    return
        "<s:Envelope xmlns:a=\"http://www.w3.org/2005/08/addressing\" xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Header>"
        "<a:Action s:mustUnderstand=\"1\">http://www.microsoft.com/SoftwareDistribution/Server/ClientWebService/GetExtendedUpdateInfo2</a:Action>"
        "<a:MessageID>urn:uuid:" + uuid + "</a:MessageID>"
        "<a:To s:mustUnderstand=\"1\">https://fe3.delivery.mp.microsoft.com/ClientWebService/client.asmx/secured</a:To>"
        "<o:Security s:mustUnderstand=\"1\" xmlns:o=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\">"
        "<Timestamp xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
        "<Created>" + created + "</Created><Expires>" + expires + "</Expires></Timestamp>"
        "<wuws:WindowsUpdateTicketsToken wsu:id=\"ClientMSA\" xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\" xmlns:wuws=\"http://schemas.microsoft.com/msus/2014/10/WindowsUpdateAuthorization\">"
        "<TicketType Name=\"MSA\" Version=\"1.0\" Policy=\"MBI_SSL\"><Device></Device></TicketType>"
        "</wuws:WindowsUpdateTicketsToken></o:Security></s:Header>"
        "<s:Body><GetExtendedUpdateInfo2 xmlns=\"http://www.microsoft.com/SoftwareDistribution/Server/ClientWebService\">"
        "<updateIDs><UpdateIdentity><UpdateID>" + updateId + "</UpdateID><RevisionNumber>" + revision + "</RevisionNumber></UpdateIdentity></updateIDs>"
        "<infoTypes><XmlUpdateFragmentType>FileUrl</XmlUpdateFragmentType><XmlUpdateFragmentType>FileDecryption</XmlUpdateFragmentType></infoTypes>"
        "<deviceAttributes>" + dev + "</deviceAttributes>"
        "</GetExtendedUpdateInfo2></s:Body></s:Envelope>";
}

// Extrait un appx (ou un bundle) vers 'dest' et verifie la presence d'AppxManifest.xml.
static bool ExtractToVersion(const fs::path& pkg, const fs::path& dest, const fs::path& tmpBase, Log& log) {
    std::error_code ec;
    if (!ExtractArchive(pkg, dest, log)) return false;
    if (fs::exists(dest / L"AppxManifest.xml")) return true;
    // Peut-etre un .appxbundle : chercher l'appx x64 a l'interieur et l'extraire.
    const fs::path inner = FindMinecraftX64Appx(dest);
    if (!inner.empty()) {
        const fs::path tmp = tmpBase; fs::remove_all(tmp, ec);
        if (ExtractArchive(inner, tmp, log) && fs::exists(tmp / L"AppxManifest.xml")) {
            fs::remove_all(dest, ec); fs::rename(tmp, dest, ec);
            return fs::exists(dest / L"AppxManifest.xml");
        }
    }
    return false;
}

// Arguments silencieux d'une dependance exe (tableau JSON ["-q"] -> "-q").
static std::wstring JsonArgs(const std::string& dep) {
    std::string a = JsonField(dep, "quiet_args");       // ex. ["-q"] ou null
    std::wstring out;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] == '"') { size_t e = a.find('"', i + 1); if (e == std::string::npos) break;
            if (!out.empty()) out += L' '; out += ToWide(a.substr(i + 1, e - i - 1)); i = e; }
    }
    return out;
}

// Installe les dependances du profil (VCLibs, Store Engagement, runtime GDK...). Best-effort.
static void InstallDependencies(const Config& cfg, const std::wstring& profile, Log& log) {
    if (profile.empty()) return;
    const std::string data = ReadFileBytes(cfg.clientsDir / L"versionsv2.json");
    const std::string profs = JsonField(data, "dependency_profiles");
    if (profs.empty()) return;
    const std::string arr = JsonField(profs, ToUtf8(profile));
    if (arr.empty()) return;

    for (const std::string& dep : JsonObjects(arr)) {
        const std::string name = JsonField(dep, "name");
        const std::string it   = JsonField(dep, "install_type");
        const std::string urlO = JsonField(dep, "url");
        const std::string url  = (urlO == "null" || urlO.empty()) ? "" : JsonField(urlO, "url");

        if (it == "ask") {   // dependance a installer manuellement (Xbox App, Gaming Services)
            if (!PackageInstalled(ToWide(name)))
                log.Warn(L"Dépendance requise : " + ToWide(name) + L" - " + ToWide(JsonField(dep, "prompt")));
            continue;
        }
        if (url.empty()) continue;

        const std::wstring ext = (it == "exe") ? L".exe" : L".appx";
        const fs::path f = cfg.clientsDir / (ToWide(name) + ext);
        log.Info(L"Dépendance : " + ToWide(name) + L"...");
        if (!HttpDownload(ToWide(url), f, log, true)) { log.Warn(L"  téléchargement échoué."); continue; }

        if (it == "exe") {
            const std::wstring args = JsonArgs(dep);
            RunHidden(L"\"" + f.wstring() + L"\" " + args, 300000);
        } else {   // appx : si une version >= est deja la, Add-AppxPackage echoue sans dommage
            RunPsCapture(L"Add-AppxPackage -Path '" + f.wstring() + L"' -ForceApplicationShutdown -ErrorAction Stop");
        }
        std::error_code ec; fs::remove(f, ec);
    }
}

// Pour les versions msixvc : remplace gamelaunchhelper.exe dans le dossier du jeu installe
// (contourne la verification de licence du Store). Necessite les droits admin. Best-effort.
static void ReplaceGameLaunchHelper(const Config& cfg, Log& log) {
    const fs::path h = cfg.clientsDir / L"gamelaunchhelper.exe";
    if (!HttpDownload(L"https://raw.githubusercontent.com/OnixClient/onix_compatible_appx/main/gamelaunchhelper.exe",
                      h, log, true)) { log.Warn(L"gamelaunchhelper.exe indisponible."); return; }
    const wchar_t* targets[] = {
        L"C:\\XboxGames\\Minecraft for Windows\\Content\\gamelaunchhelper.exe",
        L"C:\\XboxGames\\7792D9CE-355A-493C-AFBD-768F4A77C3B0\\Content\\gamelaunchhelper.exe",
    };
    std::error_code ec;
    for (const wchar_t* t : targets) {
        const fs::path tp(t);
        if (!fs::exists(tp.parent_path(), ec)) continue;
        RunPsCapture(L"attrib -R '" + std::wstring(t) + L"' 2>$null; Copy-Item -Path '" + h.wstring() +
                     L"' -Destination '" + std::wstring(t) + L"' -Force; attrib +R '" + std::wstring(t) + L"'");
        log.Info(L"gamelaunchhelper.exe remplace dans le jeu.");
    }
    fs::remove(h, ec);
}

// Desactive durablement la Xbox Game Bar (reglages utilisateur HKCU, reversibles dans
// Parametres > Jeux > Xbox Game Bar).
static void DisableGameBarPopup() {
    RunHidden(L"reg add \"HKCU\\Software\\Microsoft\\GameBar\" /v UseNexusForGameBarEnabled /t REG_DWORD /d 0 /f", 10000);
    RunHidden(L"reg add \"HKCU\\Software\\Microsoft\\GameBar\" /v ShowStartupPanel /t REG_DWORD /d 0 /f", 10000);
    RunHidden(L"reg add \"HKCU\\System\\GameConfigStore\" /v GameDVR_Enabled /t REG_DWORD /d 0 /f", 10000);
    RunHidden(L"reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR\" /v AppCaptureEnabled /t REG_DWORD /d 0 /f", 10000);
}

// Ferme la Xbox Game Bar si elle s'ouvre. Les services Gaming la relancent, donc on insiste
// pendant 'seconds' secondes, en tache de fond (ne bloque pas l'install/le lancement).
static void SuppressGameBar(int seconds) {
    DisableGameBarPopup();
    std::thread([seconds] {
        for (int i = 0; i < seconds * 2; ++i) {
            RunHidden(L"taskkill /F /IM GameBar.exe /IM GameBarFTServer.exe /IM GameBarElevatedFT_Alias.exe", 4000);
            Sleep(500);
        }
    }).detach();
}

// Diagnostic : journalise les fenetres visibles (titre + executable), pour identifier la
// "fenetre service de jeu" a fermer. Lance en tache de fond pendant l'installation.
static void LogVisibleWindows(Log& log) {
    struct Ctx { Log* log; };
    auto enumProc = [](HWND h, LPARAM p) -> BOOL {
        Ctx* c = reinterpret_cast<Ctx*>(p);
        if (!IsWindowVisible(h)) return TRUE;
        wchar_t title[256] = {}; GetWindowTextW(h, title, 255);
        if (!title[0]) return TRUE;
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        std::wstring exe = L"?";
        HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (ph) {
            wchar_t buf[MAX_PATH]; DWORD sz = MAX_PATH;
            if (QueryFullProcessImageNameW(ph, 0, buf, &sz)) {
                std::wstring full = buf; const size_t s = full.find_last_of(L"\\");
                exe = (s == std::wstring::npos) ? full : full.substr(s + 1);
            }
            CloseHandle(ph);
        }
        c->log->Info(L"[Fenêtre] " + std::wstring(title) + L"  (" + exe + L")");
        return TRUE;
    };
    std::thread([&log, enumProc] {
        for (int t : { 2, 4, 7, 10 }) {   // captures a plusieurs instants
            Sleep((DWORD)t * 1000);
            Ctx c{ &log };
            EnumWindows(enumProc, reinterpret_cast<LPARAM>(&c));
        }
    }).detach();
}

// Installe une version depuis OnixClient : dependances du profil, puis telechargement direct
// du paquet et installation (Add-AppxPackage, valable pour .appx comme .msixvc).
static bool DoInstall(const LaunchRequest& req, Log& log) {
    const McVersion& mc = req.mc;
    if (mc.url.empty()) { log.Error(L"Pas d'URL de téléchargement pour cette version."); return false; }
    std::error_code ec; fs::create_directories(req.cfg.clientsDir, ec);

    // 0. Versions msixvc : couper la Xbox Game Bar + journaliser les fenetres (diagnostic).
    if (mc.installType == L"msixvc") {
        log.Info(L"Neutralisation de la Xbox Game Bar…");
        SuppressGameBar(30);
        LogVisibleWindows(log);   // identifie la "fenetre service de jeu" a cibler
    }

    // 1. Dependances (VCLibs, etc.).
    log.Info(L"Préparation des dépendances (" + mc.depProfile + L")...");
    InstallDependencies(req.cfg, mc.depProfile, log);

    // 2. Telechargement du paquet (URL directe, repli sur le miroir).
    const std::wstring ext = (mc.installType == L"msixvc") ? L".msixvc" : L".appx";
    const fs::path pkg = req.cfg.clientsDir / (mc.version + ext);
    log.Info(L"Téléchargement de Minecraft " + mc.version + L" (peut durer une minute)…");
    bool dl = HttpDownload(mc.url, pkg, log);
    if (!dl && !mc.mirrorUrl.empty()) {
        log.Warn(L"URL primaire indisponible, essai du miroir…");
        dl = HttpDownload(mc.mirrorUrl, pkg, log);
    }
    if (!dl) { log.Error(L"Téléchargement échoué."); return false; }

    // 3. Installation (remplace la version en place, meme plus recente).
    log.Info(L"Installation du paquet…");
    const std::wstring m = RunPsCapture(L"Add-AppxPackage -Path '" + pkg.wstring() +
                                        L"' -ForceUpdateFromAnyVersion -ForceApplicationShutdown -ErrorAction Stop");
    fs::remove(pkg, ec);
    if (m != L"OK") { log.Error(L"Installation échouée : " + m); return false; }

    // 4. Versions msixvc : corriger le lanceur du jeu.
    if (mc.installType == L"msixvc") ReplaceGameLaunchHelper(req.cfg, log);

    log.Good(L"Version " + mc.version + L" installée et active !");
    return true;
}

// ============================ Lancement d'une version ===========================
// Enregistre la version choisie aupres de Windows (appx extrait) puis la rend lancable
// par le protocole minecraft:. Best-effort : si l'enregistrement echoue, on lancera la
// version actuellement enregistree.
// Execute une commande PowerShell (dans try/catch) et renvoie "OK" ou le message d'erreur.
static std::wstring RunPsCapture(const std::wstring& inner) {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    const fs::path res = fs::path(tmp) / L"nexus_ps.txt";
    std::error_code ec; fs::remove(res, ec);
    const std::wstring ps =
        L"powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command "
        L"\"try { " + inner + L" ; 'OK' | Out-File -Encoding utf8 '" + res.wstring() +
        L"' } catch { $_.Exception.Message | Out-File -Encoding utf8 '" + res.wstring() + L"' }\"";
    RunHidden(ps, 180000);
    std::wstring msg = Trim(ReadTextUtf8(res));
    fs::remove(res, ec);
    return msg;
}

// Numero de la version Minecraft actuellement installee (ex. "1.26.6301.0"), vide si aucune.
std::wstring GetInstalledMcVersion() {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    const fs::path res = fs::path(tmp) / L"nexus_ver.txt";
    std::error_code ec; fs::remove(res, ec);
    const std::wstring ps =
        L"powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command "
        L"\"(Get-AppxPackage Microsoft.MinecraftUWP).Version | Out-File -Encoding utf8 '" + res.wstring() + L"'\"";
    RunHidden(ps, 30000);
    std::wstring v = Trim(ReadTextUtf8(res));
    fs::remove(res, ec);
    return v;
}

// true si un paquet appx (par nom d'identite) est installe sur la machine.
static bool PackageInstalled(const std::wstring& name) {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    const fs::path res = fs::path(tmp) / L"nexus_dep.txt";
    std::error_code ec; fs::remove(res, ec);
    const std::wstring ps =
        L"powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command "
        L"\"if (Get-AppxPackage " + name + L") { 'yes' | Out-File -Encoding utf8 '" + res.wstring() + L"' }\"";
    RunHidden(ps, 30000);
    const std::wstring v = Trim(ReadTextUtf8(res));
    fs::remove(res, ec);
    return v == L"yes";
}

// Lance Minecraft (version actuellement installee), cache le launcher, injecte le client si
// disponible, puis attend la fermeture du jeu et restaure le launcher.
static void DoLaunch(const LaunchRequest& req, Log& log) {
    const bool connect = !req.serverUri.empty();
    const std::wstring uri = connect ? req.serverUri : L"minecraft:";
    DWORD pid = FindProcess(kProcessName);
    // On lance l'URI si Minecraft n'est pas deja ouvert, ou s'il faut rejoindre un serveur
    // (l'URI de connexion fait rejoindre le serveur meme sur une instance deja lancee).
    if (!pid || connect) {
        log.Info(connect ? L"Connexion au serveur..." : L"Lancement de Minecraft Bedrock...");
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
            log.Error(L"Impossible de lancer Minecraft (est-il installé ?).");
            return;
        }
    }
    if (!pid) {
        for (int i = 0; i < 120 && !pid; ++i) { Sleep(500); pid = FindProcess(kProcessName); }
        if (!pid) { log.Error(L"Minecraft ne s'est pas lance dans les 60 secondes."); return; }
    }
    log.Good(L"Minecraft trouvé (PID " + std::to_wstring(pid) + L").");

    // Le lancement d'un jeu GDK peut ouvrir la Xbox Game Bar : on la neutralise un moment.
    SuppressGameBar(15);

    // 3. Minecraft tourne -> on cache le launcher (si l'option est active ; il reviendra a la
    //    fermeture du jeu). Sinon on le laisse visible.
    const bool hide = req.cfg.hideWhilePlaying && req.window;
    if (hide) ShowWindow(req.window, SW_HIDE);

    // 4. Injection du client, si disponible.
    if (!req.hasClient) {
        log.Warn(L"Aucun client Nexus pour cette version : Minecraft tourne seul.");
    } else if (HasModule(pid, L"Nexus.dll")) {
        log.Warn(L"Nexus est déjà injecté.");
    } else {
        // Resoudre la DLL du client (locale ou telechargee).
        fs::path dll = req.client.dll;
        bool dllOk = true;
        if (dll.empty() || !fs::exists(dll)) {
            if (!req.client.url.empty()) {
                dll = req.cfg.clientsDir / (req.client.name + L".dll");
                log.Info(L"Téléchargement du client : " + req.client.name + L"...");
                if (!HttpDownload(req.client.url, dll, log)) { log.Error(L"Téléchargement du client échoué."); dllOk = false; }
                else log.Good(L"Client téléchargé.");
            } else { log.Error(L"DLL du client introuvable et aucune URL fournie."); dllOk = false; }
        }
        if (dllOk) {
            log.Info(L"Attente du chargement du rendu...");
            for (int i = 0; i < 120; ++i) {
                if (HasModule(pid, L"d3d12.dll") || HasModule(pid, L"d3d11.dll")) break;
                Sleep(500);
            }
            Sleep(3000);  // laisse le jeu creer sa swapchain
            log.Info(L"Injection de " + dll.filename().wstring() + L"...");
            if (InjectDll(pid, dll, log)) log.Good(L"Injection réussie ! INSER = menu, FIN = décharger.");
            else                          log.Error(L"L'injection a échoué.");
        }
    }

    // 5. Attendre la fermeture de Minecraft, puis reafficher le launcher au premier plan.
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (h) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }
    if (hide) {
        ShowWindow(req.window, SW_SHOW);
        SetForegroundWindow(req.window);
        log.Info(L"Minecraft fermé. Launcher restauré.");
    }
}

void RunFlow(const LaunchRequest& req, Log& log, std::atomic<bool>& busy, std::atomic<bool>& installedOk) {
    struct BusyGuard { std::atomic<bool>& b; ~BusyGuard() { b = false; } } guard{ busy };
    installedOk = false;
    if (req.needInstall) {
        if (!DoInstall(req, log)) { log.Error(L"Installation échouée : lancement annulé."); return; }
        installedOk = true;  // la liste sera rafraichie
    }
    DoLaunch(req, log);
}

} // namespace Launcher
