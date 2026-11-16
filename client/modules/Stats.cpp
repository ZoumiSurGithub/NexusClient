#include "Stats.h"
#include "core/Config.h"
#include "core/SigScan.h"
#include "core/Logger.h"
#include "game/Game.h"
#include "ui/Ui.h"
#include <windows.h>
#include <MinHook.h>
#include <mutex>
#include <deque>
#include <atomic>

namespace {
    std::mutex g_mutex;
    std::deque<ULONGLONG> g_left, g_right;   // horodatage des clics (ms) pour le CPS
    int   g_combo = 0, g_maxCombo = 0;
    void* g_lastTarget = nullptr;            // derniere entite touchee (identite du combo)
    ULONGLONG g_lastHit = 0;                 // horodatage du dernier coup qui a touche
    float g_reach = 0.f;                     // reach du dernier coup porte sur une entite
    ULONGLONG g_lastReach = 0;
    float g_reachTaken = 0.f;                // reach du dernier coup RECU (portee de l'attaquant)
    ULONGLONG g_lastReachTaken = 0;

    HANDLE g_thread = nullptr;
    std::atomic<bool> g_stop{ false };

    // Hook Actor::attack : le jeu ne l'appelle que lorsqu'un acteur en frappe un autre
    // (donc jamais un clic dans le vide). On compte le combo du joueur local uniquement.
    using AttackFn = void* (__fastcall*)(void* self, void* ret, void* target, void* cause, void* a4);
    uintptr_t g_attackAddr = 0;
    AttackFn  g_origAttack = nullptr;

    void Prune(std::deque<ULONGLONG>& d, ULONGLONG now) {
        while (!d.empty() && now - d.front() > 1000) d.pop_front();
    }

    void OnLocalAttack(void* target) {
        std::lock_guard lock(g_mutex);
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG window = (ULONGLONG)Config::GetSettings().comboResetMs;
        // Meme cible dans la fenetre de temps -> on enchaine ; sinon la serie repart a 1.
        if (target == g_lastTarget && g_lastHit && now - g_lastHit <= window) g_combo += 1;
        else                                                                  g_combo = 1;
        g_lastTarget = target;
        g_lastHit = now;
        if (g_combo > g_maxCombo) g_maxCombo = g_combo;
    }

    // Lit le timer d'invincibilite de la cible (POD : compatible __try).
    int ReadInvuln(void* target) {
        __try {
            return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(target) +
                                           Config::GetOffsets().invulnTimeOffset);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }

    void* __fastcall HkAttack(void* self, void* ret, void* target, void* cause, void* a4) {
        void* lp = Game::LocalPlayer();
        const bool outgoing = self && target && self == lp;              // c'est nous qui frappons
        const bool incoming = self && target && target == lp && self != lp;  // on nous frappe

        // Le coup n'inflige des degats que si la cible n'est plus en i-frames : son timer
        // d'invincibilite doit etre a 0 au moment du coup (sinon il est absorbe). Le jeu
        // remet le timer au max plus tard (dans le tick), donc on ne peut pas comparer
        // avant/apres l'appel : on teste simplement l'etat de la cible a l'instant du coup.
        const bool deals = outgoing && ReadInvuln(target) <= 0;

        if (outgoing) {
            const float reach = Game::ReachBetween(lp, target);         // notre portee sur la cible
            if (reach >= 0.f) { std::lock_guard lock(g_mutex); g_reach = reach; g_lastReach = GetTickCount64(); }
        } else if (incoming) {
            const float reach = Game::ReachBetween(self, lp);           // portee de l'attaquant sur nous
            if (reach >= 0.f) { std::lock_guard lock(g_mutex); g_reachTaken = reach; g_lastReachTaken = GetTickCount64(); }
        }

        void* r = g_origAttack(self, ret, target, cause, a4);
        if (deals) OnLocalAttack(target);
        return r;
    }

    DWORD WINAPI Worker(LPVOID) {
        bool lPrev = false, rPrev = false;
        while (!g_stop.load()) {
            // On ne compte pas les clics quand le menu Nexus est ouvert (clics sur l'UI).
            const bool menu = Ui::IsMenuOpen();
            const bool lNow = !menu && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            const bool rNow = !menu && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            const ULONGLONG now = GetTickCount64();

            if (lNow && !lPrev) { std::lock_guard lock(g_mutex); g_left.push_back(now); }
            if (rNow && !rPrev) { std::lock_guard lock(g_mutex); g_right.push_back(now); }
            lPrev = lNow; rPrev = rNow;
            Sleep(3);
        }
        return 0;
    }
}

void Stats::Init() {
    g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);

    g_attackAddr = SigScan::Find(Config::GetOffsets().attackSig, "combo.attack_signature");
    if (g_attackAddr) {
        auto t = reinterpret_cast<void*>(g_attackAddr);
        if (MH_CreateHook(t, &HkAttack, reinterpret_cast<void**>(&g_origAttack)) == MH_OK &&
            MH_EnableHook(t) == MH_OK) {
            LOG_INFO("Combo : hook Actor::attack installe (coups sur entite)");
        } else {
            LOG_WARN("Combo : hook Actor::attack impossible (combo indisponible)");
            g_attackAddr = 0;
        }
    } else {
        LOG_WARN("Combo : signature Actor::attack introuvable (combo indisponible)");
    }
}

void Stats::Shutdown() {
    g_stop = true;
    if (g_thread) { WaitForSingleObject(g_thread, 1000); CloseHandle(g_thread); g_thread = nullptr; }
    if (g_attackAddr) MH_DisableHook(reinterpret_cast<void*>(g_attackAddr));
}

int Stats::LeftCps() {
    const ULONGLONG now = GetTickCount64();
    std::lock_guard lock(g_mutex);
    Prune(g_left, now);
    return static_cast<int>(g_left.size());
}
int Stats::RightCps() {
    const ULONGLONG now = GetTickCount64();
    std::lock_guard lock(g_mutex);
    Prune(g_right, now);
    return static_cast<int>(g_right.size());
}
int Stats::Combo() {
    const ULONGLONG now = GetTickCount64();
    std::lock_guard lock(g_mutex);
    if (g_lastHit && now - g_lastHit > (ULONGLONG)Config::GetSettings().comboResetMs) return 0;
    return g_combo;
}
int Stats::MaxCombo() { std::lock_guard lock(g_mutex); return g_maxCombo; }
float Stats::Reach() {
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG window = (ULONGLONG)Config::GetSettings().comboResetMs;   // meme delai que le combo
    std::lock_guard lock(g_mutex);
    if (g_lastReach && now - g_lastReach > window) return 0.f;
    return g_reach;
}
float Stats::ReachTaken() {
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG window = (ULONGLONG)Config::GetSettings().comboResetMs;
    std::lock_guard lock(g_mutex);
    if (g_lastReachTaken && now - g_lastReachTaken > window) return 0.f;
    return g_reachTaken;
}
