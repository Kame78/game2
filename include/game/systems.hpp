#pragma once
#include "engine/ecs/registry.hpp"

namespace game::systems {
    void PlayerMovementSystem(engine::ecs::Registry& registry);
    void CombatSystem(engine::ecs::Registry& registry);
    void ProjectileSystem(engine::ecs::Registry& registry);
    void EnemyAISystem(engine::ecs::Registry& registry);
    void EnemySpawnSystem(engine::ecs::Registry& registry);
    void NetworkSyncSystem(engine::ecs::Registry& registry);
    void Render3DSystem(engine::ecs::Registry& registry);
    void SwordViewmodelSystem(engine::ecs::Registry& registry);
    void HealthBarSystem(engine::ecs::Registry& registry);
}