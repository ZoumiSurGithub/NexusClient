#pragma once

struct ImFont;
struct ImVec4;

namespace Ui {
    void Render();          // appele chaque image entre ImGui::NewFrame et ImGui::Render
    void ToggleMenu();
    bool IsMenuOpen();
    inline ImFont* TitleFont = nullptr;   // Segoe UI Semibold, charge par RenderHook
    ImVec4 AccentColor(float a = 1.f);    // couleur d'accent courante (anime si Rainbow actif)
}

namespace Hud {
    void RenderArmor(bool editMode);
    void RenderFps(bool editMode);
    void RenderCpsCombo(bool editMode);   // CPS + Combo dans un seul panneau
    void RenderZoom(bool editMode);       // indicateur de zoom (visible en zoom, placable en edition)
    void RenderWelcome();                 // message d'accueil (toast) a l'arrivee dans un monde/serveur
    void RenderScreenshotToast();         // flash + confirmation apres une capture d'ecran
    bool IsDragging();        // un element du HUD est en train d'etre deplace (menu ouvert)
    void ResetDrag();
}

namespace Menu {
    void Render();
}
