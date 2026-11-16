#pragma once
#include <array>
#include <cstdint>

namespace Game {
    enum Slot { Helmet = 0, Chestplate = 1, Leggings = 2, Boots = 3, Held = 4, SlotCount = 5 };

    struct ItemInfo {
        bool present = false;     // un item est dans le slot
        int  damage = 0;          // degats subis
        int  maxDamage = 0;       // 0 = item sans durabilite (citrouille, bloc, ...)
        int  count = 0;           // nombre dans la pile
        char name[48] = "";       // nom affiche (ex. "Casque en diamant"), UTF-8
        int Durability() const { return maxDamage - damage; }
        float Ratio() const { return maxDamage > 0 ? float(maxDamage - damage) / float(maxDamage) : 1.f; }
    };

    struct Snapshot {
        bool inGame = false;
        bool demo = false;
        std::array<ItemInfo, SlotCount> items{};
        bool  healthValid = false;   // vie lue via chaine de pointeurs
        float health = 0.f;
        float maxHealth = 0.f;
    };

    struct FunctionStatus { const char* name; bool found; bool required; };

    bool Init();          // resout les signatures, installe le hook joueur (MinHook deja initialise)
    void Shutdown();
    Snapshot Read();      // appele depuis le thread de rendu, chaque image
    Snapshot Demo();
    void* LocalPlayer();  // LocalPlayer* courant, ou nullptr (menus du jeu / offsets manquants)      // valeurs factices animees (mode demo / apercu dans le menu)
    bool IsConfigured();  // toutes les signatures obligatoires ont ete trouvees

    // Pour le rendu d'items via le jeu (pointeurs ItemStack du joueur local).
    void* ArmorStack(int slot);   // ItemStack d'une piece d'armure (0..3), 0 si vide
    void* HeldStack();            // ItemStack de l'objet en main
    std::array<FunctionStatus, 5> GetFunctionStatus();

    // --- Barres de vie / ESP ---
    struct EntityInfo { void* actor; float x, y, z; float height; bool isPlayer;
                        bool hasHealth; float health, maxHealth;
                        bool visible; int category; char name[32]; };
    // Remplit 'out' avec les entites du monde (sans allocation). Renvoie le nombre ecrit.
    int CollectEntities(EntityInfo* out, int maxCount);
    // Relit juste la position (de rendu) d'un acteur deja connu. Peu couteux.
    bool ReadEntityPos(void* actor, float& x, float& y, float& z);
    float ReachBetween(void* from, void* to);   // distance oeil de 'from' -> hitbox de 'to' (blocs), -1 si indispo
    float AttackReach(void* target);            // raccourci : reach du joueur local vers 'target'
    // Camera du joueur local : position de l'oeil + angles (degres). false si indisponible.
    bool LocalPlayerCamera(float& eyeX, float& eyeY, float& eyeZ, float& pitchDeg, float& yawDeg);
    // Matrice vue-projection du jeu (column-major, 16 floats). false si indisponible.
    bool GetViewProjMatrix(float vp[16]);
    // Matrices brutes vue et projection (16 floats chacune). false si indisponible.
    bool GetRawMatrices(float view[16], float proj[16]);
    // Diagnostic : scanne le GameRenderer pour localiser la position camera (~oeil).
    void DiagRenderer(float eyeX, float eyeY, float eyeZ);
    // Diagnostic : scanne l'Actor joueur pour trouver une position de rendu interpolee.
    void DiagActor(float eyeX, float eyeY, float eyeZ);
    // Vie courante + max d'une entite via l'ECS (0xFD3B0613). false si indisponible.
    bool ReadHealth(void* actor, float& cur, float& maxHp);
    // Ligne de vue : aucun bloc (non-air) entre l'oeil et la cible ? (raycast de blocs)
    // (definie mais plus appelee : occlusion retiree car instable sur cette version de MC)
    bool HasLineOfSight(float ex, float ey, float ez, float tx, float ty, float tz);
    // Diagnostic : teste des index surs pour isSolidBlockingBlock (sol=1/air=0).
    // Categorie d'une entite : 0=autre, 1=joueur, 2=animal, 3=monstre.
    int EntityCategory(void* actor);
    // Nom/pseudo de l'entite (nametag). Remplit buf, "" si absent. Lecture sure.
    bool ReadEntityName(void* actor, char* buf, int bufN);
    // Type de l'entite via le composant ECS d'identite (ex. "Skeleton"). "" si inconnu.
    bool ReadEntityType(void* actor, char* buf, int bufN);
    // Force (ou relache) l'etat "sprint enfonce" du joueur local via le MoveInputComponent.
    bool SetMoveSprint(bool on);
    // Diagnostic : dumpe le MoveInputComponent pour localiser le bit sprintDown.
    void DiagSprint();
    // Diagnostic : log les chaines (std::string) trouvees dans l'Actor (offset + contenu).
    void DiagEntityStrings(void* actor);
    // Diagnostic : scanne le JOUEUR LOCAL (pseudo connu) pour localiser l'offset du nametag.
    void DiagLocalPlayerStrings();
    // Diagnostic : enumere les composants ECS et cherche une chaine (nom) dans chacun.
    void DiagEntityComponents(void* actor);
}
