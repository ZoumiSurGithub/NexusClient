#pragma once
// Rendu des items via le moteur du jeu (ItemRenderer) : affiche la vraie texture
// de l'objet (vanilla OU custom serveur). Hook sur ScreenView::setupAndRender.
namespace ItemRender {
    bool Init();       // resout les signatures + installe le hook (MinHook deja initialise)
    void Shutdown();
    bool Active();     // true si le hook de rendu est en place (icones disponibles)
    void* ClientInstance();   // dernier ClientInstance capture (0 si pas encore vu)
    float GuiScaleFrac();     // 1/guiScale du jeu (pour calculer la taille de la hotbar, etc.)

    // File d'icones a dessiner, remplie par le HUD (ImGui) chaque image, lue par
    // le hook de rendu du jeu. Coordonnees en PIXELS ecran ; sizePx = taille voulue.
    void ClearIcons();
    void QueueIcon(void* itemStack, float screenX, float screenY, float sizePx);
    // Fond de panneau (dessine par le jeu, SOUS les icones) : remplissage + bordure.
    // Coordonnees et epaisseur en pixels ecran ; couleurs RGBA 0..1.
    void QueueBackground(float x, float y, float w, float h, float rad,
                         float fr, float fg, float fb, float fa,
                         float br, float bg, float bb, float ba, float borderPx);
}
