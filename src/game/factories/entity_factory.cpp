#include "game/factories/entity_factory.hpp"
#include "game/enemy_model.hpp"
#include "game/spells.hpp"

// --- NEW: Generalized EntityFactory implementation ---
namespace game::factories {

namespace {
    constexpr float kFeetToMeters = 0.3048f;
    // Legacy capsule height used for giant+ combat stat ratios (pre-zombie merge).
    constexpr float kLegacyBaseHeight = 2.0f;
    constexpr float kZombieHeight = 2.55f;
    constexpr float kZombieWidth  = 0.9f;
    constexpr float kBaseAttackRange = 1.8f;
    constexpr float kBaseDamage = 8.0f;
    constexpr float kBaseHP = 100.0f;

    float zombieBodyHeight() {
        return game::enemy_model::IsReady()
            ? game::enemy_model::GetTargetHeight()
            : kZombieHeight;
    }

    void setZombieRender(game::RenderComponent& r, float height, float width,
                         Color color) {
        r.color = color;
        r.width = width;
        r.height = height;
        r.depth = width;
        // Box marks the shared Quaternius zombie mesh path (all EnemyAI tiers).
        r.visual = game::CharacterVisual::Box;
    }
}

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

        const float bodyH = zombieBodyHeight();

        game::RenderComponent r;
        setZombieRender(r, bodyH, kZombieWidth, RED);

        game::HealthComponent hp;
        hp.current = kBaseHP;
        hp.max = kBaseHP;

        game::EnemyAIComponent ai;
        ai.netId = netId;
        ai.speed = 1.0f; // root-motion multiplier
        ai.attackRange = kBaseAttackRange;

        reg.transforms.Insert(enemy, t);
        reg.renderables.Insert(enemy, r);
        reg.healths.Insert(enemy, hp);
        reg.enemyAIs.Insert(enemy, ai);

        return enemy;
    }

    engine::ecs::Entity EntityFactory::CreateEliteEnemy(engine::ecs::Registry& reg, Vector3 spawnPos, uint32_t netId) {
        // ~2x prior capsule (4 m tall, 2 m wide).
        constexpr float kHeight = 4.0f;
        constexpr float kWidth  = 2.0f;
        constexpr float kSizeMul = kHeight / kLegacyBaseHeight; // 2x

        engine::ecs::Entity enemy = engine::ecs::CreateEntity(reg);

        game::TransformComponent t;
        t.position = spawnPos;

        game::RenderComponent r;
        setZombieRender(r, kHeight, kWidth, Color{180, 40, 40, 255});

        game::HealthComponent hp;
        hp.current = kBaseHP * 2.0f;
        hp.max = hp.current;

        game::EnemyAIComponent ai;
        ai.netId = netId;
        ai.speed = 1.0f;
        ai.attackDamage = kBaseDamage * kSizeMul;
        ai.attackRange = kBaseAttackRange * kSizeMul;

        reg.transforms.Insert(enemy, t);
        reg.renderables.Insert(enemy, r);
        reg.healths.Insert(enemy, hp);
        reg.enemyAIs.Insert(enemy, ai);

        return enemy;
    }

    engine::ecs::Entity EntityFactory::CreateGiantEnemy(engine::ecs::Registry& reg, Vector3 spawnPos, uint32_t netId) {
        // 25 ft body; combat stats scale with height vs legacy 2 m capsule.
        constexpr float kHeight = 25.0f * kFeetToMeters; // 7.62 m
        constexpr float kScale  = kHeight / kLegacyBaseHeight;  // ~3.81x
        const float width = kZombieWidth * (kHeight / kZombieHeight);

        engine::ecs::Entity enemy = engine::ecs::CreateEntity(reg);

        game::TransformComponent t;
        t.position = spawnPos;

        game::RenderComponent r;
        setZombieRender(r, kHeight, width, Color{90, 25, 25, 255});

        game::HealthComponent hp;
        hp.max = kBaseHP * kScale;
        hp.current = hp.max;

        game::EnemyAIComponent ai;
        ai.netId = netId;
        ai.attackDamage = kBaseDamage * kScale;
        ai.attackRange = kBaseAttackRange * kScale;
        // Root-motion multiplier (stride also x height ratio in AI).
        ai.speed = 0.85f;

        reg.transforms.Insert(enemy, t);
        reg.renderables.Insert(enemy, r);
        reg.healths.Insert(enemy, hp);
        reg.enemyAIs.Insert(enemy, ai);

        return enemy;
    }

    engine::ecs::Entity EntityFactory::CreateColossalEnemy(engine::ecs::Registry& reg, Vector3 spawnPos, uint32_t netId) {
        // 100 ft body; combat stats = 3x the 25 ft giant.
        constexpr float kHeight     = 100.0f * kFeetToMeters; // 30.48 m
        constexpr float kSizeScale  = kHeight / kLegacyBaseHeight;
        constexpr float kGiantScale = (25.0f * kFeetToMeters) / kLegacyBaseHeight;
        constexpr float kStatMul    = 3.0f; // vs 25 ft giant
        const float width = kZombieWidth * (kHeight / kZombieHeight);

        engine::ecs::Entity enemy = engine::ecs::CreateEntity(reg);

        game::TransformComponent t;
        t.position = spawnPos;

        game::RenderComponent r;
        setZombieRender(r, kHeight, width, Color{55, 15, 20, 255});

        game::HealthComponent hp;
        hp.max = kBaseHP * kGiantScale * kStatMul;
        hp.current = hp.max;

        game::EnemyAIComponent ai;
        ai.netId = netId;
        ai.attackDamage = kBaseDamage * kGiantScale * kStatMul;
        ai.attackRange = kBaseAttackRange * kSizeScale;
        ai.speed = 0.70f;

        reg.transforms.Insert(enemy, t);
        reg.renderables.Insert(enemy, r);
        reg.healths.Insert(enemy, hp);
        reg.enemyAIs.Insert(enemy, ai);

        return enemy;
    }

    engine::ecs::Entity EntityFactory::CreateTitanEnemy(engine::ecs::Registry& reg, Vector3 spawnPos, uint32_t netId) {
        // 250 ft body; combat stats = 2.5x the 100 ft colossal (height ratio).
        constexpr float kHeight       = 250.0f * kFeetToMeters; // 76.2 m
        constexpr float kSizeScale    = kHeight / kLegacyBaseHeight;
        constexpr float kGiantScale   = (25.0f * kFeetToMeters) / kLegacyBaseHeight;
        constexpr float kColossalStat = 3.0f;   // vs 25 ft giant
        constexpr float kStatMul      = kColossalStat * (250.0f / 100.0f); // 7.5x giant
        const float width = kZombieWidth * (kHeight / kZombieHeight);

        engine::ecs::Entity enemy = engine::ecs::CreateEntity(reg);

        game::TransformComponent t;
        t.position = spawnPos;

        game::RenderComponent r;
        setZombieRender(r, kHeight, width, Color{35, 8, 12, 255});

        game::HealthComponent hp;
        hp.max = kBaseHP * kGiantScale * kStatMul;
        hp.current = hp.max;

        game::EnemyAIComponent ai;
        ai.netId = netId;
        ai.attackDamage = kBaseDamage * kGiantScale * kStatMul;
        ai.attackRange = kBaseAttackRange * kSizeScale;
        ai.speed = 0.55f;

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
