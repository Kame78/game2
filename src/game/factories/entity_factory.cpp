#include "game/factories/entity_factory.hpp"
#include "game/enemy_model.hpp"
#include "game/spells.hpp"

// --- NEW: Generalized EntityFactory implementation ---
namespace game::factories {

    engine::ecs::Entity EntityFactory::CreatePlayer(engine::ecs::Registry& reg, Vector3 spawnPos,
                                                    game::SpellElement spellClass) {
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

        game::SpellCasterComponent caster;
        caster.selectedElement = (uint8_t)spellClass;
        caster.selectedSlot = 0;

        reg.transforms.Insert(playerEntity, playerTransform);
        reg.cameras.Insert(playerEntity, playerCam);
        reg.playerInputs.Insert(playerEntity, playerInput);
        reg.healths.Insert(playerEntity, playerHP);
        reg.spellCasters.Insert(playerEntity, caster);

        return playerEntity;
    }

    engine::ecs::Entity EntityFactory::CreateEnemy(engine::ecs::Registry& reg, Vector3 spawnPos, uint32_t netId) {
        engine::ecs::Entity enemy = engine::ecs::CreateEntity(reg);
        
        game::TransformComponent t;
        t.position = spawnPos;
        
        const float bodyH = game::enemy_model::IsReady()
            ? game::enemy_model::GetTargetHeight()
            : 2.55f;

        game::RenderComponent r;
        r.color = RED;
        // Box visual → shared zombie mesh path in render/AI.
        r.width = 0.9f;
        r.height = bodyH;
        r.depth = 0.9f;
        r.visual = game::CharacterVisual::Box;
        
        game::HealthComponent hp;
        hp.current = 100.0f;
        hp.max = 100.0f;
        
        game::EnemyAIComponent ai;
        ai.netId = netId;
        ai.speed = 1.0f; // root-motion multiplier

        reg.transforms.Insert(enemy, t);
        reg.renderables.Insert(enemy, r);
        reg.healths.Insert(enemy, hp);
        reg.enemyAIs.Insert(enemy, ai);

        return enemy;
    }

    engine::ecs::Entity EntityFactory::CreateEliteEnemy(engine::ecs::Registry& reg, Vector3 spawnPos, uint32_t netId) {
        engine::ecs::Entity enemy = engine::ecs::CreateEntity(reg);

        game::TransformComponent t;
        t.position = spawnPos;

        game::RenderComponent r;
        r.color = Color{180, 40, 40, 255};
        r.width = 2.0f;
        r.height = 4.0f;
        r.depth = 2.0f;
        r.visual = game::CharacterVisual::Elite;

        game::HealthComponent hp;
        hp.current = 200.0f;
        hp.max = 200.0f;

        game::EnemyAIComponent ai;
        ai.netId = netId;

        reg.transforms.Insert(enemy, t);
        reg.renderables.Insert(enemy, r);
        reg.healths.Insert(enemy, hp);
        reg.enemyAIs.Insert(enemy, ai);

        return enemy;
    }

    engine::ecs::Entity EntityFactory::CreateGiantEnemy(engine::ecs::Registry& reg, Vector3 spawnPos, uint32_t netId) {
        // Base enemy is 2.0 units (~6.56 ft). 25 ft → 25 * 0.3048 m.
        constexpr float kFeetToMeters = 0.3048f;
        constexpr float kBaseHeight   = 2.0f;
        constexpr float kHeight       = 25.0f * kFeetToMeters; // 7.62 m
        constexpr float kScale        = kHeight / kBaseHeight;  // ~3.81×

        engine::ecs::Entity enemy = engine::ecs::CreateEntity(reg);

        game::TransformComponent t;
        t.position = spawnPos;

        game::RenderComponent r;
        r.color = Color{90, 25, 25, 255};
        r.width = 1.0f * kScale;
        r.height = kHeight;
        r.depth = 1.0f * kScale;
        r.visual = game::CharacterVisual::Elite;

        game::HealthComponent hp;
        hp.max = 100.0f * kScale;
        hp.current = hp.max;

        game::EnemyAIComponent ai;
        ai.netId = netId;
        ai.attackDamage = 8.0f * kScale;
        ai.attackRange = 1.8f * kScale;
        ai.speed = 2.4f; // slower than base (3.0) — big frame

        reg.transforms.Insert(enemy, t);
        reg.renderables.Insert(enemy, r);
        reg.healths.Insert(enemy, hp);
        reg.enemyAIs.Insert(enemy, ai);

        return enemy;
    }

    engine::ecs::Entity EntityFactory::CreateColossalEnemy(engine::ecs::Registry& reg, Vector3 spawnPos, uint32_t netId) {
        // 100 ft body; combat stats = 3× the 25 ft giant.
        constexpr float kFeetToMeters = 0.3048f;
        constexpr float kBaseHeight   = 2.0f;
        constexpr float kHeight       = 100.0f * kFeetToMeters; // 30.48 m
        constexpr float kSizeScale    = kHeight / kBaseHeight;
        constexpr float kGiantScale   = (25.0f * kFeetToMeters) / kBaseHeight;
        constexpr float kStatMul      = 3.0f; // vs 25 ft giant

        engine::ecs::Entity enemy = engine::ecs::CreateEntity(reg);

        game::TransformComponent t;
        t.position = spawnPos;

        game::RenderComponent r;
        r.color = Color{55, 15, 20, 255};
        r.width = 1.0f * kSizeScale;
        r.height = kHeight;
        r.depth = 1.0f * kSizeScale;
        r.visual = game::CharacterVisual::Elite;

        game::HealthComponent hp;
        hp.max = 100.0f * kGiantScale * kStatMul;
        hp.current = hp.max;

        game::EnemyAIComponent ai;
        ai.netId = netId;
        ai.attackDamage = 8.0f * kGiantScale * kStatMul;
        ai.attackRange = 1.8f * kSizeScale; // reach matches 100 ft frame
        ai.speed = 1.8f;

        reg.transforms.Insert(enemy, t);
        reg.renderables.Insert(enemy, r);
        reg.healths.Insert(enemy, hp);
        reg.enemyAIs.Insert(enemy, ai);

        return enemy;
    }

    engine::ecs::Entity EntityFactory::CreateTitanEnemy(engine::ecs::Registry& reg, Vector3 spawnPos, uint32_t netId) {
        // 250 ft body; combat stats = 2.5× the 100 ft colossal (height ratio).
        constexpr float kFeetToMeters = 0.3048f;
        constexpr float kBaseHeight   = 2.0f;
        constexpr float kHeight       = 250.0f * kFeetToMeters; // 76.2 m
        constexpr float kSizeScale    = kHeight / kBaseHeight;
        constexpr float kGiantScale   = (25.0f * kFeetToMeters) / kBaseHeight;
        constexpr float kColossalStat = 3.0f;   // vs 25 ft giant
        constexpr float kStatMul      = kColossalStat * (250.0f / 100.0f); // 7.5× giant

        engine::ecs::Entity enemy = engine::ecs::CreateEntity(reg);

        game::TransformComponent t;
        t.position = spawnPos;

        game::RenderComponent r;
        r.color = Color{35, 8, 12, 255};
        r.width = 1.0f * kSizeScale;
        r.height = kHeight;
        r.depth = 1.0f * kSizeScale;
        r.visual = game::CharacterVisual::Elite;

        game::HealthComponent hp;
        hp.max = 100.0f * kGiantScale * kStatMul;
        hp.current = hp.max;

        game::EnemyAIComponent ai;
        ai.netId = netId;
        ai.attackDamage = 8.0f * kGiantScale * kStatMul;
        ai.attackRange = 1.8f * kSizeScale;
        ai.speed = 1.2f;

        reg.transforms.Insert(enemy, t);
        reg.renderables.Insert(enemy, r);
        reg.healths.Insert(enemy, hp);
        reg.enemyAIs.Insert(enemy, ai);

        return enemy;
    }

    engine::ecs::Entity EntityFactory::CreateProjectile(engine::ecs::Registry& reg, Vector3 startPos, Vector3 direction) {
        return CreateProjectileFromSpell(reg, startPos, direction, (int)game::SpellId::Fireball);
    }

    engine::ecs::Entity EntityFactory::CreateProjectileFromSpell(engine::ecs::Registry& reg, Vector3 startPos,
                                                                 Vector3 direction, int spellId) {
        const game::SpellDef& def = game::GetSpellDef(spellId);
        engine::ecs::Entity proj = engine::ecs::CreateEntity(reg);

        game::TransformComponent t;
        t.position = startPos;

        game::ProjectileComponent p;
        p.direction = direction;
        p.speed     = def.projectileSpeed;
        p.damage    = def.damage;
        p.aoeRadius = def.aoeRadius;
        p.lifetime  = def.lifetime;
        p.radius    = def.projectileRadius;
        p.spellId   = (uint8_t)spellId;
        p.piercing  = def.piercing;
        p.pierceCount = 0;

        reg.transforms.Insert(proj, t);
        reg.projectiles.Insert(proj, p);

        return proj;
    }

    engine::ecs::Entity EntityFactory::CreateSpawner(engine::ecs::Registry& reg, Vector3 spawnPos) {
        engine::ecs::Entity e = engine::ecs::CreateEntity(reg);

        game::TransformComponent t;
        t.position = spawnPos;

        game::RenderComponent r;
        r.color = Color{255, 180, 40, 255};
        r.width = 1.4f;
        r.height = 0.4f;
        r.depth = 1.4f;

        game::SpawnerComponent s;

        reg.transforms.Insert(e, t);
        reg.renderables.Insert(e, r);
        reg.spawners.Insert(e, s);
        return e;
    }

    engine::ecs::Entity EntityFactory::CreateLandmarkProxy(engine::ecs::Registry& reg, Vector3 spawnPos, int typeIndex) {
        engine::ecs::Entity e = engine::ecs::CreateEntity(reg);

        game::TransformComponent t;
        t.position = spawnPos;

        game::RenderComponent r;
        r.color = Color{160, 100, 255, 255};
        r.width = 2.0f;
        r.height = 3.0f;
        r.depth = 2.0f;

        game::LandmarkProxyComponent lm;
        lm.typeIndex = typeIndex;

        reg.transforms.Insert(e, t);
        reg.renderables.Insert(e, r);
        reg.landmarkProxies.Insert(e, lm);
        return e;
    }

}
