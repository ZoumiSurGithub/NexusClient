#pragma once
#include <cstdint>
#include <string>

// Capture d'ecran du jeu : lit le back buffer dans le hook de rendu (RenderHook)
// et enregistre un PNG dans %APPDATA%\Nexus\screenshots.
namespace Screenshot {
    // A appeler chaque image (thread de rendu) : vrai au moment ou la touche assignee est pressee
    // (menu ferme : detecte via GetAsyncKeyState ; menu ouvert : via RequestCapture() du hook clavier).
    bool PollRequest();

    // Demande une capture (appelee par le hook clavier quand le menu est ouvert, car la touche
    // y est bloquee pour le jeu et GetAsyncKeyState peut ne pas la voir).
    void RequestCapture();

    // Enregistre une image (pixels BGRA ou RGBA selon dxgiFormat). Les pixels sont copies
    // immediatement ; l'encodage PNG se fait sur un thread dedie (ne bloque pas le rendu).
    void Save(unsigned width, unsigned height, uint32_t dxgiFormat, const void* pixels, unsigned rowPitch);

    // Dossier des captures (%APPDATA%\Nexus\screenshots), cree au besoin.
    std::wstring Folder();

    // Instant (GetTickCount64) de la derniere capture, pour afficher une confirmation (flash + toast).
    unsigned long long LastShotTick();
}
