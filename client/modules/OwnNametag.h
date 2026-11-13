#pragma once

// Affiche ton propre nametag (pseudo au-dessus de la tete) en vue a la 3e personne (F5).
// Hook de la fonction du jeu qui decide si le nametag d'une entite doit etre affiche :
// pour le joueur local, on repond "oui".
namespace OwnNametag {
    bool Init();
    void Shutdown();
    bool IsConfigured();
}
