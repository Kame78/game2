#include "game/systems.hpp"
#include "game/factories/entity_factory.hpp"
#include "engine/math/noise.hpp"
#include "raymath.h"
#include <cstdlib>

namespace game::systems {

    static int totalSpawned = 0;
    static const int MAX_ACTIVE = 20;
    static const int MAX_TOTAL  = 50;
    static uint32_t nextNetId   = 1000;

    static int countNearbyEnemies(engine::ecs::Registry& reg, Vector3 center, float radius) {
        int n = 0;
        const float r2 = radius * radius;
        for (size_t i = 0; i < reg.enemyAIs.data.size(); ++i) {
            engine::ecs::Entity e = {reg.enemyAIs.indexToEntity[i]};
            if (!reg.transforms.Has(e)) continue;
            Vector3 p = reg.transforms.Get(e).position;
            float dx = p.x - center.x;
            float dz = p.z - center.z;
            if (dx * dx + dz * dz <= r2) ++n;
        }
        return n;
    }

    void EnemySpawnSystem(engine::ecs::Registry& reg) {
        if (reg.playerInputs.data.empty()) return;
        engine::ecs::Entity player = {reg.playerInputs.indexToEntity[0]};
        if (!reg.transforms.Has(player)) return;
        Vector3 playerPos = reg.transforms.Get(player).position;

        const float dt = GetFrameTime();

        // Editor-placed spawners (additive; does not replace default ring spawn).
        for (size_t i = 0; i < reg.spawners.data.size(); ++i) {
            engine::ecs::Entity se = {reg.spawners.indexToEntity[i]};
            if (!reg.transforms.Has(se)) continue;
            auto& sp = reg.spawners.data[i];
            Vector3 origin = reg.transforms.Get(se).position;
            sp.timer -= dt;
            if (sp.timer > 0.0f) continue;
            sp.timer = sp.interval;

            if (totalSpawned >= MAX_TOTAL) continue;
            if (countNearbyEnemies(reg, origin, sp.radius) >= sp.maxAlive) continue;
            if ((int)reg.enemyAIs.data.size() >= MAX_ACTIVE) continue;

            float angle = ((float)rand() / (float)RAND_MAX) * 2.0f * PI;
            float dist = ((float)rand() / (float)RAND_MAX) * sp.radius;
            float sx = origin.x + cosf(angle) * dist;
            float sz = origin.z + sinf(angle) * dist;
            Vector3 pos = {sx, engine::math::WorldHeight(sx, sz) + 1.0f, sz};
            factories::EntityFactory::CreateEnemy(reg, pos, nextNetId++);
            totalSpawned++;
        }

        int active = (int)reg.enemyAIs.data.size();
        while (active < MAX_ACTIVE && totalSpawned < MAX_TOTAL) {
            // Spawn in a ring 15-25 units from player
            float angle = ((float)rand() / (float)RAND_MAX) * 2.0f * PI;
            float dist = 15.0f + ((float)rand() / (float)RAND_MAX) * 10.0f;
            float sx = playerPos.x + cosf(angle) * dist;
            float sz = playerPos.z + sinf(angle) * dist;
            Vector3 pos = {
                sx,
                engine::math::WorldHeight(sx, sz) + 1.0f,
                sz
            };

            factories::EntityFactory::CreateEnemy(reg, pos, nextNetId++);

            totalSpawned++;
            active++;
        }
    }

}
