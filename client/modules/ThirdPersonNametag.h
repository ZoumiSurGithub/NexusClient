#pragma once

// Affiche ton propre nametag au-dessus de toi en 3e personne (F5).
// Methode : NOP du saut conditionnel qui, normalement, saute le rendu du
// nametag du joueur local (patch d'octets, comme Latite).
namespace ThirdPersonNametag {
    bool Init();              // resout les signatures
    void Shutdown();          // restaure les octets d'origine
    void Update();            // chaque frame : patch uniquement en 3e personne
    bool IsConfigured();
}
