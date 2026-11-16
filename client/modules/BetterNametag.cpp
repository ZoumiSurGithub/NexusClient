#include "BetterNametag.h"
#include "core/Config.h"
#include "core/SigScan.h"
#include "core/Logger.h"
#include "modules/HealthBars.h"
#include <windows.h>
#include <MinHook.h>
#include <cstring>
#include <cstdio>

// BaseActorRenderer::renderText(screenCtx, viewData, std::string* tagData, font, mesh).
// On modifie *tagData EN PLACE (dans la capacite existante, sans realloc ni free :
// crucial car le jeu et la DLL n'ont pas le meme tas en /MT), puis on restaure.
// L'Actor courant vient de ActorRenderDispatcher::render (hooke en parallele).
namespace {
    using RenderTextFn = void (__fastcall*)(void*, void*, void*, void*, void*);
    uintptr_t     g_target = 0;
    RenderTextFn  g_original = nullptr;

    // Layout std::string MSVC : [0]=buffer SSO / ptr heap, [0x10]=size, [0x18]=capacity.
    inline size_t StrSize(void* s)  { return *reinterpret_cast<size_t*>(reinterpret_cast<char*>(s) + 0x10); }
    inline size_t StrCap(void* s)   { return *reinterpret_cast<size_t*>(reinterpret_cast<char*>(s) + 0x18); }
    inline char*  StrData(void* s)  { return StrCap(s) <= 15 ? reinterpret_cast<char*>(s)
                                                             : *reinterpret_cast<char**>(s); }
    inline void   StrSetSize(void* s, size_t n) { *reinterpret_cast<size_t*>(reinterpret_cast<char*>(s) + 0x10) = n; }

    // Ajoute 'suffix' a la fin SI ca rentre dans la capacite (sinon ne touche a rien).
    // Renvoie l'ancienne taille (pour restaurer), ou (size_t)-1 si rien n'a ete fait.
    size_t AppendInPlace(void* s, const char* suffix) {
        __try {
            const size_t size = StrSize(s);
            const size_t cap  = StrCap(s);
            const size_t slen = std::strlen(suffix);
            if (size > 4096 || size + slen > cap) return static_cast<size_t>(-1);
            char* data = StrData(s);
            std::memcpy(data + size, suffix, slen);
            data[size + slen] = 0;
            StrSetSize(s, size + slen);
            return size;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return static_cast<size_t>(-1); }
    }

    void RestoreInPlace(void* s, size_t origSize) {
        __try {
            char* data = StrData(s);
            StrSetSize(s, origSize);
            data[origSize] = 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }

    // Nettoie les codes de formatage Minecraft (§ = "C2 A7" UTF-8 ou "A7" Latin-1).
    void StripCodes(const char* raw, size_t rawLen, char* out, size_t outN) {
        size_t o = 0;
        for (size_t i = 0; i < rawLen && o + 1 < outN; ) {
            const unsigned char c0 = static_cast<unsigned char>(raw[i]);
            const unsigned char c1 = (i + 1 < rawLen) ? static_cast<unsigned char>(raw[i + 1]) : 0;
            if (c0 == 0xC2 && c1 == 0xA7 && i + 2 < rawLen) { i += 3; continue; }
            if (c0 == 0xA7 && i + 1 < rawLen) { i += 2; continue; }
            out[o++] = raw[i++];
        }
        while (o > 0 && out[o - 1] == ' ') --o;
        out[o] = 0;
    }

    void __fastcall HkRenderText(void* a1, void* a2, void* tagData, void* a4, void* a5) {
        if (!tagData || !Config::GetSettings().betterNametag) {
            g_original(a1, a2, tagData, a4, a5);
            return;
        }
        // On relie le nametag a l'entite par son NOM (meme chaine source) -> sa vie.
        char suffix[32]; suffix[0] = 0;
        __try {
            const size_t size = StrSize(tagData);
            const char*  data = StrData(tagData);
            if (size > 0 && size < 128 && data) {
                char clean[48];
                StripCodes(data, size, clean, sizeof(clean));
                float cur = 0.f, mx = 0.f;
                if (clean[0] && HealthBars::GetHealthByName(clean, cur, mx) && mx > 0.f) {
                    _snprintf_s(suffix, sizeof(suffix), _TRUNCATE, " \xC2\xA7""a%d/%d",
                                static_cast<int>(cur + 0.5f), static_cast<int>(mx + 0.5f));
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { suffix[0] = 0; }

        size_t orig = static_cast<size_t>(-1);
        if (suffix[0]) orig = AppendInPlace(tagData, suffix);
        g_original(a1, a2, tagData, a4, a5);
        if (orig != static_cast<size_t>(-1)) RestoreInPlace(tagData, orig);
    }
}

bool BetterNametag::Init() {
    g_target = SigScan::Find(Config::GetOffsets().nametagRenderSig, "BaseActorRenderer::renderText");
    if (!g_target) { LOG_WARN("Better Nametag : signature renderText introuvable"); return false; }
    auto target = reinterpret_cast<void*>(g_target);
    if (MH_CreateHook(target, &HkRenderText, reinterpret_cast<void**>(&g_original)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG_ERROR("Better Nametag : echec installation du hook");
        g_target = 0;
        return false;
    }
    LOG_INFO("Better Nametag : hook renderText installe (0x%llX)", (unsigned long long)g_target);
    return true;
}

void BetterNametag::Shutdown() {
    if (g_target) MH_DisableHook(reinterpret_cast<void*>(g_target));
}

bool BetterNametag::IsConfigured() { return g_target != 0; }
