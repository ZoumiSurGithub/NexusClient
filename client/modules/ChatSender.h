#pragma once

// Envoi de messages / commandes dans le chat du jeu, exactement comme un joueur :
// fermeture du menu Nexus, touche du chat, saisie du texte, Entree. Aucun offset necessaire.
namespace ChatSender {
    // Marque des touches simulees par Nexus (GetMessageExtraInfo) : le suivi du chat les ignore.
    constexpr unsigned long long kInjectedTag = 0x4E455855ull; // "NEXU"

    bool Send(const char* utf8);                 // ouvre le chat, colle, Entree. false si occupe / vide
    // Chat DEJA ouvert : efface le texte actuel (eraseCount caracteres, -1 = inconnu),
    // tape le nouveau, puis Entree si enter.
    bool Insert(const char* utf8, bool enter, int eraseCount = -1);
    bool Busy();
}
