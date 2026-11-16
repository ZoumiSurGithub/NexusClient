#pragma once
// =============================================================================
//  Couche entite ECS pour lire les composants EnTT de Minecraft Bedrock.
//  Reproduite d'apres le SDK de Latite (LatiteClient/latite, GPLv3).
//  Necessite EnTT 3.15.0 (commit fe8d7d7...) vendorise dans third_party/entt.
//  NOTE LICENCE : ce fichier derive de code GPLv3 -> Nexus est GPLv3.
// =============================================================================
#include <cstdint>
#include <concepts>
#include <type_traits>
#include <string>
#include <memory>
#pragma warning(push, 0)        // EnTT est bruyant sous /W4 : on tait ses warnings
#include <entt/entt.hpp>
#pragma warning(pop)

// --- Identifiant d'entite Bedrock : 18 bits d'index + 14 bits de version ------
struct EntityId {
    uint32_t rawId;
    [[nodiscard]] constexpr bool operator==(const EntityId& other) const = default;
    [[nodiscard]] constexpr operator uint32_t() const { return this->rawId; }
};

struct EntityIdTraits {
    using value_type = EntityId;
    using entity_type = uint32_t;
    using version_type = uint16_t;
    static constexpr entity_type entity_mask = 0x3FFFF;  // 18 bits bas
    static constexpr entity_type version_mask = 0x3FFF;  // 14 bits haut
};

struct IEntityComponent {};

// --- Specialisations EnTT (doivent matcher le build de Bedrock) ---------------
template<>
struct entt::entt_traits<EntityId> : entt::basic_entt_traits<EntityIdTraits> {
    static constexpr std::size_t page_size = 2048;
};

template<std::derived_from<IEntityComponent> Type>
struct entt::component_traits<Type, EntityId> {
    using element_type = Type;
    using entity_type = EntityId;
    static constexpr bool in_place_delete = true;
    static constexpr std::size_t page_size = 128 * !std::is_empty_v<Type>;
};

template<typename Type>
struct entt::storage_type<Type, EntityId> {
    using type = basic_storage<Type, EntityId>;
};

template<std::derived_from<IEntityComponent> Type>
struct entt::type_hash<Type> {
    [[nodiscard]] static consteval id_type value() noexcept {
        constexpr auto hash = Type::type_hash;  // identite du composant (hash Bedrock)
        return hash;
    }
    [[nodiscard]] consteval operator id_type() const noexcept { return value(); }
};

class EntityRegistry : public std::enable_shared_from_this<EntityRegistry> {
public:
    std::string name;
    entt::basic_registry<EntityId> ownedRegistry;
    uint32_t id;
};

// Pose a Actor + 0x8 ; les references sont des pointeurs en memoire.
class EntityContext {
public:
    EntityRegistry& registry;                        // +0x00
    entt::basic_registry<EntityId>& enttRegistry;    // +0x08
    EntityId entity;                                 // +0x10
};

// --- Composant d'equipement : { handContainer, armorContainer } ---------------
struct ActorEquipmentComponent : IEntityComponent {
    static constexpr uint32_t type_hash = 0xB06141A9;
    void* handContainer;    // Inventory*  (+0x00)
    void* armorContainer;   // Inventory*  (+0x08)
};

// --- Composant d'attribut de vie (hash trouve par scan sur 26.63) --------------
// Contenu opaque : on atteint l'AttributeInstance par ses pointeurs et on lit
// le cluster [max,max,min,max,courante]. Lecture brute cote appelant.
struct HealthAttributeComponent : IEntityComponent {
    static constexpr uint32_t type_hash = 0xFD3B0613;
    void* ptr0;   // premier pointeur -> bloc contenant l'instance
};
