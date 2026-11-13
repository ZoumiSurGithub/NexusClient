# Nexus – Client Minecraft Bedrock

Client pour **Minecraft Bedrock sur Windows** : HUD d'armure, compteur FPS, barres de vie, zoom, commandes rapides, Rich Presence Discord…

Créé par **Claude** (Anthropic) · signatures et tests : **Zoumi**.
*Nexus n'est ni affilié ni approuvé par Mojang ou Microsoft.*

## Installation

1. Télécharge **`Nexus-vX.Y.Z.zip`** depuis la page des versions (Releases).
2. Extrais-le dans un dossier à toi (ex. `Documents\Nexus`). **Ne le lance pas depuis le zip.**
3. Double-clique sur **`NexusLauncher.exe`**.
   - Si Windows affiche « Windows a protégé votre ordinateur » : *Informations complémentaires* → *Exécuter quand même*. Le launcher n'est pas signé numériquement (un certificat coûte cher pour un projet gratuit).
   - Certains antivirus signalent Nexus car il s'injecte dans le jeu (c'est le principe de tous les clients Bedrock). Tu peux ajouter le dossier de Nexus aux exceptions.
4. Le launcher ouvre Minecraft et charge Nexus.

**Optionnel :** double-clique sur `NexusTitle.mcpack`, puis active **Nexus Titre** dans *Paramètres → Ressources globales* pour avoir le logo NEXUS sur l'écran d'accueil.

## Utilisation

| Touche | Action |
|---|---|
| **INSER** | Ouvrir / fermer le menu Nexus |
| **FIN** | Décharger Nexus |
| **C** (modifiable) | Zoom |
| **Alt + 1…8** | Pré-commande quand le chat est ouvert |

Lance toujours Minecraft **avec `NexusLauncher.exe`** pour avoir Nexus.

## Mises à jour

Le launcher vérifie les mises à jour **à chaque lancement**. Quand une nouvelle version existe, elle est **obligatoire** : le launcher la télécharge, vérifie son intégrité, l'installe puis redémarre tout seul. Tes réglages (`client.ini`) et tes commandes (`commands.txt`) sont conservés.

Après une mise à jour de **Minecraft**, certaines fonctions (armure, barres de vie, zoom…) peuvent être grisées le temps qu'une mise à jour de Nexus sorte : le reste continue de marcher.

## Sur les serveurs

Nexus n'ajoute pas d'avantage injuste (les barres de vie ne s'affichent pas à travers les murs), mais certains serveurs interdisent **tout** client modifié. Respecte les règles des serveurs sur lesquels tu joues.

## Désinstaller

Supprime simplement le dossier de Nexus. Minecraft n'est pas modifié.
