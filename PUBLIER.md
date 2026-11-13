# Publier Nexus (guide du mainteneur)

La publication est **automatique** : tu pousses un tag `vX.Y.Z` sur GitHub, GitHub compile Nexus et crée la Release, puis les launchers des joueurs installent la mise à jour au lancement suivant.

## Mise en place (une seule fois)

1. Crée un dépôt GitHub **public**, par exemple `Zoumi/Nexus`. Les mises à jour se téléchargent depuis ses Releases, donc il doit être public.
2. Si le dépôt n'est pas `Zoumi/Nexus`, modifie `NEXUS_UPDATE_REPO` dans `CMakeLists.txt`. Ce n'est utile que pour les compilations locales : GitHub Actions utilise automatiquement le bon dépôt.
3. Envoie le projet :
   ```bat
   git init
   git add .
   git commit -m "Nexus 1.0.0"
   git branch -M main
   git remote add origin https://github.com/Zoumi/Nexus.git
   git push -u origin main
   ```
4. Sur GitHub : *Settings → Actions → General → Workflow permissions* → **Read and write permissions**. Le workflow en a besoin pour créer les Releases.

## Publier une version

1. Ajoute une section en haut de `CHANGELOG.md` :
   ```markdown
   ## [1.1.0] - 2026-11-20

   - Signatures mises à jour pour Minecraft 26.70
   - Nouveau : ...
   ```
   Le workflow **refuse** de publier si la section manque. Ces lignes s'affichent dans le launcher des joueurs.
2. Commit, puis crée et pousse le tag :
   ```bat
   git add .
   git commit -m "Nexus 1.1.0"
   git tag v1.1.0
   git push origin main v1.1.0
   ```
3. Suis la compilation dans l'onglet **Actions** du dépôt (environ 5 minutes). La Release apparaît ensuite dans **Releases** avec :
   - `Nexus-v1.1.0.zip` : l'installation complète pour les nouveaux joueurs ;
   - `NexusLauncher.exe`, `Nexus.dll`, `offsets.ini`, `NexusTitle.mcpack` : les fichiers mis à jour ;
   - `nexus-update.txt` : le manifeste (version, notes, empreintes SHA-256) lu par les launchers.

## Numéros de version

- **Correctif** (signatures après une mise à jour de Minecraft, bug) : `1.0.0` → `1.0.1`.
- **Nouvelle fonction** : `1.0.1` → `1.1.0`.
- **Gros changement** : `1.1.0` → `2.0.0`.

Les compilations locales portent la version `0.0.0-dev` et ne forcent jamais de mise à jour. Tu peux donc tester tranquillement.

## Après une mise à jour de Minecraft

1. Retrouve les signatures cassées (onglet *Diagnostic* et `Nexus.log`) et corrige `offsets.ini`.
2. Teste en local.
3. Publie un correctif (`v1.0.1`). Tous les joueurs reçoivent le nouveau `offsets.ini` automatiquement, sans rien recompiler de leur côté.

## Comment fonctionne la mise à jour obligatoire

Au lancement, `NexusLauncher.exe` :
1. lit `https://github.com/<dépôt>/releases/latest/download/nexus-update.txt` ;
2. si la version est plus récente que la sienne, télécharge chaque fichier listé et **vérifie son SHA-256** ;
3. renomme les anciens fichiers en `.old`, ce qui fonctionne même si le launcher tourne ou si la DLL est chargée dans le jeu, met les nouveaux à la place, puis se relance ;
4. supprime les `.old` au lancement suivant.

Si la mise à jour échoue, le launcher **refuse de lancer Nexus** et affiche le lien vers la Release pour une installation manuelle. S'il ne peut pas joindre GitHub (pas d'Internet), il laisse jouer avec la version installée.

Le manifeste ne peut jamais contenir `client.ini`, `commands.txt` ou `Nexus.log` : les réglages des joueurs ne sont jamais écrasés.

## Bon à savoir

- **Antivirus / SmartScreen** : un launcher non signé qui injecte une DLL sera parfois signalé. Pour réduire le problème : un certificat de signature de code (payant), et signaler les faux positifs aux éditeurs d'antivirus (Microsoft, Avast…) avec le lien de la Release.
- **Licence** : ajoute un fichier `LICENSE` pour dire ce que les autres ont le droit de faire avec le code (par exemple MIT si tu veux qu'il soit libre, ou « tous droits réservés »).
- **Mojang** : ne distribue jamais de fichiers du jeu, et précise que Nexus n'est pas officiel (c'est déjà fait dans `JOUEURS.md`).
