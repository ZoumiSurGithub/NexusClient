#include "AutoSprint.h"
#include "core/Config.h"
#include "core/SigScan.h"
#include "core/Logger.h"
#include <windows.h>
#include <MinHook.h>

// Principe : on hooke la fonction qui, a chaque tick, recopie l'etat des touches
// de deplacement dans une structure du jeu (ex : MoveInputHandler::tick).
// Juste apres l'original, on force a 1 le booleen "sprint enfonce" : pour le jeu,
// c'est exactement comme si tu tenais la touche Sprint.

namespace {
    using TickFn = void* (__fastcall*)(void*, void*, void*, void*);

    uintptr_t g_target = 0;
    TickFn    g_original = nullptr;

    bool SafeWriteBool(uintptr_t addr) {
        __try { *reinterpret_cast<volatile uint8_t*>(addr) = 1; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void* __fastcall HkTick(void* a1, void* a2, void* a3, void* a4) {
        void* result = g_original(a1, a2, a3, a4);

        const Offsets& o = Config::GetOffsets();
        if (Config::GetSettings().autoSprint && o.sprintOffset >= 0 &&
            o.sprintHookArg >= 0 && o.sprintHookArg < 4) {
            void* args[4] = { a1, a2, a3, a4 };
            if (auto obj = reinterpret_cast<uintptr_t>(args[o.sprintHookArg]))
                SafeWriteBool(obj + o.sprintOffset);
        }
        return result;
    }
}

bool AutoSprint::Init() {
    const Offsets& o = Config::GetOffsets();
    g_target = SigScan::Find(o.sprintHookSig, "autosprint.hook_signature");
    if (!g_target) return false;
    if (o.sprintOffset < 0) { LOG_WARN("autosprint.sprint_offset non renseigne"); }

    auto target = reinterpret_cast<void*>(g_target);
    if (MH_CreateHook(target, &HkTick, reinterpret_cast<void**>(&g_original)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG_ERROR("Impossible d'installer le hook auto-sprint");
        g_target = 0;
        return false;
    }
    LOG_INFO("Hook auto-sprint installe");
    return true;
}

void AutoSprint::Shutdown() {
    if (g_target) MH_DisableHook(reinterpret_cast<void*>(g_target));
}

bool AutoSprint::IsConfigured() {
    return g_target != 0 && Config::GetOffsets().sprintOffset >= 0;
}
