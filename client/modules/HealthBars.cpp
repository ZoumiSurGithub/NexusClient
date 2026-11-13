#include "HealthBars.h"
#include "core/Config.h"
#include "core/SigScan.h"
#include "core/Logger.h"
#include "game/Game.h"
#include <windows.h>
#include <MinHook.h>
#include <imgui.h>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cfloat>

// Fonctionnement
// 1. Un hook sur une fonction appelee pour chaque entite (ex : Actor::normalTick)
//    collecte les Actor* rencontres. Une entite non revue depuis 500 ms est oubliee.
// 1b. Dans ce meme hook (donc sur le THREAD DU JEU, ou le monde est stable), on teste
//    la ligne de vue avec la fonction du jeu Actor::canSee (lancer de rayon a travers
//    les blocs). Une entite cachee par un mur, ou invisible, n'a pas de barre.
//    Pas de ligne de vue configuree = module desactive : pas d'informations a travers les murs.
// 2. A chaque image : position, vie, type de chaque entite (appels proteges).
// 3. Projection 3D -> ecran a partir de la position et de l'orientation du joueur
//    local et du FOV (reglable dans le menu pour aligner les barres).

namespace {
    using HookFn      = void* (__fastcall*)(void*, void*, void*, void*);
    using IntGetterFn = int (__fastcall*)(void*);
    using TypeIdFn    = uint32_t (__fastcall*)(void*);
    using NameTagFn   = const void* (__fastcall*)(void*);
    using CanSeeFn    = bool (__fastcall*)(void* self, void* target);
    using BoolVFn     = bool (__fastcall*)(void*);

    uintptr_t   g_hookTarget = 0;
    HookFn      g_original = nullptr;
    IntGetterFn g_getHealth = nullptr;
    IntGetterFn g_getMaxHealth = nullptr;
    CanSeeFn    g_canSee = nullptr;

    constexpr ULONGLONG kCheckInterval = 100;      // ms entre deux tests de ligne de vue par entite
    constexpr ULONGLONG kVisibleGrace  = 150;      // la barre disparait 150 ms apres la perte de vue

    struct Entry {
        ULONGLONG lastSeen = 0;      // derniere fois que le jeu a traite l'entite
        ULONGLONG lastCheck = 0;     // dernier test de ligne de vue
        ULONGLONG visibleUntil = 0;  // visible (ligne de vue degagee) jusqu'a cet instant
    };
    std::mutex g_mutex;
    std::unordered_map<void*, Entry> g_seen;

    bool SafeCanSee(void* self, void* target, bool& out) {
        __try { out = g_canSee(self, target); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool SafeIsInvisible(void* actor, int index, bool& out) {
        __try { out = (*reinterpret_cast<BoolVFn**>(actor))[index](actor); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // ---- Collecte ------------------------------------------------------------------
    void* __fastcall HkEntity(void* a1, void* a2, void* a3, void* a4) {
        void* result = g_original(a1, a2, a3, a4);

        void* args[4] = { a1, a2, a3, a4 };
        const Offsets& o = Config::GetOffsets();
        const int idx = o.entityHookArg;
        if (idx < 0 || idx >= 4 || !args[idx] || !g_canSee) return result;
        void* actor = args[idx];
        void* local = Game::LocalPlayer();
        if (!local || actor == local || !Config::GetSettings().healthEnabled) return result;

        const ULONGLONG now = GetTickCount64();
        bool needCheck;
        {
            std::lock_guard lock(g_mutex);
            Entry& e = g_seen[actor];
            e.lastSeen = now;
            needCheck = now - e.lastCheck >= kCheckInterval;
            if (needCheck) e.lastCheck = now;
        }
        if (!needCheck) return result;

        // Test sur le thread du jeu : le monde (chunks, blocs) n'est pas modifie pendant l'appel.
        bool visible = false;
        if (!SafeCanSee(local, actor, visible)) visible = false;
        if (visible && o.isInvisibleIndex >= 0) {
            bool invisible = false;
            if (SafeIsInvisible(actor, o.isInvisibleIndex, invisible) && invisible) visible = false;
        }
        if (visible) {
            std::lock_guard lock(g_mutex);
            g_seen[actor].visibleUntil = now + kVisibleGrace;
        }
        return result;
    }

    // ---- Lectures protegees ----------------------------------------------------------
    bool SafeReadPtr(uintptr_t addr, uintptr_t& out) {
        __try { out = *reinterpret_cast<uintptr_t*>(addr); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool SafeReadFloats(uintptr_t addr, float* out, int n) {
        __try { for (int i = 0; i < n; ++i) out[i] = reinterpret_cast<float*>(addr)[i]; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool SafeInt(IntGetterFn fn, void* actor, int& out) {
        __try { out = fn(actor); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool SafeTypeId(void* actor, int index, uint32_t& out) {
        __try { out = (*reinterpret_cast<TypeIdFn**>(actor))[index](actor); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // std::string MSVC : buffer local de 16 o (ou pointeur), taille a +0x10, capacite a +0x18
    bool SafeNameTag(void* actor, int index, char* out, size_t outSize) {
        __try {
            auto str = reinterpret_cast<uintptr_t>((*reinterpret_cast<NameTagFn**>(actor))[index](actor));
            if (!str) return false;
            const size_t size = *reinterpret_cast<size_t*>(str + 0x10);
            const size_t cap  = *reinterpret_cast<size_t*>(str + 0x18);
            const char* data  = cap >= 16 ? *reinterpret_cast<const char**>(str) : reinterpret_cast<const char*>(str);
            const size_t n = size < outSize - 1 ? size : outSize - 1;
            for (size_t i = 0; i < n; ++i) out[i] = data[i];
            out[n] = 0;
            return n > 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Chaine : on dereference tous les elements sauf le dernier, qui est l'offset du champ.
    bool ReadChainFloats(void* actor, const std::vector<ptrdiff_t>& chain, float* out, int n) {
        if (chain.empty()) return false;
        uintptr_t p = reinterpret_cast<uintptr_t>(actor);
        for (size_t i = 0; i + 1 < chain.size(); ++i)
            if (!SafeReadPtr(p + chain[i], p) || !p) return false;
        return SafeReadFloats(p + chain.back(), out, n);
    }

    // ---- Categories (bits de l'enum ActorType de Bedrock) ------------------------------
    enum class Category { Player, Animal, Monster, Other, Ignored };
    Category Classify(uint32_t type) {
        if (type == 0x13F)    return Category::Player;
        if (!(type & 0x100))  return Category::Ignored;  // pas un Mob : objets, fleches, XP...
        if (type & 0x800)     return Category::Monster;
        if (type & 0x1000)    return Category::Animal;
        return Category::Other;                          // villageois, golems, chauves-souris...
    }

    // ---- Projection ----------------------------------------------------------------
    struct Vec3 { float x, y, z; };
    Vec3 Sub(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    Vec3 Cross(Vec3 a, Vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }

    struct Camera { Vec3 eye, forward, right, up; float focal; ImVec2 center; };

    bool BuildCamera(void* player, Camera& cam) {
        const Offsets& o = Config::GetOffsets();
        float pos[3], rot[2];
        if (!ReadChainFloats(player, o.positionChain, pos, 3) || !ReadChainFloats(player, o.rotationChain, rot, 2))
            return false;
        const float pitch = rot[0] * 3.14159265f / 180.f, yaw = rot[1] * 3.14159265f / 180.f;
        cam.eye = { pos[0], pos[1], pos[2] };   // la position d'un joueur Bedrock est a hauteur des yeux
        // Minecraft : yaw 0 = sud (+Z), pitch positif = regard vers le bas
        cam.forward = { -std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
        cam.right = Cross(cam.forward, { 0.f, 1.f, 0.f });
        const float len = std::sqrt(Dot(cam.right, cam.right));
        if (len < 1e-4f) return false;
        cam.right = { cam.right.x / len, cam.right.y / len, cam.right.z / len };
        cam.up = Cross(cam.right, cam.forward);

        const ImVec2 size = ImGui::GetIO().DisplaySize;
        cam.center = { size.x * 0.5f, size.y * 0.5f };
        cam.focal = (size.y * 0.5f) / std::tan(Config::GetSettings().fov * 0.5f * 3.14159265f / 180.f);
        return true;
    }

    bool WorldToScreen(const Camera& cam, Vec3 world, ImVec2& out) {
        const Vec3 d = Sub(world, cam.eye);
        const float z = Dot(d, cam.forward);
        if (z < 0.2f) return false;            // derriere la camera
        out.x = cam.center.x + Dot(d, cam.right) / z * cam.focal;
        out.y = cam.center.y - Dot(d, cam.up) / z * cam.focal;
        return true;
    }

    ImU32 HealthColor(float r) {
        r = std::clamp(r, 0.f, 1.f);
        const ImVec4 red(0.95f, 0.25f, 0.25f, 1.f), orange(1.f, 0.70f, 0.15f, 1.f), green(0.35f, 0.90f, 0.40f, 1.f);
        auto mix = [](ImVec4 a, ImVec4 b, float t) {
            return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.f);
        };
        return ImGui::ColorConvertFloat4ToU32(r < 0.5f ? mix(red, orange, r * 2.f) : mix(orange, green, (r - 0.5f) * 2.f));
    }

    char g_missing[256] = "";
}

bool HealthBars::Init() {
    const Offsets& o = Config::GetOffsets();
    g_getHealth    = reinterpret_cast<IntGetterFn>(SigScan::Find(o.getHealthSig, "entities.Actor_getHealth"));
    g_getMaxHealth = reinterpret_cast<IntGetterFn>(SigScan::Find(o.getMaxHealthSig, "entities.Actor_getMaxHealth"));
    g_hookTarget   = SigScan::Find(o.entityHookSig, "entities.hook_signature");
    g_canSee       = reinterpret_cast<CanSeeFn>(SigScan::Find(o.canSeeSig, "entities.Actor_canSee"));

    if (g_hookTarget) {
        auto target = reinterpret_cast<void*>(g_hookTarget);
        if (MH_CreateHook(target, &HkEntity, reinterpret_cast<void**>(&g_original)) != MH_OK ||
            MH_EnableHook(target) != MH_OK) {
            LOG_ERROR("Impossible d'installer le hook entites");
            g_hookTarget = 0;
        } else {
            LOG_INFO("Hook entites installe");
        }
    }

    std::string missing;
    if (!g_hookTarget)                    missing += "hook_signature, ";
    if (o.positionChain.empty())          missing += "position_chain, ";
    if (o.rotationChain.empty())          missing += "rotation_chain, ";
    if (!g_getHealth)                     missing += "Actor_getHealth, ";
    if (!g_getMaxHealth)                  missing += "Actor_getMaxHealth, ";
    if (o.getEntityTypeIdIndex < 0)       missing += "vtable_getEntityTypeId, ";
    if (!g_canSee)                        missing += "Actor_canSee (ligne de vue), ";
    if (!missing.empty()) missing.resize(missing.size() - 2);
    snprintf(g_missing, sizeof(g_missing), "%s", missing.c_str());

    LOG_INFO(IsConfigured() ? "Barres de vie : pretes" : "Barres de vie : offsets incomplets");
    return IsConfigured();
}

void HealthBars::Shutdown() {
    if (g_hookTarget) MH_DisableHook(reinterpret_cast<void*>(g_hookTarget));
}

bool HealthBars::IsConfigured() { return g_missing[0] == 0; }
const char* HealthBars::MissingText() { return g_missing; }

void HealthBars::Render() {
    const Settings& s = Config::GetSettings();
    if (!s.healthEnabled || !IsConfigured()) return;

    void* local = Game::LocalPlayer();   // necessite aussi la section [player]
    if (!local) return;

    Camera cam;
    if (!BuildCamera(local, cam)) return;

    // Copie des entites VISIBLES (et nettoyage des entites disparues)
    std::vector<void*> actors;
    {
        std::lock_guard lock(g_mutex);
        const ULONGLONG now = GetTickCount64();
        for (auto it = g_seen.begin(); it != g_seen.end();) {
            if (now - it->second.lastSeen > 500) { it = g_seen.erase(it); continue; }
            if (now <= it->second.visibleUntil) actors.push_back(it->first);
            ++it;
        }
    }

    const Offsets& o = Config::GetOffsets();
    ImDrawList* dl = ImGui::GetBackgroundDrawList(); // sous le menu et le HUD
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize() * 0.85f * s.hpBarScale;

    for (void* actor : actors) {
        if (actor == local) continue;

        uint32_t type = 0;
        if (!SafeTypeId(actor, o.getEntityTypeIdIndex, type)) continue;
        const Category cat = Classify(type);
        if (cat == Category::Ignored ||
            (cat == Category::Player && !s.hpPlayers) || (cat == Category::Animal && !s.hpAnimals) ||
            (cat == Category::Monster && !s.hpMonsters) || (cat == Category::Other && !s.hpOthers))
            continue;

        int hp = 0, maxHp = 0;
        if (!SafeInt(g_getHealth, actor, hp) || !SafeInt(g_getMaxHealth, actor, maxHp) || maxHp <= 0 || hp <= 0) continue;
        if (s.hpHideFull && hp >= maxHp) continue;

        float pos[3];
        if (!ReadChainFloats(actor, o.positionChain, pos, 3)) continue;
        Vec3 top{ pos[0], pos[1], pos[2] };

        // Haut de l'entite : boite de collision si dispo, sinon estimation
        float aabb[6];
        if (ReadChainFloats(actor, o.aabbChain, aabb, 6) && aabb[4] > aabb[1]) top.y = aabb[4] + 0.35f;
        else top.y += cat == Category::Player ? 0.55f : 1.6f;  // joueur : position = yeux ; mob : pieds

        const Vec3 d = Sub(top, cam.eye);
        const float dist = std::sqrt(Dot(d, d));
        if (dist > s.hpMaxDistance || dist < 0.5f) continue;

        ImVec2 sp;
        if (!WorldToScreen(cam, top, sp)) continue;

        // Taille : diminue avec la distance, bornee pour rester lisible
        const float k = std::clamp(10.f / dist, 0.55f, 1.4f) * s.hpBarScale;
        const float w = 60.f * k, h = 6.f * k;
        const ImVec2 a(sp.x - w * 0.5f, sp.y - h), b(sp.x + w * 0.5f, sp.y);
        const float ratio = float(hp) / float(maxHp);

        dl->AddRectFilled({ a.x - 1.5f, a.y - 1.5f }, { b.x + 1.5f, b.y + 1.5f }, IM_COL32(0, 0, 0, 170), h);
        dl->AddRectFilled(a, { a.x + w * std::clamp(ratio, 0.f, 1.f), b.y }, HealthColor(ratio), h);

        // Texte au-dessus : nom (joueurs) et/ou valeur
        char label[96] = "";
        char name[48] = "";
        const bool hasName = s.hpShowNames && cat == Category::Player && o.getNameTagIndex >= 0 &&
                             SafeNameTag(actor, o.getNameTagIndex, name, sizeof(name));
        if (hasName && s.hpShowValue) snprintf(label, sizeof(label), "%s  %d/%d", name, hp, maxHp);
        else if (hasName)             snprintf(label, sizeof(label), "%s", name);
        else if (s.hpShowValue)       snprintf(label, sizeof(label), "%d/%d", hp, maxHp);

        if (label[0]) {
            const float fs = fontSize * std::clamp(k, 0.75f, 1.2f);
            const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, label);
            const ImVec2 tp(sp.x - ts.x * 0.5f, a.y - ts.y - 3.f);
            dl->AddText(font, fs, { tp.x + 1.f, tp.y + 1.f }, IM_COL32(0, 0, 0, 200), label);
            dl->AddText(font, fs, tp, IM_COL32(255, 255, 255, 235), label);
        }
    }
}
