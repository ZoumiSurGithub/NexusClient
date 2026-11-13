#include "ChatOverlay.h"
#include "ChatSender.h"
#include "core/Config.h"
#include <atomic>
#include <mutex>

namespace {
    std::atomic<bool> g_open{ false };
    std::atomic<bool> g_visible{ false };
    std::atomic<bool> g_hovered{ false };
    std::string g_shortcuts[8];
    int g_shortcutCount = 0;
    std::atomic<ULONGLONG> g_lastCaptured{ 0 };   // derniere image ou le jeu tenait la souris
    std::atomic<ULONGLONG> g_openedAt{ 0 };
    std::atomic<ULONGLONG> g_capturedSince{ 0 };  // depuis quand la souris est capturee en continu

    std::mutex  g_mutex;
    std::string g_typed;
    bool g_known = true;
    bool g_skipChar = false;   // le caractere de la touche T qui ouvre le chat n'est pas tape

    // En jeu (sans ecran), Minecraft cache le curseur et le bloque au centre.
    bool GameCapturesMouse(HWND hwnd) {
        CURSORINFO ci{ sizeof(ci) };
        if (GetCursorInfo(&ci) && (!(ci.flags & CURSOR_SHOWING) || !ci.hCursor)) return true;
        RECT clip{}, client{};
        if (GetClipCursor(&clip) && GetClientRect(hwnd, &client)) {
            const LONG w = clip.right - clip.left, h = clip.bottom - clip.top;
            if (w < 64 && h < 64) return true;
        }
        return false;
    }

    bool CapturedRecently() { return GetTickCount64() - g_lastCaptured.load() < 400; }

    void AppendUtf8(std::string& s, wchar_t c) {
        char buf[8];
        const int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, buf, sizeof(buf), nullptr, nullptr);
        if (n > 0) s.append(buf, n);
    }
    void PopUtf8(std::string& s) {
        while (!s.empty()) {
            const unsigned char c = static_cast<unsigned char>(s.back());
            s.pop_back();
            if ((c & 0xC0) != 0x80) break;   // debut du caractere UTF-8 atteint
        }
    }

    void Open(const char* initial, bool skipNextChar) {
        std::lock_guard lock(g_mutex);
        g_typed = initial;
        g_known = true;
        g_skipChar = skipNextChar;
        g_open = true;
        g_openedAt = GetTickCount64();
        g_capturedSince = 0;
    }
}

void ChatOverlay::OnMessage(HWND, UINT msg, WPARAM wp, LPARAM lp) {
    if (ChatSender::Busy()) return;   // Nexus est en train de taper : on ne suit pas ses propres touches
    if (GetMessageExtraInfo() == static_cast<LPARAM>(ChatSender::kInjectedTag)) return;

    if (!g_open) {
        if (!CapturedRecently()) return;           // un autre ecran (inventaire, pause...) est ouvert
        if (msg == WM_KEYDOWN && !(lp & (1 << 30)) && static_cast<int>(wp) == Config::GetSettings().chatKey)
            Open("", true);
        else if (msg == WM_CHAR && wp == L'/')      // "/" ouvre le chat deja pre-rempli
            Open("/", false);
        return;
    }

    std::lock_guard lock(g_mutex);
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_RETURN || wp == VK_ESCAPE) { g_open = false; g_typed.clear(); break; }
        if (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_HOME || wp == VK_END || wp == VK_DELETE ||
            wp == VK_UP || wp == VK_DOWN || wp == VK_TAB)
            g_known = false;                       // curseur deplace / historique : on ne suit plus exactement
        else if (GetKeyState(VK_CONTROL) < 0)
            g_known = false;                       // Ctrl+V, Ctrl+A, Ctrl+Retour...
        break;
    case WM_CHAR:
        if (g_skipChar) { g_skipChar = false; break; }
        if (wp == 8) PopUtf8(g_typed);             // Retour arriere
        else if (wp >= 32) AppendUtf8(g_typed, static_cast<wchar_t>(wp));
        break;
    }
}

void ChatOverlay::Sample(HWND hwnd) {
    if (!hwnd) return;
    const ULONGLONG now = GetTickCount64();
    if (GameCapturesMouse(hwnd)) {
        g_lastCaptured = now;
        if (g_open) {
            if (!g_capturedSince) g_capturedSince = now;
            // Le jeu a repris la souris durablement : le chat a ete ferme autrement
            if (now - g_openedAt > 600 && now - g_capturedSince > 300) g_open = false;
        }
    } else {
        g_capturedSince = 0;
    }
}

bool ChatOverlay::IsChatOpen() { return g_open; }

std::string ChatOverlay::Typed(bool& known) {
    std::lock_guard lock(g_mutex);
    known = g_known;
    return g_typed;
}

void ChatOverlay::Use(const std::string& text) {
    const bool complete = !text.empty() && text.back() == ' ';
    int toErase = -1;   // -1 = inconnu : on efface "large"
    {
        std::lock_guard lock(g_mutex);
        if (g_known) {
            const int n = MultiByteToWideChar(CP_UTF8, 0, g_typed.c_str(), -1, nullptr, 0);
            toErase = n > 0 ? n - 1 : 0;
        }
    }
    if (!ChatSender::Insert(text.c_str(), !complete, toErase)) return;
    std::lock_guard lock(g_mutex);
    g_typed = text;
    g_known = true;
    if (!complete) g_open = false;   // envoye : le chat se ferme
}

void ChatOverlay::SetHovered(bool v) { g_hovered = v; }
bool ChatOverlay::IsHovered() { return g_hovered && g_visible; }

void ChatOverlay::SetShortcutTexts(const std::string* texts, int count) {
    std::lock_guard lock(g_mutex);
    g_shortcutCount = count < 8 ? count : 8;
    for (int i = 0; i < g_shortcutCount; ++i) g_shortcuts[i] = texts[i];
}

bool ChatOverlay::HandleShortcut(int index) {
    std::string text;
    {
        std::lock_guard lock(g_mutex);
        if (!g_open || index < 0 || index >= g_shortcutCount) return false;
        text = g_shortcuts[index];
    }
    Use(text);
    return true;
}

void ChatOverlay::SetVisible(bool v) { g_visible = v; }
bool ChatOverlay::IsVisible() { return g_visible; }
