#pragma once
#include <cstddef>
// Lecture d'items (armure, main) via ECS + fonctions du jeu.
// Inclut EnTT -> compile a part. L'appelant DOIT envelopper les appels dans
// __try/__except (crash possible si un offset est faux).

struct NxItemOut {
    bool present;
    int  damage;
    int  maxDamage;
    int  count;
    char name[48];   // UTF-8, '\0'-termine, codes couleur retires
};

struct NxItemCfg {
    void* getDamageFn;              // ItemStackBase::getDamageValue
    int   maxDmgVtIndex;            // Item::getMaxDamage (vtable)
    void* getHoverNameFn;           // ItemStackBase::getHoverName (peut etre nul)
    std::ptrdiff_t itemCountOff;    // ItemStackBase -> count (0x22)
    std::ptrdiff_t itemPtrOff;      // ItemStackBase -> Item** (0x8)
    std::ptrdiff_t itemIdStrOff;    // Item -> std::string de l'id (0xD8 = HashedString id +0x8)
    std::ptrdiff_t suppliesOff;     // Player -> PlayerInventory* (0x5E0)
    std::ptrdiff_t invOff;          // PlayerInventory -> Inventory* (0xB8)
    std::ptrdiff_t selSlotOff;      // PlayerInventory -> selectedSlot (0x10)
    int   getItemIndex;             // Inventory::getItem (vtable 7)
};

// Durabilite + quantite depuis un ItemStack* (SANS le nom). nullptr/vide -> present=false.
bool  Nexus_ReadItemCore(void* stack, const NxItemCfg* cfg, NxItemOut* out);
// Nom affiche uniquement (getHoverName), isole car le plus risque. Ecrit "" si indispo.
void  Nexus_ReadItemName(void* stack, const NxItemCfg* cfg, char* out, int cap);
// Stack d'une piece d'armure (slot 0..3) via le composant ECS.
void* Nexus_GetArmorStack(void* localPlayer, int slot, int getItemIndex);
// Stack de l'objet en main (slot selectionne de l'inventaire).
void* Nexus_GetHeldStack(void* localPlayer, const NxItemCfg* cfg);
// Compte total d'exemplaires du MEME item que 'refStack' dans l'inventaire (36 slots),
// compare par pointeur Item* (meme type, enchantements/durabilite ignores). 0 si indispo.
int   Nexus_CountMatching(void* localPlayer, const NxItemCfg* cfg, void* refStack);
// Remplit hashes[]/ptrs[] avec les composants presents sur l'entite. Renvoie le nombre.
int   Nexus_ListComponents(void* actor, unsigned int* hashes, void** ptrs, int maxN);
