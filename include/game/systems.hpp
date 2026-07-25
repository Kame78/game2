#pragma once
#include "engine/ecs/registry.hpp"
#include "engine/networking.hpp"

namespace game::systems {
    extern bool g_showEditor;
    extern float g_playerMoveSpeed;

    void PlayerMovementSystem(engine::ecs::Registry& registry);
    void CombatSystem(engine::ecs::Registry& registry);
    void ProjectileSystem(engine::ecs::Registry& registry);
    void SpellSystem(engine::ecs::Registry& registry);
    void SpellVfxRenderSystem(engine::ecs::Registry& registry); // inside BeginMode3D
    bool TryCastSpell(engine::ecs::Registry& registry, int spellId, bool freeCast = false);
    void NotifyProjectileImpact(engine::ecs::Registry& registry, uint8_t spellId, Vector3 pos);
    void EnemyAISystem(engine::ecs::Registry& registry);
    void EnemySpawnSystem(engine::ecs::Registry& registry);
    void NetworkSyncSystem(engine::ecs::Registry& registry);
    void Render3DSystem(engine::ecs::Registry& registry);
    void SwordViewmodelSystem(engine::ecs::Registry& registry);
    void WeaponViewmodelInit();
    void WeaponViewmodelShutdown();
    void HealthBarSystem(engine::ecs::Registry& registry);
    void EditorInputSystem(engine::ecs::Registry& registry);
    void EditorUISystem(engine::ecs::Registry& registry);
    void EditorDebugDrawSystem(engine::ecs::Registry& registry); // inside BeginMode3D
    void DrawRemotePlayer(const engine::networking::PlayerState& remote);
    void SpawnRemoteFireballs(engine::ecs::Registry& registry);
    bool IsSwinging();
}