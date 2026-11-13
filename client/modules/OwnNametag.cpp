#include "OwnNametag.h"
#include "core/Config.h"
#include "core/SigScan.h"
#include "core/Logger.h"
#include "game/Game.h"
#include <windows.h>
#include <MinHook.h>

// Fonction hookee : bool XXX(..., Actor* ...) qui renvoie "afficher le nametag ?"
// (ex. Actor::canShowNameTag ou la verification du rendu des nametags).
namespace {
    using CheckFn = bool (__fastcall*)(void*, void*, void*, void*);
    uintptr_t g_target = 0;
    CheckFn   g_original = nullptr;

    bool __fastcall HkCheck(void* a1, void* a2, void* a3, void* a4) {
        const bool result = g_original(a1, a2, a3, a4);
        if (result || !Config::GetSettings().showOwnNametag) return result;

        void* args[4] = { a1, a2, a3, a4 };
        const int idx = Config::GetOffsets().nametagActorArg;
        if (idx < 0 || idx > 3 || !args[idx]) return result;

        void* local = Game::LocalPlayer();
        return local && args[idx] == local ? true : result;
    }
}

bool OwnNametag::Init() {
    g_target = SigScan::Find(Config::GetOffsets().nametagSig, "nametag.hook_signature");
    if (!g_target) return false;
    auto target = reinterpret_cast<void*>(g_target);
    if (MH_CreateHook(target, &HkCheck, reinterpret_cast<void**>(&g_original)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG_ERROR("Impossible d'installer le hook nametag");
        g_target = 0;
        return false;
    }
    LOG_INFO("Hook nametag installe");
    return true;
}

void OwnNametag::Shutdown() {
    if (g_target) MH_DisableHook(reinterpret_cast<void*>(g_target));
}

bool OwnNametag::IsConfigured() { return g_target != 0; }
