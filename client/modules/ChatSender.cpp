#include "ChatSender.h"
#include "core/Config.h"
#include "core/Logger.h"
#include "hooks/InputHook.h"
#include "ui/Ui.h"
#include <windows.h>
#include <atomic>
#include <string>

namespace {
    std::atomic<bool> g_busy{ false };
    std::wstring g_text;
    bool g_insertMode = false;   // true : le chat est deja ouvert, on remplace son contenu
    bool g_pressEnter = true;
    int  g_eraseCount = -1;

    std::wstring Utf8ToWide(const char* s) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        std::wstring w(n > 0 ? n - 1 : 0, L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
        return w;
    }

    // ---- Blocage du clavier physique pendant la saisie ---------------------------------
    // Un hook clavier bas niveau ecarte les appuis NON simules (LLKHF_INJECTED absent)
    // avant meme qu'ils n'atteignent le jeu, quelle que soit sa facon de lire le clavier
    // (messages, Raw Input ou GameInput). Les relachements passent : aucune touche ne reste "bloquee".
    HHOOK  g_kbHook = nullptr;
    DWORD  g_hookThreadId = 0;
    HANDLE g_hookThread = nullptr;
    HANDLE g_hookReady = nullptr;

    LRESULT CALLBACK LowLevelKeyboard(int code, WPARAM wp, LPARAM lp) {
        if (code == HC_ACTION) {
            const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
            const bool injected = (k->flags & LLKHF_INJECTED) != 0;
            const bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
            if (!injected && down) return 1;     // frappe du joueur : ignoree
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    DWORD WINAPI HookThread(LPVOID) {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&LowLevelKeyboard), &self);
        g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboard, self, 0);
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);   // cree la file de messages du thread
        SetEvent(g_hookReady);
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {}   // necessaire au fonctionnement du hook
        if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
        return 0;
    }

    void BlockKeyboard() {
        g_hookReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_hookThread = CreateThread(nullptr, 0, HookThread, nullptr, 0, &g_hookThreadId);
        if (g_hookThread) WaitForSingleObject(g_hookReady, 300);
        if (!g_kbHook) LOG_WARN("Chat : blocage du clavier impossible (le joueur peut taper pendant la saisie)");
    }

    void UnblockKeyboard() {
        if (g_hookThread) {
            PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
            WaitForSingleObject(g_hookThread, 1000);
            CloseHandle(g_hookThread);
            g_hookThread = nullptr;
        }
        if (g_hookReady) { CloseHandle(g_hookReady); g_hookReady = nullptr; }
    }

    // Touches envoyees par code de balayage : le jeu (GameInput / Raw Input) les voit
    // comme de vraies touches du clavier.
    void Key(WORD vk, bool down) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
        if (vk == VK_RMENU || vk == VK_RCONTROL || vk == VK_END || vk == VK_HOME || vk == VK_DELETE ||
            vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN)
            in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        in.ki.dwExtraInfo = static_cast<ULONG_PTR>(ChatSender::kInjectedTag);
        SendInput(1, &in, sizeof(in));
    }
    void Tap(WORD vk) { Key(vk, true); Sleep(35); Key(vk, false); Sleep(35); }

    // Saisie de texte avec de VRAIES touches : pour chaque caractere, on cherche la touche
    // (et les modificateurs Maj / AltGr) qui le produit sur la disposition du clavier
    // du joueur (AZERTY, QWERTY...), et on l'appuie. Le jeu lit ainsi exactement ce
    // qu'il lirait si tu tapais toi-meme. Repli Unicode pour les caracteres absents
    // du clavier (ex : lettres a accent mort comme "e circonflexe").
    void SendUnicode(wchar_t c) {
        INPUT in[2]{};
        for (int i = 0; i < 2; ++i) {
            in[i].type = INPUT_KEYBOARD;
            in[i].ki.wScan = c;
            in[i].ki.dwFlags = KEYEVENTF_UNICODE | (i ? KEYEVENTF_KEYUP : 0);
            in[i].ki.dwExtraInfo = static_cast<ULONG_PTR>(ChatSender::kInjectedTag);
        }
        SendInput(2, in, sizeof(INPUT));
    }

    void TypeText(const std::wstring& text, HKL layout) {
        for (wchar_t c : text) {
            const SHORT r = VkKeyScanExW(c, layout);
            if (r == -1) { SendUnicode(c); Sleep(8); continue; }

            const WORD vk = LOBYTE(r);
            const BYTE mods = HIBYTE(r);              // 1 = Maj, 2 = Ctrl, 4 = Alt (6 = AltGr)
            if (mods & ~7) { SendUnicode(c); Sleep(8); continue; }
            if (mods & 1) Key(VK_LSHIFT, true);
            if (mods & 2) Key(VK_LCONTROL, true);
            if (mods & 4) Key(VK_RMENU, true);
            if (mods) Sleep(6);
            Key(vk, true); Sleep(8); Key(vk, false);
            if (mods & 4) Key(VK_RMENU, false);
            if (mods & 2) Key(VK_LCONTROL, false);
            if (mods & 1) Key(VK_LSHIFT, false);
            Sleep(10);
        }
    }

    void Erase(int count) {
        if (count < 0) { Tap(VK_END); count = 100; }   // longueur inconnue : on se place a la fin et on efface large
        for (int i = 0; i < count; ++i) {
            Key(VK_BACK, true); Key(VK_BACK, false);
            Sleep(3);
        }
        Sleep(30);
    }

    DWORD WINAPI Worker(LPVOID) {
        // 1. Attendre que le menu Nexus soit ferme et que le jeu ait repris la main
        if (!g_insertMode) {
            for (int i = 0; i < 50 && Ui::IsMenuOpen(); ++i) Sleep(20);
            Sleep(180);
        }

        HWND game = InputHook::Window();
        DWORD fgPid = 0;
        HWND fg = GetForegroundWindow();
        if (fg) GetWindowThreadProcessId(fg, &fgPid);
        if (!game || fgPid != GetCurrentProcessId()) {
            LOG_WARN("Chat : Minecraft n'est pas au premier plan (fenetre active d'un autre programme), envoi annule");
            g_busy = false;
            return 0;
        }

        // Si le joueur tient encore Alt (raccourci Alt+1), Ctrl ou Maj, le jeu verrait
        // "Alt + lettre" et ignorerait la saisie : on attend qu'il les relache.
        for (int i = 0; i < 100; ++i) {
            if (GetAsyncKeyState(VK_MENU) >= 0 && GetAsyncKeyState(VK_CONTROL) >= 0 && GetAsyncKeyState(VK_SHIFT) >= 0) break;
            Sleep(15);
        }
        Sleep(30);

        BlockKeyboard();   // a partir d'ici, tes frappes ne se melangent plus au texte

        // 2. Ouvrir le chat (sauf s'il l'est deja) ou effacer ce qui y est tape
        if (!g_insertMode) {
            Tap(static_cast<WORD>(Config::GetSettings().chatKey));
            Sleep(280);                   // le temps que l'ecran de chat s'ouvre
        } else {
            Erase(g_eraseCount);
        }

        // 3. Taper le texte, puis valider
        const HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(game, nullptr));
        LOG_INFO("Chat : saisie de %zu caracteres (mode %s)", g_text.size(), g_insertMode ? "insertion" : "envoi");
        TypeText(g_text, layout);
        Sleep(60);
        if (g_pressEnter) Tap(VK_RETURN);
        Sleep(150);

        UnblockKeyboard();
        LOG_INFO("Chat : message envoye");
        g_busy = false;
        return 0;
    }
}

bool ChatSender::Insert(const char* utf8, bool enter, int eraseCount) {
    if (!utf8 || !*utf8 || g_busy) return false;
    std::wstring text = Utf8ToWide(utf8);
    for (auto& c : text) if (c == L'\r' || c == L'\n') c = L' ';
    if (text.size() > 256) text.resize(256);

    g_text = std::move(text);
    g_insertMode = true;
    g_pressEnter = enter;
    g_eraseCount = eraseCount;
    g_busy = true;
    if (HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) { CloseHandle(t); return true; }
    g_busy = false;
    return false;
}

bool ChatSender::Send(const char* utf8) {
    if (!utf8 || !*utf8 || g_busy) return false;
    g_insertMode = false;
    g_pressEnter = true;

    std::wstring text = Utf8ToWide(utf8);
    for (auto& c : text) if (c == L'\r' || c == L'\n') c = L' ';   // une seule ligne
    if (text.size() > 256) text.resize(256);

    g_text = std::move(text);
    g_busy = true;
    if (Ui::IsMenuOpen()) Ui::ToggleMenu();   // le chat du jeu doit recevoir les touches

    if (HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) { CloseHandle(t); return true; }
    g_busy = false;
    return false;
}

bool ChatSender::Busy() { return g_busy; }
