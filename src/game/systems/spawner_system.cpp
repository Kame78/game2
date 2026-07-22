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

    void EnemySpawnSystem(engine::ecs::Registry& reg) {
        if (reg.playerInputs.data.empty()) return;
        engine::ecs::Entity player = {reg.playerInputs.indexToEntity[0]};
        if (!reg.transforms.Has(player)) return;
        Vector3 playerPos = reg.transforms.Get(player).position;

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

            // Use the generalized EntityFactory to create the enemy entity
            factories::EntityFactory::CreateEnemy(reg, pos, nextNetId++);

            totalSpawned++;
            active++;
        }
    }

}
