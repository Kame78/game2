#include "game/factories/entity_factory.hpp"

// --- NEW: Generalized EntityFactory implementation ---
namespace game::factories {

    engine::ecs::Entity EntityFactory::CreatePlayer(engine::ecs::Registry& reg, Vector3 spawnPos) {
        engine::ecs::Entity playerEntity = engine::ecs::CreateEntity(reg);

        game::TransformComponent playerTransform;
        playerTransform.position = spawnPos;

        game::CameraComponent playerCam;
        playerCam.camera.position = playerTransform.position;
        playerCam.camera.target   = {spawnPos.x, spawnPos.y, spawnPos.z + 1.0f};
        playerCam.camera.up       = {0.0f, 1.0f, 0.0f};
        playerCam.camera.fovy     = 90.0f;
        playerCam.camera.projection = CAMERA_PERSPECTIVE;

        game::PlayerInputComponent playerInput;
        game::HealthComponent playerHP;
        playerHP.current = 100.0f;
        playerHP.max     = 100.0f;

        reg.transforms.Insert(playerEntity, playerTransform);
        reg.cameras.Insert(playerEntity, playerCam);
        reg.playerInputs.Insert(playerEntity, playerInput);
        reg.healths.Insert(playerEntity, playerHP);

        return playerEntity;
    }

    engine::ecs::Entity EntityFactory::CreateEnemy(engine::ecs::Registry& reg, Vector3 spawnPos, uint32_t netId) {
        engine::ecs::Entity enemy = engine::ecs::CreateEntity(reg);
        
        game::TransformComponent t;
        t.position = spawnPos;
        
        game::RenderComponent r;
        r.color = RED;
        r.width = 1.0f;
        r.height = 2.0f;
        r.depth = 1.0f;
        
        game::HealthComponent hp;
        hp.current = 100.0f;
        hp.max = 100.0f;
        
        game::EnemyAIComponent ai;
        ai.netId = netId;

        reg.transforms.Insert(enemy, t);
        reg.renderables.Insert(enemy, r);
        reg.healths.Insert(enemy, hp);
        reg.enemyAIs.Insert(enemy, ai);

        return enemy;
    }

    engine::ecs::Entity EntityFactory::CreateProjectile(engine::ecs::Registry& reg, Vector3 startPos, Vector3 direction) {
        engine::ecs::Entity proj = engine::ecs::CreateEntity(reg);

        game::TransformComponent t;
        t.position = startPos;

        game::ProjectileComponent p;
        p.direction = direction;
        p.speed     = 25.0f;
        p.damage    = 50.0f;
        p.aoeRadius = 4.0f;
        p.lifetime  = 3.0f;
        p.radius    = 0.25f;

        reg.transforms.Insert(proj, t);
        reg.projectiles.Insert(proj, p);

        return proj;
    }

}
