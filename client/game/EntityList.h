#pragma once
#include <vector>
// Enumeration des entites du monde via ClientInstance -> Minecraft -> GameSession
// -> Level -> getRuntimeActorList(). Reutilise pour les barres de vie.
namespace EntityList {
    // Renvoie la liste des Actor* du monde (vide si indisponible). Ne libere PAS le
    // vecteur renvoye par le jeu (allocateurs differents) -> on copie puis on oublie.
    std::vector<void*> Collect();

    // Sonde de diagnostic : journalise la marche jusqu'au Level + le nombre d'entites.
    void Probe();
}
