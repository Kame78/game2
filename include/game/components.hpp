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
        float speed          = 3.0f;
        float attackRange    = 1.8f;
        float attackDamage   = 8.0f;
        float attackCooldown = 1.0f;
        float attackTimer    = 0.0f;
        uint32_t netId       = 0;  // network ID for multiplayer sync
    };

    struct ProjectileComponent {
        Vector3 direction = {0.0f, 0.0f, 0.0f};
        float speed       = 25.0f;
        float damage      = 50.0f;
        float aoeRadius   = 4.0f;
        float lifetime    = 3.0f;   // seconds before auto-despawn
        float radius      = 0.25f;  // visual + collision radius
    };
}