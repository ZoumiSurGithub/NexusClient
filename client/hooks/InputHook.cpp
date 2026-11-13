#include "InputHook.h"
#include "Client.h"
#include "ui/Ui.h"
#include "core/Logger.h"
#include "core/Config.h"
#include "modules/ChatOverlay.h"
#include "modules/ChatSender.h"
#include <imgui.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#include <mutex>
#include <vector>

namespace {
    HWND    g_hwnd = nullptr;
    WNDPROC g_original = nullptr;

    struct QueuedMsg { HWND hwnd; UINT msg; WPARAM wp; LPARAM lp; };
    std::mutex g_queueMutex;
    std::vector<QueuedMsg> g_queue;

    // Appele sur le thread de la fenetre : on ne touche PAS a ImGui ici
    void QueueForImGui(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        std::lock_guard lock(g_queueMutex);
        if (g_queue.size() < 512) g_queue.push_back({ hwnd, msg, wp, lp });
    }

    constexpr UINT WM_NEXUS_MENU = WM_APP + 0x4B48;
    bool g_focusFaked = false;

    // Envoie au jeu (et seulement a lui) les messages d'une perte ou d'un retour de focus.
    // Comme quand tu fais Alt+Tab : il relache la souris et arrete de lire la camera.
    void SendFakeFocus(HWND hwnd, bool active) {
        CallWindowProcW(g_original, hwnd, WM_ACTIVATEAPP, active ? TRUE : FALSE, 0);
        CallWindowProcW(g_original, hwnd, WM_ACTIVATE, active ? WA_ACTIVE : WA_INACTIVE, 0);
        CallWindowProcW(g_original, hwnd, active ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
        g_focusFaked = !active;
    }

    bool IsInputMessage(UINT msg) {
        return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
               (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
               msg == WM_INPUT;
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NEXUS_MENU) {
            const bool open = wp != 0;
            if (open && Config::GetSettings().menuBlockMode == 1 && !g_focusFaked) SendFakeFocus(hwnd, false);
            else if (!open && g_focusFaked) SendFakeFocus(hwnd, true);
            return 0;
        }
        // Tant que le menu est ouvert, le jeu ne doit pas "reprendre" le focus.
        if (g_focusFaked && (msg == WM_SETFOCUS || (msg == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE) ||
                             (msg == WM_ACTIVATEAPP && wp))) {
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        // (Les frappes du joueur pendant une saisie Nexus sont ecartees par le hook clavier
        //  bas niveau de ChatSender : ici, impossible de distinguer les caracteres simules,
        //  car WM_CHAR ne garde pas la marque des touches injectees.)

        if (!Ui::IsMenuOpen()) {
            // Alt+1..8 : utiliser la pre-commande correspondante (le jeu ne recoit pas la touche)
            if (ChatOverlay::IsVisible() && (msg == WM_SYSKEYDOWN || msg == WM_SYSCHAR || msg == WM_SYSKEYUP)) {
                const bool digit = wp >= '1' && wp <= '8';
                if (digit && msg == WM_SYSKEYDOWN && !(lp & (1 << 30))) ChatOverlay::HandleShortcut(static_cast<int>(wp - '1'));
                if (digit) return 0;
            }
            ChatOverlay::OnMessage(hwnd, msg, wp, lp);
            // Panneau de pre-commandes affiche : il recoit la souris ; les clics dessus ne vont pas au jeu
            if (ChatOverlay::IsVisible() && ImGui::GetCurrentContext() &&
                msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) {
                QueueForImGui(hwnd, msg, wp, lp);
                // Lecture d'un simple booleen calcule a l'image precedente (sans modifier ImGui)
                if (msg != WM_MOUSEMOVE && ChatOverlay::IsHovered()) return 0;
            }
        }

        const bool firstPress = msg == WM_KEYDOWN && !(lp & (1 << 30));

        if (firstPress && wp == VK_INSERT) { Ui::ToggleMenu(); return 0; }
        if (firstPress && wp == VK_END)    { Client::RequestUnload(); return 0; }
        if (firstPress && wp == VK_ESCAPE && Ui::IsMenuOpen()) { Ui::ToggleMenu(); return 0; }

        if (Ui::IsMenuOpen() && ImGui::GetCurrentContext()) {
            QueueForImGui(hwnd, msg, wp, lp);
            if (IsInputMessage(msg)) return 0; // le jeu ne recoit rien tant que le menu est ouvert
        }
        return CallWindowProcW(g_original, hwnd, msg, wp, lp);
    }
}

void InputHook::Init(HWND hwnd) {
    g_hwnd = hwnd;
    g_original = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
    if (!g_original) LOG_ERROR("Impossible de sous-classer la fenetre du jeu (menu inutilisable)");
}

HWND InputHook::Window() { return g_hwnd; }

void InputHook::FlushToImGui() {
    std::vector<QueuedMsg> pending;
    {
        std::lock_guard lock(g_queueMutex);
        pending.swap(g_queue);
    }
    for (const auto& m : pending) ImGui_ImplWin32_WndProcHandler(m.hwnd, m.msg, m.wp, m.lp);
}

void InputHook::NotifyMenuChanged(bool open) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_NEXUS_MENU, open ? 1 : 0, 0); // traite sur le thread de la fenetre
}

void InputHook::Shutdown() {
    if (g_hwnd && g_original && g_focusFaked) SendFakeFocus(g_hwnd, true);
    if (g_hwnd && g_original) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original));
    g_hwnd = nullptr;
    g_original = nullptr;
}
