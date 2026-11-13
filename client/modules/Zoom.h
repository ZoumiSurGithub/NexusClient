#pragma once

// Zoom : tant que la touche est maintenue (ou basculee), le champ de vision est divise
// par le niveau de zoom. Hook de la fonction du jeu qui renvoie le FOV de la camera.
namespace Zoom {
    bool Init();              // MinHook deja initialise
    void Shutdown();
    void Update();            // chaque image (thread de rendu) : lit la touche
    bool IsConfigured();
    bool IsZooming();
}
