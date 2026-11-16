# Notes de reverse-engineering — Nexus

> ## ✅ ARMURE FONCTIONNELLE (v26.63) — lecture ECS robuste
> Chaîne complète qui marche : hook `LocalPlayer::applyTurnDelta` → LocalPlayer →
> `Actor+0x8 = EntityContext` → EnTT `try_get<ActorEquipmentComponent>` (hash
> `0xB06141A9`) → `armorContainer->getItem(slot)` (vtable 7) → `ItemStackBase::
> getDamageValue` (signature) + `Item::getMaxDamage` (vtable 0x24).
> Couche EnTT = `third_party/entt` (3.15.0) + `client/game/sdk/EcsSdk.h` + `ArmorEcs.cpp`.
>
> ### ⚠️ OBLIGATOIRE : compiler en **Release** (x64-release)
> Minecraft est un build Release. En Debug, l'*iterator debugging* MSVC
> (`_ITERATOR_DEBUG_LEVEL=2`, runtime `/MTd`) change le layout des conteneurs std::
> qu'EnTT utilise → lecture du registre du jeu incohérente (assertion « power of two »).
> En Release (`/MT`) le layout correspond et les assertions EnTT sont désactivées.


> Journal des trouvailles sur Minecraft Bedrock (Windows).
> `Minecraft.Windows.exe` est rechargé à une adresse différente à chaque lancement (ASLR) :
> les adresses absolues ci-dessous ne valent que pour une session, seuls comptent
> les **RVA** (offset dans le module) et les **chaînes de pointeurs** (module + offsets).

Version du jeu testée : _(à remplir : Paramètres → à propos, ex. 1.21.x)_

---

## ✅ Confirmé

### Fonction `Actor::getHealth`
- **RVA : `0x2977A00`** (IDA : `sub_142977A00`, base image `0x140000000`).
- Signature : `int __fastcall getHealth(Actor* this)` — prend l'Actor dans `rcx`,
  renvoie la vie **arrondie en entier**.
- Interne : appelle `Actor::getAttribute` (`sub_142BF38E0`), puis lit le *currentValue*
  de l'AttributeInstance et le convertit float→int (`_mm_cvttps_epi32`).

### Offset vie dans l'AttributeInstance
- **`AttributeInstance.currentValue = 0x7C`** (instruction `movss xmm0,[rax+7Ch]`).
- Trouvé avec Cheat Engine (scan Float = vie, gel) puis « Find out what accesses ».

### Chaîne de pointeurs vers la vie du joueur local (Cheat Engine)
Validée sur **3 redémarrages** du jeu (rescan memory à chaque fois).

```
Minecraft.Windows.exe + <BASE>        ; offset base : À RELIRE dans l'éditeur CE (case du bas)
    + 0x328
    + 0x98
    + 0x750
    + 0x50
    + 0x0
    + 0x18
    + 0x324   = vie (float)
```

Objets intermédiaires observés (une session) :
`B=…81F3410  C=…8508020  D=…F5233C10  E=…D57975C0  F=…5828C020  G=…1074F000`
→ la vie est à `G + 0x324`.

> ⚠️ Reste à relire : l'**offset base** (`Minecraft.Windows.exe + ?`), caché dans la
> case du bas de l'éditeur « Change Address ». Le reste de la chaîne est complet.

### Chaîne de pointeurs vers la durabilité d'armure (Cheat Engine)
Validée sur **2 redémarrages** (rescan memory). Casque **diamant** (maxDamage = 363).

```
Minecraft.Windows.exe + 0x12968228     ; case base (affichée "12968228" par CE)
    + 0x808
    + 0x850
    + 0x48
    + 0x100
    + 0x58
    + 0x0
    + 0xB3C   = damage du casque (uint32)
```

- La valeur lue est le **damage** (usure) de l'ItemStack : **0 = neuf**, augmente quand l'armure encaisse.
- **Durabilité restante = maxDamage − damage** (diamant : 363 − damage).
- Champ voisin à `+0x4` (donc `+0xB40`) contient ~413/414 : **pas** la durabilité, ignorer.
- ⚠️ Vérifier hex vs décimal pour la case base `12968228` au moment du code
  (CE l'affiche sans préfixe ; le client testera module+offset en live).

> **Les 3 autres pièces** (plastron, jambières, bottes) sont très probablement dans le
> même conteneur d'ItemStacks, à un **pas fixe** du casque. À trouver : scanner la
> durabilité d'une 2ᵉ pièce et comparer l'adresse → en déduire le stride, puis les 4
> slots se lisent avec le même chemin en changeant un offset.

---

## 🧠 Leçon importante : ce build utilise l'ECS

En remontant la chaîne, les nœuds intermédiaires **ne sont pas des objets C++**
(pas de vtable dans la zone module `2C3F5…`) et **se pointent l'un l'autre**
(`E → F`). Ce sont des **conteneurs de composants ECS**, pas un « objet joueur ».

**Conséquence :** sur cette version, il n'existe pas de pointeur d'`Actor` unique
qu'on lirait avec des offsets fixes (`Actor + X`). Les données (vie, armure…) vivent
dans des **composants indexés par entité**.

**Stratégie retenue :** un **pointer-scan par valeur** (scan → gel → redémarrages →
rescan), comme pour la vie. Fiable, reproductible, indépendant de la structure interne.

---

## 🎯 À faire (prochaine session)

1. ✅ ~~Durabilité d'armure (casque)~~ — **trouvée** (chaîne ci-dessus).
2. **Stride des 4 slots d'armure** : scanner la durabilité d'une 2ᵉ pièce, comparer
   l'adresse au casque → en déduire le pas, pour lire plastron/jambières/bottes.
3. Relire l'offset base de la chaîne de **vie** (hier) : seul chiffre manquant.
4. Adapter `client/core/Config` pour résoudre une **chaîne module+offsets** (au lieu du
   hook `[player]`) → lire vie + armure directement.
5. Positions X/Y/Z (pour health bars / nametags) : même méthode si besoin.

## ✅✅ LocalPlayer résolu de façon stable (v26.63) — VALIDÉ

- Méthode : **hook de `LocalPlayer::applyTurnDelta`** (appelée quand on bouge la vue) ;
  son `this` (rcx) EST le LocalPlayer. Une seule signature, aucun offset de marche.
- Signature (Latite, vérifiée unique sur v26.63) :
  `55 41 57 41 56 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 44 0F 29 55 ? 44 0F 29 4D ? 44 0F 29 45 ? 0F 29 7D ? 0F 29 75 ? 48 C7 45 ? ? ? ? ? 49 89 D4`
- Câblé dans `offsets.ini` `[player]` : `hook_signature` = ci-dessus, `pointer_chain` vide.
- Résultat en jeu : `LocalPlayer=0x…` avec `vtable=0x7FF6…` (zone module) → vrai Actor. ✔
- La marche de globals (`winMain→Platform_GameCore→GameCore→MinecraftGame→map`) a
  échoué : `GameCore→MinecraftGame` (0x48) et l'offset de la map (0x9D0) ont changé en
  26.63. Abandonnée au profit du hook (plus simple, plus robuste).

> **Reste pour l'armure** : `Actor+0x8 = EntityContext`, puis `enttRegistry.try_get<
> ActorEquipmentComponent>(entity)` (hash `0xB06141A9`) → `armorContainer` (+0x8) →
> `getItem(slot)` (virtuel) → ItemStack → `getDamageValue` (sig ✓) + `Item::getMaxDamage`
> (vtable 0x24). **Le `try_get` EnTT est le vrai mur** (voir plus bas).

---

## 🏛️ Approche robuste — d'après le SDK Latite (open-source, GPLv3)

> Référence extraite de `LatiteClient/latite` (`src/mc/...`). **Attention** : tous les
> offsets / index de vtable / signatures ci-dessous sont **spécifiques à LA version que
> Latite cible** — à **vérifier/réajuster sur ton propre dump** (ils changent à chaque
> mise à jour du jeu). Latite est **GPLv3** : réutiliser leur code = publier sous GPLv3.

**Comment ils récupèrent le joueur local (stable, pas de pointer-scan) :**
- `ClientInstance` est obtenue depuis un global (`ClientInterface.clientInstance` à +0x10,
  via `MinecraftGame`/la map des clients).
- `LocalPlayer* = ClientInstance::getLocalPlayer()` = **vtable index 0x1E** (`callVirtual<LocalPlayer*>(this, 0x1E)`).
- Champs utiles de `ClientInstance` : minecraftGame +0x1C8, minecraft +0x1D0,
  levelRenderer +0x1E0, inputHandler +0x200.

**Armure (ECS) :**
- `Actor::getArmor(slot)` = `tryGetComponent<ActorEquipmentComponent>()->armorContainer->getItem(slot)`.
  → confirme que l'armure est dans un **composant ECS** (d'où l'échec des offsets fixes).
- `Player::supplies` (PlayerInventory*) = +0x5E0 ; `PlayerInventory::inventory` = +0xB8.

**Durabilité :**
- `ItemStackBase::getDamageValue()` → fonction, **signature Latite** :
  `56 57 48 83 EC ? 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 ? 48 8B 41 ? 48 85 C0 74 ? 48 83 38`
- `ItemStackBase::getItem()` → renvoie l'`Item*`.
- `Item::getMaxDamage()` = **vtable index 0x24** (`callVirtual<int>(this, 0x24)`).

**Note clé :** Latite n'invente pas ces offsets à la main — ils utilisent un **dumper
automatique** (commentaires `@dump-wbds` dans leur code) qui génère les offsets depuis le
binaire. C'est ça qui rend le maintien par version viable.

→ Ces fonctions (`getDamageValue`, `Item_getMaxDamage` vtable, `Actor_getArmor`) sont
**exactement** les champs déjà prévus dans `offsets.ini` (sections `[functions]` / `[vtable]`).
Le seul vrai morceau manquant côté architecture : obtenir le `LocalPlayer` via `ClientInstance`
au lieu du hook `[player]`.

---

## Méthode pointer-scan (rappel)
1. Scanner la valeur (Float ou 4 Bytes selon le cas), la filtrer, la **geler** pour confirmer.
2. Clic droit → *Pointer scan for this address* (Max level 7, Max offset 4095).
3. Fermer le jeu, relancer, retrouver la valeur, *Pointer scanner → Rescan memory*.
4. Répéter 2–3 fois jusqu'à une poignée de chemins stables.
5. Valider le chemin en live (gel + dégâts), puis le noter ici.

## Rendu des icones d'items via le moteur du jeu (option B) — phase 2

Pourquoi : les textures d'items ne sont PAS des PNG sur disque dans Bedrock 1.26
(seulement ~1206 PNG = skins/UI/pack_icons). Et un pack vanilla telecharge ne
couvrirait pas les textures custom d'un serveur. Donc on passe par le renderer du
jeu : il dessine la vraie texture chargee en memoire (vanilla OU custom serveur).

Chaine (d'apres Latite, GPLv3) :
  Hook ScreenView::setupAndRender(ScreenView* view [rcx], MinecraftUIRenderContext* ctx [rdx])
    -> appeler l'original d'abord (le jeu dessine son UI)
    -> construire un BaseActorRenderContext :
         cinst       = *(ctx + 0x8)      (MinecraftUIRenderContext -> ClientInstance*, apres vtable)
         screenCtx   = *(ctx + 0x10)     (-> ScreenContext*)
         minecraftGame = *(cinst + 0x1C8)
         ctor(buf[0x600] mis a zero, screenCtx, cinst, minecraftGame)
         itemRenderer = *(buf + 0x58)
    -> itemRenderer->renderGuiItemNew(buf, itemStack, 0, x, y, opacity, scale, 1.0, false)

Signatures (v26.63, verifiees uniques) -> section [render] de offsets.ini :
  setupAndRender      call:E8 ? ? ? ? 90 48 8B 4B ? 48 85 C9 74 ? 48 8B 01 48 8B 40 ? 48 89 FA FF 15 ? ? ? ? 90 48 8D 4D
  renderGuiItemNew    call:E8 ? ? ? ? 90 48 8D 55 ? 4C 8D 85 ? ? ? ? 48 89 F1 E8 ? ? ? ? 90 80 BF
  BaseActorRenderContext::ctor  55 41 57 41 56 56 57 53 48 83 EC ? 48 8D 6C 24 ? 48 C7 45 ? ? ? ? ? 4C 89 C6 48 89 D7 49 89 CF

Offsets INCERTAINS (a calibrer si crash — les logs "IR:" disent lequel) :
  uictx_cinst=0x8, uictx_screen_context=0x10, clientinstance_minecraftgame=0x1C8,
  baseactorctx_itemrenderer=0x58.
  Rappel : l'offset minecraftGame avait deja change sur v26.63 (GameCore+0x48 faux),
  donc 0x1C8 est a surveiller en priorite.

Code : client/game/ItemRender.{h,cpp}. Test phase 2 = dessine le casque a (100,100)
quand le mode d'affichage armure = Icone. Protege par __try, logs "IR:" (10 premiers).
