#pragma once
#include "engine/ecs/registry.hpp"
#include "game/spells.hpp"
#include "raylib.h"

// --- NEW: Generalized EntityFactory for assembling ECS entities ---
namespace game::factories {

    class EntityFactory {
    public:
        static engine::ecs::Entity CreatePlayer(engine::ecs::Registry& reg, Vector3 spawnPos,
                                                game::SpellElement spellClass = game::SpellElement::Fire);
        static engine::ecs::Entity CreateEnemy(engine::ecs::Registry& reg, Vector3 spawnPos, uint32_t netId);
        // 2× size and 2× HP vs base enemy. Manual spawn only (editor / tools).
        static engine::ecs::Entity CreateEliteEnemy(engine::ecs::Registry& reg, Vector3 spawnPos, uint32_t netId);
        // ~25 ft tall; HP / damage / reach scale with height vs base enemy. Manual spawn only.
        static engine::ecs::Entity CreateGiantEnemy(engine::ecs::Registry& reg, Vector3 spawnPos, uint32_t netId);
        // ~100 ft tall; HP / damage = 3× the 25 ft giant. Manual spawn only.
        static engine::ecs::Entity CreateColossalEnemy(engine::ecs::Registry& reg, Vector3 spawnPos, uint32_t netId);
        // ~250 ft tall; HP / damage = 2.5× the 100 ft colossal. Manual spawn only.
        static engine::ecs::Entity CreateTitanEnemy(engine::ecs::Registry& reg, Vector3 spawnPos, uint32_t netId);
        static engine::ecs::Entity CreateProjectile(engine::ecs::Registry& reg, Vector3 startPos, Vector3 direction);
        static engine::ecs::Entity CreateProjectileFromSpell(engine::ecs::Registry& reg, Vector3 startPos,
                                                             Vector3 direction, int spellId);
        static engine::ecs::Entity CreateSpawner(engine::ecs::Registry& reg, Vector3 spawnPos);
        static engine::ecs::Entity CreateLandmarkProxy(engine::ecs::Registry& reg, Vector3 spawnPos, int typeIndex);
    };

}
