#include "Zoom.h"
#include "core/Config.h"
#include "core/SigScan.h"
#include "core/Logger.h"
#include "hooks/InputHook.h"
#include "modules/ChatOverlay.h"
#include "ui/Ui.h"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cmath>

// Fonction hookee : float LevelRendererPlayer::getFov(float partialTicks, bool applyEffects)
// (prototype utilise par la plupart des clients Bedrock ; voir docs/TROUVER_LES_OFFSETS.md)
namespace {
    using GetFovFn = float (__fastcall*)(void* self, float partialTicks, bool applyEffects);

    uintptr_t g_target = 0;
    GetFovFn  g_original = nullptr;

    std::atomic<float> g_targetFactor{ 1.f };   // 1 = pas de zoom, 4 = zoom x4
    float g_currentFactor = 1.f;                // valeur lissee (thread du jeu)
    ULONGLONG g_lastCall = 0;

    bool g_toggled = false;
    bool g_keyWasDown = false;

    float __fastcall HkGetFov(void* self, float partialTicks, bool applyEffects) {
        const float fov = g_original(self, partialTicks, applyEffects);

        const Settings& s = Config::GetSettings();
        const float target = g_targetFactor.load();
        const ULONGLONG now = GetTickCount64();
        const float dt = g_lastCall ? std::fmin(0.1f, (now - g_lastCall) / 1000.f) : 0.f;
        g_lastCall = now;

        if (s.zoomSmooth) g_currentFactor += (target - g_currentFactor) * std::fmin(1.f, dt * 12.f);
        else              g_currentFactor = target;

        return g_currentFactor > 1.001f ? fov / g_currentFactor : fov;
    }

    bool KeyDown(int vk) { return vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; }
}

bool Zoom::Init() {
    g_target = SigScan::Find(Config::GetOffsets().zoomFovSig, "zoom.getFov_signature");
    if (!g_target) return false;
    auto target = reinterpret_cast<void*>(g_target);
    if (MH_CreateHook(target, &HkGetFov, reinterpret_cast<void**>(&g_original)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG_ERROR("Impossible d'installer le hook de zoom");
        g_target = 0;
        return false;
    }
    LOG_INFO("Hook zoom installe");
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
