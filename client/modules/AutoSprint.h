#pragma once

// Auto-sprint : force le joueur local a courir en permanence (comme si la touche
// sprint etait tenue). Le jeu ne court que si tu avances, donc aucun effet a l'arret.
namespace AutoSprint {
    bool Init();        // conserve pour compatibilite (plus de hook) ; renvoie true
    void Shutdown();
    void Update();      // chaque frame : applique l'etat sprint selon le reglage
    bool IsConfigured();
}
