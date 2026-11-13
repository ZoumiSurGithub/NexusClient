# Nexus

Nexus est un client (launcher + DLL) pour **Minecraft Bedrock (Windows)** qui ajoute au HUD :

- la **durabilité de chaque pièce d'armure** (barre colorée + valeur et/ou %, clignote quand elle est basse) ;
- la durabilité de l'**objet en main** ;
- un **compteur FPS** ;
- une page **Chat** : envoyer un message ou une commande, et des **commandes rapides** personnalisables (enregistrées dans `commands.txt`), qui s'affichent aussi à côté du chat de Minecraft quand tu l'ouvres ;
- un **menu** (touche INSER) pour tout régler et déplacer les éléments à la souris.

Rendu : hook DirectX 12 (repli DirectX 11) + Dear ImGui. Hooks : MinHook.

## Publication et mises à jour

- **Joueurs** : voir [JOUEURS.md](JOUEURS.md) (installation, touches, mises à jour). Ce fichier est inclus dans le zip de chaque Release sous le nom `LISEZMOI.md`.
- **Publier une version** : voir [PUBLIER.md](PUBLIER.md). En résumé, ajoute une section au [CHANGELOG](CHANGELOG.md), pousse un tag `vX.Y.Z`, et GitHub compile et publie. Les launchers installent ensuite la mise à jour automatiquement (elle est obligatoire).

## Compiler

Prérequis : **Visual Studio 2022** (charge de travail « Développement Desktop en C++ ») et CMake ≥ 3.20 (inclus dans VS). Les dépendances (MinHook, ImGui) sont téléchargées automatiquement au premier build.

```bat
cmake -B build -A x64
cmake --build build --config Release
```

Ou ouvre simplement le dossier dans Visual Studio (*Fichier → Ouvrir → Dossier*), choisis **x64-Release**, puis *Générer tout*.

Résultat dans `build/Release/` :

```
NexusLauncher.exe
Nexus.dll
offsets.ini
```

## Discord Rich Presence

Ton profil Discord affiche « Joue à Nexus » avec : *Dans les menus*, *En solo* ou *Sur un serveur* (adresse `nom:port` et joueurs connectés).

Rien à configurer : Nexus utilise l'application Discord **Nexus** (ID `1570931183657685075`) et son logo `discord_nexus`. Discord doit simplement être ouvert sur le PC, avec le partage d'activité activé.

Le serveur est détecté par le trafic réseau du jeu ; le nombre de joueurs vient du même « ping » que la liste des serveurs de Minecraft. Tu peux masquer l'adresse ou les joueurs dans le menu.

## Logo NEXUS sur l'écran d'accueil

`NexusTitle.mcpack` (copié à côté du launcher) est un pack de ressources qui remplace le logo MINECRAFT par NEXUS :

1. Double-clique sur `NexusTitle.mcpack` : Minecraft l'importe.
2. Dans Minecraft : *Paramètres → Ressources globales*, active **Nexus Titre**.

Les sources du pack sont dans `resourcepack/NexusTitle/`. Les images (icône, logo) sont dans `assets/` et sont générées par `assets/make_assets.py`.

## Utiliser

1. **Configure `offsets.ini`** pour ta version du jeu ([guide](docs/TROUVER_LES_OFFSETS.md)). Sans ça, l'overlay fonctionne (FPS, menu, mode démo) mais l'armure n'est pas lue.
2. Lance `NexusLauncher.exe`. Il démarre Minecraft si besoin, attend le chargement du rendu, puis injecte la DLL.
3. En jeu :

| Touche | Action |
|---|---|
| **INSER** | Ouvrir/fermer le menu (mieux depuis l'écran pause, quand la souris est libre) |
| **Échap** | Fermer le menu |
| **FIN** | Décharger la DLL |

Pendant que le menu est ouvert, fais glisser le HUD d'armure et le compteur FPS pour les placer. Les réglages sont enregistrés dans `client.ini` à la fermeture du menu.

**Premier test conseillé** : active *Diagnostic → Mode démo*. Des valeurs factices s'affichent, ce qui confirme que l'injection et le rendu fonctionnent avant de chercher les offsets.

## Structure

```
launcher/main.cpp          Lancement du jeu + injection (LoadLibraryW / CreateRemoteThread)
client/dllmain.cpp         Thread principal, chargement et déchargement propres
client/core/               Logger, Config (offsets.ini / client.ini), scanner de signatures
client/hooks/RenderHook    Present / Present1 / ResizeBuffers / ExecuteCommandLists, ImGui DX12/DX11
client/hooks/InputHook     WndProc : touches, souris pour le menu
client/game/Game           Hook joueur + lecture de l'armure (appels protégés par __try/__except)
client/ui/Hud.cpp          Dessin du HUD d'armure et des FPS
client/ui/Menu.cpp         Menu de configuration
client/modules/AutoSprint Auto-sprint (hook du tick des touches de déplacement)
offsets.ini                Signatures et offsets à adapter à chaque version du jeu
```

**Ajouter un module** (coordonnées, horloge, CPS…) : ajoute une fonction `Hud::RenderXxx(bool edit)` dans `Hud.cpp` sur le modèle de `RenderFps`, appelle-la dans `Ui::Render()` (`Menu.cpp`), puis ajoute ses réglages dans `Settings` (`Config.h`) et dans le menu.

## Dépannage

| Problème | Piste |
|---|---|
| `OpenProcess` / `CreateRemoteThread` échoue | Lance le launcher en administrateur ; vérifie que l'antivirus ne le bloque pas (l'injection de DLL est souvent signalée) |
| Rien ne s'affiche | Regarde `Nexus.log` à côté de la DLL. « Overlay initialisé » doit y apparaître |
| Le menu ne réagit pas à la souris | Ouvre-le depuis l'écran pause (Échap) : en jeu, Minecraft capture la souris |
| Armure non affichée | Menu → Diagnostic : complète les lignes `[--]` dans `offsets.ini`, puis réinjecte (FIN, puis relance le launcher) |
| Le jeu plante après une mise à jour | Une signature pointe au mauvais endroit : vide les lignes de `[functions]` et retrouve-les |

## Avertissement

Les mods injectés ne sont pas pris en charge par Mojang. Ce HUD ne donne aucun avantage de jeu, mais certains serveurs (The Hive, CubeCraft…) peuvent sanctionner tout client modifié. Utilise-le en solo, sur tes propres mondes ou sur des serveurs qui l'autorisent.

## Crédits

- **Code du client** (launcher, DLL, hooks de rendu et d'entrées, menu, modules, outils de génération d'images) : **Claude**, l'IA d'Anthropic.
- **Signatures, offsets et tests en jeu** : **Zoumi**.

Pour changer ces noms, modifie `client/Credits.h` (menu) et la bannière dans `launcher/main.cpp`.
