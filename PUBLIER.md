# Publier Nexus (guide du mainteneur)

La publication est **automatique** : tu pousses un tag `vX.Y.Z` sur GitHub, GitHub compile Nexus et crée la Release, puis les launchers des joueurs installent la mise à jour au lancement suivant.

## Mise en place (une seule fois)

1. Crée un dépôt GitHub **public**, par exemple `ZoumiSurGithub/NexusClient`. Les mises à jour se téléchargent depuis ses Releases, donc il doit être public.
2. Si le dépôt n'est pas `ZoumiSurGithub/NexusClient`, modifie `NEXUS_UPDATE_REPO` dans `CMakeLists.txt`. Ce n'est utile que pour les compilations locales : GitHub Actions utilise automatiquement le bon dépôt.
3. Envoie le projet :
   ```bat
   git init
   git add .
   git commit -m "Nexus 1.0.0"
   git branch -M main
   git remote add origin https://github.com/ZoumiSurGithub/NexusClient.git
   git push -u origin main
   ```
4. Sur GitHub : *Settings → Actions → General → Workflow permissions* → **Read and write permissions**. Le workflow en a besoin pour créer les Releases.
5. **Dépose tes signatures en secret** (elles ne seront jamais dans le dépôt public) : *Settings → Secrets and variables → Actions → New repository secret*.
   - **Name** : `OFFSETS_INI`
   - **Secret** : colle **tout le contenu** de ton `offsets.ini` rempli (ouvre-le, Ctrl+A, Ctrl+C).
   - Quand tu mets à jour tes signatures (après une maj de Minecraft), modifie ce secret, puis republie.

## Où vivent les signatures

Les signatures sont ton travail de rétro-ingénierie ; Nexus les protège à trois niveaux :

- **Jamais dans le dépôt public.** `offsets.ini` est dans `.gitignore`. Seul `offsets.example.ini` (le modèle, sans signatures) est versionné. Ta copie locale remplie reste sur ton PC.
- **Fournies au build via le secret `OFFSETS_INI`.** GitHub reconstitue `offsets.ini` juste le temps de compiler, puis le jette.
- **Intégrées à la DLL, obfusquées (XOR).** Elles ne sont **pas** livrées en fichier à côté de la DLL. Les joueurs reçoivent les signatures sans pouvoir les lire ni les modifier. La DLL les déchiffre en mémoire au lancement (via un fichier temporaire effacé aussitôt).

> L'obfuscation XOR **ralentit** la copie, elle ne la rend pas impossible : quelqu'un de déterminé qui analyse la DLL peut retrouver les signatures. Pour un client gratuit, c'est le compromis habituel. Une protection plus forte (packer, chiffrement avec clé distante) existe mais complique la maintenance.

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
3. Suis la compilation dans l'onglet **Actions** du dépôt (environ 5 minutes). La Release ne contient que **deux fichiers**, pour rester simple côté joueurs :
   - **`Nexus-v1.1.0.zip`** : ce que les joueurs téléchargent et extraient (il sert aussi de source aux mises à jour) ;
   - `nexus-update.txt` : le petit manifeste lu par les launchers (version, notes, empreinte du zip). Les joueurs l'ignorent.

## Numéros de version

- **Correctif** (signatures après une mise à jour de Minecraft, bug) : `1.0.0` → `1.0.1`.
- **Nouvelle fonction** : `1.0.1` → `1.1.0`.
- **Gros changement** : `1.1.0` → `2.0.0`.

Les compilations locales portent la version `0.0.0-dev` et ne forcent jamais de mise à jour. Tu peux donc tester tranquillement.

## Après une mise à jour de Minecraft

1. Retrouve les signatures cassées (onglet *Diagnostic* et `Nexus.log`) et corrige ton `offsets.ini` **local**.
2. Teste en local (ton `offsets.ini` à côté de la DLL est prioritaire sur les signatures intégrées).
3. **Mets à jour le secret `OFFSETS_INI`** avec le nouveau contenu.
4. Ajoute une section au CHANGELOG, puis publie un correctif (`v1.0.1`). Les joueurs reçoivent la nouvelle `Nexus.dll` (signatures intégrées) automatiquement.

## Comment fonctionne la mise à jour obligatoire

Au lancement, `NexusLauncher.exe` :
1. lit `https://github.com/<dépôt>/releases/latest/download/nexus-update.txt` ;
2. si la version est plus récente que la sienne, télécharge le **zip** de la release et **vérifie son SHA-256** ;
3. en extrait les fichiers listés (`apply=...`), renomme les anciens en `.old` — ce qui marche même si le launcher tourne ou si la DLL est chargée dans le jeu — met les nouveaux à la place, puis se relance ;
4. supprime les `.old` au lancement suivant.

Si la mise à jour échoue, le launcher **refuse de lancer Nexus** et affiche le lien vers la Release pour une installation manuelle. S'il ne peut pas joindre GitHub (pas d'Internet), il laisse jouer avec la version installée.

Le manifeste ne peut jamais désigner `client.ini`, `commands.txt`, `Nexus.log` ni `offsets.ini` : les réglages des joueurs ne sont jamais écrasés, et les signatures voyagent **à l'intérieur** de `Nexus.dll` (dans le zip).

## Bon à savoir

- **Antivirus / SmartScreen** : un launcher non signé qui injecte une DLL sera parfois signalé. Pour réduire le problème : un certificat de signature de code (payant), et signaler les faux positifs aux éditeurs d'antivirus (Microsoft, Avast…) avec le lien de la Release.
- **Licence** : ajoute un fichier `LICENSE` pour dire ce que les autres ont le droit de faire avec le code (par exemple MIT si tu veux qu'il soit libre, ou « tous droits réservés »).
- **Mojang** : ne distribue jamais de fichiers du jeu, et précise que Nexus n'est pas officiel (c'est déjà fait dans `JOUEURS.md`).
