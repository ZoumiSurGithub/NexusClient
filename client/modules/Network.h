#pragma once
#include <string>
#include <cstdint>

// Detection du serveur multijoueur, sans offset :
//  - hooks Winsock (sendto, WSASendTo, connect/send) : le jeu parle en UDP (RakNet) au
//    serveur ; la destination la plus active = le serveur sur lequel tu joues ;
//  - hooks DNS (getaddrinfo...) : on retrouve le nom que tu as tape (play.exemple.net)
//    a partir de l'adresse IP ;
//  - ping RakNet "Unconnected Ping" (le meme que la liste des serveurs du jeu) :
//    nombre de joueurs connectes, maximum et MOTD.
// Solo : un monde local ouvre le port UDP 19132/19133 (partage LAN) ; ou le joueur
// local est connu via les offsets [player].
namespace Network {
    struct ServerInfo {
        bool        active = false;
        std::string host;          // nom tape (si connu) ou adresse IP
        std::string ip;
        uint16_t    port = 0;
        int         online = -1;   // -1 = inconnu (le serveur n'a pas repondu au ping)
        int         maxPlayers = -1;
        std::string motd;          // sans les codes couleur
        int         pingMs = -1;   // latence mesuree (aller-retour), -1 = inconnue
        unsigned long long since = 0; // GetTickCount64 de l'arrivee sur ce serveur
    };

    void Init();                   // installe les hooks (MinHook deja initialise)
    void Tick();                   // a appeler regulierement (thread Discord) : ping du serveur
    ServerInfo GetServer();
    bool IsHostingWorld();         // monde local ouvert
    std::string DebugText();       // pour le panneau Diagnostic
}
