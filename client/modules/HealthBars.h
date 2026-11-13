#pragma once

// Barres de vie au-dessus des entites proches (joueurs, animaux, monstres...),
// avec filtres par categorie et distance maximale.
namespace HealthBars {
    bool Init();          // resout les signatures, installe le hook entites (MinHook deja initialise)
    void Shutdown();
    void Render();        // appele chaque image depuis Ui::Render (thread de rendu)
    bool IsConfigured();
    const char* MissingText();  // ce qu'il manque dans offsets.ini, pour le menu
}
