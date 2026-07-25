#include "game/systems.hpp"
#include "engine/math/noise.hpp"
#include "raymath.h"
#include <cmath>

namespace game::systems {

    void EnemyAISystem(engine::ecs::Registry& reg) {
        float dt = GetFrameTime();

        if (reg.playerInputs.data.empty()) return;
        engine::ecs::Entity player = {reg.playerInputs.indexToEntity[0]};
        if (!reg.transforms.Has(player) || !reg.healths.Has(player)) return;

        Vector3 playerPos = reg.transforms.Get(player).position;
        auto&   playerHP  = reg.healths.Get(player);

        for (int i = (int)reg.enemyAIs.data.size() - 1; i >= 0; i--) {
            engine::ecs::Entity enemy = {reg.enemyAIs.indexToEntity[i]};
            if (!reg.transforms.Has(enemy) || !reg.healths.Has(enemy)) continue;

            auto& ai   = reg.enemyAIs.data[i];
            auto& t    = reg.transforms.Get(enemy);
            auto& eHP  = reg.healths.Get(enemy);

            // Check if dead
            if (eHP.current <= 0.0f) {
                engine::ecs::DestroyEntity(reg, enemy);
                continue;
            }

            // Move towards player
            Vector3 toPlayer = Vector3Subtract(playerPos, t.position);
            toPlayer.y = 0.0f; // Walk on terrain floor
            float dist = Vector3Length(toPlayer);

            float halfH = 1.0f;
            if (reg.renderables.Has(enemy)) {
                halfH = reg.renderables.Get(enemy).height * 0.5f;
            }

            if (dist > ai.attackRange) {
                Vector3 dir = Vector3Normalize(toPlayer);
                t.position.x += dir.x * ai.speed * dt;
                t.position.z += dir.z * ai.speed * dt;
                if (reg.renderables.Has(enemy)) {
                    reg.renderables.Get(enemy).facingYaw = atan2f(dir.x, dir.z);
                }

                // Snap to ground height (center of AABB = half render height)
                float groundY = engine::math::WorldHeight(t.position.x, t.position.z);
                t.position.y = groundY + halfH;
            } else {
                // Stay grounded while attacking
                float groundY = engine::math::WorldHeight(t.position.x, t.position.z);
                t.position.y = groundY + halfH;
                if (dist > 0.01f && reg.renderables.Has(enemy)) {
                    Vector3 dir = Vector3Normalize(toPlayer);
                    reg.renderables.Get(enemy).facingYaw = atan2f(dir.x, dir.z);
                }

                // Attack cooldown
                ai.attackTimer -= dt;
                if (ai.attackTimer <= 0.0f) {
                    ai.attackTimer = ai.attackCooldown;
                    playerHP.current -= ai.attackDamage;
                }
            }
        }
    }

}
