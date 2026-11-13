#pragma once
#include <windows.h>
#include <string>

// Detecte l'ouverture du chat de Minecraft (sans offset) et suit ce que le joueur tape,
// pour afficher les pre-commandes correspondantes a cote de la barre de saisie.
//
// Detection : quand la touche du chat (T) ou "/" est pressee alors que le jeu tenait
// la souris capturee (en jeu, sans ecran ouvert), le chat vient de s'ouvrir. Il se
// ferme avec Entree, Echap, ou quand le jeu recapture la souris.
namespace ChatOverlay {
    void OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp); // depuis le WndProc (menu Nexus ferme)
    void Sample(HWND hwnd);                                    // chaque image : etat de capture de la souris

    bool IsChatOpen();
    std::string Typed(bool& known);   // texte tape dans le chat (known = false si on a perdu le fil)

    // Utilise une pre-commande : remplace le texte du chat. Si le texte se termine par
    // un espace (ex : "/tp "), on le laisse a completer ; sinon on l'envoie.
    void Use(const std::string& text);

    void SetVisible(bool v);          // le panneau est-il affiche cette image ?
    bool IsVisible();
    void SetHovered(bool v);          // souris sur le panneau : le jeu ne doit pas voir les clics
    bool IsHovered();

    // Raccourcis Alt+1..8 : le rendu publie la liste affichee, le WndProc declenche
    void SetShortcutTexts(const std::string* texts, int count);
    bool HandleShortcut(int index);   // index 0..7 ; false si rien a cet index
}
