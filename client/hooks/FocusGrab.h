#pragma once

// Retire le focus au jeu pendant que le menu est ouvert. GameInput ne livre l'input
// qu'a la fenetre active : sans focus, le jeu ne recoit plus ni camera ni clics, quel
// que soit le canal (polling ou callback). Nexus prend le focus sur une petite fenetre
// a lui ; la souris survole toujours la fenetre du jeu, donc ImGui recoit les clics via
// le WndProc du jeu, et le clavier via notre fenetre (transmis a ImGui).
namespace FocusGrab {
    void Init();                 // cree la fenetre (apres InputHook::Init)
    void Shutdown();
    void OnMenuToggled(bool open);

    // Etat des modificateurs vu par notre hook clavier (fiable meme quand le hook bloque la
    // touche pour le jeu, contrairement a GetAsyncKeyState/GetKeyState).
    bool IsCtrlDown();
    bool IsShiftDown();
    bool IsAltDown();
}
