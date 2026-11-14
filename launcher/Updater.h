#pragma once
#include <filesystem>

// Mises a jour obligatoires via GitHub Releases.
//
// A chaque lancement, le launcher lit le manifeste de la derniere version publiee :
//   https://github.com/<depot>/releases/latest/download/nexus-update.txt
// Format (une ligne par entree) :
//   version=1.2.0
//   note=Texte du changelog (une ligne par note)
//   package=Nexus-v1.2.0.zip|<sha256>
//   apply=Nexus.dll
//   apply=NexusLauncher.exe
// Si la version publiee est plus recente, le zip est telecharge, son empreinte
// SHA-256 verifiee, puis les fichiers "apply" en sont extraits et remplaces ;
// le launcher se relance ensuite tout seul.
// Les fichiers du joueur (client.ini, commands.txt, Nexus.log) ne sont jamais touches.
namespace Updater {
    enum class Result {
        UpToDate,       // rien a faire, on peut jouer
        Updated,        // mise a jour installee, nouveau launcher relance : quitter
        Failed,         // mise a jour obligatoire impossible : ne pas lancer le jeu
        Offline,        // impossible de verifier (pas d'Internet, GitHub indisponible) : on joue quand meme
        DevBuild,       // version de developpement : pas de verification
    };

    void CleanupOldFiles(const std::filesystem::path& dir);   // supprime les *.old d'une mise a jour precedente
    Result Run(const std::filesystem::path& dir, const std::filesystem::path& selfExe);
}
