#pragma once

// Zoom : tant que la touche est maintenue (ou basculee), la camera est grossie par le
// niveau de zoom. On hooke LevelRenderer::renderLevel et on multiplie les champs fovX/fovY
// du LevelRendererPlayer (methode Latite, fiable sur cette version).
namespace Zoom {
    bool Init();              // MinHook deja initialise
    void Shutdown();
    void Update();            // chaque image (thread de rendu) : lit la touche
    bool IsConfigured();
    bool IsZooming();
    float CurrentFactor();   // facteur lisse courant (1 = pas de zoom), pour l'indicateur a l'ecran
}
