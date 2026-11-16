// Point d'entree de Nexus.dll
#include "Client.h"
#include "core/Logger.h"
#include "core/Config.h"
#include "hooks/RenderHook.h"
#include "hooks/MouseBlock.h"
#include "hooks/FocusGrab.h"
#include "game/Game.h"
#include "game/ItemRender.h"
#include "modules/AutoSprint.h"
#include "modules/HealthBars.h"
#include "modules/Stats.h"
#include "modules/Zoom.h"
#include "modules/OwnNametag.h"
#include "modules/ThirdPersonNametag.h"
#include "modules/ChatSender.h"
#include "modules/Network.h"
#include "modules/DiscordRPC.h"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <filesystem>
#include <shlobj.h>

namespace {
    HMODULE g_module = nullptr;
    std::atomic<bool> g_unload{ false };

    std::filesystem::path ModuleFolder() {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(g_module, path, MAX_PATH);
        return std::filesystem::path(path).parent_path();
    }

    // %APPDATA%\\Nexus : reglages, commandes et journal (survivent a une desinstallation).
    std::filesystem::path DataFolder() {
        PWSTR appdata = nullptr;
        std::filesystem::path dir;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata)) && appdata)
            dir = std::filesystem::path(appdata) / L"Nexus";
        if (appdata) CoTaskMemFree(appdata);
        if (dir.empty()) dir = ModuleFolder();   // repli si AppData indisponible
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    // Journalise un crash NON GERE (n'importe quel thread) : module + offset fautifs.
    // Ne se declenche pas sur nos lectures memoire protegees par __try (seulement sur un
    // vrai plantage). Purement diagnostic : on chaine ensuite le gestionnaire du jeu.
    LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
    LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        void* addr = er->ExceptionAddress;
        HMODULE mod = nullptr; char name[MAX_PATH] = "?"; uintptr_t base = 0;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(addr), &mod) && mod) {
            GetModuleFileNameA(mod, name, sizeof(name));
            base = reinterpret_cast<uintptr_t>(mod);
        }
        LOG_ERROR("CRASH: code=0x%08X addr=%p module=%s +0x%llX",
                  er->ExceptionCode, addr, name, (unsigned long long)(reinterpret_cast<uintptr_t>(addr) - base));
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
            LOG_ERROR("CRASH: acces %s a 0x%llX",
                      er->ExceptionInformation[0] == 1 ? "ecriture" : "lecture",
                      (unsigned long long)er->ExceptionInformation[1]);
        return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
    }

    // Gestionnaire vectoriel (premiere chance) : journalise TOUTES les exceptions (y compris
    // corruption de tas 0xC0000374 et depassement de pile /GS 0xC0000409 qui echappent au
    // filtre non-gere). On ignore quelques codes benins tres frequents pour limiter le bruit.
    LONG WINAPI VehLogger(EXCEPTION_POINTERS* ep) {
        const DWORD code = ep->ExceptionRecord->ExceptionCode;
        // Codes benins/non fatals a ignorer (poses par le debogueur, C++ EH, etc.)
        if (code == 0x406D1388u /*nom de thread*/ || code == 0x40010006u /*OutputDebugString*/ ||
            code == 0x4001000Au /*idem large*/ || (code & 0x20000000u) /*codes applicatifs*/)
            return EXCEPTION_CONTINUE_SEARCH;
        static ULONGLONG lastLog = 0; static unsigned n = 0;
        const ULONGLONG now = GetTickCount64();
        ++n;
        // Seuls ces codes terminent brutalement le processus (les violations d'acces 0xC0000005
        // sont le plus souvent interceptees par un __try et ne sont donc pas "fatales").
        const bool fatal = (code == 0xC0000374u /*tas*/ || code == 0xC0000409u /*pile/GS*/ ||
                            code == 0xC000001Du /*instr. illegale*/ || code == 0xC0000096u /*instr. privilegiee*/);
        if (!fatal && now - lastLog < 50) return EXCEPTION_CONTINUE_SEARCH;   // throttle le bruit non fatal
        lastLog = now;
        void* addr = ep->ExceptionRecord->ExceptionAddress;
        HMODULE mod = nullptr; char name[MAX_PATH] = "?"; uintptr_t base = 0;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(addr), &mod) && mod) {
            GetModuleFileNameA(mod, name, sizeof(name)); base = reinterpret_cast<uintptr_t>(mod);
        }
        LOG_ERROR("EXC(#%u)%s: code=0x%08X addr=%p module=%s +0x%llX", n, fatal ? " FATAL" : "",
                  code, addr, name, (unsigned long long)(reinterpret_cast<uintptr_t>(addr) - base));
        return EXCEPTION_CONTINUE_SEARCH;
    }

    DWORD WINAPI MainThread(LPVOID) {
        const auto folder = ModuleFolder();
        const auto dataFolder = DataFolder();
        Logger::Init(dataFolder / L"Nexus.log");
        g_prevFilter = SetUnhandledExceptionFilter(&CrashHandler);
        AddVectoredExceptionHandler(1, &VehLogger);   // premiere chance : capture meme les crashs fatals
        LOG_INFO("Nexus charge depuis %ls (donnees : %ls)", folder.c_str(), dataFolder.c_str());

        Config::Init(folder, dataFolder);

        if (MH_Initialize() != MH_OK) {
            LOG_ERROR("MH_Initialize a echoue");
            Logger::Shutdown();
            FreeLibraryAndExitThread(g_module, 1);
        }

        Game::Init();              // un echec ici n'empeche pas l'overlay (mode demo, FPS)
        ItemRender::Init();        // rendu des icones d'items via le moteur du jeu
        AutoSprint::Init();        // idem : sans offsets, le module est simplement inactif
        HealthBars::Init();
        Stats::Init();
        Zoom::Init();
        OwnNametag::Init();
        ThirdPersonNametag::Init();
        if (!RenderHook::Init()) {
            LOG_ERROR("Hook de rendu impossible, dechargement");
            g_unload = true;
        }

        MouseBlock::Init();        // camera figee quand le menu est ouvert
        FocusGrab::Init();         // retrait du focus au jeu quand le menu est ouvert
        Network::Init();           // detection du serveur (pour Discord)
        DiscordRPC::Start();

        while (!g_unload) {
            MouseBlock::Tick();    // detecte GameInput des que la souris a bouge
            Sleep(100);
        }

        // --- Dechargement propre -----------------------------------------------
        LOG_INFO("Dechargement...");
        Config::SaveSettings();    // on n'oublie aucun reglage au dechargement
        for (int i = 0; i < 100 && ChatSender::Busy(); ++i) Sleep(30); // envoi de chat en cours
        DiscordRPC::Stop();        // efface la presence Discord
        RenderHook::RequestShutdown();
        for (int i = 0; i < 40 && !RenderHook::IsShutdownComplete(); ++i) Sleep(50);

        FocusGrab::Shutdown();
        Game::Shutdown();
        AutoSprint::Shutdown();
        HealthBars::Shutdown();
        ThirdPersonNametag::Shutdown();
        Stats::Shutdown();
        Zoom::Shutdown();
        OwnNametag::Shutdown();
        MH_DisableHook(MH_ALL_HOOKS);
        Sleep(200);                // laisse les threads du jeu sortir de nos hooks
        MH_Uninitialize();

        LOG_INFO("Decharge.");
        Logger::Shutdown();
        FreeLibraryAndExitThread(g_module, 0);
    }
}

void Client::RequestUnload() { g_unload = true; }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        if (HANDLE t = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
