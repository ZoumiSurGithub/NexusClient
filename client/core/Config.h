#pragma once
#include <string>
#include <vector>
#include <filesystem>

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

    ptrdiff_t itemStackItem = 0x8;                // WeakPtr<Item> dans ItemStackBase
    ptrdiff_t itemStackCount = -1;                // uint8 count (-1 = ne pas verifier)
    int itemGetMaxDamageIndex = -1;               // index vtable de Item::getMaxDamage()

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

    // Zoom : float LevelRendererPlayer::getFov(float partialTicks, bool applyEffects)
    std::string zoomFovSig;
    // Nametag en F5 : fonction "afficher le nametag ?" (retour bool) et argument contenant l'Actor*
    std::string nametagSig;
    int nametagActorArg = 0;
};

constexpr const char* kDiscordClientId = "1570931183657685075";   // application Discord "Nexus"
constexpr const char* kDiscordImage = "discord_nexus";   // Art Asset du logo Nexus

struct Settings {
    // HUD d'armure
    bool  armorEnabled = true;
    float armorX = 10.f, armorY = 200.f;
    float scale = 1.0f;
    int   layout = 0;            // 0 = vertical, 1 = horizontal
    int   valueMode = 2;         // 0 = valeur, 1 = pourcentage, 2 = les deux
    bool  showHeldItem = true;
    bool  showEmptySlots = false;
    int   lowThreshold = 15;     // % sous lequel la piece clignote
    float bgAlpha = 0.35f;

    // FPS
    bool  fpsEnabled = true;
    float fpsX = 10.f, fpsY = 10.f;   // position libre (fpsAnchor = 0)
    int   fpsAnchor = 1;     // 0 libre, 1 haut-gauche, 2 haut-centre, 3 haut-droite, 4 bas-gauche, 5 bas-centre, 6 bas-droite
    float fpsMargin = 10.f;
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
    bool  zoomEnabled = true;
    int   zoomKey = 'C';          // code de touche virtuel (souris : 4 = molette, 5/6 = boutons lateraux)
    int   zoomMode = 0;           // 0 = maintenir, 1 = basculer
    float zoomFactor = 4.f;       // x2 a x10
    bool  zoomSmooth = true;

    // Barres de vie
    bool  healthEnabled = true;
    bool  hpPlayers = true, hpAnimals = true, hpMonsters = true, hpOthers = false;
    float hpMaxDistance = 24.f;
    bool  hpHideFull = false;
    bool  hpShowValue = true;
    bool  hpShowNames = true;
    float hpBarScale = 1.f;
    float fov = 70.f;            // champ de vision vertical (calibrage de la projection)

    // Interface
    // Blocage souris/clavier du jeu quand le menu est ouvert :
    // 0 = discret (le jeu croit juste ne plus etre la fenetre active, sans pause)
    // 1 = Alt+Tab complet (messages de perte de focus : le jeu peut se mettre en pause)
    // 2 = desactive (seulement GameInput / Raw Input)
    int   menuBlockMode = 0;
    float accent[3] = { 0.62f, 0.46f, 1.00f };   // violet Nexus

    // Chat
    int   chatKey = 'T';      // touche "Ouvrir le chat" du jeu (code virtuel)
    bool  chatOverlay = true; // panneau de pre-commandes quand le chat du jeu est ouvert
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
    void Init(const std::filesystem::path& folder);
    void LoadOffsets();
    void LoadSettings();
    void SaveSettings();

    Offsets& GetOffsets();
    Settings& GetSettings();

    // Commandes rapides : commands.txt a cote de la DLL, une par ligne "Nom|texte"
    std::vector<QuickCommand>& Commands();
    void LoadCommands();
    void SaveCommands();
    const std::filesystem::path& Folder();
}
