#pragma once

// Auto-sprint : simule la touche "Sprint" maintenue enfoncee.
// Le jeu applique toujours ses propres regles (avancer, faim, pas accroupi...).
namespace AutoSprint {
    bool Init();        // resout la signature et installe le hook (MinHook deja initialise)
    void Shutdown();
    bool IsConfigured();
}
