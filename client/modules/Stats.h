#pragma once

// CPS (clics par seconde) et combo, a partir des clics physiques du joueur.
// Un petit fil echantillonne les boutons toutes les ~3 ms pour rester precis meme
// en clic rapide. Aucun offset necessaire.
namespace Stats {
    void Init();
    void Shutdown();
    int  LeftCps();        // clics gauche dans la derniere seconde
    int  RightCps();       // clics droit dans la derniere seconde
    int  Combo();          // coups consecutifs portes sur une entite (0 si la serie a expire)
    int  MaxCombo();       // plus long combo depuis l'injection
    float Reach();         // distance (blocs) du dernier coup qu'on a porte (0 apres 3 s)
    float ReachTaken();    // distance (blocs) du dernier coup recu = portee de l'attaquant (0 apres 3 s)
}
