# Changelog

Chaque version publiée doit avoir sa section ici **avant** de pousser le tag : c'est ce texte que les joueurs voient dans le launcher au moment de la mise à jour (une ligne par point).

## [0.0.3] - 2026-11-17

- Nouveau module Capture d'écran : appuie sur ta touche au choix et l'image (avec ton HUD) est enregistrée, avec une confirmation à l'écran
- Captures disponibles aussi bien menu ouvert que fermé, et les raccourcis Windows (Impr. écran, Win+Maj+S) ne sont plus bloqués par le menu
- Nouveau logo Nexus : dans le launcher, le menu en jeu et l'icône de l'application
- La version du client s'affiche désormais dans l'en-tête du menu
- Interface du launcher peaufinée et finitions diverses

## [0.0.2] - 2026-11-16

- Par défaut, seuls le compteur FPS et la Rich Presence Discord sont activés — les autres modules s'activent à la demande
- Connexions rapides plus compactes, centrées et toujours visibles en haut du launcher, quel que soit l'onglet
- Logos des serveurs affichés sans déformation (recadrage propre), quels que soient leur taille et leur format (PNG, JPG…)
- Barre latérale des onglets fixe (plus de défilement intempestif)
- Notes de version récupérées directement depuis GitHub (onglet Nouveautés toujours à jour, sans réinstaller)
- Finitions et corrections d'interface

## [0.0.1] - 2026-11-16

Première version publique de Nexus, client Minecraft Bedrock (Windows).

- Launcher graphique moderne : barre latérale avec onglets Nouveautés, Paramètres et Journal, animations fluides (survol, clic, transitions)
- Choix de la version de Minecraft depuis le launcher, avec installation automatique au besoin
- Versions récupérées directement et à jour (via OnixClient) : téléchargement indépendant, installation uwp et msixvc, dépendances gérées
- Connexions rapides : serveurs mis en avant par l'équipe Nexus, un clic lance Minecraft et rejoint le serveur directement
- Le client ne s'injecte que sur les versions de Minecraft qu'il prend en charge
- Message d'accueil en jeu rappelant comment ouvrir le menu, à l'arrivée sur un monde ou un serveur
- HUD armure : durabilité des pièces et de l'objet en main, position et taille réglables
- Compteur FPS configurable (position, taille, couleurs)
- Compteur CPS, combos et reach (portée donnée et reçue)
- Indicateur de zoom à l'écran, zoom sur touche configurable (maintenir ou basculer)
- Barres de vie des entités (ne traversent pas les murs)
- Affichage de ton pseudo au-dessus de ta tête en vue F5
- Auto-sprint
- Chat : commandes rapides et pré-commandes affichées à côté du chat du jeu
- Rich Presence Discord (serveur, joueurs connectés, ping)
- HUD : système de position unifié (ancrage, marges, taille, opacité), compensation selon la résolution
- Apparence Rainbow : accent arc-en-ciel animé, appliqué au menu comme au HUD
- Repères jaunes « recommandé petit PC » sur les réglages les plus gourmands
- Menu en jeu et launcher entièrement en français, accents et caractères spéciaux affichés correctement
- Xbox Game Bar neutralisée pendant l'installation d'une version
- Mises à jour automatiques du launcher
