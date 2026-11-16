// Rendu d'items via le jeu : hook ScreenView::setupAndRender -> renderGuiItemNew.
#include "ItemRender.h"
#include "Game.h"
#include "core/Config.h"
#include "core/SigScan.h"
#include "core/Logger.h"
#include <windows.h>
#include <MinHook.h>
#include <cstring>
#include <vector>
#include <mutex>
#include <atomic>
#include <cmath>

namespace {
    using SetupFn  = void(__fastcall*)(void* view, void* uictx);
    using CtorFn   = void* (__fastcall*)(void* self, void* screenCtx, void* cinst, void* mcgame);
    // Signature BRUTE de renderGuiItemNew (ordre reel des args, pas le wrapper Latite) :
    // (this, ctx, item, mode, x, y, ench, opacity, a9, scale, 17)
    using RenderFn = void(__fastcall*)(void* itemRenderer, void* baseCtx, void* item, int mode,
                                       float x, float y, bool ench, float opacity, float a9, float scale, int last);

    uintptr_t g_setupTarget = 0;
    SetupFn   g_origSetup   = nullptr;
    CtorFn    g_barcCtor    = nullptr;
    RenderFn  g_renderItem  = nullptr;
    std::atomic<void*> g_cinst{ nullptr };   // dernier ClientInstance vu (pour les autres modules)

    // File d'icones + fond (remplie par le HUD, lue par le hook de rendu).
    struct IconReq { void* stack; float x, y, sizePx; };
    struct BgReq   { bool on; float x, y, w, h, rad;
                     float fr, fg, fb, fa;          // remplissage
                     float br, bg, bb, ba, bt; };   // bordure (couleur + epaisseur px)
    std::mutex            g_iconMtx;
    std::vector<IconReq>  g_icons;
    BgReq                 g_bg{ false, 0,0,0,0,0, 0,0,0,0, 0,0,0,0,0 };

    bool SafeReadPtr(uintptr_t a, uintptr_t& out) {
        __try { out = *reinterpret_cast<uintptr_t*>(a); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // fillRectangle(RectangleArea const&, Color const&, float) = index vtable 0x10.
    // RectangleArea du jeu : stockage {left, right, top, bottom}.
    struct RectArea { float left, right, top, bottom; };
    struct ColorF   { float r, g, b, a; };
    using FillRectFn = void(__fastcall*)(void* self, const RectArea*, const ColorF*, float);

    // Un rectangle plein (coords en pixels, converties en unites UI par frac).
    void FillPx(FillRectFn fn, void* uictx, float l, float t, float r, float b, const ColorF& c, float frac) {
        if (r <= l || b <= t) return;
        RectArea rc{ l * frac, r * frac, t * frac, b * frac };
        fn(uictx, &rc, &c, c.a);
    }

    // Rectangle a coins arrondis : bande centrale pleine + bandes haut/bas en escalier.
    void FillRounded(FillRectFn fn, void* uictx, float x, float y, float w, float h, float rad,
                     const ColorF& c, float frac) {
        rad = (std::min)(rad, (std::min)(w, h) * 0.5f);
        if (rad < 0.5f) { FillPx(fn, uictx, x, y, x + w, y + h, c, frac); return; }
        FillPx(fn, uictx, x, y + rad, x + w, y + h - rad, c, frac);     // milieu
        const int steps = (int)ceilf(rad);
        for (int i = 0; i < steps; ++i) {
            const float yy = (float)i;
            const float d  = yy + 0.5f - rad;
            const float inset = rad - sqrtf((std::max)(0.f, rad * rad - d * d));
            FillPx(fn, uictx, x + inset, y + yy,         x + w - inset, y + yy + 1.f, c, frac);       // haut
            FillPx(fn, uictx, x + inset, y + h - yy - 1.f, x + w - inset, y + h - yy, c, frac);       // bas
        }
    }

    // Bordure arrondie : cotes droits (en retrait des coins) + petits carres le long des arcs.
    void BorderRounded(FillRectFn fn, void* uictx, float x, float y, float w, float h, float rad,
                       float t, const ColorF& c, float frac) {
        rad = (std::min)(rad, (std::min)(w, h) * 0.5f);
        FillPx(fn, uictx, x + rad, y,         x + w - rad, y + t,     c, frac);   // haut
        FillPx(fn, uictx, x + rad, y + h - t, x + w - rad, y + h,     c, frac);   // bas
        FillPx(fn, uictx, x,       y + rad,   x + t,       y + h - rad, c, frac); // gauche
        FillPx(fn, uictx, x + w - t, y + rad, x + w,       y + h - rad, c, frac); // droite
        if (rad < 0.5f) return;
        const int steps = (int)ceilf(rad * 1.6f);
        const float cxL = x + rad, cxR = x + w - rad, cyT = y + rad, cyB = y + h - rad;
        const float ro = rad, ri = rad - t;
        for (int i = 0; i <= steps; ++i) {
            const float a = (float)i / (float)steps * 1.57079633f;   // 0..90 deg
            const float co = cosf(a), si = sinf(a);
            // point milieu de l'anneau (entre rayon interieur et exterieur)
            const float rm = (ro + ri) * 0.5f, hs = t * 0.6f;
            const float dx = rm * co, dy = rm * si;
            FillPx(fn, uictx, cxL - dx - hs, cyT - dy - hs, cxL - dx + hs, cyT - dy + hs, c, frac); // haut-gauche
            FillPx(fn, uictx, cxR + dx - hs, cyT - dy - hs, cxR + dx + hs, cyT - dy + hs, c, frac); // haut-droite
            FillPx(fn, uictx, cxL - dx - hs, cyB + dy - hs, cxL - dx + hs, cyB + dy + hs, c, frac); // bas-gauche
            FillPx(fn, uictx, cxR + dx - hs, cyB + dy - hs, cxR + dx + hs, cyB + dy + hs, c, frac); // bas-droite
        }
    }

    void DrawBackground(void* uictx, const BgReq& bg, float frac) {
        if (!bg.on) return;
        __try {
            void** vt = *reinterpret_cast<void***>(uictx);
            auto fn = reinterpret_cast<FillRectFn>(vt[0x10]);
            const ColorF fill{ bg.fr, bg.fg, bg.fb, bg.fa };
            FillRounded(fn, uictx, bg.x, bg.y, bg.w, bg.h, bg.rad, fill, frac);
            if (bg.ba > 0.f && bg.bt > 0.f) {
                const ColorF bc{ bg.br, bg.bg, bg.bb, bg.ba };
                BorderRounded(fn, uictx, bg.x, bg.y, bg.w, bg.h, bg.rad, bg.bt, bc, frac);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Lit guiScaleFrac (= 1/guiScale) depuis ClientInstance->GuiData ; secours 1/3.
    float ReadGuiScaleFrac(uintptr_t cinst) {
        const Offsets& o = Config::GetOffsets();
        uintptr_t gd = 0;
        float frac = 1.0f / 3.0f;
        if (SafeReadPtr(cinst + o.ciGuiData, gd) && gd) {
            __try { frac = *reinterpret_cast<float*>(gd + o.gdGuiScaleFrac); }
            __except (EXCEPTION_EXECUTE_HANDLER) { frac = 1.0f / 3.0f; }
        }
        if (!(frac > 0.02f && frac < 4.0f)) frac = 1.0f / 3.0f;
        return frac;
    }

    // Dessine le fond puis toutes les icones : contexte construit une seule fois.
    void DrawIcons(void* uictx, const std::vector<IconReq>& icons, const BgReq& bg, bool diag) {
        const Offsets& o = Config::GetOffsets();
        const uintptr_t ui = reinterpret_cast<uintptr_t>(uictx);
        uintptr_t cinst = 0, screenCtx = 0, mcgame = 0;
        if (!SafeReadPtr(ui + o.uictxCinst, cinst) || !cinst)            { if (diag) LOG_INFO("IR: cinst nul"); return; }
        if (!SafeReadPtr(ui + o.uictxScreenCtx, screenCtx) || !screenCtx) { if (diag) LOG_INFO("IR: screenCtx nul"); return; }
        if (!SafeReadPtr(cinst + o.ciMinecraftGame, mcgame) || !mcgame)  { if (diag) LOG_INFO("IR: mcgame nul"); return; }

        const float frac = ReadGuiScaleFrac(cinst);
        DrawBackground(uictx, bg, frac);   // le fond d'abord, SOUS les icones

        __try {
            alignas(16) unsigned char buf[0x600];
            memset(buf, 0, sizeof(buf));
            g_barcCtor(buf, reinterpret_cast<void*>(screenCtx), reinterpret_cast<void*>(cinst), reinterpret_cast<void*>(mcgame));
            void* itemRenderer = *reinterpret_cast<void**>(buf + o.barcItemRenderer);
            if (!itemRenderer) { if (diag) LOG_INFO("IR: itemRenderer nul"); return; }

            for (const IconReq& ic : icons) {
                if (!ic.stack) continue;
                // taille a l'ecran = ic.sizePx (independant du guiScale) :
                const float sizeModifier = ic.sizePx * frac / 16.0f;
                const float scale        = sizeModifier * frac * 3.0f;
                g_renderItem(itemRenderer, buf, ic.stack, 0 /*mode*/,
                             ic.x * frac, ic.y * frac, false /*ench*/, 1.0f /*opacity*/,
                             1.0f /*a9*/, scale, 17 /*last*/);
            }
            if (diag) LOG_INFO("IR: %zu icone(s) dessinee(s), frac=%.3f", icons.size(), frac);
        } __except (EXCEPTION_EXECUTE_HANDLER) { if (diag) LOG_INFO("IR: crash pendant le rendu"); }
    }

    void __fastcall HkSetupAndRender(void* view, void* uictx) {
        g_origSetup(view, uictx);   // le jeu dessine son interface d'abord

        // Capture le ClientInstance pour les autres modules (barres de vie, etc.).
        uintptr_t ci = 0;
        if (SafeReadPtr(reinterpret_cast<uintptr_t>(uictx) + Config::GetOffsets().uictxCinst, ci) && ci)
            g_cinst.store(reinterpret_cast<void*>(ci), std::memory_order_relaxed);

        // Minecraft pas au premier plan (Alt+Tab vers une autre appli) : l'overlay ne met plus
        // a jour la file d'icones/fond, donc d'anciennes valeurs resteraient dessinees par le
        // jeu (panneau "fantome" par-dessus l'autre appli). On ne dessine rien dans ce cas.
        if (HWND fg = GetForegroundWindow()) {
            DWORD fgPid = 0; GetWindowThreadProcessId(fg, &fgPid);
            if (fgPid != GetCurrentProcessId()) return;
        }

        std::vector<IconReq> icons;
        BgReq bg;
        { std::lock_guard<std::mutex> lk(g_iconMtx); icons = g_icons; bg = g_bg; }
        if (icons.empty() && !bg.on) return;

        static int diagN = 0;
        const bool diag = diagN < 5;
        DrawIcons(uictx, icons, bg, diag);
        if (diag) ++diagN;
    }
}

void ItemRender::ClearIcons() {
    std::lock_guard<std::mutex> lk(g_iconMtx);
    g_icons.clear();
    g_bg.on = false;
}

void ItemRender::QueueBackground(float x, float y, float w, float h, float rad,
                                 float fr, float fg, float fb, float fa,
                                 float br, float bg, float bb, float ba, float borderPx) {
    std::lock_guard<std::mutex> lk(g_iconMtx);
    g_bg = { true, x, y, w, h, rad, fr, fg, fb, fa, br, bg, bb, ba, borderPx };
}

void ItemRender::QueueIcon(void* itemStack, float screenX, float screenY, float sizePx) {
    if (!itemStack) return;
    std::lock_guard<std::mutex> lk(g_iconMtx);
    if (g_icons.size() < 64) g_icons.push_back({ itemStack, screenX, screenY, sizePx });
}

bool ItemRender::Active() { return g_setupTarget && g_renderItem && g_barcCtor; }

void* ItemRender::ClientInstance() { return g_cinst.load(std::memory_order_relaxed); }

float ItemRender::GuiScaleFrac() {
    void* ci = g_cinst.load(std::memory_order_relaxed);
    if (!ci) return 1.0f / 3.0f;
    return ReadGuiScaleFrac(reinterpret_cast<uintptr_t>(ci));
}

bool ItemRender::Init() {
    const Offsets& o = Config::GetOffsets();
    if (o.setupAndRenderSig.empty()) { LOG_INFO("Rendu d'items : non configure ([render] vide)"); return false; }

    g_setupTarget = SigScan::Find(o.setupAndRenderSig, "ScreenView::setupAndRender");
    g_renderItem  = reinterpret_cast<RenderFn>(SigScan::Find(o.renderGuiItemSig, "ItemRenderer::renderGuiItemNew"));
    g_barcCtor    = reinterpret_cast<CtorFn>(SigScan::Find(o.baseActorCtxSig, "BaseActorRenderContext::ctor"));
    if (!g_setupTarget || !g_renderItem || !g_barcCtor) {
        LOG_WARN("Rendu d'items : signatures incompletes (setup=%p render=%p ctor=%p)",
                 reinterpret_cast<void*>(g_setupTarget), reinterpret_cast<void*>(g_renderItem), reinterpret_cast<void*>(g_barcCtor));
        return false;
    }
    auto target = reinterpret_cast<void*>(g_setupTarget);
    if (MH_CreateHook(target, &HkSetupAndRender, reinterpret_cast<void**>(&g_origSetup)) == MH_OK &&
        MH_EnableHook(target) == MH_OK) {
        LOG_INFO("Rendu d'items : hook setupAndRender installe (0x%llX)", (unsigned long long)g_setupTarget);
        return true;
    }
    LOG_WARN("Rendu d'items : echec installation du hook");
    return false;
}

void ItemRender::Shutdown() {
    if (g_setupTarget) MH_DisableHook(reinterpret_cast<void*>(g_setupTarget));
}
