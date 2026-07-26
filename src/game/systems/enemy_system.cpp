#include "game/systems.hpp"
#include "game/enemy_model.hpp"
#include "game/dungeon/dungeon.hpp"
#include "engine/math/noise.hpp"
#include "raymath.h"
#include <cmath>

namespace game::systems {

namespace {
    constexpr float kNewTargetHeight = 2.55f;
    // Run only when speed multiplier is intentionally raised.
    constexpr float kRunSpeedGate = 1.35f;
    // Separation radius at base zombie width (0.9 m) — scales with RenderComponent.width.
    constexpr float kBaseEnemyRadius = 0.50f;
    constexpr float kBaseEnemyWidth  = 0.9f;
    constexpr int   kSeparateIters = 3;

    float enemyHalfHeight(engine::ecs::Registry& reg, engine::ecs::Entity e) {
        if (reg.renderables.Has(e)) {
            return reg.renderables.Get(e).height * 0.5f;
        }
        if (game::enemy_model::IsReady()) {
            return game::enemy_model::GetTargetHeight() * 0.5f;
        }
        return kNewTargetHeight * 0.5f;
    }

    float enemySepRadius(engine::ecs::Registry& reg, engine::ecs::Entity e) {
        if (reg.renderables.Has(e)) {
            const float w = reg.renderables.Get(e).width;
            if (w > 1e-4f) {
                return kBaseEnemyRadius * (w / kBaseEnemyWidth);
            }
        }
        return kBaseEnemyRadius;
    }

    float enemySizeMul(engine::ecs::Registry& reg, engine::ecs::Entity e) {
        const float targetH = game::enemy_model::IsReady()
            ? game::enemy_model::GetTargetHeight()
            : kNewTargetHeight;
        if (reg.renderables.Has(e) && targetH > 1e-4f) {
            return reg.renderables.Get(e).height / targetH;
        }
        return 1.0f;
    }

    void setEnemyClip(game::EnemyAIComponent& ai, game::enemy_model::AnimClip clip) {
        const int clipId = static_cast<int>(clip);
        const int newIndex = game::enemy_model::GetAnimIndex(clip);
        if (newIndex < 0) return;

        if (ai.animClip == clipId && ai.animIndex == newIndex) return;

        ai.animClip  = clipId;
        ai.animIndex = newIndex;
        ai.animFrame = 0;
        ai.animTimer = 0.0f;
    }

    void snapEnemyToGround(game::TransformComponent& t, float halfH) {
        float groundY = game::dungeon::GroundY(t.position.x, t.position.z);
        t.position.y = groundY + halfH;
    }

    void separateEnemies(engine::ecs::Registry& reg) {
        auto& ais = reg.enemyAIs;

        for (int iter = 0; iter < kSeparateIters; ++iter) {
            for (size_t i = 0; i < ais.data.size(); ++i) {
                engine::ecs::Entity ei = {ais.indexToEntity[i]};
                if (!reg.transforms.Has(ei)) continue;
                auto& ti = reg.transforms.Get(ei);
                const float ri = enemySepRadius(reg, ei);

                for (size_t j = i + 1; j < ais.data.size(); ++j) {
                    engine::ecs::Entity ej = {ais.indexToEntity[j]};
                    if (!reg.transforms.Has(ej)) continue;
                    auto& tj = reg.transforms.Get(ej);
                    const float rj = enemySepRadius(reg, ej);
                    const float minDist = ri + rj;
                    const float minDist2 = minDist * minDist;

                    float dx = tj.position.x - ti.position.x;
                    float dz = tj.position.z - ti.position.z;
                    float d2 = dx * dx + dz * dz;
                    if (d2 >= minDist2) continue;

                    float dist = sqrtf(d2);
                    float nx, nz;
                    if (dist < 1e-4f) {
                        nx = 1.0f;
                        nz = 0.0f;
                        dist = 0.0f;
                    } else {
                        nx = dx / dist;
                        nz = dz / dist;
                    }

                    float push = (minDist - dist) * 0.5f;
                    ti.position.x -= nx * push;
                    ti.position.z -= nz * push;
                    tj.position.x += nx * push;
                    tj.position.z += nz * push;
                }
            }
        }

        for (size_t i = 0; i < ais.data.size(); ++i) {
            engine::ecs::Entity e = {ais.indexToEntity[i]};
            if (!reg.transforms.Has(e)) continue;
            snapEnemyToGround(reg.transforms.Get(e), enemyHalfHeight(reg, e));
        }
    }

    // Model +Z forward with yaw (matches DrawModelEx / player forward).
    void applyModelMotionXZ(game::TransformComponent& t, Vector2 motionXZ, float yawDeg) {
        if (fabsf(motionXZ.x) < 1e-8f && fabsf(motionXZ.y) < 1e-8f) return;
        float yaw = yawDeg * DEG2RAD;
        float s = sinf(yaw);
        float c = cosf(yaw);
        // motionXZ.x = model X, motionXZ.y = model Z (forward)
        t.position.x += motionXZ.x * c + motionXZ.y * s;
        t.position.z += -motionXZ.x * s + motionXZ.y * c;
    }
}

    void EnemyAISystem(engine::ecs::Registry& reg) {
        float dt = GetFrameTime();
        if (dt < 1e-6f) dt = 1e-6f;

        if (reg.playerInputs.data.empty()) return;
        engine::ecs::Entity player = {reg.playerInputs.indexToEntity[0]};
        if (!reg.transforms.Has(player) || !reg.healths.Has(player)) return;

        Vector3 playerPos = reg.transforms.Get(player).position;
        auto&   playerHP  = reg.healths.Get(player);

        for (int i = (int)reg.enemyAIs.data.size() - 1; i >= 0; i--) {
            engine::ecs::Entity enemy = {reg.enemyAIs.indexToEntity[i]};
            if (!reg.healths.Has(enemy)) continue;
            if (reg.healths.Get(enemy).current <= 0.0f) {
                engine::ecs::DestroyEntity(reg, enemy);
            }
        }

        const bool zombieReady = game::enemy_model::IsReady();

        for (size_t i = 0; i < reg.enemyAIs.data.size(); ++i) {
            engine::ecs::Entity enemy = {reg.enemyAIs.indexToEntity[i]};
            if (!reg.transforms.Has(enemy) || !reg.healths.Has(enemy)) continue;

            auto& ai = reg.enemyAIs.data[i];
            auto& t  = reg.transforms.Get(enemy);
            const float halfH = enemyHalfHeight(reg, enemy);

            Vector3 toPlayer = {
                playerPos.x - t.position.x,
                0.0f,
                playerPos.z - t.position.z
            };
            float dist = Vector3Length(toPlayer);

            if (zombieReady) {
                if (dist > 0.01f) {
                    t.rotation.y = atan2f(toPlayer.x, toPlayer.z) * RAD2DEG;
                }

                const bool inAttackRange = dist <= ai.attackRange;
                ai.animPlaybackRate = ai.animRateScale;
                const float sizeMul = enemySizeMul(reg, enemy);

                if (!inAttackRange) {
                    ai.attackAnim = false;
                    if (ai.speed >= kRunSpeedGate) {
                        setEnemyClip(ai, game::enemy_model::AnimClip::Run);
                    } else {
                        setEnemyClip(ai, game::enemy_model::AnimClip::Walk);
                    }

                    Vector2 motion = {0.0f, 0.0f};
                    game::enemy_model::AdvanceAnimation(
                        ai.animIndex, dt, ai.animPlaybackRate,
                        ai.animFrame, ai.animTimer, motion);

                    // speed = root-motion multiplier; sizeMul keeps stride proportional
                    // to visual scale so large zombies don't skate.
                    motion.x *= ai.speed * sizeMul;
                    motion.y *= ai.speed * sizeMul;
                    applyModelMotionXZ(t, motion, t.rotation.y);
                    snapEnemyToGround(t, halfH);
                } else {
                    ai.attackTimer -= dt;
                    if (ai.attackTimer <= 0.0f) {
                        ai.attackTimer = ai.attackCooldown;
                        playerHP.current -= ai.attackDamage;
                        ai.attackAnim = true;
                        ai.animClip = -1;
                        setEnemyClip(ai, game::enemy_model::AnimClip::Attack);
                    }

                    if (ai.attackAnim) {
                        setEnemyClip(ai, game::enemy_model::AnimClip::Attack);
                    } else {
                        setEnemyClip(ai, game::enemy_model::AnimClip::Idle);
                    }

                    Vector2 motion = {0.0f, 0.0f};
                    bool wrapped = game::enemy_model::AdvanceAnimation(
                        ai.animIndex, dt, 1.0f,
                        ai.animFrame, ai.animTimer, motion);
                    // Idle/Bite: discard translation (in-place clips).
                    (void)motion;

                    if (wrapped && ai.attackAnim &&
                        ai.animClip == static_cast<int>(game::enemy_model::AnimClip::Attack)) {
                        ai.attackAnim = false;
                    }
                    snapEnemyToGround(t, halfH);
                }
            } else {
                // Fallback if zombie mesh failed to load: simple chase.
                if (dist > ai.attackRange) {
                    Vector3 dir = Vector3Normalize(toPlayer);
                    t.position.x += dir.x * ai.speed * dt;
                    t.position.z += dir.z * ai.speed * dt;
                    if (reg.renderables.Has(enemy)) {
                        reg.renderables.Get(enemy).facingYaw = atan2f(dir.x, dir.z);
                    }
                    snapEnemyToGround(t, halfH);
                } else {
                    snapEnemyToGround(t, halfH);
                    if (dist > 0.01f && reg.renderables.Has(enemy)) {
                        Vector3 dir = Vector3Normalize(toPlayer);
                        reg.renderables.Get(enemy).facingYaw = atan2f(dir.x, dir.z);
                    }

                    ai.attackTimer -= dt;
                    if (ai.attackTimer <= 0.0f) {
                        ai.attackTimer = ai.attackCooldown;
                        playerHP.current -= ai.attackDamage;
                    }
                }
            }
        }

        separateEnemies(reg);
    }

}
