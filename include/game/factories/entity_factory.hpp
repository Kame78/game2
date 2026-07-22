#pragma once
#include "engine/ecs/registry.hpp"
#include "raylib.h"

// --- NEW: Generalized EntityFactory for assembling ECS entities ---
namespace game::factories {

    class EntityFactory {
    public:
        static engine::ecs::Entity CreatePlayer(engine::ecs::Registry& reg, Vector3 spawnPos);
        static engine::ecs::Entity CreateEnemy(engine::ecs::Registry& reg, Vector3 spawnPos, uint32_t netId);
        static engine::ecs::Entity CreateProjectile(engine::ecs::Registry& reg, Vector3 startPos, Vector3 direction);
    };

}
