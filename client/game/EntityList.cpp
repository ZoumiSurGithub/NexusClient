// Enumeration des entites via le niveau. Etape 1 : marche jusqu'au Level + compte.
#include "EntityList.h"
#include "ItemRender.h"
#include "core/Config.h"
#include "core/Logger.h"
#include <windows.h>

namespace {
    bool SafeReadPtr(uintptr_t a, uintptr_t& out) {
        __try { out = *reinterpret_cast<uintptr_t*>(a); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // ClientInstance -> Minecraft -> GameSession -> Level. 0 si un maillon manque.
    uintptr_t ResolveLevel(bool diag) {
        const Offsets& o = Config::GetOffsets();
        uintptr_t ci = reinterpret_cast<uintptr_t>(ItemRender::ClientInstance());
        if (!ci) { if (diag) LOG_INFO("EL: pas de ClientInstance (bouge en jeu)"); return 0; }
        uintptr_t mc = 0, gs = 0, level = 0;
        if (!SafeReadPtr(ci + o.ciMinecraft, mc) || !mc)     { if (diag) LOG_INFO("EL: minecraft nul (ci+0x%llX)", (unsigned long long)o.ciMinecraft); return 0; }
        if (!SafeReadPtr(mc + o.mcGameSession, gs) || !gs)   { if (diag) LOG_INFO("EL: gameSession nul (mc+0x%llX)", (unsigned long long)o.mcGameSession); return 0; }
        if (!SafeReadPtr(gs + o.gsLevel, level) || !level)   { if (diag) LOG_INFO("EL: level nul (gs+0x%llX)", (unsigned long long)o.gsLevel); return 0; }
        if (diag) {
            uintptr_t lvt = 0; SafeReadPtr(level, lvt);
            LOG_INFO("EL: ci=0x%llX mc=0x%llX gs=0x%llX level=0x%llX vtable=0x%llX",
                     (unsigned long long)ci, (unsigned long long)mc, (unsigned long long)gs,
                     (unsigned long long)level, (unsigned long long)lvt);
        }
        return level;
    }

    // std::vector<Actor*> renvoye par valeur : buffer de retour en RCX, this en RDX.
    using ListFn = void* (__fastcall*)(void* retBuf, void* self);
    struct GameVec { void** begin; void** end; void** cap; };

    bool CallActorList(uintptr_t level, GameVec& out, bool diag) {
        const Offsets& o = Config::GetOffsets();
        out = { nullptr, nullptr, nullptr };
        __try {
            void** vt = *reinterpret_cast<void***>(level);
            auto fn = reinterpret_cast<ListFn>(vt[o.levelActorListVtIdx]);
            fn(&out, reinterpret_cast<void*>(level));   // le jeu remplit 'out'
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (diag) LOG_INFO("EL: crash getRuntimeActorList (vtable idx 0x%X a verifier)", o.levelActorListVtIdx);
            return false;
        }
    }
}

std::vector<void*> EntityList::Collect() {
    std::vector<void*> result;
    uintptr_t level = ResolveLevel(false);
    if (!level) return result;
    GameVec v;
    if (!CallActorList(level, v, false)) return result;
    // Copie prudente (buffer alloue par le jeu : on NE libere PAS, allocateurs differents).
    __try {
        if (v.begin && v.end && v.end > v.begin) {
            const size_t n = static_cast<size_t>(v.end - v.begin);
            if (n < 100000) { result.reserve(n); for (size_t i = 0; i < n; ++i) if (v.begin[i]) result.push_back(v.begin[i]); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return result;
}

void EntityList::Probe() {
    static ULONGLONG last = 0; static int n = 0;
    const ULONGLONG now = GetTickCount64();
    if (n >= 20 || now - last < 1000) return;
    last = now; ++n;

    uintptr_t level = ResolveLevel(true);
    if (!level) return;
    GameVec v;
    if (!CallActorList(level, v, true)) return;
    size_t count = 0;
    __try { if (v.begin && v.end && v.end >= v.begin) count = static_cast<size_t>(v.end - v.begin); }
    __except (EXCEPTION_EXECUTE_HANDLER) { LOG_INFO("EL: vecteur illisible"); return; }
    LOG_INFO("EL: getRuntimeActorList -> %zu entites (vtable idx 0x%X)", count, Config::GetOffsets().levelActorListVtIdx);
    if (count > 0 && count < 100000) {
        __try {
            uintptr_t a0 = reinterpret_cast<uintptr_t>(v.begin[0]), vt0 = 0;
            SafeReadPtr(a0, vt0);
            LOG_INFO("EL: 1ere entite=0x%llX vtable=0x%llX", (unsigned long long)a0, (unsigned long long)vt0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}
