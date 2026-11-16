#include "FocusGrab.h"
#include "InputHook.h"
#include "Client.h"
#include "ui/Ui.h"
#include "core/Config.h"
#include "core/Logger.h"
#include <windows.h>
#include <imgui.h>

// Notre fenetre tourne sur son PROPRE fil avec une boucle de messages : c'est ce fil
// qui recoit le clavier quand on prend le focus, et qui peut changer l'avant-plan.
namespace {
    const wchar_t* kClass = L"NexusFocusGrab";
    constexpr UINT WM_NEXUS_TOGGLE = WM_APP + 0x51;

    HANDLE g_thread = nullptr;
    DWORD  g_threadId = 0;
    HWND   g_win = nullptr;
    HANDLE g_ready = nullptr;

    bool IsKeyMsg(UINT m) {
        return m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP ||
               m == WM_CHAR || m == WM_DEADCHAR || m == WM_SYSCHAR;
    }

    void GrabFocus() {
        // A l'ecran, sous le curseur : c'est dans cette configuration que Windows accepte
        // de donner le focus a la fenetre (et donc de le retirer au jeu).
        POINT p; GetCursorPos(&p);
        SetWindowPos(g_win, HWND_TOPMOST, p.x, p.y, 1, 1, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        const DWORD cur = GetCurrentThreadId();
        const DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        if (fg && fg != cur) AttachThreadInput(cur, fg, TRUE);
        SetForegroundWindow(g_win);
        SetActiveWindow(g_win);
        SetFocus(g_win);
        if (fg && fg != cur) AttachThreadInput(cur, fg, FALSE);
        const HWND nowFg = GetForegroundWindow();
        LOG_INFO("FocusGrab : apres prise de focus, fenetre active = %p (la notre = %p, jeu = %p)",
                 (void*)nowFg, (void*)g_win, (void*)InputHook::Window());
        if (nowFg != g_win)
            LOG_WARN("FocusGrab : Windows a REFUSE le changement de focus (le jeu garde la main)");
    }

    void ReleaseFocus() {
        ShowWindow(g_win, SW_HIDE);
        if (HWND game = InputHook::Window()) {
            const DWORD cur = GetCurrentThreadId();
            const DWORD gt = GetWindowThreadProcessId(game, nullptr);
            if (gt && gt != cur) AttachThreadInput(cur, gt, TRUE);
            SetForegroundWindow(game);
            SetActiveWindow(game);
            SetFocus(game);
            if (gt && gt != cur) AttachThreadInput(cur, gt, FALSE);
        }
        LOG_INFO("FocusGrab : focus rendu au jeu");
    }

    // --- Saisie clavier du menu (hook bas-niveau) ---
    // Minecraft Bedrock enregistre son clavier en Raw Input RIDEV_NOLEGACY : Windows ne
    // genere plus AUCUN message clavier classique (ni vol de focus ni WndProc ne recoivent
    // de touche). On lit donc les frappes sous la couche Raw Input via WH_KEYBOARD_LL
    // (independant du focus) et on les injecte dans la file ImGui. Tant que le menu est
    // ouvert, chaque touche est bloquee pour le jeu, sauf les raccourcis Nexus (Inser/Fin).
    HHOOK g_kbLL = nullptr;
    BYTE  g_kb[256] = {};        // etat clavier local pour ToUnicodeEx (Maj/Ctrl/Alt/Verr.Maj)

    LRESULT CALLBACK LLKeyboard(int code, WPARAM wp, LPARAM lp) {
        if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wp, lp);
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        const bool injected = (k->flags & LLKHF_INJECTED) != 0;
        if (!Ui::IsMenuOpen() || injected) return CallNextHookEx(nullptr, code, wp, lp);

        // Laisser le clavier a une AUTRE application si elle est au premier plan (Alt+Tab).
        // Un premier plan nul (transition/vol de focus) n'est PAS une autre appli : on capture.
        const HWND fg = GetForegroundWindow();
        if (fg) {
            DWORD fgPid = 0;
            GetWindowThreadProcessId(fg, &fgPid);
            if (fgPid != GetCurrentProcessId()) return CallNextHookEx(nullptr, code, wp, lp);
        }

        const DWORD vk = k->vkCode;
        if (vk == VK_INSERT || vk == VK_END)            // raccourcis Nexus : laisser le sondage les voir
            return CallNextHookEx(nullptr, code, wp, lp);

        const bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        const bool up   = (wp == WM_KEYUP   || wp == WM_SYSKEYUP);

        if (vk == VK_ESCAPE) { if (down) Ui::ToggleMenu(); return 1; }   // ESC ferme le menu

        if (vk < 256) {
            if (down) g_kb[vk] |= 0x80; else if (up) g_kb[vk] &= ~0x80;
            if (vk == VK_CAPITAL && down) g_kb[VK_CAPITAL] ^= 0x01;      // bascule du verrou maj
        }
        g_kb[VK_SHIFT]   = (g_kb[VK_LSHIFT]   | g_kb[VK_RSHIFT])   & 0x80;
        g_kb[VK_CONTROL] = (g_kb[VK_LCONTROL] | g_kb[VK_RCONTROL]) & 0x80;
        g_kb[VK_MENU]    = (g_kb[VK_LMENU]    | g_kb[VK_RMENU])    & 0x80;

        if (ImGui::GetCurrentContext()) {
            HWND gw = InputHook::Window();
            InputHook::QueueMessage(gw, down ? WM_KEYDOWN : WM_KEYUP, vk, 0);   // touches (fleches, retour, entree...)
            if (down) {
                WCHAR buf[8] = {};
                const UINT sc = k->scanCode ? k->scanCode : MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
                const int n = ToUnicodeEx(vk, sc, g_kb, buf, 8, 4 /* ne pas modifier l'etat noyau */,
                                          GetKeyboardLayout(0));
                for (int i = 0; i < n; ++i)
                    if (buf[i] >= 0x20 && buf[i] != 0x7F)                       // caracteres imprimables
                        InputHook::QueueMessage(gw, WM_CHAR, static_cast<WPARAM>(buf[i]), 0);
            }
        }
        // On bloque les APPUIS (pour ne pas agir dans le jeu pendant le menu), mais on laisse
        // TOUJOURS passer les RELACHEMENTS : sinon une touche enfoncee avant l'ouverture du
        // menu resterait "coincee" cote jeu (avance/saut automatique en entrant dans un monde).
        return up ? CallNextHookEx(nullptr, code, wp, lp) : 1;
    }

    LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NEXUS_TOGGLE) {
            if (Config::GetSettings().menuBlockMode != 2) { if (wp) GrabFocus(); else ReleaseFocus(); }
            return 0;
        }
        const bool firstPress = msg == WM_KEYDOWN && !(lp & (1 << 30));
        if (firstPress && wp == VK_ESCAPE && Ui::IsMenuOpen()) { Ui::ToggleMenu(); return 0; }

        if (Ui::IsMenuOpen() && IsKeyMsg(msg) && ImGui::GetCurrentContext())
            InputHook::QueueMessage(InputHook::Window(), msg, wp, lp);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    DWORD WINAPI ThreadProc(LPVOID) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        g_win = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, kClass, L"Nexus",
                                WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
        if (g_win) SetLayeredWindowAttributes(g_win, 0, 1, LWA_ALPHA);
        else LOG_ERROR("FocusGrab : creation de la fenetre impossible");

        // Hook clavier bas-niveau : seul moyen de lire le clavier avec Bedrock (Raw Input).
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&LLKeyboard), &self);
        g_kbLL = SetWindowsHookExW(WH_KEYBOARD_LL, LLKeyboard, self, 0);
        if (g_kbLL) LOG_INFO("FocusGrab : hook clavier bas-niveau installe (saisie du menu)");
        else        LOG_WARN("FocusGrab : hook clavier bas-niveau impossible (saisie du menu indisponible)");

        SetEvent(g_ready);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (g_kbLL) { UnhookWindowsHookEx(g_kbLL); g_kbLL = nullptr; }
        if (g_win) { DestroyWindow(g_win); g_win = nullptr; }
        UnregisterClassW(kClass, wc.hInstance);
        return 0;
    }
}

// Etat des modificateurs suivi par le hook bas-niveau (voir g_kb). Fiable meme quand la
// touche est bloquee pour le jeu : c'est nous qui l'avons lue en premier.
bool FocusGrab::IsCtrlDown()  { return (g_kb[VK_CONTROL] & 0x80) != 0; }
bool FocusGrab::IsShiftDown() { return (g_kb[VK_SHIFT]   & 0x80) != 0; }
bool FocusGrab::IsAltDown()   { return (g_kb[VK_MENU]    & 0x80) != 0; }

void FocusGrab::Init() {
    g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, ThreadProc, nullptr, 0, &g_threadId);
    if (g_thread) WaitForSingleObject(g_ready, 2000);
}

void FocusGrab::Shutdown() {
    if (g_win) PostMessageW(g_win, WM_NEXUS_TOGGLE, 0, 0);   // rend le focus
    if (g_threadId) PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    if (g_thread) { WaitForSingleObject(g_thread, 2000); CloseHandle(g_thread); g_thread = nullptr; }
    if (g_ready) { CloseHandle(g_ready); g_ready = nullptr; }
}

void FocusGrab::OnMenuToggled(bool open) {
    if (g_win) PostMessageW(g_win, WM_NEXUS_TOGGLE, open ? 1 : 0, 0);
}
