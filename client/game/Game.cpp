#include "Game.h"
#include "core/Config.h"
#include "core/SigScan.h"
#include "core/Logger.h"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cmath>

// Toutes les lectures et appels de fonctions du jeu passent par des wrappers
// __try/__except : si un offset est faux, on obtient "pas de donnees" au lieu
// d'un crash du jeu. (Ces fonctions ne doivent contenir aucun objet C++.)

namespace {
    using PlayerHookFn      = void* (__fastcall*)(void*, void*, void*, void*);
    using GetArmorFn        = const void* (__fastcall*)(void* actor, int slot);
    using GetSelectedItemFn = const void* (__fastcall*)(void* player);
    using GetDamageValueFn  = short (__fastcall*)(const void* stack);
    using GetMaxDamageFn    = short (__fastcall*)(const void* item);

    uintptr_t         g_hookTarget = 0;
    PlayerHookFn      g_origPlayerHook = nullptr;
    GetArmorFn        g_getArmor = nullptr;
    GetSelectedItemFn g_getSelectedItem = nullptr;
    GetDamageValueFn  g_getDamageValue = nullptr;

    std::atomic<void*>    g_captured{ nullptr };   // objet recu par la fonction hookee
    std::atomic<uint64_t> g_capturedAt{ 0 };       // GetTickCount64 de la derniere capture

    // ---- Hook ---------------------------------------------------------------
    void* __fastcall HkPlayerHook(void* a1, void* a2, void* a3, void* a4) {
        void* args[4] = { a1, a2, a3, a4 };
        const int idx = Config::GetOffsets().playerHookArg;
        if (idx >= 0 && idx < 4) {
            g_captured.store(args[idx], std::memory_order_relaxed);
            g_capturedAt.store(GetTickCount64(), std::memory_order_relaxed);
        }
        return g_origPlayerHook(a1, a2, a3, a4);
    }

    // ---- Wrappers proteges ----------------------------------------------------
    bool SafeReadPtr(uintptr_t addr, uintptr_t& out) {
        __try { out = *reinterpret_cast<uintptr_t*>(addr); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool SafeReadU8(uintptr_t addr, uint8_t& out) {
        __try { out = *reinterpret_cast<uint8_t*>(addr); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    const void* SafeGetArmor(void* actor, int slot) {
        __try { return g_getArmor(actor, slot); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    const void* SafeGetSelected(void* player) {
        __try { return g_getSelectedItem(player); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    bool SafeGetDamage(const void* stack, short& out) {
        __try { out = g_getDamageValue(stack); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool SafeGetMaxDamage(const void* item, int vtIndex, short& out) {
        __try {
            auto vtable = *reinterpret_cast<GetMaxDamageFn* const*>(item);
            out = vtable[vtIndex](item);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void* ResolvePlayer() {
        void* base = g_captured.load(std::memory_order_relaxed);
        // Pas de capture depuis 1 s : on est dans un menu / le monde a ete quitte.
        if (!base || GetTickCount64() - g_capturedAt.load(std::memory_order_relaxed) > 1000) return nullptr;

        uintptr_t p = reinterpret_cast<uintptr_t>(base);
        for (ptrdiff_t off : Config::GetOffsets().playerChain) {
            if (!SafeReadPtr(p + off, p) || !p) return nullptr;
        }
        return reinterpret_cast<void*>(p);
    }

    Game::ItemInfo ReadStack(const void* stack) {
        Game::ItemInfo info;
        if (!stack) return info;
        const Offsets& o = Config::GetOffsets();
        const auto s = reinterpret_cast<uintptr_t>(stack);

        // ItemStackBase::mItem est un WeakPtr<Item> -> SharedCounter<Item>* -> Item*
        uintptr_t counter = 0, item = 0;
        if (!SafeReadPtr(s + o.itemStackItem, counter) || !counter) return info;
        if (!SafeReadPtr(counter, item) || !item) return info;

        if (o.itemStackCount >= 0) {
            uint8_t count = 0;
            if (!SafeReadU8(s + o.itemStackCount, count) || count == 0) return info;
        }
        info.present = true;

        short maxDmg = 0, dmg = 0;
        if (o.itemGetMaxDamageIndex >= 0 &&
            SafeGetMaxDamage(reinterpret_cast<void*>(item), o.itemGetMaxDamageIndex, maxDmg) && maxDmg > 0 &&
            SafeGetDamage(stack, dmg)) {
            info.maxDamage = maxDmg;
            info.damage = dmg < 0 ? 0 : (dmg > maxDmg ? maxDmg : dmg);
        }
        return info;
    }

    Game::Snapshot DemoSnapshot() {
        Game::Snapshot snap;
        snap.inGame = true;
        snap.demo = true;
        const double t = GetTickCount64() / 1000.0;
        const int maxes[Game::SlotCount] = { 363, 528, 495, 429, 1561 }; // diamant + epee diamant
        for (int i = 0; i < Game::SlotCount; ++i) {
            auto& it = snap.items[i];
            it.present = true;
            it.maxDamage = maxes[i];
            const double phase = 0.5 + 0.5 * std::sin(t * 0.3 + i * 1.3);
            it.damage = static_cast<int>(it.maxDamage * (1.0 - phase * 0.97));
        }
        return snap;
    }
}

bool Game::Init() {
    const Offsets& o = Config::GetOffsets();

    g_hookTarget      = SigScan::Find(o.playerHookSig, "player.hook_signature");
    g_getArmor        = reinterpret_cast<GetArmorFn>(SigScan::Find(o.getArmorSig, "Actor_getArmor"));
    g_getSelectedItem = reinterpret_cast<GetSelectedItemFn>(SigScan::Find(o.getSelectedItemSig, "Player_getSelectedItem"));
    g_getDamageValue  = reinterpret_cast<GetDamageValueFn>(SigScan::Find(o.getDamageValueSig, "ItemStackBase_getDamageValue"));

    if (o.itemGetMaxDamageIndex < 0) LOG_WARN("vtable.Item_getMaxDamage non renseigne");

    if (g_hookTarget) {
        auto target = reinterpret_cast<void*>(g_hookTarget);
        if (MH_CreateHook(target, &HkPlayerHook, reinterpret_cast<void**>(&g_origPlayerHook)) == MH_OK &&
            MH_EnableHook(target) == MH_OK) {
            LOG_INFO("Hook joueur installe");
        } else {
            LOG_ERROR("Impossible d'installer le hook joueur");
            g_hookTarget = 0;
        }
    }

    LOG_INFO(IsConfigured() ? "Lecture de l'armure : prete"
                            : "Lecture de l'armure : offsets incomplets (le mode demo reste disponible)");
    return IsConfigured();
}

void Game::Shutdown() {
    if (g_hookTarget) MH_DisableHook(reinterpret_cast<void*>(g_hookTarget));
}

bool Game::IsConfigured() {
    return g_hookTarget && g_getArmor && g_getDamageValue && Config::GetOffsets().itemGetMaxDamageIndex >= 0;
}

std::array<Game::FunctionStatus, 5> Game::GetFunctionStatus() {
    return { {
        { "Hook joueur",              g_hookTarget != 0,          true },
        { "Actor::getArmor",          g_getArmor != nullptr,       true },
        { "ItemStack::getDamageValue", g_getDamageValue != nullptr, true },
        { "Item::getMaxDamage (vtable)", Config::GetOffsets().itemGetMaxDamageIndex >= 0, true },
        { "Player::getSelectedItem",  g_getSelectedItem != nullptr, false },
    } };
}

Game::Snapshot Game::Demo() { return DemoSnapshot(); }

void* Game::LocalPlayer() { return g_hookTarget ? ResolvePlayer() : nullptr; }

Game::Snapshot Game::Read() {
    if (Config::GetSettings().demoMode) return DemoSnapshot();

    Snapshot snap;
    if (!IsConfigured()) return snap;

    void* player = ResolvePlayer();
    if (!player) return snap;
    snap.inGame = true;

    for (int slot = Helmet; slot <= Boots; ++slot)
        snap.items[slot] = ReadStack(SafeGetArmor(player, slot));

    if (g_getSelectedItem)
        snap.items[Held] = ReadStack(SafeGetSelected(player));

    return snap;
}
