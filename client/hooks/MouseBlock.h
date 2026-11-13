#pragma once

// Empeche la camera du joueur de bouger quand le menu du client est ouvert.
//
// Bedrock (version GDK) lit la souris via GameInput, pas via les messages de la
// fenetre. On hooke donc IGameInputReading::GetMouseState et on "gele" les
// positions cumulees tant que le menu est ouvert. A la fermeture, un decalage
// est applique pour que la camera ne saute pas.
// En complement : Raw Input (GetRawInputData), ClipCursor et SetCursorPos.
namespace MouseBlock {
    void Init();             // hooks Win32 (MinHook deja initialise)
    void Tick();             // a appeler regulierement : detecte GameInput des qu'une lecture souris existe
    const char* Status();    // texte pour le panneau Diagnostic
    bool GameInputHooked();

    // Compteurs d'appels (pour savoir quelle voie le jeu utilise vraiment)
    enum Counter { GameInputMouse, RawInputData, RawInputBuffer, ForegroundCheck, CounterCount };
    const char* CounterName(int c);
    unsigned PerSecond(int c);   // appels par seconde, mesures sur la derniere seconde
}
