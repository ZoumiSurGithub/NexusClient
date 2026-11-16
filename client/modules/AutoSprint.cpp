#include "AutoSprint.h"
#include "core/Config.h"
#include "core/Logger.h"
#include "game/Game.h"
#include <windows.h>

// Methode : chaque frame, on force le bit "sprintDown" du MoveInputComponent du
// joueur local (via l'ECS). Plus besoin de hooker une fonction de tick : on ecrit
// directement l'etat d'input, ce qui est robuste et sans signature a trouver.
// Le jeu n'applique le sprint que si le joueur avance -> rien a l'arret.

bool AutoSprint::Init() {
    LOG_INFO("Auto-sprint : pret (ecriture directe de l'input)");
    return true;
}

void AutoSprint::Shutdown() {
    Game::SetMoveSprint(false);   // relache au dechargement
}

void AutoSprint::Update() {
    // L'ecriture du sprint se fait dans le hook joueur (thread du jeu), pas ici.
}

bool AutoSprint::IsConfigured() { return true; }
