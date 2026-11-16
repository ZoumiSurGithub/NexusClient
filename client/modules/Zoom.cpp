#include "Zoom.h"
#include "core/Config.h"
#include "core/SigScan.h"
#include "core/Logger.h"
#include "modules/ChatOverlay.h"
#include "ui/Ui.h"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <cmath>

// Methode Latite (fiable sur cette version) : on hooke LevelRenderer::renderLevel et,
// apres l'appel original, on multiplie les champs fovX/fovY du LevelRendererPlayer par
// le facteur de zoom. Un facteur de 4 = x4. Pas besoin de hooker getFov.
namespace {
    using RenderLevelFn = void (__fastcall*)(void* lvl, void* screenCtx, void* unk);

    uintptr_t     g_target = 0;
    RenderLevelFn g_original = nullptr;

    std::atomic<float> g_targetFactor{ 1.f };   // 1 = pas de zoom, 4 = zoom x4
    float     g_currentFactor = 1.f;            // valeur lissee (thread du jeu)
    ULONGLONG g_lastCall = 0;

    bool g_toggled = false;
    bool g_keyWasDown = false;

    // Ecrit les champs fov (code POD : compatible __try, pas d'objets C++ a derouler).
    void ApplyFov(void* lvl, float factor) {
        const Offsets& o = Config::GetOffsets();
        __try {
            void* lrp = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(lvl) + o.lrpOffset);
            if (!lrp) return;
            float* fx = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(lrp) + o.fovXOffset);
            float* fy = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(lrp) + o.fovYOffset);
            *fx *= factor;
            *fy *= factor;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void __fastcall HkRenderLevel(void* lvl, void* screenCtx, void* unk) {
        g_original(lvl, screenCtx, unk);
        if (!lvl) return;

        const Settings& s = Config::GetSettings();
        const float target = g_targetFactor.load();
        const ULONGLONG now = GetTickCount64();
        const float dt = g_lastCall ? std::fmin(0.1f, (now - g_lastCall) / 1000.f) : 0.f;
        g_lastCall = now;

        if (s.zoomSmooth) g_currentFactor += (target - g_currentFactor) * std::fmin(1.f, dt * 12.f);
        else              g_currentFactor = target;

        if (g_currentFactor > 1.001f) ApplyFov(lvl, g_currentFactor);
    }

    bool KeyDown(int vk) { return vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; }
}

bool Zoom::Init() {
    g_target = SigScan::Find(Config::GetOffsets().zoomRenderLevelSig, "zoom.renderlevel_signature");
    if (!g_target) return false;
    auto target = reinterpret_cast<void*>(g_target);
    if (MH_CreateHook(target, &HkRenderLevel, reinterpret_cast<void**>(&g_original)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG_ERROR("Impossible d'installer le hook de zoom");
        g_target = 0;
        return false;
    }
    LOG_INFO("Hook zoom installe (renderLevel 0x%p)", target);
    return true;
}

void Zoom::Shutdown() {
    if (g_target) MH_DisableHook(reinterpret_cast<void*>(g_target));
}

void Zoom::Update() {
    const Settings& s = Config::GetSettings();

    // Pas de zoom dans le menu Nexus, quand tu tapes dans le chat, ou hors du jeu
    DWORD pid = 0;
    if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &pid);
    const bool usable = s.zoomEnabled && !Ui::IsMenuOpen() && !ChatOverlay::IsChatOpen() &&
                        pid == GetCurrentProcessId();

    const bool down = usable && KeyDown(s.zoomKey);
    if (s.zoomMode == 1) {                         // bascule : un appui active, un autre desactive
        if (down && !g_keyWasDown) g_toggled = !g_toggled;
        if (!usable) g_toggled = false;
    }
    g_keyWasDown = down;

    const bool zooming = s.zoomMode == 1 ? g_toggled : down;
    g_targetFactor = zooming ? std::fmax(1.f, s.zoomFactor) : 1.f;
}

bool Zoom::IsConfigured() { return g_target != 0; }
bool Zoom::IsZooming() { return g_targetFactor.load() > 1.f; }
float Zoom::CurrentFactor() { return g_currentFactor; }
