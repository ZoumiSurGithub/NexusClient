#pragma once
#include <string>

// Rich Presence Discord via l'IPC local de Discord (tube nomme \\.\pipe\discord-ipc-N),
// sans SDK. Affiche : dans les menus / en solo / sur un serveur (adresse, joueurs).
// Necessite l'ID d'une application Discord (voir le README).
namespace DiscordRPC {
    void Start();            // thread de fond
    void Stop();             // efface la presence et ferme la connexion
    std::string Status();    // etat de la connexion, pour le menu
    void Preview(std::string& details, std::string& state, std::string& hover); // ce qui est affiche
}
