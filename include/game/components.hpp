#pragma once
#include "raylib.h"

namespace game {
    struct TransformComponent {
        Vector3 position = {0.0f, 0.0f, 0.0f};
        Vector3 rotation = {0.0f, 0.0f, 0.0f};
        Vector3 scale    = {1.0f, 1.0f, 1.0f};
    };

    struct CameraComponent {
        Camera3D camera = {};
        float yaw   = 0.0f;
        float pitch = 0.0f;
    };

    struct PlayerInputComponent {
        float velocityY = 0.0f;
        bool grounded   = true;
        bool isFlying   = false;
        bool noClip     = false;
    };

    struct RenderComponent {
        Color color  = WHITE;
        float width  = 1.0f;
        float height = 1.0f;
        float depth  = 1.0f;
    };

    struct HealthComponent {
        float current = 100.0f;
        float max     = 100.0f;
    };

    struct EnemyAIComponent {
        // Optional root-motion multiplier (1.0 = authored/synthetic stride).
        float speed          = 1.0f;
        float attackRange    = 1.8f;
        float attackDamage   = 8.0f;
        float attackCooldown = 1.0f;
        float attackTimer    = 0.0f;
        uint32_t netId       = 0;  // network ID for multiplayer sync

        // Per-enemy animation state (advanced in AI; posed at draw time).
        int   animClip   = -1;     // game::enemy_model::AnimClip (-1 = unset)
        int   animIndex  = -1;     // resolved ModelAnimation index
        int   animFrame  = 0;
        float animTimer  = 0.0f;
        float animPlaybackRate = 1.0f;
        bool  attackAnim = false;  // play Bite through once after a hit
    };

    struct ProjectileComponent {
        Vector3 direction = {0.0f, 0.0f, 0.0f};
        float speed       = 25.0f;
        float damage      = 50.0f;
        float aoeRadius   = 4.0f;
        float lifetime    = 3.0f;   // seconds before auto-despawn
        float radius      = 0.25f;  // visual + collision radius
    };

    // Editor-placed spawn point (EnemySpawnSystem also drains these).
    struct SpawnerComponent {
        float radius   = 18.0f;
        float interval = 4.0f;
        float timer    = 0.0f;
        int   maxAlive = 4;
    };

    // Editor-placed landmark proxy (visual only — does not flatten terrain).
    struct LandmarkProxyComponent {
        int typeIndex = 0; // maps to LandmarkType / LANDMARKS table
    };
}