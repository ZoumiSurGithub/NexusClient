#pragma once
#include <string>
#include <vector>
#include <filesystem>

// Chaine de pointeurs : module Minecraft.Windows.exe + offsets successifs.
// offs[0]   = RVA de la case base (dans le module), dereferencee pour obtenir le 1er noeud
// offs[1..] = dereferencements successifs
// offs.back() = offset du champ final (ajoute, PAS dereference)
// Trouvee au pointer-scan Cheat Engine (voir docs/NOTES_REVERSE.md).
struct PtrChain {
    bool set = false;
    std::vector<ptrdiff_t> offs;
};

// Offsets et signatures du jeu : lus depuis offsets.ini (a mettre a jour a chaque
// version de Minecraft, voir docs/TROUVER_LES_OFFSETS.md). Rien n'est code en dur.
struct Offsets {
    // Fonction hookee qui recoit, dans l'un de ses 4 premiers arguments, un objet
    // a partir duquel on remonte au LocalPlayer (ex : GameMode::baseTick, arg 0, chaine 0x8).
    std::string playerHookSig;
    int playerHookArg = 0;
    std::vector<ptrdiff_t> playerChain;           // dereferencements successifs

    std::string getArmorSig;                      // const ItemStack& Actor::getArmor(ArmorSlot)
    std::string getSelectedItemSig;               // const ItemStack& Player::getSelectedItem() (optionnel)
    std::string getDamageValueSig;                // short ItemStackBase::getDamageValue()
    std::string getHoverNameSig;                  // std::string ItemStackBase::getHoverName() (nom affiche)

    // Lecture de l'objet en main / inventaire (offsets v26.63, d'apres Latite)
    ptrdiff_t itemCountOffset    = 0x22;          // uint8 count dans ItemStackBase
    ptrdiff_t itemIdStringOffset = 0xD8;          // Item -> std::string id (HashedString id +0x8)
    ptrdiff_t playerSupplies     = 0x5E0;         // Player -> PlayerInventory*
    ptrdiff_t invInventory       = 0xB8;          // PlayerInventory -> Inventory*
    ptrdiff_t invSelectedSlot    = 0x10;          // PlayerInventory -> selectedSlot (int)
    int       inventoryGetItemIndex = 7;          // index vtable de Inventory::getItem(int)

    ptrdiff_t itemStackItem = 0x8;                // WeakPtr<Item> dans ItemStackBase
    ptrdiff_t itemStackCount = -1;                // uint8 count (-1 = ne pas verifier)
    int itemGetMaxDamageIndex = -1;               // index vtable de Item::getMaxDamage()

    // --- Lecture par chaine de pointeurs (section [pointers] de offsets.ini) ----
    // Alternative au hook joueur, pour les builds sans RTTI (ECS) ou l'on ne peut
    // pas remonter a l'Actor : on lit chaque valeur via un chemin module+offsets.
    PtrChain healthChain;                         // float : vie du joueur local
    PtrChain maxHealthChain;                      // float : vie max (optionnel)
    PtrChain armorChain[4];                        // int (damage) : casque, plastron, jambieres, bottes
    int      armorMax[4] = { 363, 528, 495, 429 };// durabilite max par piece (diamant par defaut)
    bool anyArmorChain() const { return armorChain[0].set || armorChain[1].set || armorChain[2].set || armorChain[3].set; }
    bool anyChain() const { return healthChain.set || anyArmorChain(); }

    // --- Resolution du LocalPlayer via ClientInstance (methode stable, section [client]) ----
    // Marche : signature -> global winMain -> Platform_GameCore -> GameCore -> MinecraftGame
    //          -> ClientInstance (client primaire) -> getLocalPlayer() (vtable).
    std::string platformSig;                      // ex: ref:3:7:4C 89 3D ? ? ? ? 4D 85 FF
    int clientGetLocalPlayerIndex = 0x1E;         // index vtable de ClientInstance::getLocalPlayer()

    // --- Rendu d'items via le jeu (ItemRenderer) : section [render] ---
    std::string setupAndRenderSig;                // call: -> ScreenView::setupAndRender
    std::string renderGuiItemSig;                 // call: -> ItemRenderer::renderGuiItemNew
    std::string baseActorCtxSig;                  // -> BaseActorRenderContext::ctor
    ptrdiff_t uictxCinst       = 0x8;             // MinecraftUIRenderContext -> ClientInstance* (apres vtable)
    ptrdiff_t uictxScreenCtx   = 0x10;            // MinecraftUIRenderContext -> ScreenContext*
    ptrdiff_t ciMinecraftGame  = 0x1C8;           // ClientInstance -> MinecraftGame*
    ptrdiff_t barcItemRenderer = 0x58;            // BaseActorRenderContext -> ItemRenderer*
    ptrdiff_t ciGuiData        = 0x668;           // ClientInstance -> GuiData*
    ptrdiff_t gdGuiScale       = 0x5C;            // GuiData.guiScale (ex: 3.0)
    ptrdiff_t gdGuiScaleFrac   = 0x60;            // GuiData.guiScaleFrac (= 1/guiScale)
    ptrdiff_t gdScreenSize     = 0x40;            // GuiData.screenSize (Vec2, pixels)

    // Auto-sprint : fonction appelee chaque tick qui met a jour l'etat des touches
    std::string sprintHookSig;
    int sprintHookArg = 0;
    ptrdiff_t sprintOffset = -1;                  // bool "sprint enfonce" dans l'objet capture

    // Barres de vie : hook appele pour chaque entite (Actor*) + acces a ses donnees
    std::string entityHookSig;
    int entityHookArg = 0;
    std::vector<ptrdiff_t> positionChain;         // Actor -> ... -> Vec3 position (dernier = offset du champ)
    std::vector<ptrdiff_t> rotationChain;         // Actor -> ... -> Vec2 (pitch, yaw) en degres
    std::vector<ptrdiff_t> aabbChain;             // Actor -> ... -> AABB (Vec3 min, Vec3 max), optionnel
    std::string getHealthSig;                     // int Actor::getHealth() const
    std::string getMaxHealthSig;                  // int Actor::getMaxHealth() const
    int getEntityTypeIdIndex = -1;                // index vtable de Actor::getEntityTypeId()
    int getNameTagIndex = -1;                     // index vtable de Actor::getNameTag() (optionnel)
    std::string canSeeSig;                        // bool Actor::canSee(const Actor&) const : ligne de vue (OBLIGATOIRE)
    int isInvisibleIndex = -1;                    // index vtable de bool Actor::isInvisible() const (optionnel)

    // --- Enumeration des entites via le niveau (ClientInstance -> ... -> Level) ---
    ptrdiff_t ciMinecraft      = 0x1D0;           // ClientInstance -> Minecraft*
    ptrdiff_t mcGameSession    = 0xD0;            // Minecraft -> GameSession*
    ptrdiff_t gsLevel          = 0x40;            // GameSession -> Level*
    int       levelActorListVtIdx = 0x145;        // index vtable de Level::getRuntimeActorList()
    ptrdiff_t levelActorStorage   = 0x4B8;        // Level -> objet stockage d'acteurs
    ptrdiff_t actorResolverRva    = 0x32B47A0;    // RVA de la fonction qui resout un Actor* depuis une cle de noeud
    ptrdiff_t actorStateVector    = 0x240;        // Actor -> StateVectorComponent* (pos = Vec3 a +0x0)
    ptrdiff_t actorRotation       = 0x250;        // Actor -> ActorRotationComponent* (pitch,yaw a +0x0,+0x4)
    ptrdiff_t actorRenderPos      = 0x5B0;        // Actor -> position de rendu interpolee (Vec3) ; 0 = desactive
    ptrdiff_t actorAABB           = 0x248;        // Actor -> AABBShapeComponent* (min Vec3 +0x0, max Vec3 +0xC) ; 0 = desactive
    ptrdiff_t actorNameTag        = 0x19E8;       // Actor -> std::string nametag distant (pseudo+codes) ; 0 = desactive
    // Occlusion (raycast de blocs, indices vtable) :
    int clientRegionVtIdx = 0x1D;                 // ClientInstance::getRegion() -> BlockSource*
    int bsGetBlockVtIdx   = 0x2;                  // BlockSource::getBlock(BlockPos const&) -> Block*
    // Index vtable de flags booleens surs (detectes par probe) pour classer les entites.
    int vtIsPlayer  = 170;                        // joueur=1
    int vtIsMob     = 94;                         // creature=1
    int vtIsMonster = 222;                        // monstre hostile=1
    // Matrice vue-projection du jeu (projection exacte, stable) :
    ptrdiff_t mcGameRenderer      = 0x1628;       // MinecraftGame -> GameRenderer*
    ptrdiff_t grViewMatrix        = 0x3C0;        // GameRenderer.lastViewMatrix (glm::mat4)
    ptrdiff_t grProjMatrix        = 0x440;        // GameRenderer.lastProjectionMatrix (glm::mat4)

    // Zoom (methode Latite) : on hooke LevelRenderer::renderLevel et on multiplie les
    // champs fovX/fovY du LevelRendererPlayer. Plus fiable que de hooker getFov.
    std::string zoomRenderLevelSig;               // site d'appel de LevelRenderer::renderLevel
    ptrdiff_t   lrpOffset  = 0x480;               // LevelRenderer -> LevelRendererPlayer*
    ptrdiff_t   fovXOffset = 0xF50;               // LevelRendererPlayer.fovX (facteur)
    ptrdiff_t   fovYOffset = 0xF64;               // LevelRendererPlayer.fovY (facteur)

    // Combo : Actor::attack (appelee quand un acteur en frappe un autre). On ne compte
    // le combo que lorsque le joueur local touche une entite (jamais un clic dans le vide).
    std::string attackSig;
    // Timer d'invincibilite de la cible (Mob::hurtEffects) : remis au max uniquement
    // quand un coup inflige reellement des degats -> permet d'ignorer les coups absorbes.
    ptrdiff_t   invulnTimeOffset = 0x1C4;
    // Nametag en F5 : fonction "afficher le nametag ?" (retour bool) et argument contenant l'Actor*
    std::string nametagSig;
    int nametagActorArg = 0;
    // Nametag 3e personne : instruction (saut 74 ??) a NOP pour afficher son propre nametag.
    std::string tpNametagSig;
    std::string getPerspectiveSig;                // Options::getPerspective (0=1ere pers.)
    ptrdiff_t   optionsOffset = 0xE00;            // ClientInstance -> Options*
};

constexpr const char* kDiscordClientId = "1570931183657685075";   // application Discord "Nexus"
constexpr const char* kDiscordImage = "discord_nexus";   // Art Asset du logo Nexus

struct Settings {
    // HUD d'armure
    bool  armorEnabled = false;
    float armorX = 10.f, armorY = 200.f;
    int   armorAnchor = 0;       // 0 libre, 1..6 coins/bords, 7/8 gauche/droite de la hotbar
    float armorMargin = 0.f;     // decalage horizontal signe
    float armorHotbarY = 10.f;   // marge verticale
    float scale = 1.0f;
    int   layout = 0;            // 0 = vertical, 1 = horizontal
    int   valueMode = 2;         // 0 = valeur, 1 = pourcentage, 2 = les deux
    int   armorDisplayMode = 0;  // 0 = nom (texte), 1 = icone (texture)
    bool  showHeldItem = true;
    int   lowThreshold = 15;     // % sous lequel la piece clignote
    float bgAlpha = 0.35f;
    // Opacite GLOBALE du HUD (0..1) : attenue tout le panneau (texte, icones, bordure, fond),
    // pas seulement le fond. Chaque HUD a la sienne (reglable independamment), toutes avec la
    // meme base par defaut (1.0 = pleinement visible).
    float armorBgAlpha = 1.0f;
    float fpsBgAlpha   = 1.0f;
    float cpsBgAlpha   = 1.0f;
    float zoomBgAlpha  = 1.0f;

    // FPS
    bool  fpsEnabled = true;
    float fpsX = 10.f, fpsY = 10.f;   // position libre (fpsAnchor = 0)
    int   fpsAnchor = 1;     // 0 libre, 1 haut-gauche, 2 haut-centre, 3 haut-droite, 4 bas-gauche, 5 bas-centre, 6 bas-droite
    float fpsMargin = 0.f;
    float fpsHotbarY = 10.f;
    float fpsScale = 1.0f;
    int   fpsColorMode = 0;  // 0 accent, 1 selon les performances, 2 blanc
    bool  fpsBackground = true;
    bool  fpsShowMs = false;
    bool  fpsShowLabel = true;
    float fpsInterval = 0.25f; // secondes entre deux mises a jour de la valeur
    int   fpsTarget = 60;      // seuil "bon" pour la couleur selon les performances

    // Mouvement
    bool  autoSprint = false;

    // Nametag en F5
    bool  showOwnNametag = true;

    // Zoom
    bool  zoomEnabled = false;
    int   zoomKey = 'C';          // code de touche virtuel (souris : 4 = molette, 5/6 = boutons lateraux)
    int   zoomMode = 0;           // 0 = maintenir, 1 = basculer
    float zoomFactor = 4.f;       // x2 a x10
    bool  zoomSmooth = true;
    // Indicateur de zoom a l'ecran (element HUD deplacable, comme le FPS)
    bool  zoomIndicator = true;
    float zoomHudX = 10.f, zoomHudY = 120.f;   // position libre (zoomAnchor = 0)
    int   zoomAnchor = 5;         // 0 libre, 1..6 coins/bords ; 5 = bas-centre par defaut
    float zoomHudMargin = 0.f;
    float zoomHotbarY = 60.f;
    float zoomHudScale = 1.f;

    // Barres de vie
    bool  healthEnabled = false;
    bool  hpPlayers = false, hpAnimals = true, hpMonsters = true, hpOthers = false;  // joueurs: vie non fiable (serveur)
    float hpMaxDistance = 24.f;
    bool  hpThroughWalls = false;   // false = occlusion active (pas de barre a travers les murs)
    bool  hpHideFull = false;
    bool  hpShowValue = true;
    bool  hpShowName = false;
    bool  hpShowNames = true;
    float hpBarScale = 1.f;

    // Nametag en 3e personne (afficher son propre pseudo au-dessus de soi en F5).
    bool  thirdPersonNametag = false;

    // CPS & Combo (interrupteur maitre du panneau)
    bool  cpsComboEnabled = false;
    bool  reachEnabled = false;      // affiche ta portee sur ta cible (blocs)
    bool  reachTakenEnabled = false; // affiche la portee des coups que tu recois (blocs)
    // CPS
    bool  cpsEnabled = false;
    float cpsX = 10.f, cpsY = 120.f;
    int   cpsAnchor = 0;         // 0 libre, 1..6 coins/bords (comme le FPS)
    float cpsMargin = 0.f;
    float cpsHotbarY = 10.f;
    float cpsScale = 1.f;
    bool  cpsShowRight = true;

    // Combo
    bool  comboEnabled = false;
    float comboX = 10.f, comboY = 160.f;
    int   comboAnchor = 0;
    float comboMargin = 10.f;
    float comboScale = 1.f;
    bool  comboShowMax = true;
    int   comboResetMs = 1000;   // au-dela de ce delai sans clic, la serie repart

    // Interface
    // Blocage souris/clavier du jeu quand le menu est ouvert :
    // 0 = discret (le jeu croit juste ne plus etre la fenetre active, sans pause)
    // 1 = Alt+Tab complet (messages de perte de focus : le jeu peut se mettre en pause)
    // 2 = desactive (seulement GameInput / Raw Input)
    int   menuBlockMode = 0;
    float accent[3] = { 0.62f, 0.46f, 1.00f };   // violet Nexus
    bool  accentRainbow = false;                  // accent anime arc-en-ciel
    float accentRainbowSpeed = 0.25f;             // tours par seconde (vitesse du defile)
    bool  welcomeMessage = true;                  // message d'accueil a l'arrivee dans un monde/serveur
    bool  devMenuOnBlur = false;                   // dev : garder le menu dessine meme si une autre appli est au premier plan

    // Capture d'ecran (enregistree dans %APPDATA%\Nexus\screenshots)
    bool  screenshotEnabled = false;
    int   screenshotKey = 0x71;                    // VK_F2 par defaut

    // Chat
    int   chatKey = 'T';      // touche "Ouvrir le chat" du jeu (code virtuel)
    bool  chatOverlay = false; // panneau de pre-commandes quand le chat du jeu est ouvert
    int   chatOverlaySide = 0;      // 0 = droite, 1 = gauche
    float chatOverlayBottom = 90.f; // distance au bas de l'ecran (au-dessus de la barre de saisie)

    // Discord Rich Presence
    bool  discordEnabled = true;
    bool  discordShowIp = true;       // afficher l'adresse du serveur
    bool  discordShowPlayers = true;  // afficher "x/y connectes"
    bool  discordShowPing = true;     // afficher la latence (ms)
    bool  discordShowMotd = true;     // MOTD du serveur au survol de l'image

    // Mode demo : affiche de fausses valeurs (utile tant que les offsets ne sont pas configures)
    bool  demoMode = false;
};

struct QuickCommand {
    std::string label;   // texte du bouton
    std::string text;    // message ou commande envoye (UTF-8)
};

namespace Config {
    void Init(const std::filesystem::path& moduleFolder, const std::filesystem::path& dataFolder);
    const std::filesystem::path& DataFolder();   // %APPDATA%\\Nexus : reglages, commandes, journal
    void LoadOffsets();
    void LoadSettings();
    void SaveSettings();
    void ResetSettings();   // remet tous les reglages par defaut

    Offsets& GetOffsets();
    Settings& GetSettings();

    // Commandes rapides : commands.txt a cote de la DLL, une par ligne "Nom|texte"
    std::vector<QuickCommand>& Commands();
    void LoadCommands();
    void SaveCommands();
    const std::filesystem::path& Folder();
}
