// Lecture d'items via ECS (EnTT) + fonctions du jeu. Pas de __try ici (objets C++).
#include "sdk/EcsSdk.h"
#include "ArmorEcs.h"
#include "core/Logger.h"
#include <cstdint>
#include <string>
#include <cstring>

namespace {
    template<typename R, typename... A>
    R callV(void* self, int index, A... args) {
        auto vt = *reinterpret_cast<void***>(self);
        return reinterpret_cast<R(__fastcall*)(void*, A...)>(vt[index])(self, args...);
    }

    // Lit une std::string MSVC (layout : [buf16|ptr] , size@+0x10 , cap@+0x18).
    void readStdString(void* strAddr, char* out, size_t cap) {
        if (cap == 0) return;
        out[0] = '\0';
        if (!strAddr) return;
        char* base = reinterpret_cast<char*>(strAddr);
        const size_t size = *reinterpret_cast<size_t*>(base + 0x10);
        const size_t res  = *reinterpret_cast<size_t*>(base + 0x18);
        const char* chars = (res < 16) ? base : *reinterpret_cast<const char**>(base);
        if (!chars || size == 0 || size > 512) return;
        size_t n = size < cap - 1 ? size : cap - 1;
        std::memcpy(out, chars, n);
        out[n] = '\0';
        for (size_t i = 0; i < n; ++i) if (out[i] == '_') out[i] = ' ';   // joli : diamond_helmet -> diamond helmet
    }
}

bool Nexus_ReadItemCore(void* stack, const NxItemCfg* cfg, NxItemOut* out) {
    out->present = false; out->damage = 0; out->maxDamage = 0; out->count = 0; out->name[0] = '\0';
    if (!stack) return false;

    // Item present ? (ItemStackBase -> Item** -> Item*)
    void** itemPtr = *reinterpret_cast<void***>(reinterpret_cast<char*>(stack) + cfg->itemPtrOff);
    if (!itemPtr) return false;
    void* item = *itemPtr;
    if (!item) return false;
    out->present = true;

    out->count = *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(stack) + cfg->itemCountOff);

    if (cfg->getDamageFn && cfg->maxDmgVtIndex >= 0) {
        const short dmg = reinterpret_cast<short(__fastcall*)(void*)>(cfg->getDamageFn)(stack);
        const int mx = callV<int>(item, cfg->maxDmgVtIndex);
        if (mx > 0) { out->maxDamage = mx; out->damage = dmg < 0 ? 0 : (dmg > mx ? mx : dmg); }
    }
    return true;
}

void Nexus_ReadItemName(void* stack, const NxItemCfg* cfg, char* out, int cap) {
    if (cap > 0) out[0] = '\0';
    if (!stack || cap <= 0) return;
    // Lecture robuste : Item.id (std::string) -> ex. "diamond helmet".
    void** itemPtr = *reinterpret_cast<void***>(reinterpret_cast<char*>(stack) + cfg->itemPtrOff);
    if (!itemPtr) return;
    void* item = *itemPtr;
    if (!item) return;
    readStdString(reinterpret_cast<char*>(item) + cfg->itemIdStrOff, out, static_cast<size_t>(cap));
}

void* Nexus_GetArmorStack(void* localPlayer, int slot, int getItemIndex) {
    if (!localPlayer) return nullptr;
    auto* ctx = reinterpret_cast<EntityContext*>(reinterpret_cast<char*>(localPlayer) + 0x8);
    auto* comp = ctx->enttRegistry.try_get<ActorEquipmentComponent>(ctx->entity);
    if (!comp || !comp->armorContainer) return nullptr;
    return callV<void*, int>(comp->armorContainer, getItemIndex, slot);
}

int Nexus_ListComponents(void* actor, unsigned int* hashes, void** ptrs, int maxN) {
    if (!actor) return 0;
    auto* ctx = reinterpret_cast<EntityContext*>(reinterpret_cast<char*>(actor) + 0x8);
    auto& reg = ctx->enttRegistry;
    int n = 0;
    for (auto&& cur : reg.storage()) {
        if (n >= maxN) break;
        auto& set = cur.second;
        if (!set.contains(ctx->entity)) continue;
        const void* comp = set.value(ctx->entity);
        if (!comp) continue;
        hashes[n] = static_cast<unsigned int>(cur.first);
        ptrs[n]   = const_cast<void*>(comp);
        ++n;
    }
    return n;
}

void* Nexus_GetHeldStack(void* localPlayer, const NxItemCfg* cfg) {
    if (!localPlayer) return nullptr;
    void* supplies = *reinterpret_cast<void**>(reinterpret_cast<char*>(localPlayer) + cfg->suppliesOff);
    if (!supplies) return nullptr;
    void* inv = *reinterpret_cast<void**>(reinterpret_cast<char*>(supplies) + cfg->invOff);
    if (!inv) return nullptr;
    const int sel = *reinterpret_cast<int*>(reinterpret_cast<char*>(supplies) + cfg->selSlotOff);
    if (sel < 0 || sel > 8) return nullptr;   // hotbar 0..8
    return callV<void*, int>(inv, cfg->getItemIndex, sel);
}

int Nexus_CountMatching(void* localPlayer, const NxItemCfg* cfg, void* refStack) {
    if (!localPlayer || !refStack) return 0;
    // Type de reference (Item*) de l'objet en main.
    void** rItemPtr = *reinterpret_cast<void***>(reinterpret_cast<char*>(refStack) + cfg->itemPtrOff);
    if (!rItemPtr) return 0;
    void* refItem = *rItemPtr;
    if (!refItem) return 0;

    void* supplies = *reinterpret_cast<void**>(reinterpret_cast<char*>(localPlayer) + cfg->suppliesOff);
    if (!supplies) return 0;
    void* inv = *reinterpret_cast<void**>(reinterpret_cast<char*>(supplies) + cfg->invOff);
    if (!inv) return 0;

    int total = 0;
    for (int slot = 0; slot < 36; ++slot) {       // inventaire joueur = 36 cases
        void* st = callV<void*, int>(inv, cfg->getItemIndex, slot);
        if (!st) continue;
        void** ip = *reinterpret_cast<void***>(reinterpret_cast<char*>(st) + cfg->itemPtrOff);
        if (!ip) continue;
        if (*ip != refItem) continue;             // meme type d'item
        total += *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(st) + cfg->itemCountOff);
    }
    return total;
}
