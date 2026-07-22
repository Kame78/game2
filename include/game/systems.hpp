#pragma once
#include "engine/ecs/registry.hpp"
#include "engine/networking.hpp"

namespace game::systems {
    extern bool g_showEditor;
    extern float g_playerMoveSpeed;

    void PlayerMovementSystem(engine::ecs::Registry& registry);
    void CombatSystem(engine::ecs::Registry& registry);
    void ProjectileSystem(engine::ecs::Registry& registry);
    void EnemyAISystem(engine::ecs::Registry& registry);
    void EnemySpawnSystem(engine::ecs::Registry& registry);
    void NetworkSyncSystem(engine::ecs::Registry& registry);
    void Render3DSystem(engine::ecs::Registry& registry);
    void SwordViewmodelSystem(engine::ecs::Registry& registry);
    void HealthBarSystem(engine::ecs::Registry& registry);
    void EditorInputSystem(engine::ecs::Registry& registry);
    void EditorUISystem(engine::ecs::Registry& registry);
    void DrawRemotePlayer(const engine::networking::PlayerState& remote);
    void SpawnRemoteFireballs(engine::ecs::Registry& registry);
    bool IsSwinging();
}