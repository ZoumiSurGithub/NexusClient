#pragma once
// Nexus Launcher - logique (hors interface graphique).
// Regroupe : journalisation, injection de la DLL, detection des versions BedrockLauncher,
// manifeste des clients (quelle build pour quelle version de Minecraft) et notes de version.
#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

namespace fs = std::filesystem;

namespace Launcher {

// ----- Journal partage (affiche dans l'interface) -------------------------------
// Les operations longues tournent sur un thread de travail ; elles ecrivent ici et
// l'interface lit la liste a chaque image.
class Log {
public:
    enum class Level { Info, Good, Warn, Error };
    struct Line { Level level; std::wstring text; };

    void Add(Level lvl, const std::wstring& text);
    void Info(const std::wstring& t)  { Add(Level::Info,  t); }
    void Good(const std::wstring& t)  { Add(Level::Good,  t); }
    void Warn(const std::wstring& t)  { Add(Level::Warn,  t); }
    void Error(const std::wstring& t) { Add(Level::Error, t); }
    std::vector<Line> Snapshot() const;
    std::wstring Status() const;   // derniere ligne (phase courante), pour l'affichage du bouton
    void Clear();
private:
    mutable std::mutex mtx_;
    std::vector<Line> lines_;
};

// ----- Version de Minecraft (depuis BedrockLauncher ou la version du Store) ------
struct McVersion {
    std::wstring label;         // ce qu'on affiche (ex. "1.26.63" ou "Version active")
    std::wstring version;       // version d'affichage OnixClient (ex. "1.26.63")
    std::wstring packageVersion;// version de paquet (ex. "1.26.6301.0"), pour comparer a l'installee
    std::wstring installType;   // "uwp" ou "msixvc"
    std::wstring depProfile;    // profil de dependances ("uwp", "uwp_engagement", "gdk")
    std::wstring url;           // URL de telechargement directe (primaire)
    std::wstring mirrorUrl;     // miroir de secours (peut etre vide)
    bool         active = false;    // true = lancer la version deja installee (minecraft:)
    bool         installed = false; // true = c'est la version Minecraft actuellement installee
};

// ----- Build de client Nexus (depuis le manifeste) ------------------------------
struct ClientBuild {
    std::wstring name;                     // ex. "Nexus 1.3.0 (1.21)"
    std::vector<std::wstring> mcVersions;  // versions MC supportees (prefixes acceptes, ex. "1.21")
    fs::path     dll;                      // chemin local de la DLL (si presente)
    std::wstring url;                      // URL de telechargement (si la DLL n'est pas locale)
    std::wstring sha256;                   // empreinte attendue (facultatif)
    bool Supports(const std::wstring& mcVersion) const;
};

// ----- Raccourci de connexion serveur (depuis servers.ini) ----------------------
struct ServerShortcut {
    std::wstring name;        // nom affiche (ex. "Linesia")
    std::wstring address;     // hote (ex. "play.linesia.net")
    int          port = 19132;// port Bedrock (defaut 19132)
    std::wstring logoPath;    // chemin d'une image (png/jpg), absolu ou relatif a l'exe
};

// ----- Note de version (CHANGELOG) ----------------------------------------------
struct ChangeEntry {
    std::wstring version;             // ex. "1.1.0"
    std::wstring date;                // ex. "2026-11-14"
    std::vector<std::wstring> notes;  // une ligne par point
};

// ----- Configuration (launcher.ini a cote de l'exe) -----------------------------
struct Config {
    fs::path exeDir;                  // dossier de l'exe
    fs::path bedrockVersionsDir;      // dossier des versions BedrockLauncher (auto-detecte sinon)
    fs::path managedVersionsDir;      // versions telechargees par Nexus (exeDir/mc-versions)
    fs::path clientsDir;              // dossier des DLL clientes telechargees
    // Reglages du launcher (section [launcher] de launcher.ini)
    bool hideWhilePlaying = true;     // cacher le launcher pendant que Minecraft tourne
    bool autoInject = true;           // injecter le client quand il est compatible
    bool centerOnStart = true;        // centrer la fenetre au demarrage
};

Config LoadConfig(const fs::path& exeDir);
void   SaveLauncherSettings(const Config& cfg);   // ecrit la section [launcher] de launcher.ini

// Detection / enumeration
std::vector<McVersion>   EnumerateMcVersions(const Config& cfg);  // "Version active" (celle installee)

// Versions Minecraft publiees par OnixClient (schema v2), les plus recentes d'abord.
// maxCount <= 0 renvoie toutes les versions. Reseau requis.
std::vector<McVersion>   FetchOnlineReleases(const Config& cfg, int maxCount, Log& log);

// Numero de la version Minecraft actuellement installee (ex. "1.26.6301.0"), vide si aucune.
std::wstring             GetInstalledMcVersion();
std::vector<ClientBuild> LoadClients(const Config& cfg, Log& log);
std::vector<ChangeEntry> LoadChangelog(const Config& cfg);

std::wstring ConnectUri(const ServerShortcut& s);  // minecraft://connect/?serverUrl=...&serverPort=...

// Contenu "mis en avant" controle par le developpeur, recupere depuis une config distante
// (featured.json sur le depot GitHub). Les joueurs ne l'editent pas.
struct Featured {
    std::vector<ServerShortcut> servers;        // serveurs en connexion rapide (logos par URL)
    std::vector<std::wstring>   clientVersions; // versions MC sur lesquelles le client est utilisable
};
Featured FetchFeatured(const Config& cfg, Log& log);  // telecharge + cache (hors-ligne = dernier connu)

// Choisit la premiere build compatible avec la version MC donnee (nullptr si aucune).
const ClientBuild* FindClientForMc(const std::vector<ClientBuild>& clients, const McVersion& mc);

// Options passees au thread de lancement.
struct LaunchRequest {
    Config      cfg;
    McVersion   mc;
    ClientBuild client;      // valide seulement si hasClient = true
    bool        hasClient = false;   // false -> on lance juste Minecraft (pas d'injection)
    bool        needInstall = false; // true -> installer la version (telechargement direct) avant de jouer
    std::wstring serverUri;          // si non vide : URI "minecraft://connect/..." pour rejoindre un serveur
    HWND        window = nullptr;    // fenetre du launcher : cachee pendant le jeu, restauree a la fermeture de MC
};

// Orchestration complete (thread de travail) : si needInstall, installe d'abord la version
// (telechargement FE3 + Add-AppxPackage), puis lance Minecraft, attend le rendu et injecte la
// DLL si hasClient. Cache/restaure la fenetre autour du jeu. 'busy' repasse a false a la fin ;
// 'installedOk' passe a true si une installation a reussi (pour rafraichir la liste).
void RunFlow(const LaunchRequest& req, Log& log, std::atomic<bool>& busy, std::atomic<bool>& installedOk);

// Injection bas-niveau (exposee pour reutilisation / tests).
DWORD FindProcess(const wchar_t* name);
bool  HasModule(DWORD pid, const wchar_t* module);
bool  InjectDll(DWORD pid, const fs::path& dll, Log& log);

} // namespace Launcher
