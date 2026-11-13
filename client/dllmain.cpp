// Point d'entree de Nexus.dll
#include "Client.h"
#include "core/Logger.h"
#include "core/Config.h"
#include "hooks/RenderHook.h"
#include "hooks/MouseBlock.h"
#include "game/Game.h"
#include "modules/AutoSprint.h"
#include "modules/HealthBars.h"
#include "modules/Zoom.h"
#include "modules/OwnNametag.h"
#include "modules/ChatSender.h"
#include "modules/Network.h"
#include "modules/DiscordRPC.h"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <filesystem>

namespace {
    HMODULE g_module = nullptr;
    std::atomic<bool> g_unload{ false };

    std::filesystem::path ModuleFolder() {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(g_module, path, MAX_PATH);
        return std::filesystem::path(path).parent_path();
    }

    DWORD WINAPI MainThread(LPVOID) {
        const auto folder = ModuleFolder();
        Logger::Init(folder / L"Nexus.log");
        LOG_INFO("Nexus charge depuis %ls", folder.c_str());

        Config::Init(folder);

        if (MH_Initialize() != MH_OK) {
            LOG_ERROR("MH_Initialize a echoue");
            Logger::Shutdown();
            FreeLibraryAndExitThread(g_module, 1);
        }

        Game::Init();              // un echec ici n'empeche pas l'overlay (mode demo, FPS)
        AutoSprint::Init();        // idem : sans offsets, le module est simplement inactif
        HealthBars::Init();
        Zoom::Init();
        OwnNametag::Init();
        if (!RenderHook::Init()) {
            LOG_ERROR("Hook de rendu impossible, dechargement");
            g_unload = true;
        }

        MouseBlock::Init();        // camera figee quand le menu est ouvert
        Network::Init();           // detection du serveur (pour Discord)
        DiscordRPC::Start();

        while (!g_unload) {
            MouseBlock::Tick();    // detecte GameInput des que la souris a bouge
            Sleep(100);
        }

        // --- Dechargement propre -----------------------------------------------
        LOG_INFO("Dechargement...");
        for (int i = 0; i < 100 && ChatSender::Busy(); ++i) Sleep(30); // envoi de chat en cours
        DiscordRPC::Stop();        // efface la presence Discord
        RenderHook::RequestShutdown();
        for (int i = 0; i < 40 && !RenderHook::IsShutdownComplete(); ++i) Sleep(50);

        Game::Shutdown();
        AutoSprint::Shutdown();
        HealthBars::Shutdown();
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
