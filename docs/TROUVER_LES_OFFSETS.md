# Trouver les offsets et signatures

Bedrock n'a pas d'API de mod et ne fournit pas de symboles. Pour lire l'armure, la DLL doit retrouver quelques fonctions du jeu par **signature** (une suite d'octets unique dans `Minecraft.Windows.exe`). Ces signatures changent avec les mises à jour du jeu. Ce guide explique comment les retrouver et les placer dans `offsets.ini`, sans recompiler.

## Outils

| Outil | Utilité |
|---|---|
| **IDA Free** ou **Ghidra** | Désassembler `Minecraft.Windows.exe`, suivre les appels, générer des signatures |
| Plugin **SigMaker** (IDA) / script **MakeSig** (Ghidra) | Générer une signature unique avec jokers |
| **Cheat Engine** | Trouver les valeurs en mémoire (durabilité) et « qui accède à cette adresse » |
| **ReClass.NET** | Explorer les structures (GameMode, Player, ItemStack) en direct |

L'exécutable de la version GDK se trouve généralement dans `C:\XboxGames\Minecraft for Windows\Content\Minecraft.Windows.exe`. S'il est illisible (protégé ou chiffré), dumpe-le depuis la mémoire pendant que le jeu tourne (Scylla, ou le dumper de Cheat Engine).

> **Astuce** : les clients open source (Latite, Flarial) publient leurs signatures pour chaque version. C'est un excellent point de départ pour vérifier tes propres résultats.

---

## 1. Le hook joueur (`[player]`)

Il faut une fonction appelée à chaque tick, qui reçoit un objet menant au joueur local.

**Méthode classique : `GameMode`.** Chaque joueur a un `GameMode` dont le 1er champ après la vtable (`+0x8`) pointe sur le joueur.

1. Dans IDA/Ghidra, cherche la RTTI `.?AVGameMode@@`, puis remonte à la vtable `GameMode::vftable`.
2. Parmi ses fonctions virtuelles, `baseTick` (ou `tick`) est appelée chaque tick et prend seulement `this`.
3. Génère une signature au début de la fonction.

```ini
[player]
hook_signature = 48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ?   ; exemple, NE PAS copier tel quel
hook_argument  = 0
pointer_chain  = 0x8
```

Si tu hookes directement une fonction qui reçoit `LocalPlayer*` (par exemple `LocalPlayer::normalTick`), laisse `pointer_chain` vide.

**Contrainte** : la fonction doit renvoyer `void`, un entier ou un pointeur (pas de `float`/`double`), et avoir au plus 4 arguments utiles.

## 2. `ItemStackBase::getDamageValue`

C'est la fonction la plus simple à trouver avec Cheat Engine :

1. Mets un casque en fer neuf et note sa durabilité restante (165).
2. Fais-toi taper : les **dégâts** stockés augmentent (0 → 1 → 2…). Cherche une valeur 2 octets qui augmente.
3. Clic droit → *Find out what accesses this address*, puis ouvre l'inventaire.
4. Une petite fonction qui lit cette valeur via le tag NBT `Damage` et la renvoie (`movzx eax, word ptr [...]`) est `getDamageValue`.

Dans IDA, la chaîne `"Damage"` mène aussi à cette fonction et à `setDamageValue`.

## 3. `Actor::getArmor`

`const ItemStack& Actor::getArmor(ArmorSlot slot) const` : 2 arguments (`rcx = acteur`, `edx = slot 0..3`).

- Dans IDA, regarde les fonctions qui appellent `getDamageValue` pendant le rendu de l'inventaire : l'appelant récupère l'`ItemStack` d'armure via `getArmor`.
- Autre piste : la chaîne `"Armor"` / `"ArmorItems"` (sauvegarde NBT de l'acteur) mène au code qui itère sur les 4 slots en appelant `getArmor`.

Si la signature tombe sur un `call` vers la fonction, utilise le préfixe `call:` :

```ini
Actor_getArmor = call:E8 ? ? ? ? 48 8B D8 48 85 C0 74 ?
```

## 4. `Player::getSelectedItem` (optionnel)

Même principe : `const ItemStack& Player::getSelectedItem() const`, 1 seul argument. Elle est appelée partout (utilisation d'objet, rendu de la main). Si tu la laisses vide, seule la ligne « Main » du HUD est désactivée.

## 5. `Item::getMaxDamage` (index de vtable)

1. Trouve la vtable de `Item` via la RTTI `.?AVItem@@`.
2. `getMaxDamage` est une fonction très courte qui renvoie un `short` lu dans l'objet : `movzx eax, word ptr [rcx+XX]` puis `ret`.
3. L'index = `(adresse de l'entrée − début de la vtable) / 8`.

```ini
[vtable]
Item_getMaxDamage = 37   ; exemple
```

## 6. Offsets d'`ItemStackBase`

```
ItemStackBase
  +0x00  vtable
  +0x08  WeakPtr<Item>   -> SharedCounter<Item>* -> Item*   (ItemStackBase_item)
  ...
  +0x??  uint8 count                                         (ItemStackBase_count)
```

`0x8` est stable depuis longtemps. `count` est optionnel (`-1`) : un slot vide a de toute façon un `Item*` nul.

---

## Vérifier

1. Lance le launcher et ouvre le menu (**INSER**), section **Diagnostic** : chaque fonction doit afficher `[OK]`.
2. Ouvre `Nexus.log` à côté de la DLL : chaque signature trouvée y apparaît avec son adresse `Minecraft.Windows.exe+0x...`.
3. Si une signature n'est **pas unique**, la DLL prend la première occurrence et les valeurs seront fausses. Allonge la signature.

Toutes les lectures sont protégées : un mauvais offset donne « pas de données », pas un crash. Une signature qui pointe au mauvais endroit peut quand même faire planter le jeu. Teste en solo.

---

## 7. Auto-sprint (`[autosprint]`)

Il faut trouver le **booléen « touche Sprint enfoncée »** que le jeu met à jour à chaque tick, puis la **fonction qui l'écrit**.

### Trouver le booléen (Cheat Engine)

1. Attache Cheat Engine à `Minecraft.Windows.exe` et mets-toi en jeu, en solo.
2. **Maintiens la touche Sprint** (Ctrl par défaut). Pendant ce temps, lance un *First Scan* : type **Byte**, valeur **1**. Astuce : configure un raccourci de scan dans *Edit → Settings → Hotkeys* pour lancer le scan sans lâcher la touche.
3. **Relâche Sprint**, puis *Next Scan* avec la valeur **0**.
4. Répète en alternant 1 et 0 jusqu'à ce qu'il reste quelques adresses.
5. Vérifie : coche une adresse, mets sa valeur à 1 et gèle-la (case *Active*), puis avance avec Z ou W. Si ton personnage sprinte sans que tu touches Ctrl, c'est la bonne adresse.

### Trouver la fonction qui l'écrit

6. Clic droit sur l'adresse → **Find out what writes to this address**. Une instruction apparaît qui s'exécute à chaque tick, par exemple `mov [rbx+2C], al`.
7. **Registre de base** : ici `rbx`, et l'offset vaut `0x2C` → `sprint_offset = 0x2C`.
8. Clique sur *Show disassembler* pour voir la fonction qui contient cette instruction. Remonte jusqu'à son début et note son adresse `Minecraft.Windows.exe+0x...`.
9. Dans IDA, va à cette adresse (`0x140000000 +` l'offset), puis appuie sur F5. Regarde d'où vient `rbx` :
   - si c'est `rcx` au début de la fonction (`mov rbx, rcx`), alors `hook_argument = 0` ;
   - si c'est `rdx`, alors `hook_argument = 1`, et ainsi de suite.
10. Crée la signature au début de cette fonction, comme dans la section 2.

Si le booléen est écrit dans un **sous-objet** (`mov [rax+2C], al`, où `rax` a été lu depuis `[rcx+0x48]`), hooke plutôt une fonction qui reçoit directement ce sous-objet. Tu peux aussi m'envoyer le pseudo-code et on adaptera le module.

---

## 8. Barres de vie (`[entities]`)

Ce module a besoin de la section `[player]` (pour la caméra) **et** des éléments suivants.

### Position et rotation (Cheat Engine)

1. **Position** : ouvre le menu de débogage (coordonnées affichées dans les paramètres du monde). Cherche ta coordonnée **Y** en **Float** (*Value between* Y−1 et Y+1). Saute ou monte d'un bloc, puis *Next Scan* (*Increased value*) ; redescends (*Decreased value*). Les trois floats X, Y, Z se suivent : X est 4 octets avant Y.
2. **Remonter jusqu'à l'Actor** : clic droit → *Find out what accesses this address*. L'instruction te donne le registre de base et l'offset (`movss xmm0, [rax+4]` → l'objet est dans `rax`). Avec *Pointer scan*, ou en remontant dans le code, trouve comment `rax` est obtenu depuis l'Actor (`mov rax, [rcx+2A0]`). Ici, `position_chain = 0x2A0,0x0`.
3. **Rotation** : même principe en tournant la tête. Cherche le **pitch** (regarde en haut ou en bas : de −90 à 90). Le **yaw** suit juste après. Le pitch doit être positif quand tu regardes vers le bas.

Le joueur local est un Actor comme les autres : les mêmes chaînes servent pour toutes les entités.

### Fonctions

- **`Actor::getHealth` / `getMaxHealth`** : de petites fonctions qui lisent l'attribut de vie (la chaîne `"minecraft:health"` mène au code des attributs). Elles prennent seulement `this` et renvoient un `int`.
- **`getEntityTypeId`** (index vtable) : renvoie un nombre fixe par type d'entité, `0x13F` pour un joueur. Dans la vtable de `Actor`, c'est une fonction très courte qui renvoie une constante ou lit un champ.
- **`getNameTag`** (optionnel) : renvoie une `std::string` contenant le pseudo.
- **Hook** : une fonction appelée chaque tick pour chaque entité (`Actor::normalTick`, `Actor::baseTick`…). Vérifie avec un point d'arrêt dans Cheat Engine qu'elle s'exécute aussi pour les animaux, pas seulement pour toi.

### Ligne de vue : `Actor::canSee` (obligatoire)

C'est ce qui empêche le module d'afficher des informations à travers les murs. Le jeu l'utilise pour que les mobs sachent s'ils te voient : elle lance un rayon des yeux d'une entité vers l'autre et renvoie `false` dès qu'un bloc opaque le coupe.

- **Signature** : `bool canSee(const Actor* this, const Actor* target)`, avec 2 arguments et un retour `bool` (`al`).
- **Dans IDA** : elle récupère la position de la tête des deux entités (deux appels identiques, l'un sur `rcx`, l'autre sur `rdx`), appelle une grosse fonction de lancer de rayon (`BlockSource::clip`) puis compare le résultat. Pour la trouver, pars du code de comportement des mobs qui ciblent un joueur, par exemple les chaînes `minecraft:behavior.nearest_attackable_target` ou `must_see`.
- **Vérification** : mets un point d'arrêt dessus avec Cheat Engine près d'un zombie. Si tu te caches derrière un mur, la valeur de `al` au retour doit passer de 1 à 0.

Optionnel : `vtable_isInvisible` (bool, sans argument) masque les entités sous effet d'invisibilité, qui resteraient sinon « visibles » pour le rayon.

### Calibrage

Si les barres sont décalées (trop haut, trop bas, ou qu'elles « glissent » quand tu tournes la tête), ajuste **Calibrage FOV** dans le menu jusqu'à ce qu'elles restent bien au-dessus des têtes. Mets d'abord le même FOV que dans les paramètres vidéo du jeu.


---

## 9. Zoom (`[zoom]`)

Il faut la fonction qui renvoie le **champ de vision** de la caméra : `float LevelRendererPlayer::getFov(float partialTicks, bool applyEffects)`. Le **prototype doit être exactement celui-là** : `rcx` = objet, `xmm1` = float, `r8b` = bool, et le résultat est renvoyé dans `xmm0`.

- **Cheat Engine** : mets ton FOV à 70 dans les paramètres vidéo et cherche un **Float** de valeur 70. Change le FOV à 90 puis fais *Next Scan* 90. Ensuite, *Find out what accesses this address* : la fonction qui lit cette valeur, la modifie (effets de vitesse, sprint) et la renvoie dans `xmm0` est `getFov`.
- **Vérification** : un point d'arrêt sur la fonction doit se déclencher à chaque image.

## 10. Nametag en F5 (`[nametag]`)

En vue à la 3e personne, le jeu n'affiche pas ton pseudo, car une vérification renvoie « non » pour le joueur local. Il faut trouver cette fonction, qui renvoie un `bool`, et indiquer quel argument contient l'entité (`actor_argument`).

- **Piste** : le code de rendu des nametags appelle, pour chaque entité, une fonction du type `canShowNameTag` / `shouldRenderNameTag`. Pose un point d'arrêt dessus près d'un autre joueur ou d'un mob nommé : elle renvoie `1` pour lui, et `0` pour toi en F5.
- La section `[player]` doit aussi être remplie, pour que Nexus sache qui est le joueur local.
