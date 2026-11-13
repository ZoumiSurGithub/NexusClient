#pragma once
#include <array>

namespace Game {
    enum Slot { Helmet = 0, Chestplate = 1, Leggings = 2, Boots = 3, Held = 4, SlotCount = 5 };

    struct ItemInfo {
        bool present = false;     // un item est dans le slot
        int  damage = 0;          // degats subis
        int  maxDamage = 0;       // 0 = item sans durabilite (citrouille, bloc, ...)
        int Durability() const { return maxDamage - damage; }
        float Ratio() const { return maxDamage > 0 ? float(maxDamage - damage) / float(maxDamage) : 1.f; }
    };

    struct Snapshot {
        bool inGame = false;
        bool demo = false;
        std::array<ItemInfo, SlotCount> items{};
    };

    struct FunctionStatus { const char* name; bool found; bool required; };

    bool Init();          // resout les signatures, installe le hook joueur (MinHook deja initialise)
    void Shutdown();
    Snapshot Read();      // appele depuis le thread de rendu, chaque image
    Snapshot Demo();
    void* LocalPlayer();  // LocalPlayer* courant, ou nullptr (menus du jeu / offsets manquants)      // valeurs factices animees (mode demo / apercu dans le menu)
    bool IsConfigured();  // toutes les signatures obligatoires ont ete trouvees
    std::array<FunctionStatus, 5> GetFunctionStatus();
}
