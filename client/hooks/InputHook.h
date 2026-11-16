#pragma once
#include <windows.h>

// Sous-classe la fenetre du jeu : touches INSER (menu) / FIN (dechargement),
// et transmet souris/clavier a ImGui quand le menu est ouvert.
namespace InputHook {
    void Init(HWND hwnd);
    void Shutdown();
    void NotifyMenuChanged(bool open);
    HWND Window();                      // fenetre du jeu (nullptr avant l'initialisation)

    // ImGui n'est pas thread-safe : les messages recus sur le thread de la fenetre sont mis
    // en file, puis transmis a ImGui sur le thread de rendu (avant ImGui::NewFrame).
    void FlushToImGui();
    void QueueMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);   // alimente ImGui depuis une autre fenetre  // simule une perte / un retour de focus pour le jeu
}
