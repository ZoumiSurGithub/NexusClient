#pragma once

// Better Nametag : greffe la vie (et plus) sur le nametag natif du jeu en
// modifiant la chaine de texte au moment du rendu (BaseActorRenderer::renderText).
// Avantage : position EXACTE calculee par le jeu -> zero decalage, toujours propre.
namespace BetterNametag {
    bool Init();
    void Shutdown();
    bool IsConfigured();
}
