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
#include <iterator>
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


    // ---- Projection ----------------------------------------------------------------
    struct Vec3 { float x, y, z; };
    Vec3 Sub(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    Vec3 Cross(Vec3 a, Vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }

    struct Camera { Vec3 eye, forward, right, up; float focal; ImVec2 center; };

    // Texte avec contour (bordure) : noir dans 8 directions puis couleur au centre.
    void DrawTextOutlined(ImDrawList* dl, ImFont* font, float size, ImVec2 p,
                          const char* txt, ImU32 col, ImU32 outline) {
        const float o = std::max(1.f, size * 0.09f);    // epaisseur du contour
        const float dirs[8][2] = { {-o,0},{o,0},{0,-o},{0,o},{-o,-o},{o,-o},{-o,o},{o,o} };
        for (auto& d : dirs)
            dl->AddText(font, size, { p.x + d[0], p.y + d[1] }, outline, txt);
        dl->AddText(font, size, p, col, txt);
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

    // Lissage anti-saccade : position ecran lissee par entite.
    struct Smooth { ImVec2 pos; ULONGLONG t; bool valid; };
    std::unordered_map<void*, Smooth> g_smooth;
    ULONGLONG g_smoothPrune = 0;

}

bool HealthBars::Init() {
    // Les barres de vie reposent desormais sur l'enumeration directe du niveau
    // (Game::CollectEntities + camera directe), pas sur des signatures d'entites.
    g_missing[0] = 0;
    LOG_INFO("Barres de vie : pretes (enumeration directe)");
    return true;
}

void HealthBars::Shutdown() {
    if (g_hookTarget) MH_DisableHook(reinterpret_cast<void*>(g_hookTarget));
}

bool HealthBars::IsConfigured() { return true; }
const char* HealthBars::MissingText() { return g_missing; }

    // Projection exacte avec les matrices brutes du jeu.
    // IMPORTANT : la matrice de vue du jeu ne contient QUE la rotation (translation nulle).
    // Il faut donc soustraire l'oeil d'abord : viewspace = Rotation * (monde - oeil).
    // Les deux matrices sont col-major (glm) : element (col c, ligne r) = m[c*4+r].
    bool W2S(const float* view, const float* proj, float ex, float ey, float ez,
             float wx, float wy, float wz, float W, float H, ImVec2& out, float& camDist) {
        const float rx = wx - ex, ry = wy - ey, rz = wz - ez;      // relatif a l'oeil
        // rotation 3x3 col-major : v[r] = sum_c view[c*4+r] * rel[c]
        const float vx = view[0] * rx + view[4] * ry + view[8]  * rz;
        const float vy = view[1] * rx + view[5] * ry + view[9]  * rz;
        const float vz = view[2] * rx + view[6] * ry + view[10] * rz;
        // projection col-major sur (vx,vy,vz,1)
        const float cx = proj[0] * vx + proj[4] * vy + proj[8]  * vz + proj[12];
        const float cy = proj[1] * vx + proj[5] * vy + proj[9]  * vz + proj[13];
        const float cw = proj[3] * vx + proj[7] * vy + proj[11] * vz + proj[15];
        camDist = cw;
        if (cw < 0.05f) return false;                             // derriere la camera
        const float ndcx = cx / cw, ndcy = cy / cw;
        if (ndcx < -1.3f || ndcx > 1.3f || ndcy < -1.3f || ndcy > 1.3f) return false;
        out.x = (ndcx * 0.5f + 0.5f) * W;
        out.y = (0.5f - ndcy * 0.5f) * H;
        return true;
    }

void HealthBars::Render() {
    const Settings& s = Config::GetSettings();
    if (!s.healthEnabled) return;

    float view[16], proj[16];
    if (!Game::GetRawMatrices(view, proj)) return;

    float ex, ey, ez, pitch, yaw;
    if (!Game::LocalPlayerCamera(ex, ey, ez, pitch, yaw)) return;

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();   // sous le menu et le HUD

    const ULONGLONG now = GetTickCount64();

    // --- Optimisation petit PC ---
    // La decouverte des entites (parcours de liste + resolution + hitbox) est le poste
    // le plus lourd : on la fait a ~30 Hz. Les positions, elles, sont relues CHAQUE frame
    // (lecture tres legere) pour garder les barres parfaitement fluides.
    static Game::EntityInfo ents[256];
    static int   n = 0;
    static ULONGLONG lastCollect = 0;
    // Distance max de travail : au-dela, on ne fait AUCUN calcul lourd (invisible de toute facon).
    const float maxD = s.hpMaxDistance;
    const float maxD2 = maxD * maxD;

    if (n == 0 || now - lastCollect >= 33) {
        n = Game::CollectEntities(ents, 256);
        lastCollect = now;
        // Passe vie + type ICI (30 Hz), hors du __try, contexte C++ propre.
        for (int i = 0; i < n; ++i) {
            // --- Rejet par distance AVANT tout calcul lourd (gros gain petit PC) ---
            const float ddx = ents[i].x - ex, ddy = ents[i].y - ey, ddz = ents[i].z - ez;
            const float d2 = ddx * ddx + ddy * ddy + ddz * ddz;
            if (ents[i].isPlayer || d2 > maxD2) {         // soi-meme ou trop loin -> on ignore
                ents[i].hasHealth = false;
                continue;
            }
            float hp = 0.f, mhp = 0.f;
            ents[i].hasHealth = Game::ReadHealth(ents[i].actor, hp, mhp) && mhp > 0.f;
            ents[i].health = hp; ents[i].maxHealth = mhp;
            if (!ents[i].hasHealth) continue;             // pas de vie -> inutile de classer/tracer
            ents[i].category = Game::EntityCategory(ents[i].actor);
            ents[i].visible = true;                       // occlusion retiree (instable) : toujours visible
            ents[i].name[0] = 0;
            if (s.hpShowName)                             // type de l'entite (ex. "Skeleton")
                Game::ReadEntityType(ents[i].actor, ents[i].name, 32);
        }
    } else {
        // Rafraichit seulement la position des entites retenues (vie lue) -> tres leger.
        for (int i = 0; i < n; ++i)
            if (ents[i].hasHealth)
                Game::ReadEntityPos(ents[i].actor, ents[i].x, ents[i].y, ents[i].z);
    }
    // Purge des entrees de lissage perimees (entites disparues).
    if (now - g_smoothPrune > 2000) {
        g_smoothPrune = now;
        for (auto it = g_smooth.begin(); it != g_smooth.end();)
            it = (now - it->second.t > 1000) ? g_smooth.erase(it) : std::next(it);
    }

    for (int i = 0; i < n; ++i) {
        if (ents[i].isPlayer) continue;                // pas de barre sur soi
        if (!ents[i].hasHealth) { g_smooth.erase(ents[i].actor); continue; }  // items, orbes, fleches...
        if (!ents[i].visible)   { g_smooth.erase(ents[i].actor); continue; }  // cache par un mur / invisible

        // Filtre par categorie (0=autre, 1=joueur, 2=animal, 3=monstre).
        const bool catOn = (ents[i].category == 1 && s.hpPlayers)
                        || (ents[i].category == 2 && s.hpAnimals)
                        || (ents[i].category == 3 && s.hpMonsters)
                        || (ents[i].category == 0 && s.hpOthers);
        if (!catOn) { g_smooth.erase(ents[i].actor); continue; }

        // Masquer si vie pleine (option).
        if (s.hpHideFull && ents[i].health >= ents[i].maxHealth - 0.01f) {
            g_smooth.erase(ents[i].actor);
            continue;
        }

        // Vraie distance monde (pour le culling et l'echelle).
        const float dx = ents[i].x - ex, dy = ents[i].y - ey, dz = ents[i].z - ez;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > s.hpMaxDistance || dist < 0.4f) continue;

        ImVec2 sp; float cd;
        // Sommet de la hitbox + petit espace, pour poser la barre juste au-dessus.
        const float topY = ents[i].y + ents[i].height + 0.30f;
        if (!W2S(view, proj, ex, ey, ez, ents[i].x, topY, ents[i].z, disp.x, disp.y, sp, cd)) {
            g_smooth.erase(ents[i].actor);
            continue;
        }

        // --- Lissage anti-saccade (exponentiel, independant du framerate) ---
        Smooth& sm = g_smooth[ents[i].actor];
        if (!sm.valid) { sm.pos = sp; }
        else {
            const float jump = std::sqrt((sp.x - sm.pos.x) * (sp.x - sm.pos.x) + (sp.y - sm.pos.y) * (sp.y - sm.pos.y));
            if (jump > 180.f) sm.pos = sp;             // grand saut (flick/teleport) -> on colle
            else {
                const float dt = sm.t ? (now - sm.t) / 1000.f : 0.016f;
                const float tau = 0.012f;              // ~12 ms : simple polissage (la source est deja synchro frame)
                const float al = 1.f - std::exp(-dt / tau);
                sm.pos.x += (sp.x - sm.pos.x) * al;
                sm.pos.y += (sp.y - sm.pos.y) * al;
            }
        }
        sm.t = now; sm.valid = true;
        sp = sm.pos;

        // Taille : diminue avec la distance, bornee pour rester lisible. Base agrandie.
        const float k = std::clamp(10.f / dist, 0.8f, 1.8f) * s.hpBarScale;
        const float w = 84.f * k, h = 9.f * k;
        const ImVec2 a(sp.x - w * 0.5f, sp.y - h), b(sp.x + w * 0.5f, sp.y);
        const float ratio = std::clamp(ents[i].health / ents[i].maxHealth, 0.f, 1.f);

        dl->AddRectFilled({ a.x - 1.5f, a.y - 1.5f }, { b.x + 1.5f, b.y + 1.5f }, IM_COL32(0, 0, 0, 180), h * 0.6f);
        dl->AddRectFilled(a, { a.x + w * ratio, b.y }, HealthColor(ratio), h * 0.6f);

        // Texte agrandi et avec bordure, empile au-dessus de la barre.
        ImFont* font = ImGui::GetFont();
        const float fsize = ImGui::GetFontSize() * std::clamp(k, 0.95f, 1.7f);
        float lineY = a.y - 3.f;                        // on remonte ligne par ligne

        // Valeur chiffree (ex : "15/20").
        if (s.hpShowValue) {
            char txt[24];
            snprintf(txt, sizeof(txt), "%d/%d", (int)(ents[i].health + 0.5f), (int)(ents[i].maxHealth + 0.5f));
            const float tw = font->CalcTextSizeA(fsize, FLT_MAX, 0.f, txt).x;
            lineY -= fsize;
            DrawTextOutlined(dl, font, fsize, { sp.x - tw * 0.5f, lineY }, txt,
                             IM_COL32(255, 255, 255, 240), IM_COL32(0, 0, 0, 230));
        }

        // Nom de l'entite / pseudo du joueur (option), au-dessus de la valeur.
        if (s.hpShowName && ents[i].name[0]) {
            const float tw = font->CalcTextSizeA(fsize, FLT_MAX, 0.f, ents[i].name).x;
            lineY -= fsize + 1.f;
            DrawTextOutlined(dl, font, fsize, { sp.x - tw * 0.5f, lineY }, ents[i].name,
                             IM_COL32(255, 255, 255, 240), IM_COL32(0, 0, 0, 230));
        }
    }
}
