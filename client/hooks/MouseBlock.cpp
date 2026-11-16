#include "MouseBlock.h"
#include "ui/Ui.h"
#include "core/Logger.h"
#include "core/Config.h"
#include "modules/ChatOverlay.h"
#include <intrin.h>
#include <windows.h>
#include <psapi.h>
#include <MinHook.h>
#include <atomic>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdio>

// ---------------------------------------------------------------------------------
// Rappel GameInput (documentation Microsoft) :
//   GameInputMouseState v0 : buttons(u32) | positionX(i64 @8) | positionY(@16) | wheelX(@24) | wheelY(@32)  = 40 o
//   GameInputMouseState v1 : buttons | positions | positionX(@8) | positionY(@16) | absX | absY | wheelX(@40) | wheelY(@48) = 56 o
//   Les positions sont CUMULEES : le jeu calcule le deplacement entre deux lectures.
// L'index de GetMouseState dans la vtable depend de la version de GameInput : on le
// detecte au demarrage en testant les methodes sur une vraie lecture souris.
// ---------------------------------------------------------------------------------

namespace {
    constexpr uint32_t kGameInputKindMouse = 0x00000020;

    using GameInputCreateFn = HRESULT(WINAPI*)(void** gameInput);
    using GetMouseStateFn   = bool(__fastcall*)(void* reading, void* state);
    using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
    using ClipCursorFn      = BOOL(WINAPI*)(const RECT*);
    using SetCursorPosFn    = BOOL(WINAPI*)(int, int);
    using GetRawInputBufferFn = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
    using GetHwndFn         = HWND(WINAPI*)();

    GetRawInputDataFn o_GetRawInputData = nullptr;
    ClipCursorFn      o_ClipCursor = nullptr;
    SetCursorPosFn    o_SetCursorPos = nullptr;
    GetRawInputBufferFn o_GetRawInputBuffer = nullptr;
    GetHwndFn         o_GetForegroundWindow = nullptr;
    GetHwndFn         o_GetFocus = nullptr;
    GetHwndFn         o_GetActiveWindow = nullptr;

    using KeyStateFn = SHORT(WINAPI*)(int);
    KeyStateFn o_GetAsyncKeyState = nullptr;
    KeyStateFn o_GetKeyState = nullptr;

    // Vrai si l'appel vient de NOTRE DLL (on ne se bloque pas nous-memes : zoom, bind de touche...)
    bool CalledBySelf(void* ret) {
        static uintptr_t base = 0; static size_t size = 0;
        if (!base) {
            HMODULE self = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(&CalledBySelf), &self)) {
                MODULEINFO mi{};
                if (GetModuleInformation(GetCurrentProcess(), self, &mi, sizeof(mi))) {
                    base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
                    size = mi.SizeOfImage;
                }
            }
        }
        const auto r = reinterpret_cast<uintptr_t>(ret);
        return base && r >= base && r < base + size;
    }
    bool IsMouseButtonVk(int vk) {
        return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
    }

    // Vrai si l'appel vient du code de Minecraft (pas de notre DLL, ni d'ImGui)
    bool CalledByGame(void* ret) {
        static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        static const size_t size = [] {
            MODULEINFO mi{};
            GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &mi, sizeof(mi));
            return static_cast<size_t>(mi.SizeOfImage);
        }();
        const auto r = reinterpret_cast<uintptr_t>(ret);
        return r >= base && r < base + size;
    }
    bool FakeFocusActive() { return Ui::IsMenuOpen() && Config::GetSettings().menuBlockMode != 2; }

    // Les clics ne doivent pas atteindre le jeu quand une surface de Nexus capte la souris.
    bool NexusWantsMouse() { return Ui::IsMenuOpen() || ChatOverlay::IsHovered(); }

    // L'interface du jeu lit souvent l'etat du bouton via GetAsyncKeyState / GetKeyState :
    // on masque les boutons de souris pour le jeu (pas pour notre propre code).
    SHORT WINAPI HkGetAsyncKeyState(int vk) {
        if (NexusWantsMouse() && IsMouseButtonVk(vk) && !CalledBySelf(_ReturnAddress())) return 0;
        return o_GetAsyncKeyState(vk);
    }
    SHORT WINAPI HkGetKeyState(int vk) {
        if (NexusWantsMouse() && IsMouseButtonVk(vk) && !CalledBySelf(_ReturnAddress())) return 0;
        return o_GetKeyState(vk);
    }

    std::atomic<unsigned> g_counts[MouseBlock::CounterCount]{};
    unsigned g_rates[MouseBlock::CounterCount]{};
    ULONGLONG g_rateTime = 0;
    void Count(int c) { g_counts[c].fetch_add(1, std::memory_order_relaxed); }

    char g_status[160] = "GameInput : en attente d'un mouvement de souris...";
    bool g_gameInputDone = false;
    int  g_attempts = 0;
    ULONGLONG g_lastAttempt = 0;

    // ---- Gel des positions cumulees --------------------------------------------------
    struct Axis { int64_t offset = 0; int64_t frozen = 0; };
    Axis g_axes[4];                 // x, y, wheelX, wheelY
    std::atomic<bool> g_frozen{ false };
    SRWLOCK g_lock = SRWLOCK_INIT;

    void Filter(uint8_t* s, size_t wheelOffset) {
        int64_t* v[4] = {
            reinterpret_cast<int64_t*>(s + 8), reinterpret_cast<int64_t*>(s + 16),
            reinterpret_cast<int64_t*>(s + wheelOffset), reinterpret_cast<int64_t*>(s + wheelOffset + 8),
        };
        AcquireSRWLockExclusive(&g_lock);
        if (Ui::IsMenuOpen()) {
            if (!g_frozen) {                         // ouverture du menu : on memorise la position vue par le jeu
                for (int i = 0; i < 4; ++i) g_axes[i].frozen = *v[i] - g_axes[i].offset;
                g_frozen = true;
            }
            for (int i = 0; i < 4; ++i) {
                g_axes[i].offset = *v[i] - g_axes[i].frozen; // tout mouvement pendant le menu est "absorbe"
                *v[i] = g_axes[i].frozen;
            }
            *reinterpret_cast<uint32_t*>(s) = 0;     // aucun bouton (pas de coup / pose de bloc)
        } else {
            g_frozen = false;
            for (int i = 0; i < 4; ++i) *v[i] -= g_axes[i].offset; // continuite : pas de saut de camera
            // Souris sur le panneau de pre-commandes : le jeu ne voit pas le clic
            // (sinon la barre de saisie du chat perd le focus)
            if (ChatOverlay::IsHovered()) *reinterpret_cast<uint32_t*>(s) = 0;
        }
        ReleaseSRWLockExclusive(&g_lock);
    }

    // Detection auto-correctrice : le bon GetMouseState n'est pas toujours au meme index
    // de vtable selon la version de GameInput. On recense tous les candidats plausibles,
    // on en accroche un, et si le jeu ne l'appelle pas on passe au suivant jusqu'a trouver
    // celui qui recoit du trafic. Un seul hook actif a la fois.
    struct Candidate { void* func; size_t wheel; const wchar_t* mod; int idx; int ver; };
    std::vector<Candidate> g_candidates;
    int g_activeCand = -1;
    void* g_activeFunc = nullptr;
    GetMouseStateFn g_activeOriginal = nullptr;
    size_t g_activeWheel = 32;
    std::atomic<uint64_t> g_mouseCalls{ 0 };   // total, jamais remis a zero
    uint64_t g_callsAtHook = 0;
    ULONGLONG g_hookTime = 0;
    bool g_locked = false;      // candidat confirme (le jeu l'appelle)
    bool g_cycledAll = false;

    bool __fastcall HkMouse(void* reading, void* state) {
        const bool ok = g_activeOriginal(reading, state);
        g_mouseCalls.fetch_add(1, std::memory_order_relaxed);
        Count(MouseBlock::GameInputMouse);
        if (ok && state) Filter(static_cast<uint8_t*>(state), g_activeWheel);
        return ok;
    }

    // Declarations anticipees (definitions plus bas)
    bool SafeCreate(GameInputCreateFn fn, void** out);
    void SafeRelease(void* obj);

    // ---- Blocage certain : IGameInput::GetCurrentReading (index 4, confirme) ------------
    // Tout l'input GameInput passe par la. Menu ouvert, on "gele" la lecture souris :
    // on renvoie toujours la meme lecture capturee a l'ouverture -> delta nul, aucun bouton.
    using GetCurrentReadingFn = HRESULT(__fastcall*)(void* self, uint32_t kind, void* device, void** out);
    GetCurrentReadingFn o_GetCurrentReading = nullptr;
    void* g_gcrTarget = nullptr;
    void* g_frozenReading = nullptr;
    bool  g_gcrHooked = false;

    void ReadingAddRef(void* r) {
        using Fn = ULONG(__fastcall*)(void*);
        __try { reinterpret_cast<Fn>((*reinterpret_cast<void***>(r))[1])(r); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    void ReadingRelease(void* r) {
        using Fn = ULONG(__fastcall*)(void*);
        __try { reinterpret_cast<Fn>((*reinterpret_cast<void***>(r))[2])(r); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    HRESULT __fastcall HkGetCurrentReading(void* self, uint32_t kind, void* device, void** out) {
        const HRESULT hr = o_GetCurrentReading(self, kind, device, out);
        if (kind & kGameInputKindMouse) Count(MouseBlock::GameInputReading);

        if (!Ui::IsMenuOpen()) {
            if (g_frozenReading) { ReadingRelease(g_frozenReading); g_frozenReading = nullptr; }
            return hr;
        }
        if (!(kind & kGameInputKindMouse) || !out) return hr;

        if (!g_frozenReading) {
            // Premiere lecture pendant le menu : on la capture comme reference figee.
            if (SUCCEEDED(hr) && *out) { g_frozenReading = *out; ReadingAddRef(g_frozenReading); }
            return hr;
        }
        // Lectures suivantes : on rend la lecture figee (le jeu voit un deplacement nul).
        if (SUCCEEDED(hr) && *out) ReadingRelease(*out);
        ReadingAddRef(g_frozenReading);
        *out = g_frozenReading;
        return S_OK;
    }

    void HookGetCurrentReading(const wchar_t* moduleName) {
        if (g_gcrHooked) return;
        HMODULE mod = GetModuleHandleW(moduleName);
        if (!mod) return;
        auto create = reinterpret_cast<GameInputCreateFn>(GetProcAddress(mod, "GameInputCreate"));
        if (!create) return;
        void* gi = nullptr;
        if (!SafeCreate(create, &gi)) return;
        void** vt = *reinterpret_cast<void***>(gi);
        void* target = vt[4];   // GetCurrentReading (on l'appelle deja via cet index)
        if (MH_CreateHook(target, &HkGetCurrentReading, reinterpret_cast<void**>(&o_GetCurrentReading)) == MH_OK &&
            MH_EnableHook(target) == MH_OK) {
            g_gcrTarget = target;
            g_gcrHooked = true;
            LOG_INFO("GameInput (%ls) : GetCurrentReading accroche (blocage par gel de la lecture)", moduleName);
        } else {
            LOG_WARN("GameInput (%ls) : hook GetCurrentReading impossible", moduleName);
        }
        SafeRelease(gi);
    }

    bool HookCandidate(int i) {
        if (g_activeFunc) {
            MH_DisableHook(g_activeFunc);
            MH_RemoveHook(g_activeFunc);
            g_activeFunc = nullptr;
            g_activeOriginal = nullptr;
        }
        Candidate& c = g_candidates[i];
        if (MH_CreateHook(c.func, &HkMouse, reinterpret_cast<void**>(&g_activeOriginal)) == MH_OK &&
            MH_EnableHook(c.func) == MH_OK) {
            g_activeFunc = c.func;
            g_activeWheel = c.wheel;
            g_activeCand = i;
            g_callsAtHook = g_mouseCalls.load();
            g_hookTime = GetTickCount64();
            LOG_INFO("GameInput : essai candidat %d/%zu (%ls index %d, v%d)", i + 1, g_candidates.size(), c.mod, c.idx, c.ver);
            snprintf(g_status, sizeof(g_status), "GameInput : test du canal %d/%zu (index %d)...",
                     i + 1, g_candidates.size(), c.idx);
            return true;
        }
        LOG_WARN("GameInput : hook du candidat %d impossible", i + 1);
        return false;
    }

    // ---- Appels proteges (pas d'objets C++ dans ces fonctions) ---------------------
    bool SafeCreate(GameInputCreateFn fn, void** out) {
        __try { return SUCCEEDED(fn(out)) && *out; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool SafeGetCurrentReading(void* gi, void** reading) {
        using Fn = HRESULT(__fastcall*)(void*, uint32_t, void*, void**);
        __try {
            auto fn = reinterpret_cast<Fn>((*reinterpret_cast<void***>(gi))[4]);
            return SUCCEEDED(fn(gi, kGameInputKindMouse, nullptr, reading)) && *reading;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool SafeGetKind(void* reading, uint32_t& kind) {
        using Fn = uint32_t(__fastcall*)(void*);
        __try { kind = reinterpret_cast<Fn>((*reinterpret_cast<void***>(reading))[3])(reading); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool SafeCall(void* fn, void* self, void* a, uint64_t& ret) {
        using Fn = uint64_t(__fastcall*)(void*, void*, void*, void*);
        __try { ret = reinterpret_cast<Fn>(fn)(self, a, nullptr, nullptr); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void SafeRelease(void* obj) {
        using Fn = ULONG(__fastcall*)(void*);
        __try { reinterpret_cast<Fn>((*reinterpret_cast<void***>(obj))[2])(obj); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    bool InModule(void* p, const MODULEINFO& mi) {
        auto a = reinterpret_cast<uintptr_t>(p), b = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
        return a >= b && a < b + mi.SizeOfImage;
    }

    // Cherche GetMouseState dans la vtable d'une vraie lecture souris.
    // Critere : renvoie true et ecrit les positions (octets 8..23) ; la taille ecrite
    // donne la version de la structure (40 = v0, 56 = v1).
    void CollectCandidates(const wchar_t* moduleName, int baseVer) {
        HMODULE mod = GetModuleHandleW(moduleName);
        if (!mod) return;
        auto create = reinterpret_cast<GameInputCreateFn>(GetProcAddress(mod, "GameInputCreate"));
        MODULEINFO mi{};
        if (!create || !GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi))) return;

        void* gi = nullptr;
        if (!SafeCreate(create, &gi)) return;

        void* reading = nullptr;
        uint32_t kind = 0;
        if (SafeGetCurrentReading(gi, &reading) && SafeGetKind(reading, kind) && (kind & kGameInputKindMouse)) {
            void** vt = *reinterpret_cast<void***>(reading);
            for (int idx = 4; idx < 48 && InModule(vt[idx], mi); ++idx) {
                alignas(16) uint8_t buf[128];
                memset(buf, 0xCC, sizeof(buf));
                uint64_t ret = 0;
                if (!SafeCall(vt[idx], reading, buf, ret) || (ret & 0xFF) != 1) continue;

                bool wrotePositions = false;
                for (int i = 8; i < 24; ++i) if (buf[i] != 0xCC) wrotePositions = true;
                int last = -1;
                for (int i = 0; i < 128; ++i) if (buf[i] != 0xCC) last = i;
                const size_t size = (last + 8) & ~size_t(7);
                if (!wrotePositions || (size != 40 && size != 56)) continue;

                void* func = vt[idx];
                bool dup = false;
                for (const auto& c : g_candidates) if (c.func == func) dup = true;
                if (dup) continue;
                g_candidates.push_back({ func, size == 56 ? 40u : 24u, moduleName, idx, size == 56 ? 1 : 0 });
                LOG_INFO("GameInput : candidat (%ls index %d, v%d)", moduleName, idx, size == 56 ? 1 : 0);
            }
        }
        if (reading) SafeRelease(reading);
        SafeRelease(gi);
    }

    // ---- Hooks Win32 de complement -------------------------------------------------
    UINT WINAPI HkGetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT header) {
        const UINT r = o_GetRawInputData(h, cmd, data, size, header);
        if (cmd == RID_INPUT && data && CalledByGame(_ReturnAddress())) Count(MouseBlock::RawInputData);
        if (cmd == RID_INPUT && data && r != UINT(-1) && r >= sizeof(RAWINPUTHEADER) && ChatOverlay::IsHovered()) {
            auto* ri = static_cast<RAWINPUT*>(data);
            if (ri->header.dwType == RIM_TYPEMOUSE) ri->data.mouse.usButtonFlags = 0;
        }
        if (Ui::IsMenuOpen() && cmd == RID_INPUT && data && r != UINT(-1) && r >= sizeof(RAWINPUTHEADER)) {
            auto* ri = static_cast<RAWINPUT*>(data);
            if (ri->header.dwType == RIM_TYPEMOUSE) {
                ri->data.mouse.lLastX = 0;
                ri->data.mouse.lLastY = 0;
                ri->data.mouse.usButtonFlags = 0;
            }
        }
        return r;
    }

    UINT WINAPI HkGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT header) {
        const UINT n = o_GetRawInputBuffer(data, size, header);
        if (data && n && n != UINT(-1) && CalledByGame(_ReturnAddress())) Count(MouseBlock::RawInputBuffer);
        if (Ui::IsMenuOpen() && data && n != UINT(-1)) {
            PRAWINPUT p = data;
            for (UINT i = 0; i < n; ++i) {
                if (p->header.dwType == RIM_TYPEMOUSE) {
                    p->data.mouse.lLastX = 0;
                    p->data.mouse.lLastY = 0;
                    p->data.mouse.usButtonFlags = 0;
                }
                // equivalent de NEXTRAWINPUTBLOCK (la macro utilise QWORD, absent avec WIN32_LEAN_AND_MEAN)
                const uintptr_t next = reinterpret_cast<uintptr_t>(p) + p->header.dwSize;
                p = reinterpret_cast<PRAWINPUT>((next + 7) & ~uintptr_t(7));
            }
        }
        return n;
    }

    // Pendant le menu, le jeu croit ne plus etre la fenetre active (comme un Alt+Tab).
    HWND WINAPI HkGetForegroundWindow() {
        if (CalledByGame(_ReturnAddress())) Count(MouseBlock::ForegroundCheck);
        if (FakeFocusActive() && CalledByGame(_ReturnAddress())) return nullptr;
        return o_GetForegroundWindow();
    }
    HWND WINAPI HkGetFocus() {
        if (FakeFocusActive() && CalledByGame(_ReturnAddress())) return nullptr;
        return o_GetFocus();
    }
    HWND WINAPI HkGetActiveWindow() {
        if (FakeFocusActive() && CalledByGame(_ReturnAddress())) return nullptr;
        return o_GetActiveWindow();
    }

    // Le jeu re-bloque le curseur au centre a chaque image : on l'en empeche pendant le menu.
    BOOL WINAPI HkClipCursor(const RECT* rect) {
        if (Ui::IsMenuOpen() && rect) return TRUE;
        return o_ClipCursor(rect);
    }
    BOOL WINAPI HkSetCursorPos(int x, int y) {
        if (Ui::IsMenuOpen()) return TRUE;
        return o_SetCursorPos(x, y);
    }

    void HookApi(const wchar_t* mod, const char* name, void* detour, void** orig) {
        void* target = nullptr;
        if (MH_CreateHookApiEx(mod, name, detour, orig, &target) == MH_OK && MH_EnableHook(target) == MH_OK)
            LOG_INFO("Hook %s installe", name);
        else
            LOG_WARN("Hook %s impossible", name);
    }
}

void MouseBlock::Init() {
    HookApi(L"user32", "GetRawInputData", &HkGetRawInputData, reinterpret_cast<void**>(&o_GetRawInputData));
    HookApi(L"user32", "GetRawInputBuffer", &HkGetRawInputBuffer, reinterpret_cast<void**>(&o_GetRawInputBuffer));
    HookApi(L"user32", "GetForegroundWindow", &HkGetForegroundWindow, reinterpret_cast<void**>(&o_GetForegroundWindow));
    HookApi(L"user32", "GetFocus", &HkGetFocus, reinterpret_cast<void**>(&o_GetFocus));
    HookApi(L"user32", "GetActiveWindow", &HkGetActiveWindow, reinterpret_cast<void**>(&o_GetActiveWindow));
    HookApi(L"user32", "GetAsyncKeyState", &HkGetAsyncKeyState, reinterpret_cast<void**>(&o_GetAsyncKeyState));
    HookApi(L"user32", "GetKeyState", &HkGetKeyState, reinterpret_cast<void**>(&o_GetKeyState));
    HookApi(L"user32", "ClipCursor", &HkClipCursor, reinterpret_cast<void**>(&o_ClipCursor));
    HookApi(L"user32", "SetCursorPos", &HkSetCursorPos, reinterpret_cast<void**>(&o_SetCursorPos));
}

void MouseBlock::Tick() {
    if (g_gameInputDone) return;
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastAttempt < 1000) return;
    g_lastAttempt = now;

    const bool hasModule = GetModuleHandleW(L"GameInput.dll") || GetModuleHandleW(L"GameInputRedist.dll");
    if (!hasModule) {
        if (++g_attempts > 30) {
            g_gameInputDone = true;
            snprintf(g_status, sizeof(g_status), "GameInput absent : blocage via Raw Input / curseur uniquement");
            LOG_INFO("GameInput non charge par le jeu");
        }
        return;
    }

    // 1) Recenser les candidats (necessite une lecture souris : on reessaie si vide).
    if (g_candidates.empty()) {
        HookGetCurrentReading(L"GameInput.dll");
        HookGetCurrentReading(L"GameInputRedist.dll");
        CollectCandidates(L"GameInput.dll", 0);
        CollectCandidates(L"GameInputRedist.dll", 0);
        if (g_candidates.empty()) {
            if (++g_attempts > 120) {
                g_gameInputDone = true;
                snprintf(g_status, sizeof(g_status), "GameInput : aucune fonction souris trouvee (voir Nexus.log)");
            }
            return;
        }
        HookCandidate(0);
        return;
    }

    // 2) Le candidat actif recoit-il du trafic ? Sinon, essayer le suivant.
    if (g_locked) { g_gameInputDone = true; return; }
    const uint64_t calls = g_mouseCalls.load() - g_callsAtHook;
    if (calls > 5) {
        g_locked = true;
        const Candidate& c = g_candidates[g_activeCand];
        snprintf(g_status, sizeof(g_status), "GameInput v%d (index %d) : souris bloquee dans le menu", c.ver, c.idx);
        LOG_INFO("GameInput : canal confirme (index %d, %llu appels)", c.idx, (unsigned long long)calls);
        g_gameInputDone = true;
        return;
    }
    if (now - g_hookTime > 2500) {   // ce candidat reste muet : suivant
        const int next = g_activeCand + 1;
        if (next < static_cast<int>(g_candidates.size())) {
            HookCandidate(next);
        } else if (!g_cycledAll) {
            g_cycledAll = true;
            HookCandidate(0);   // on reboucle une fois, au cas ou le trafic arrive plus tard
        } else {
            // Aucun candidat GameInput n'est appele : le jeu lit la souris autrement.
            snprintf(g_status, sizeof(g_status),
                     "Aucun canal GameInput actif : le jeu lit la souris autrement (RawInput/HID).");
            LOG_WARN("GameInput : aucun des %zu candidats n'est appele par le jeu", g_candidates.size());
            g_gameInputDone = true;
        }
    }
}

const char* MouseBlock::Status() { return g_status; }
bool MouseBlock::GameInputHooked() { return g_gcrHooked || g_locked; }

const char* MouseBlock::CounterName(int c) {
    static const char* names[] = { "GameInput GetMouseState", "GameInput GetCurrentReading", "GetRawInputData",
                                   "GetRawInputBuffer", "GetForegroundWindow (jeu)" };
    return c >= 0 && c < CounterCount ? names[c] : "?";
}

unsigned MouseBlock::PerSecond(int c) {
    const ULONGLONG now = GetTickCount64();
    if (now - g_rateTime >= 1000) {
        for (int i = 0; i < CounterCount; ++i) g_rates[i] = g_counts[i].exchange(0);
        g_rateTime = now;
    }
    return c >= 0 && c < CounterCount ? g_rates[c] : 0;
}
