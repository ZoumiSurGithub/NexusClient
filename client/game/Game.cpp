#include "Game.h"
#include "ArmorEcs.h"
#include "ItemRender.h"
#include "core/Config.h"
#include "core/SigScan.h"
#include "core/Logger.h"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <memory>

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
    uintptr_t         g_getHoverName = 0;   // ItemStackBase::getHoverName

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
        // Auto-sprint : on ecrit AVANT l'update (pour que la fonction traite le bit)
        // ET apres (pour qu'il tienne jusqu'au prochain tick). Thread du jeu.
        const bool sprint = Config::GetSettings().autoSprint;
        if (sprint) Game::SetMoveSprint(true);
        void* r = g_origPlayerHook(a1, a2, a3, a4);
        if (sprint) Game::SetMoveSprint(true);
        return r;
    }

    // ---- Wrappers proteges ----------------------------------------------------
    bool SafeReadPtr(uintptr_t addr, uintptr_t& out) {
        __try { out = *reinterpret_cast<uintptr_t*>(addr); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // --- Diagnostic barres de vie : enumeration des entites via le niveau ---
    // ClientInstance -> Minecraft -> GameSession -> Level -> getRuntimeActorList().
    void ProbeEntities() {
        static uint64_t last = 0; static int n = 0;
        const uint64_t now = GetTickCount64();
        if (n >= 20 || now - last < 1000) return;
        last = now; ++n;

        const Offsets& o = Config::GetOffsets();
        uintptr_t ci = reinterpret_cast<uintptr_t>(ItemRender::ClientInstance());
        if (!ci) { LOG_INFO("EL: pas de ClientInstance (ItemRender pas encore actif ?)"); return; }
        uintptr_t mc = 0, gs = 0, level = 0;
        if (!SafeReadPtr(ci + o.ciMinecraft, mc) || !mc)   { LOG_INFO("EL: minecraft nul (ci+0x%llX)", (unsigned long long)o.ciMinecraft); return; }
        if (!SafeReadPtr(mc + o.mcGameSession, gs) || !gs) { LOG_INFO("EL: gameSession nul (mc+0x%llX)", (unsigned long long)o.mcGameSession); return; }
        if (!SafeReadPtr(gs + o.gsLevel, level) || !level) { LOG_INFO("EL: level nul (gs+0x%llX)", (unsigned long long)o.gsLevel); return; }
        uintptr_t lvt = 0; SafeReadPtr(level, lvt);
        LOG_INFO("EL: ci=0x%llX mc=0x%llX gs=0x%llX level=0x%llX vtable=0x%llX",
                 (unsigned long long)ci, (unsigned long long)mc, (unsigned long long)gs,
                 (unsigned long long)level, (unsigned long long)lvt);

        // Appel vtable DESACTIVE tant que l'index n'est pas confirme (un mauvais index
        // execute une fonction au hasard et plante). Index < 0 => on liste les entrees
        // de la vtable (en RVA) une seule fois, pour les identifier dans IDA sans risque.
        if (o.levelActorListVtIdx < 0) {
            static bool dumped = false;
            if (!dumped) {
                dumped = true;
                const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                LOG_INFO("EL: base module=0x%llX  vtable Level RVA=0x%llX",
                         (unsigned long long)base, (unsigned long long)(lvt - base));
                __try {
                    void** vt = reinterpret_cast<void**>(lvt);
                    for (int i = 0x138; i <= 0x158; ++i) {   // plage autour de 0x145 (Latite)
                        uintptr_t fn = reinterpret_cast<uintptr_t>(vt[i]);
                        LOG_INFO("EL: vtable[0x%X] -> abs 0x%llX (RVA 0x%llX)",
                                 i, (unsigned long long)fn, (unsigned long long)(fn - base));
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) { LOG_INFO("EL: dump vtable interrompu"); }
            }
            return;
        }

        // Lecture DIRECTE de la liste d'acteurs (sans allocation, pas de fuite) :
        // storage = *(Level+0x4B8) ; sentinelle = *(storage+0x10) ; liste chainee
        // (noeud+0x0 = suivant, noeud+0x10 = cle) ; resolveur(storage, cle, 0) -> Actor*.
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        using ResolveFn = void* (__fastcall*)(void* storage, uint64_t key, long long zero);
        auto resolve = reinterpret_cast<ResolveFn>(base + o.actorResolverRva);

        size_t count = 0; uintptr_t samples[3] = { 0, 0, 0 };
        __try {
            uintptr_t storage = 0, sentinel = 0;
            if (SafeReadPtr(level + o.levelActorStorage, storage) && storage &&
                SafeReadPtr(storage + 0x10, sentinel) && sentinel) {
                uintptr_t node = 0; SafeReadPtr(sentinel, node);   // premier noeud
                int guard = 0;
                while (node && node != sentinel && guard++ < 20000) {
                    uintptr_t key = 0;
                    if (SafeReadPtr(node + 0x10, key)) {
                        void* actor = resolve(reinterpret_cast<void*>(storage), key, 0);
                        if (actor) { if (count < 3) samples[count] = reinterpret_cast<uintptr_t>(actor); ++count; }
                    }
                    uintptr_t next = 0; if (!SafeReadPtr(node, next)) break;
                    node = next;
                }
            }
            LOG_INFO("EL: liste directe -> %zu entites", count);
            // Position des 3 premieres : Actor -> StateVectorComponent(+0x240) -> pos(Vec3 @0).
            for (int k = 0; k < 3 && samples[k]; ++k) {
                uintptr_t sv = 0, vt = 0;
                SafeReadPtr(samples[k], vt);
                if (SafeReadPtr(samples[k] + o.actorStateVector, sv) && sv) {
                    float x = *reinterpret_cast<float*>(sv + 0);
                    float y = *reinterpret_cast<float*>(sv + 4);
                    float z = *reinterpret_cast<float*>(sv + 8);
                    LOG_INFO("EL: actor=0x%llX vtable=0x%llX pos=(%.1f, %.1f, %.1f)",
                             (unsigned long long)samples[k], (unsigned long long)vt, x, y, z);
                } else {
                    LOG_INFO("EL: actor=0x%llX stateVector nul (actor+0x%llX)",
                             (unsigned long long)samples[k], (unsigned long long)o.actorStateVector);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG_INFO("EL: crash lecture directe/position (resolveur RVA 0x%llX a verifier)", (unsigned long long)o.actorResolverRva);
        }
    }
    bool SafeReadU8(uintptr_t addr, uint8_t& out) {
        __try { out = *reinterpret_cast<uint8_t*>(addr); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool SafeReadF32(uintptr_t addr, float& out) {
        __try { out = *reinterpret_cast<float*>(addr); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool SafeReadI32(uintptr_t addr, int& out) {
        __try { out = *reinterpret_cast<int*>(addr); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Base du module du jeu (= l'exe principal dans lequel la DLL est injectee).
    uintptr_t ModuleBase() {
        static uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!base) base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"Minecraft.Windows.exe"));
        return base;
    }

    // ---- Resolution stable du LocalPlayer via ClientInstance (methode Latite) --------
    // Offsets de la marche (v26.63, d'apres le SDK Latite ; a reverifier a chaque MAJ).
    constexpr ptrdiff_t kWinMainToPlatform   = 0x8;    // winMain -> Platform_GameCore
    constexpr ptrdiff_t kPlatformToGameCore  = 0x78;   // Platform_GameCore -> GameCore
    constexpr ptrdiff_t kGameCoreToGame      = 0x48;   // GameCore -> MinecraftGame
    constexpr ptrdiff_t kGameToClientsMap    = 0x9D0;  // MinecraftGame -> std::map<u8,ClientInterface>
    // Layout std::map MSVC : map{ _Myhead*(+0), _Mysize(+8) } ; noeud{ _Left(+0),_Parent,_Right,flags, _Myval(+0x20) }
    constexpr ptrdiff_t kNodeMyval           = 0x20;   // _Myval dans le noeud
    constexpr ptrdiff_t kPairToClientIface   = 0x8;    // ClientInterface apres la cle uint8 (aligne 8)
    constexpr ptrdiff_t kClientIfaceToInst   = 0x10;   // shared_ptr<ClientInstance>.ptr

    uintptr_t g_platformGlobal = 0;    // adresse du global winMain (resolue via signature)

    // Resout le LocalPlayer. Si diag=true, journalise chaque etape (debug).
    void* ResolveLocalPlayerCI(bool diag) {
        using ull = unsigned long long;
        if (!g_platformGlobal) return nullptr;
        uintptr_t winMain = 0, platform = 0, gameCore = 0, mcgame = 0;
        if (!SafeReadPtr(g_platformGlobal, winMain) || !winMain) { if (diag) LOG_INFO("LP: winMain nul"); return nullptr; }
        if (!SafeReadPtr(winMain + kWinMainToPlatform, platform) || !platform) { if (diag) LOG_INFO("LP: platform nul"); return nullptr; }
        if (!SafeReadPtr(platform + kPlatformToGameCore, gameCore) || !gameCore) { if (diag) LOG_INFO("LP: gameCore nul"); return nullptr; }
        if (!SafeReadPtr(gameCore + kGameCoreToGame, mcgame) || !mcgame) { if (diag) LOG_INFO("LP: mcgame nul"); return nullptr; }

        if (diag) {
            uintptr_t vW = 0, vP = 0, vG = 0, vM = 0;
            SafeReadPtr(winMain, vW); SafeReadPtr(platform, vP); SafeReadPtr(gameCore, vG); SafeReadPtr(mcgame, vM);
            LOG_INFO("LP marche: winMain=0x%llX platform=0x%llX gameCore=0x%llX mcgame=0x%llX (vt mcgame=0x%llX)",
                     (ull)winMain, (ull)platform, (ull)gameCore, (ull)mcgame, (ull)vM);
        }

        // La map std::map<uint8_t, ClientInterface> est a mcgame + kGameToClientsMap.
        // L'offset change selon la version : si celui par defaut echoue, on balaie
        // une plage de mcgame pour trouver un champ { head*, size petit } plausible.
        uintptr_t mapAddr = mcgame + kGameToClientsMap;
        uintptr_t head = 0, size = 0, leftmost = 0, clientInstance = 0;
        bool okMap = SafeReadPtr(mapAddr, head) && head && SafeReadPtr(mapAddr + 8, size) && size > 0 && size < 8;
        if (!okMap) {
            // balayage : cherche un couple (pointeur non nul, taille 1..4) aligne sur 8
            for (ptrdiff_t off = 0x800; off <= 0xC00; off += 8) {
                uintptr_t h = 0, s = 0;
                if (SafeReadPtr(mcgame + off, h) && h > 0x10000 && SafeReadPtr(mcgame + off + 8, s) && s >= 1 && s <= 4) {
                    uintptr_t lm = 0, ci = 0, vt = 0;
                    if (SafeReadPtr(h, lm) && lm &&
                        SafeReadPtr(lm + kNodeMyval + kPairToClientIface + kClientIfaceToInst, ci) && ci &&
                        SafeReadPtr(ci, vt) && vt > 0x7FF000000000ull) {   // vtable plausible (zone module)
                        mapAddr = mcgame + off; head = h; size = s; okMap = true;
                        if (diag) LOG_INFO("LP: map trouvee par balayage a mcgame+0x%llX (size=%llu)", (ull)off, (ull)s);
                        break;
                    }
                }
            }
        }
        if (!okMap) { if (diag) LOG_INFO("LP: map clients introuvable (ni 0x9D0 ni balayage)"); return nullptr; }
        if (!SafeReadPtr(head, leftmost) || !leftmost) { if (diag) LOG_INFO("LP: leftmost nul"); return nullptr; }
        const uintptr_t clientIface = leftmost + kNodeMyval + kPairToClientIface;
        if (!SafeReadPtr(clientIface + kClientIfaceToInst, clientInstance) || !clientInstance) { if (diag) LOG_INFO("LP: clientInstance nul"); return nullptr; }

        uintptr_t vt = 0, fnAddr = 0;
        if (!SafeReadPtr(clientInstance, vt) || !vt) { if (diag) LOG_INFO("LP: vtable CI nulle"); return nullptr; }
        const int idx = Config::GetOffsets().clientGetLocalPlayerIndex;
        if (!SafeReadPtr(vt + static_cast<uintptr_t>(idx) * 8, fnAddr) || !fnAddr) { if (diag) LOG_INFO("LP: getLocalPlayer[%d] nul", idx); return nullptr; }

        void* lp = nullptr;
        __try { lp = reinterpret_cast<void*(__fastcall*)(void*)>(fnAddr)(reinterpret_cast<void*>(clientInstance)); }
        __except (EXCEPTION_EXECUTE_HANDLER) { if (diag) LOG_INFO("LP: crash dans getLocalPlayer"); return nullptr; }

        if (diag) {
            uintptr_t lpVt = 0; SafeReadPtr(reinterpret_cast<uintptr_t>(lp), lpVt);
            LOG_INFO("LP OK: mcgame=0x%llX clients=%llu CI=0x%llX LocalPlayer=0x%llX vtable=0x%llX",
                     (ull)mcgame, (ull)size, (ull)clientInstance, (ull)(uintptr_t)lp, (ull)lpVt);
        }
        return lp;
    }

    // Resout une chaine module+offsets -> adresse du champ final (sans le lire).
    bool ResolveChainAddr(const PtrChain& c, uintptr_t& outAddr) {
        if (!c.set || c.offs.size() < 2) return false;
        const uintptr_t base = ModuleBase();
        if (!base) return false;
        uintptr_t p = base + c.offs[0];
        if (!SafeReadPtr(p, p) || !p) return false;                 // noeud 0 (dereference la case base)
        for (size_t i = 1; i + 1 < c.offs.size(); ++i)              // noeuds intermediaires
            if (!SafeReadPtr(p + c.offs[i], p) || !p) return false;
        outAddr = p + c.offs.back();                                // + offset du champ
        return true;
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
        const char* names[Game::SlotCount] = {
            "diamond helmet", "diamond chestplate", "diamond leggings", "diamond boots", "diamond sword"
        };
        for (int i = 0; i < Game::SlotCount; ++i) {
            auto& it = snap.items[i];
            it.present = true;
            it.maxDamage = maxes[i];
            it.count = 1;
            strncpy(it.name, names[i], sizeof(it.name) - 1);
            const double phase = 0.5 + 0.5 * std::sin(t * 0.3 + i * 1.3);
            it.damage = static_cast<int>(it.maxDamage * (1.0 - phase * 0.97));
        }
        return snap;
    }

    // Diagnostic : logge etape par etape ou la chaine casse (pour regler les offsets).
    void DiagChain(const char* name, const PtrChain& c) {
        using ull = unsigned long long;
        if (!c.set) { LOG_INFO("DIAG %s: non defini", name); return; }
        const uintptr_t base = ModuleBase();
        uintptr_t p = base + c.offs[0], v = 0;
        if (!SafeReadPtr(p, v)) {
            LOG_INFO("DIAG %s: echec base @0x%llX (module 0x%llX + 0x%llX)",
                     name, (ull)p, (ull)base, (ull)c.offs[0]);
            return;
        }
        LOG_INFO("DIAG %s: module=0x%llX node0=0x%llX", name, (ull)base, (ull)v);
        p = v;
        for (size_t i = 1; i + 1 < c.offs.size(); ++i) {
            if (!SafeReadPtr(p + c.offs[i], v)) {
                LOG_INFO("DIAG %s: echec step %zu (@0x%llX + 0x%llX)", name, i, (ull)p, (ull)c.offs[i]);
                return;
            }
            p = v;
        }
        const uintptr_t fa = p + c.offs.back();
        int iv = -999; float fv = -999.f;
        SafeReadI32(fa, iv); SafeReadF32(fa, fv);
        LOG_INFO("DIAG %s: champ @0x%llX  int=%d  float=%.2f", name, (ull)fa, iv, fv);
    }

    // La voie ECS est prete si : hook joueur installe + getDamageValue + index getMaxDamage.
    bool EcsArmorReady() {
        return g_hookTarget && g_getDamageValue && Config::GetOffsets().itemGetMaxDamageIndex >= 0;
    }

    NxItemCfg BuildItemCfg() {
        const Offsets& o = Config::GetOffsets();
        NxItemCfg c{};
        c.getDamageFn     = reinterpret_cast<void*>(g_getDamageValue);
        c.maxDmgVtIndex   = o.itemGetMaxDamageIndex;
        c.getHoverNameFn  = reinterpret_cast<void*>(g_getHoverName);
        c.itemCountOff    = o.itemCountOffset;
        c.itemPtrOff      = o.itemStackItem;
        c.itemIdStrOff    = o.itemIdStringOffset;
        c.suppliesOff     = o.playerSupplies;
        c.invOff          = o.invInventory;
        c.selSlotOff      = o.invSelectedSlot;
        c.getItemIndex    = o.inventoryGetItemIndex;
        return c;
    }

    // Appels proteges (peuvent crasher si un offset est faux).
    void* SafeArmorStack(void* lp, int slot, int idx) {
        __try { return Nexus_GetArmorStack(lp, slot, idx); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    void* SafeHeldStack(void* lp, const NxItemCfg& cfg) {
        __try { return Nexus_GetHeldStack(lp, &cfg); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    int SafeCountMatching(void* lp, const NxItemCfg& cfg, void* stack) {
        __try { return Nexus_CountMatching(lp, &cfg, stack); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    void FillFromStack(void* stack, const NxItemCfg& cfg, Game::ItemInfo& it) {
        // 1) Durabilite + quantite (robuste).
        NxItemOut o{}; bool ok = false;
        __try { ok = Nexus_ReadItemCore(stack, &cfg, &o); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        if (!ok || !o.present) return;
        it.present = true; it.damage = o.damage; it.maxDamage = o.maxDamage; it.count = o.count;

        // 2) Nom (isole : s'il plante, on garde l'item sans nom).
        char nm[48] = "";
        __try { Nexus_ReadItemName(stack, &cfg, nm, static_cast<int>(sizeof(nm))); }
        __except (EXCEPTION_EXECUTE_HANDLER) { nm[0] = '\0'; }
        std::strncpy(it.name, nm, sizeof(it.name) - 1);
        it.name[sizeof(it.name) - 1] = '\0';
    }

    // Lecture via LocalPlayer capture + composant ECS : armure (nom+durabilite) + objet en main.
    Game::Snapshot ReadViaEcs() {
        Game::Snapshot snap;
        void* lp = ResolvePlayer();
        if (!lp) return snap;
        snap.inGame = true;

        const NxItemCfg cfg = BuildItemCfg();
        for (int slot = 0; slot < 4; ++slot)
            FillFromStack(SafeArmorStack(lp, slot, cfg.getItemIndex), cfg, snap.items[slot]);
        void* held = SafeHeldStack(lp, cfg);
        FillFromStack(held, cfg, snap.items[Game::Held]);
        // Objet en main : on compte TOUS les exemplaires du meme item dans l'inventaire
        // (main incluse), pour afficher le total meme s'il n'y en a qu'un.
        if (snap.items[Game::Held].present) {
            const int total = SafeCountMatching(lp, cfg, held);
            if (total > 0) snap.items[Game::Held].count = total;
        }

        static bool logged = false;
        if (!logged && snap.items[Game::Helmet].present) {
            logged = true;
            LOG_INFO("Armure ECS lue : '%s' %d/%d",
                     snap.items[Game::Helmet].name,
                     snap.items[Game::Helmet].maxDamage - snap.items[Game::Helmet].damage,
                     snap.items[Game::Helmet].maxDamage);
        }
        return snap;
    }

    // Lecture directe par chaines de pointeurs (build ECS sans hook/RTTI).
    Game::Snapshot ReadViaChains() {
        const Offsets& o = Config::GetOffsets();
        Game::Snapshot snap;

        // Diagnostic limite dans le temps (12 lignes max, une par seconde).
        {
            static uint64_t lastDiag = 0; static int diagCount = 0;
            const uint64_t now = GetTickCount64();
            if (diagCount < 12 && now - lastDiag > 1000) {
                lastDiag = now; ++diagCount;
                DiagChain("casque", o.armorChain[0]);
                if (o.healthChain.set) DiagChain("vie", o.healthChain);
            }
        }

        // --- Vie du joueur local (float) ---
        uintptr_t a = 0; float hp = 0.f;
        if (o.healthChain.set && ResolveChainAddr(o.healthChain, a) &&
            SafeReadF32(a, hp) && hp >= 0.f && hp <= 1024.f) {
            snap.health = hp; snap.healthValid = true; snap.inGame = true;
            uintptr_t ma = 0; float mh = 0.f;
            if (o.maxHealthChain.set && ResolveChainAddr(o.maxHealthChain, ma) &&
                SafeReadF32(ma, mh) && mh > 0.f && mh <= 1024.f)
                snap.maxHealth = mh;
        }

        // --- Durabilite d'armure (damage, int) ---
        for (int i = 0; i < 4; ++i) {
            if (!o.armorChain[i].set) continue;
            uintptr_t da = 0; int dmg = 0;
            if (ResolveChainAddr(o.armorChain[i], da) && SafeReadI32(da, dmg)) {
                const int mx = o.armorMax[i];
                if (mx > 0 && dmg >= 0 && dmg <= mx) {   // valeur plausible => piece equipee
                    auto& it = snap.items[i];
                    it.present = true; it.maxDamage = mx; it.damage = dmg;
                    snap.inGame = true;
                }
            }
        }

        // Journalise une fois ce qu'on a reussi a lire (aide au diagnostic).
        static bool logged = false;
        if (!logged && snap.inGame) {
            logged = true;
            LOG_INFO("Chaines lues : base=0x%llX vie=%.1f casque=%d/%d",
                     (unsigned long long)ModuleBase(),
                     snap.healthValid ? snap.health : -1.f,
                     snap.items[Game::Helmet].present ? snap.items[Game::Helmet].damage : -1,
                     snap.items[Game::Helmet].maxDamage);
        }
        return snap;
    }
}

bool Game::Init() {
    const Offsets& o = Config::GetOffsets();

    // Resolution stable du LocalPlayer (methode ClientInstance) : signature du global racine.
    if (!o.platformSig.empty()) {
        g_platformGlobal = SigScan::Find(o.platformSig, "Platform_GameCore(global)");
        LOG_INFO(g_platformGlobal ? "Racine ClientInstance : global trouve (0x%llX)"
                                  : "Racine ClientInstance : signature introuvable",
                 (unsigned long long)g_platformGlobal);
    }

    if (o.anyChain()) {
        LOG_INFO("Lecture par chaine de pointeurs activee (vie:%s armure:%s)",
                 o.healthChain.set ? "oui" : "non", o.anyArmorChain() ? "oui" : "non");
        return true;   // pas besoin du hook/signatures sur ce build
    }

    g_hookTarget      = SigScan::Find(o.playerHookSig, "player.hook_signature");
    g_getArmor        = reinterpret_cast<GetArmorFn>(SigScan::Find(o.getArmorSig, "Actor_getArmor"));
    g_getSelectedItem = reinterpret_cast<GetSelectedItemFn>(SigScan::Find(o.getSelectedItemSig, "Player_getSelectedItem"));
    g_getDamageValue  = reinterpret_cast<GetDamageValueFn>(SigScan::Find(o.getDamageValueSig, "ItemStackBase_getDamageValue"));
    g_getHoverName    = SigScan::Find(o.getHoverNameSig, "ItemStackBase_getHoverName");

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

namespace {
    // Marche ClientInstance -> Minecraft -> GameSession -> Level (0 si indisponible).
    uintptr_t ResolveLevelPtr() {
        const Offsets& o = Config::GetOffsets();
        uintptr_t ci = reinterpret_cast<uintptr_t>(ItemRender::ClientInstance());
        if (!ci) return 0;
        uintptr_t mc = 0, gs = 0, level = 0;
        if (!SafeReadPtr(ci + o.ciMinecraft, mc) || !mc) return 0;
        if (!SafeReadPtr(mc + o.mcGameSession, gs) || !gs) return 0;
        if (!SafeReadPtr(gs + o.gsLevel, level) || !level) return 0;
        return level;
    }
    bool ReadActorPos(uintptr_t actor, float& x, float& y, float& z) {
        const Offsets& o = Config::GetOffsets();
        // 1) Position physique (StateVector) : la reference, toujours valide.
        uintptr_t sv = 0;
        if (!SafeReadPtr(actor + o.actorStateVector, sv) || !sv) return false;
        float sx, sy, sz;
        __try { sx = *reinterpret_cast<float*>(sv + 0); sy = *reinterpret_cast<float*>(sv + 4); sz = *reinterpret_cast<float*>(sv + 8); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

        // 2) Position de RENDU interpolee (Actor+0x5B0) : synchro avec la frame -> anti-saccade.
        //    On ne l'utilise que si elle est finie et proche de la position physique (securite).
        if (o.actorRenderPos > 0) {
            __try {
                const float rx = *reinterpret_cast<float*>(actor + o.actorRenderPos + 0);
                const float ry = *reinterpret_cast<float*>(actor + o.actorRenderPos + 4);
                const float rz = *reinterpret_cast<float*>(actor + o.actorRenderPos + 8);
                if (std::isfinite(rx) && std::isfinite(ry) && std::isfinite(rz) &&
                    std::fabs(rx - sx) < 8.f && std::fabs(ry - sy) < 8.f && std::fabs(rz - sz) < 8.f) {
                    x = rx; y = ry; z = rz; return true;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
        x = sx; y = sy; z = sz; return true;
    }

    // Decalage vertical entre l'origine de l'entite et le SOMMET de sa hitbox.
    // AABB a Actor+actorAABB : min Vec3 (+0x0), max Vec3 (+0xC). On renvoie
    // max.y - pos_physique.y (meme repere que la position), donc ajoutable a la
    // position de rendu. Repli sur 'fallback' si la boite parait invalide.
    float ReadTopOffset(uintptr_t actor, float fallback) {
        const Offsets& o = Config::GetOffsets();
        if (o.actorAABB <= 0) return fallback;
        uintptr_t sv = 0, bb = 0;
        if (!SafeReadPtr(actor + o.actorStateVector, sv) || !sv) return fallback;
        if (!SafeReadPtr(actor + o.actorAABB, bb) || !bb) return fallback;
        __try {
            const float posY = reinterpret_cast<float*>(sv + 0x4)[0];    // pos physique .y
            const float minY = reinterpret_cast<float*>(bb + 0x4)[0];    // min.y
            const float maxY = reinterpret_cast<float*>(bb + 0x10)[0];   // max.y (+0xC +0x4)
            const float hitH = maxY - minY;
            if (std::isfinite(hitH) && hitH > 0.1f && hitH < 6.f) {
                const float top = maxY - posY;                          // origine -> sommet
                if (std::isfinite(top) && top > -1.f && top < 6.f) return top;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        return fallback;
    }
}

int Game::CollectEntities(EntityInfo* out, int maxCount) {
    if (!out || maxCount <= 0) return 0;
    const Offsets& o = Config::GetOffsets();
    void* lp = ResolvePlayer();
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    using ResolveFn = void* (__fastcall*)(void* storage, uint64_t key, long long zero);
    auto resolve = reinterpret_cast<ResolveFn>(base + o.actorResolverRva);
    int n = 0;
    __try {
        uintptr_t level = ResolveLevelPtr();
        if (!level) return 0;
        uintptr_t storage = 0, sentinel = 0;
        if (!SafeReadPtr(level + o.levelActorStorage, storage) || !storage) return 0;
        if (!SafeReadPtr(storage + 0x10, sentinel) || !sentinel) return 0;
        uintptr_t node = 0; SafeReadPtr(sentinel, node);
        int guard = 0;
        while (node && node != sentinel && guard++ < 20000 && n < maxCount) {
            uintptr_t key = 0;
            if (SafeReadPtr(node + 0x10, key)) {
                void* actor = resolve(reinterpret_cast<void*>(storage), key, 0);
                float x, y, z;
                if (actor && ReadActorPos(reinterpret_cast<uintptr_t>(actor), x, y, z)) {
                    EntityInfo& e = out[n++];
                    e.actor = actor; e.x = x; e.y = y; e.z = z;
                    e.height = ReadTopOffset(reinterpret_cast<uintptr_t>(actor), 1.9f);
                    e.isPlayer = (actor == lp);
                    // La vie (ECS, code C++) est lue APRES, hors de ce bloc __try :
                    // melanger SEH et deroulement d'objets C++ ici casse l'execution.
                    e.hasHealth = false; e.health = 0.f; e.maxHealth = 0.f;
                }
            }
            uintptr_t next = 0; if (!SafeReadPtr(node, next)) break;
            node = next;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return n;
}

static bool HpIsHeap(uintptr_t p) { return p >= 0x10000 && p < 0x7FFFFFFFFFFF && !(p & 0x3); }

// Signature d'un AttributeInstance de vie : [max, max, min=0, max, courante].
// Exiger min==0 et trois 'max' egaux rend la detection fiable (peu de faux positifs).
static bool HpFind(uintptr_t base, float& cur, float& mx) {
    const float* f = reinterpret_cast<const float*>(base);
    for (int k = 0; k + 5 < 0x100 / 4; ++k) {
        const float m = f[k];
        if (m >= 1.f && m <= 2048.f && f[k + 1] == m && f[k + 2] == 0.f && f[k + 3] == m) {
            const float c = f[k + 4];
            if (c >= 0.f && c <= m + 0.5f) { cur = c > m ? m : c; mx = m; return true; }
        }
    }
    return false;
}

// Teste le cluster de vie EXACTEMENT a l'offset 'off' d'un bloc.
static bool HpAt(uintptr_t base, int off, float& cur, float& mx) {
    const float* f = reinterpret_cast<const float*>(base + off);
    const float m = f[0];
    if (m >= 1.f && m <= 2048.f && f[1] == m && f[2] == 0.f && f[3] == m && f[4] >= 0.f && f[4] <= m + 0.5f) {
        mx = m; cur = f[4] > m ? m : f[4]; return true;
    }
    return false;
}

// Lectures protegees INDIVIDUELLEMENT : un mauvais pointeur fait sauter CE bloc,
// pas tout le scan (sinon on n'atteint jamais le bloc de vie plus loin).
static bool HpRead(uintptr_t addr, uintptr_t& out) {
    __try { out = *reinterpret_cast<uintptr_t*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool HpAtSafe(uintptr_t base, int off, float& c, float& m) {
    __try { return HpAt(base, off, c, m); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool HpFindSafe(uintptr_t base, float& c, float& m) {
    __try { return HpFind(base, c, m); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Dans le composant de vie : l'instance de VIE a son cluster a l'offset 0x6C
// (les autres attributs sont ailleurs, ex 0x84). On cible 0x6C en priorite,
// avec repli sur le premier cluster trouve si 0x6C est absent.
static bool HpScanComp(uintptr_t c, float& cur, float& mx) {
    bool any = false; float aCur = 0.f, aMx = 0.f;
    for (int o = 0; o < 0x40; o += 8) {
        uintptr_t p1;
        if (!HpRead(c + o, p1) || !HpIsHeap(p1)) continue;
        if (HpAtSafe(p1, 0x6C, cur, mx)) return true;             // vie (priorite)
        for (int o2 = 0; o2 < 0x40; o2 += 8) {
            uintptr_t p2;
            if (!HpRead(p1 + o2, p2) || !HpIsHeap(p2)) continue;
            if (HpAtSafe(p2, 0x6C, cur, mx)) return true;
        }
        if (!any && HpFindSafe(p1, aCur, aMx)) any = true;        // repli : 1er cluster
    }
    if (any) { cur = aCur; mx = aMx; return true; }
    return false;
}

bool Game::ReadHealth(void* actor, float& cur, float& mx) {
    static unsigned int hashes[400];
    static void* ptrs[400];
    int ncomp = 0;
    __try { ncomp = Nexus_ListComponents(actor, hashes, ptrs, 400); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    // Uniquement le composant de vie (0xFD3B0613), atteint via la table EnTT fiable.
    for (int i = 0; i < ncomp; ++i) {
        if (hashes[i] != 0xFD3B0613u) continue;
        const uintptr_t c = reinterpret_cast<uintptr_t>(ptrs[i]);
        if (HpIsHeap(c) && HpScanComp(c, cur, mx)) return true;
    }
    return false;
}

bool Game::ReadEntityPos(void* actor, float& x, float& y, float& z) {
    if (!actor) return false;
    return ReadActorPos(reinterpret_cast<uintptr_t>(actor), x, y, z);
}

// Reach entre deux acteurs : distance entre l'oeil de 'from' (pieds + 1.62) et le point le
// plus proche de 'to' en hauteur (independant de la camera, donc correct en 3e personne).
float Game::ReachBetween(void* from, void* to) {
    if (!from || !to) return -1.f;
    float fx, fy, fz, tx, ty, tz;
    if (!ReadActorPos(reinterpret_cast<uintptr_t>(from), fx, fy, fz)) return -1.f;
    if (!ReadActorPos(reinterpret_cast<uintptr_t>(to), tx, ty, tz)) return -1.f;
    const float eyeY = fy + 1.62f;                                 // hauteur des yeux de l'attaquant
    const float cy = eyeY < ty ? ty : (eyeY > ty + 1.8f ? ty + 1.8f : eyeY);  // point le plus proche (hitbox ~1.8)
    const float dx = fx - tx, dy = eyeY - cy, dz = fz - tz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float Game::AttackReach(void* target) { return ReachBetween(LocalPlayer(), target); }

// Diagnostic : trouve le MoveInputComponent et dumpe ses 1ers octets.
void Game::DiagSprint() {
    void* lp = LocalPlayer();
    if (!lp) { LOG_INFO("SPRINT: joueur local indispo"); return; }
    static unsigned int hashes[400];
    static void* ptrs[400];
    int nc = 0;
    __try { nc = Nexus_ListComponents(lp, hashes, ptrs, 400); }
    __except (EXCEPTION_EXECUTE_HANDLER) { LOG_INFO("SPRINT: enum crash"); return; }
    void* comp = nullptr;
    for (int i = 0; i < nc; ++i) if (hashes[i] == 0x018B1887u) { comp = ptrs[i]; break; }
    if (!comp) { LOG_INFO("SPRINT: composant 0x018B1887 ABSENT (%d composants)", nc); return; }
    __try {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(comp);
        LOG_INFO("SPRINT comp=%p : %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
            comp, b[0],b[1],b[2],b[3], b[4],b[5],b[6],b[7], b[8],b[9],b[10],b[11], b[12],b[13],b[14],b[15]);
        LOG_INFO("SPRINT comp+0x10 : %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
            b[16],b[17],b[18],b[19], b[20],b[21],b[22],b[23], b[24],b[25],b[26],b[27], b[28],b[29],b[30],b[31]);
    } __except (EXCEPTION_EXECUTE_HANDLER) { LOG_INFO("SPRINT: lecture crash"); }
}

// Auto-sprint : on force le bit "sprintDown" de rawInputState (et inputState) du
// MoveInputComponent (hash 0x018B1887). Pour le jeu, c'est comme tenir la touche sprint.
bool Game::SetMoveSprint(bool on) {
    void* lp = LocalPlayer();
    if (!lp) return false;
    static unsigned int hashes[400];
    static void* ptrs[400];
    int nc = 0;
    __try { nc = Nexus_ListComponents(lp, hashes, ptrs, 400); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    ptrdiff_t raw = Config::GetOffsets().sprintOffset;         // octet de rawInputState.sprintDown
    if (raw < 0) raw = 0x11;                                   // defaut ECS (offsets.ini peut avoir -1)
    for (int i = 0; i < nc; ++i) {
        if (hashes[i] != 0x018B1887u || !ptrs[i]) continue;
        __try {
            uint8_t* b = reinterpret_cast<uint8_t*>(ptrs[i]);
            const uint8_t m = 0x01;                            // sprintDown = bit 0 de cet octet
            if (on) b[raw] |= m;                               // on ecrit UNIQUEMENT sprintDown
            else    b[raw] &= ~m;                              // (l'octet 0x01 = input "avancer")
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    return false;
}

// BlockSource de la dimension via ClientInstance::getRegion() (vtable, d'apres Latite).
static void* GetBlockSource() {
    uintptr_t ci = reinterpret_cast<uintptr_t>(ItemRender::ClientInstance());
    if (!ci) return nullptr;
    const Offsets& o = Config::GetOffsets();
    __try {
        void** vt = *reinterpret_cast<void***>(ci);
        return reinterpret_cast<void* (__fastcall*)(void*)>(vt[o.clientRegionVtIdx])(reinterpret_cast<void*>(ci));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// getBlock(BlockPos const&) -> Block*. L'air est un bloc singleton (meme pointeur partout).
static bool MemReadable(const void* p, size_t n);   // decl anticipee
static void* BsGetBlock(void* bs, int idx, int x, int y, int z) {
    struct BP { int x, y, z; } p{ x, y, z };
    // Validation COUTEUSE (VirtualQuery) faite une seule fois par bs, pas par cellule.
    static void* s_bs = nullptr; static void* s_fn = nullptr;
    if (bs != s_bs) {
        s_bs = bs; s_fn = nullptr;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (MemReadable(bs, 8)) {
            void** vt = *reinterpret_cast<void***>(bs);
            if (MemReadable(vt + idx, 8)) {
                const uintptr_t fa = reinterpret_cast<uintptr_t>(vt[idx]);
                if (fa >= base && fa <= base + 0x10000000ull) s_fn = vt[idx];
            }
        }
    }
    if (!s_fn) return nullptr;
    __try {
        return reinterpret_cast<void* (__fastcall*)(void*, const BP*)>(s_fn)(bs, &p);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Verifie qu'une plage memoire est reellement lisible (page committee, non garde).
// Evite toute faute d'acces SANS dependre de SEH (qui peut etre optimise en /EHsc).
static bool MemReadable(const void* p, size_t n) {
    if (!p || n == 0) return false;
    const char* cur = reinterpret_cast<const char*>(p);
    const char* end = cur + n;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(cur, &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        const DWORD prot = mbi.Protect & 0xFF;
        const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(prot & readable)) return false;
        cur = reinterpret_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;  // page suivante
    }
    return true;
}

// Ligne de vue : marche de l'oeil vers la cible, false si un bloc (non-air) est sur le trajet.
// Version simple et stable : compare au bloc d'air singleton (pas de lecture de nom).
// Les plantes/feuillages comptent comme "plein" ici (compromis accepte : pas de crash, leger).
bool Game::HasLineOfSight(float ex, float ey, float ez, float tx, float ty, float tz) {
    void* bs = GetBlockSource();
    if (!bs) return true;                              // pas de source -> ne pas masquer
    const Offsets& o = Config::GetOffsets();
    const int gb = o.bsGetBlockVtIdx;

    // Bloc d'air de reference (singleton), capture tant qu'on reste dans le meme monde.
    static void* s_air = nullptr; static void* s_airBs = nullptr;
    if (s_airBs != bs) {                               // nouveau monde -> recapture
        s_air = BsGetBlock(bs, gb, static_cast<int>(ex), 330, static_cast<int>(ez));
        s_airBs = bs;
    }
    if (!s_air) return true;                           // air non capture -> ne pas masquer

    const float dx = tx - ex, dy = ty - ey, dz = tz - ez;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 0.1f) return true;
    // Pas de 0.5 bloc (plus leger qu'avant) ; un mur fait >=1 bloc donc rien n'est manque.
    const int steps = static_cast<int>(dist / 0.5f) + 1;
    int lx = 0x7fffffff, ly = 0x7fffffff, lz = 0x7fffffff;
    for (int s = 1; s < steps; ++s) {                  // saute l'oeil et le dernier point (l'entite)
        const float t = static_cast<float>(s) / steps;
        const int bx = static_cast<int>(std::floor(ex + dx * t));
        const int by = static_cast<int>(std::floor(ey + dy * t));
        const int bz = static_cast<int>(std::floor(ez + dz * t));
        if (bx == lx && by == ly && bz == lz) continue;
        lx = bx; ly = by; lz = bz;
        void* blk = BsGetBlock(bs, gb, bx, by, bz);
        if (blk && blk != s_air) return false;         // bloc non-air -> vue bloquee
    }
    return true;
}

// Lit un std::string MSVC (SSO ou heap) a base+off, sans faute (VirtualQuery).
// N'accepte que de l'ASCII imprimable (filtre le binaire). "" si invalide.
static bool SafeReadStdString(void* base, ptrdiff_t off, char* out, int outN) {
    if (out && outN) out[0] = 0;
    if (!base || !out || outN < 2) return false;
    char* so = reinterpret_cast<char*>(base) + off;
    if (!MemReadable(so, 0x20)) return false;
    const size_t size = *reinterpret_cast<size_t*>(so + 0x10);
    const size_t cap  = *reinterpret_cast<size_t*>(so + 0x18);
    if (size == 0 || size > 256) return false;
    const char* data = (cap <= 15) ? so : *reinterpret_cast<const char**>(so);
    int n = static_cast<int>(size) < (outN - 1) ? static_cast<int>(size) : (outN - 1);
    if (!MemReadable(data, n)) return false;
    for (int i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        if (c < 0x20) return false;                    // caractere de controle -> pas du texte
        out[i] = static_cast<char>(c);                 // accepte ASCII + UTF-8 (accents, codes couleur)
    }
    out[n] = 0;
    return true;
}

// Retire les codes de formatage Minecraft (§ = UTF-8 "C2 A7" ou simple "A7", + 1 car).
static void StripFormatCodes(const char* raw, char* buf, int bufN) {
    int o = 0;
    for (int i = 0; raw[i] && o < bufN - 1; ) {
        const unsigned char c0 = static_cast<unsigned char>(raw[i]);
        const unsigned char c1 = static_cast<unsigned char>(raw[i + 1]);
        if (c0 == 0xC2 && c1 == 0xA7 && raw[i + 2]) { i += 3; continue; }  // §x (UTF-8)
        if (c0 == 0xA7 && raw[i + 1]) { i += 2; continue; }               // §x (Latin-1)
        buf[o++] = raw[i++];
    }
    while (o > 0 && buf[o - 1] == ' ') --o;            // trim espaces de fin
    buf[o] = 0;
}

// Trouve l'offset du nom dans l'acteur : 1ere chaine "nom" (longueur 3-48,
// ne commencant pas par "minecraft" qui designe les ID/persona techniques).
static ptrdiff_t FindNameOffset(void* actor) {
    // Optimisation : on valide TOUT le bloc de l'acteur en un seul VirtualQuery,
    // puis on inspecte les en-tetes std::string directement (pas de syscall par offset).
    if (!MemReadable(actor, 0x2040)) return -1;
    char* base = reinterpret_cast<char*>(actor);
    __try {
        for (ptrdiff_t off = 0x8; off <= 0x2000; off += 0x8) {
            char* so = base + off;
            const size_t size = *reinterpret_cast<size_t*>(so + 0x10);  // _Mysize
            const size_t cap  = *reinterpret_cast<size_t*>(so + 0x18);  // _Myres
            if (size < 3 || size > 48) continue;                        // pas une longueur de nom
            const char* data;
            if (cap <= 15) data = so;                                   // SSO (dans le bloc valide)
            else {                                                      // heap -> valider le pointeur
                data = *reinterpret_cast<const char**>(so);
                if (!MemReadable(data, static_cast<int>(size))) continue;
            }
            bool printable = true;
            for (size_t k = 0; k < size; ++k)
                if (static_cast<unsigned char>(data[k]) < 0x20) { printable = false; break; }
            if (!printable) continue;
            if (size >= 9 && std::memcmp(data, "minecraft", 9) == 0) continue;  // id / persona
            return off;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return -1;
}

bool Game::ReadEntityName(void* actor, char* buf, int bufN) {
    if (buf && bufN) buf[0] = 0;
    if (!actor || bufN < 2) return false;

    // Cache par acteur : l'offset du nom est cherche une fois, puis reutilise.
    struct NameCache { void* actor; ptrdiff_t off; ULONGLONG t; };
    static NameCache cache[256] = {};
    const ULONGLONG now = GetTickCount64();
    const int slot = static_cast<int>((reinterpret_cast<uintptr_t>(actor) >> 5) & 255);

    ptrdiff_t off;
    if (cache[slot].actor == actor && now - cache[slot].t < 3000) {
        off = cache[slot].off;                          // offset connu (ou -1 = pas de nom)
    } else {
        off = FindNameOffset(actor);                    // (re)cherche, max 1x / 3 s / acteur
        cache[slot] = { actor, off, now };
    }
    if (off <= 0) return false;

    char raw[96];
    bool ok = false;
    __try { ok = SafeReadStdString(actor, off, raw, sizeof(raw)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (!ok) {
        cache[slot].actor = nullptr;                    // offset perime -> rescan prochaine fois
        return false;
    }
    StripFormatCodes(raw, buf, bufN);
    return buf[0] != 0;
}

// Type d'entite via le composant d'identite ECS (hash 0xDEB6534F) :
// identifiant a +0x20 (ex. "skeleton") -> embelli en "Skeleton".
bool Game::ReadEntityType(void* actor, char* buf, int bufN) {
    if (buf && bufN) buf[0] = 0;
    if (!actor || bufN < 2) return false;
    static unsigned int hashes[400];
    static void* ptrs[400];
    int nc = 0;
    __try { nc = Nexus_ListComponents(actor, hashes, ptrs, 400); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    for (int i = 0; i < nc; ++i) {
        if (hashes[i] != 0xDEB6534Fu || !ptrs[i]) continue;
        char raw[48];
        if (!SafeReadStdString(ptrs[i], 0x20, raw, sizeof(raw))) return false;  // type a +0x20

        // Nom anglais embelli ("cave_spider" -> "Cave spider"). La localisation
        // sera geree par le futur systeme de langue du client.
        int o = 0;
        for (int k = 0; raw[k] && o < bufN - 1; ++k) {
            char c = raw[k];
            if (c == '_') c = ' ';
            if (o == 0 && c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
            buf[o++] = c;
        }
        buf[o] = 0;
        return buf[0] != 0;
    }
    return false;
}

// Scan de calibrage : log toutes les chaines imprimables trouvees dans l'Actor.
// Chaque offset est isole dans son propre __try (en plus du VirtualQuery) : robuste.
void Game::DiagEntityStrings(void* actor) {
    if (!actor) { LOG_INFO("NAMESCAN: actor nul"); return; }
    int found = 0;
    for (ptrdiff_t off = 0x8; off <= 0x2000; off += 0x8) {
        char tmp[64];
        bool ok = false;
        __try { ok = SafeReadStdString(actor, off, tmp, sizeof(tmp)); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        if (ok && std::strlen(tmp) >= 2) {
            LOG_INFO("NAMESCAN +0x%03X : \"%s\"", static_cast<unsigned>(off), tmp);
            ++found;
        }
    }
    if (!found) LOG_INFO("NAMESCAN: aucune chaine trouvee dans l'actor %p", actor);
}

void Game::DiagLocalPlayerStrings() {
    void* lp = ResolvePlayer();
    if (!lp) { LOG_INFO("NAMESCAN: joueur local indispo"); return; }
    LOG_INFO("NAMESCAN: scan joueur local %p (cherche ton pseudo)", lp);
    DiagEntityStrings(lp);
}

// Enumere les composants ECS de l'entite et cherche une chaine (nom) dans chacun.
void Game::DiagEntityComponents(void* actor) {
    if (!actor) { LOG_INFO("COMPSCAN: actor nul"); return; }
    static unsigned int hashes[400];
    static void* ptrs[400];
    int ncomp = 0;
    __try { ncomp = Nexus_ListComponents(actor, hashes, ptrs, 400); }
    __except (EXCEPTION_EXECUTE_HANDLER) { LOG_INFO("COMPSCAN: enum crash"); return; }
    LOG_INFO("COMPSCAN: %d composants pour %p", ncomp, actor);
    for (int i = 0; i < ncomp; ++i) {
        void* c = ptrs[i];
        if (!c) continue;
        for (ptrdiff_t off = 0; off <= 0x400; off += 0x8) {
            char tmp[64];
            bool ok = false;
            __try { ok = SafeReadStdString(c, off, tmp, sizeof(tmp)); }
            __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
            if (!ok || std::strlen(tmp) < 3) continue;
            if (std::strncmp(tmp, "minecraft", 9) == 0) continue;   // ignore ID techniques
            LOG_INFO("COMPSCAN hash=%08X +0x%03X : \"%s\"", hashes[i], static_cast<unsigned>(off), tmp);
        }
    }
}

// Getter sur : lecture + ret dans les 1ers octets, SANS call/jmp (aucun effet de bord).
static bool IsSafeGetter(const uint8_t* fn) {
    for (int k = 0; k < 28; ++k) {
        const uint8_t b = fn[k];
        if (b == 0xC3) return true;
        if (b == 0xE8 || b == 0xE9 || b == 0xEB || b == 0xFF) return false;
    }
    return false;
}

// Appelle un flag booleen (virtuelle, getter sur) de l'entite. def si impossible.
static bool CallFlag(void* actor, int idx, bool def) {
    if (idx < 0) return def;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    __try {
        void** vt = *reinterpret_cast<void***>(actor);
        const uint8_t* fn = reinterpret_cast<const uint8_t*>(vt[idx]);
        const uintptr_t a = reinterpret_cast<uintptr_t>(fn);
        if (!fn || a < base || a > base + 0x10000000ull || !IsSafeGetter(fn)) return def;
        return reinterpret_cast<uint32_t (__fastcall*)(void*)>(fn)(actor) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return def; }
}

// Categorie : 0=autre, 1=joueur, 2=animal, 3=monstre (via flags booleens du jeu).
int Game::EntityCategory(void* actor) {
    if (!actor) return 0;
    const Offsets& o = Config::GetOffsets();
    if (CallFlag(actor, o.vtIsPlayer, false))  return 1;   // joueur
    if (CallFlag(actor, o.vtIsMonster, false)) return 3;   // monstre hostile
    if (CallFlag(actor, o.vtIsMob, false))     return 2;   // autre creature = animal/passif
    return 0;                                              // autre (sans drapeau)
}

// Getter sur : lecture + ret dans les 1ers octets, SANS call/jmp (aucun effet de bord).
bool Game::LocalPlayerCamera(float& eyeX, float& eyeY, float& eyeZ, float& pitchDeg, float& yawDeg) {
    void* lp = ResolvePlayer();
    if (!lp) return false;
    const Offsets& o = Config::GetOffsets();

    // Oeil = position de caméra interpolée : lerp(posOld, pos, t).
    // On reconstruit le partial tick 't' par timing a partir de la pos physique.
    uintptr_t sv = 0;
    if (!SafeReadPtr(reinterpret_cast<uintptr_t>(lp) + o.actorStateVector, sv) || !sv) return false;
    float pos[3], posOld[3];
    __try {
        pos[0]    = reinterpret_cast<float*>(sv + 0x0)[0]; pos[1]    = reinterpret_cast<float*>(sv + 0x0)[1]; pos[2]    = reinterpret_cast<float*>(sv + 0x0)[2];
        posOld[0] = reinterpret_cast<float*>(sv + 0xC)[0]; posOld[1] = reinterpret_cast<float*>(sv + 0xC)[1]; posOld[2] = reinterpret_cast<float*>(sv + 0xC)[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

    static float   s_lastPos[3] = { 0, 0, 0 };
    static ULONGLONG s_tickStart = 0, s_tickDur = 50;
    const ULONGLONG now = GetTickCount64();
    const bool changed = std::fabs(pos[0] - s_lastPos[0]) > 1e-4f || std::fabs(pos[1] - s_lastPos[1]) > 1e-4f || std::fabs(pos[2] - s_lastPos[2]) > 1e-4f;
    if (changed) {
        const ULONGLONG d = now - s_tickStart;
        if (d >= 8 && d <= 120) s_tickDur = (s_tickDur * 3 + d) / 4;   // duree de tick lissee (~50 ms)
        s_tickStart = now;
        s_lastPos[0] = pos[0]; s_lastPos[1] = pos[1]; s_lastPos[2] = pos[2];
    }
    float t = s_tickDur ? static_cast<float>(now - s_tickStart) / static_cast<float>(s_tickDur) : 1.f;
    if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
    eyeX = posOld[0] + (pos[0] - posOld[0]) * t;
    eyeY = posOld[1] + (pos[1] - posOld[1]) * t;
    eyeZ = posOld[2] + (pos[2] - posOld[2]) * t;

    pitchDeg = 0.f; yawDeg = 0.f;
    uintptr_t rot = 0;
    if (SafeReadPtr(reinterpret_cast<uintptr_t>(lp) + o.actorRotation, rot) && rot) {
        __try { pitchDeg = *reinterpret_cast<float*>(rot + 0); yawDeg = *reinterpret_cast<float*>(rot + 4); }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    return true;
}

bool Game::GetRawMatrices(float view[16], float proj[16]) {
    const Offsets& o = Config::GetOffsets();
    uintptr_t cinst = reinterpret_cast<uintptr_t>(ItemRender::ClientInstance());
    if (!cinst) return false;
    uintptr_t mcg = 0, gr = 0;
    if (!SafeReadPtr(cinst + o.ciMinecraftGame, mcg) || !mcg) return false;
    if (!SafeReadPtr(mcg + o.mcGameRenderer, gr) || !gr) return false;
    __try {
        for (int i = 0; i < 16; ++i) view[i] = reinterpret_cast<const float*>(gr + o.grViewMatrix)[i];
        for (int i = 0; i < 16; ++i) proj[i] = reinterpret_cast<const float*>(gr + o.grProjMatrix)[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Game::GetViewProjMatrix(float vp[16]) {
    const Offsets& o = Config::GetOffsets();
    uintptr_t cinst = reinterpret_cast<uintptr_t>(ItemRender::ClientInstance());
    if (!cinst) return false;
    uintptr_t mcg = 0, gr = 0;
    if (!SafeReadPtr(cinst + o.ciMinecraftGame, mcg) || !mcg) return false;   // MinecraftGame
    if (!SafeReadPtr(mcg + o.mcGameRenderer, gr) || !gr) return false;        // GameRenderer
    __try {
        const float* view = reinterpret_cast<const float*>(gr + o.grViewMatrix);
        const float* proj = reinterpret_cast<const float*>(gr + o.grProjMatrix);
        // Convention C (validee) : column-major, clip = view * (proj * P).
        // vp[c*4+r] tel que clip[r] = sum_c vp[c*4+r] * P[c].
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) {
                float s = 0.f;
                for (int k = 0; k < 4; ++k) s += view[k * 4 + r] * proj[c * 4 + k];
                vp[c * 4 + r] = s;
            }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool NearF(float a, float b) { return std::fabs(a - b) < 3.0f; }
static bool NearD(double a, double b) { return std::fabs(a - b) < 3.0; }

// Scanne [base, base+len) pour 3 floats ET 3 doubles consecutifs proches de l'oeil.
static void ScanBlock(uintptr_t base, int len, float ex, float ey, float ez, const char* tag) {
    __try {
        const float* f = reinterpret_cast<const float*>(base);
        for (int off = 0; off + 12 <= len; off += 4)
            if (NearF(f[off/4], ex) && NearF(f[off/4+1], ey) && NearF(f[off/4+2], ez))
                LOG_INFO("DIAG %s FLT +0x%X = (%.3f,%.3f,%.3f)", tag, off, f[off/4], f[off/4+1], f[off/4+2]);
        const double* d = reinterpret_cast<const double*>(base);
        for (int off = 0; off + 24 <= len; off += 8)
            if (NearD(d[off/8], ex) && NearD(d[off/8+1], ey) && NearD(d[off/8+2], ez))
                LOG_INFO("DIAG %s DBL +0x%X = (%.3f,%.3f,%.3f)", tag, off, d[off/8], d[off/8+1], d[off/8+2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

void Game::DiagRenderer(float eyeX, float eyeY, float eyeZ) {
    const Offsets& o = Config::GetOffsets();
    uintptr_t cinst = reinterpret_cast<uintptr_t>(ItemRender::ClientInstance());
    if (!cinst) return;
    uintptr_t mcg = 0, gr = 0;
    if (!SafeReadPtr(cinst + o.ciMinecraftGame, mcg) || !mcg) return;
    if (!SafeReadPtr(mcg + o.mcGameRenderer, gr) || !gr) return;

    // 1) GameRenderer lui-meme (floats + doubles), large.
    ScanBlock(gr, 0x4000, eyeX, eyeY, eyeZ, "GR");
    // 2) Suit chaque pointeur de GR[0..0x800] et scanne l'objet pointe (camera, scene...).
    __try {
        for (int off = 0; off < 0x800; off += 8) {
            uintptr_t p = 0;
            if (!SafeReadPtr(gr + off, p) || p < 0x10000 || (p & 0x7)) continue;
            // heuristique : pointeur plausible en espace utilisateur
            if (p > 0x7FFFFFFFFFFF) continue;
            char tag[32]; _snprintf_s(tag, sizeof(tag), _TRUNCATE, "GR->+0x%X", off);
            ScanBlock(p, 0x400, eyeX, eyeY, eyeZ, tag);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    LOG_INFO("DIAG oeil lu = (%.3f, %.3f, %.3f) ; gr=%p", eyeX, eyeY, eyeZ, (void*)gr);
}

void Game::DiagActor(float eyeX, float eyeY, float eyeZ) {
    void* lp = ResolvePlayer();
    if (!lp) return;
    const uintptr_t a = reinterpret_cast<uintptr_t>(lp);
    // Scanne l'Actor (0..0x2000) : floats + doubles proches de l'oeil.
    // On s'attend a trouver la pos tick (StateVector) ET peut-etre une pos de rendu.
    ScanBlock(a, 0x2000, eyeX, eyeY, eyeZ, "ACT");
    // Suit aussi les pointeurs internes (composants ECS) sur 0..0x400.
    __try {
        for (int off = 0; off < 0x400; off += 8) {
            uintptr_t p = 0;
            if (!SafeReadPtr(a + off, p) || p < 0x10000 || (p & 0x7) || p > 0x7FFFFFFFFFFF) continue;
            char tag[32]; _snprintf_s(tag, sizeof(tag), _TRUNCATE, "ACT->+0x%X", off);
            ScanBlock(p, 0x80, eyeX, eyeY, eyeZ, tag);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

void* Game::ArmorStack(int slot) {
    void* lp = ResolvePlayer();
    if (!lp || slot < 0 || slot > 3) return nullptr;
    return SafeArmorStack(lp, slot, Config::GetOffsets().inventoryGetItemIndex);
}

void* Game::HeldStack() {
    void* lp = ResolvePlayer();
    if (!lp) return nullptr;
    const NxItemCfg cfg = BuildItemCfg();
    return SafeHeldStack(lp, cfg);
}

bool Game::IsConfigured() {
    if (EcsArmorReady()) return true;                        // lecture via ECS (hook + sig)
    if (Config::GetOffsets().anyArmorChain()) return true;   // lecture par chaine de pointeurs
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


    // Etape 1 (diagnostic) : prouver la capture du LocalPlayer via le hook [player].
    {
        static uint64_t lastP = 0; static int pc = 0;
        const uint64_t now = GetTickCount64();
        if (pc < 20 && now - lastP > 1000) {
            lastP = now; ++pc;
            void* lp = ResolvePlayer();
            if (lp) {
                uintptr_t vt = 0; SafeReadPtr(reinterpret_cast<uintptr_t>(lp), vt);
                LOG_INFO("Joueur capture: LocalPlayer=0x%llX vtable=0x%llX",
                         (unsigned long long)(uintptr_t)lp, (unsigned long long)vt);
            } else {
                LOG_INFO("Joueur: pas encore capture (bouge la souris/vise en jeu)");
            }
        }
    }

    // Voie robuste : LocalPlayer capture (hook) + armure via composant ECS.
    if (EcsArmorReady()) return ReadViaEcs();

    // Sinon : lecture directe par chaines de pointeurs.
    if (Config::GetOffsets().anyChain()) return ReadViaChains();

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
