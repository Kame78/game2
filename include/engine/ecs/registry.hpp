#pragma once
#include <vector>
#include <cstdint>
#include <unordered_map>
#include "game/components.hpp" // The Static ECS explicitly knows about your game components!

namespace engine::ecs {

    // Generational Entity ID
    struct Entity {
        uint32_t id;
        bool operator==(const Entity& other) const { return id == other.id; }
        
        uint32_t GetIndex() const { return id & 0xFFFFFF; } // Lower 24 bits
        uint32_t GetGeneration() const { return (id >> 24) & 0xFF; } // Upper 8 bits
    };

    // Pure Struct Component Array (Zero virtuals)
    template <typename T>
    struct SparseSet {
        std::vector<T> data;
        std::unordered_map<uint32_t, size_t> entityToIndex;  // raw index → dense index
        std::unordered_map<size_t, uint32_t> indexToEntity;  // dense index → full entity id

        void Insert(Entity entity, T component) {
            uint32_t index = entity.GetIndex();
            size_t dataIndex = data.size();
            entityToIndex[index] = dataIndex;
            indexToEntity[dataIndex] = entity.id;  // Store full ID with generation
            data.push_back(component);
        }

        void Remove(Entity entity) {
            uint32_t index = entity.GetIndex();
            if (entityToIndex.find(index) == entityToIndex.end()) return;
            
            size_t dataIndexOfRemoved = entityToIndex[index];
            size_t indexOfLastElement = data.size() - 1;
            
            data[dataIndexOfRemoved] = data[indexOfLastElement];
            uint32_t fullIdOfLast = indexToEntity[indexOfLastElement];
            uint32_t rawIndexOfLast = fullIdOfLast & 0xFFFFFF;
            
            entityToIndex[rawIndexOfLast] = dataIndexOfRemoved;
            indexToEntity[dataIndexOfRemoved] = fullIdOfLast;
            
            entityToIndex.erase(index);
            indexToEntity.erase(indexOfLastElement);
            data.pop_back();
        }

        T& Get(Entity entity) { return data[entityToIndex[entity.GetIndex()]]; }
        bool Has(Entity entity) { return entityToIndex.find(entity.GetIndex()) != entityToIndex.end(); }
    };

    // The Central Registry Struct
    struct Registry {
        // Recycling Data
        std::vector<uint8_t> generations;
        std::vector<uint32_t> freeIndices;
        uint32_t activeEntityCount = 0;

        // Your Hardcoded Component Arrays (Update this when you invent new components!)
        SparseSet<game::TransformComponent> transforms;
        SparseSet<game::CameraComponent> cameras;
        SparseSet<game::PlayerInputComponent> playerInputs;
        SparseSet<game::RenderComponent> renderables;
        SparseSet<game::HealthComponent> healths;
        SparseSet<game::EnemyAIComponent> enemyAIs;
        SparseSet<game::ProjectileComponent> projectiles;
        SparseSet<game::SpawnerComponent> spawners;
        SparseSet<game::LandmarkProxyComponent> landmarkProxies;
    };

    // --- Global Namespaced Functions ---

    inline Entity CreateEntity(Registry& reg) {
        uint32_t index;
        if (reg.freeIndices.empty()) {
            index = reg.generations.size();
            reg.generations.push_back(0); // Start at Generation 0
        } else {
            index = reg.freeIndices.back(); // Recycle an old index
            reg.freeIndices.pop_back();
        }
        reg.activeEntityCount++;
        return Entity{ (uint32_t(reg.generations[index]) << 24) | index };
    }

    inline void DestroyEntity(Registry& reg, Entity entity) {
        uint32_t index = entity.GetIndex();
        
        // Prevent destroying stale entities or destroying twice
        if (index >= reg.generations.size() || reg.generations[index] != entity.GetGeneration()) return;

        // 1. Manually clean up data (Update this when you invent new components!)
        reg.transforms.Remove(entity);
        reg.cameras.Remove(entity);
        reg.playerInputs.Remove(entity);
        reg.renderables.Remove(entity);
        reg.healths.Remove(entity);
        reg.enemyAIs.Remove(entity);
        reg.projectiles.Remove(entity);
        reg.spawners.Remove(entity);
        reg.landmarkProxies.Remove(entity);
        
        // 2. Recycle the ID
        reg.generations[index]++; // Increment generation so old IDs pointing here become invalid
        reg.freeIndices.push_back(index);
        reg.activeEntityCount--;
    }

    inline bool IsValid(Registry& reg, Entity entity) {
        uint32_t idx = entity.GetIndex();
        return idx < reg.generations.size() && reg.generations[idx] == entity.GetGeneration();
    }
}