#include "game/systems.hpp"
#include "engine/input.hpp"
#include "core/window.hpp"
#include "raymath.h"
#include <cmath>

namespace game::systems {

    void PlayerMovementSystem(engine::ecs::Registry& registry) {
        float dt = core::window::GetDeltaTime();
        
        // Pure C-Style iteration: Look at everything with PlayerInput, then check if it has Transform/Camera
        for (auto const& [index, dataIndex] : registry.playerInputs.entityToIndex) {
            
            // Reconstruct the Generational Entity ID
            engine::ecs::Entity entity = { (uint32_t(registry.generations[index]) << 24) | index };

            if (registry.transforms.Has(entity) && registry.cameras.Has(entity)) {
                auto& input = registry.playerInputs.Get(entity);
                auto& transform = registry.transforms.Get(entity);
                auto& camComp = registry.cameras.Get(entity);

                // 1. Mouse Look
                Vector2 mouseDelta = engine::input::GetMouseDelta();
                transform.rotation.y -= mouseDelta.x * 0.003f;
                transform.rotation.x -= mouseDelta.y * 0.003f;

                if (transform.rotation.x > 1.5f) transform.rotation.x = 1.5f;
                if (transform.rotation.x < -1.5f) transform.rotation.x = -1.5f;

                // 2. Keyboard Movement
                Vector3 forward = { sinf(transform.rotation.y), 0.0f, cosf(transform.rotation.y) };
                Vector3 right = { cosf(transform.rotation.y), 0.0f, -sinf(transform.rotation.y) };

                Vector3 move = {0.0f, 0.0f, 0.0f};
                if (engine::input::IsActionDown("MoveForward"))  move = Vector3Add(move, forward);
                if (engine::input::IsActionDown("MoveBackward")) move = Vector3Subtract(move, forward);
                if (engine::input::IsActionDown("MoveRight"))    move = Vector3Add(move, right);
                if (engine::input::IsActionDown("MoveLeft"))     move = Vector3Subtract(move, right);

                move = Vector3Normalize(move);
                transform.position.x += move.x * input.speed * dt;
                transform.position.z += move.z * input.speed * dt;

                // 3. Gravity & Jumping
                const float GRAVITY = -25.0f;
                if (!input.isGrounded) {
                    input.verticalVelocity += GRAVITY * dt;
                }

                if (engine::input::IsActionPressed("Jump") && input.isGrounded) {
                    input.verticalVelocity = 10.0f;
                    input.isGrounded = false;
                }

                transform.position.y += input.verticalVelocity * dt;

                if (transform.position.y <= 2.0f) { 
                    transform.position.y = 2.0f;
                    input.verticalVelocity = 0.0f;
                    input.isGrounded = true;
                }

                // 4. Update Camera
                camComp.camera.position = transform.position;
                Vector3 lookDir;
                lookDir.x = cosf(transform.rotation.x) * sinf(transform.rotation.y);
                lookDir.y = sinf(transform.rotation.x);
                lookDir.z = cosf(transform.rotation.x) * cosf(transform.rotation.y);
                camComp.camera.target = Vector3Add(camComp.camera.position, lookDir);
            }
        }
    }

    void Render3DSystem(engine::ecs::Registry& registry) {
        // Iterate anything that has a RenderComponent
        for (auto const& [index, dataIndex] : registry.renderables.entityToIndex) {
            
            engine::ecs::Entity entity = { (uint32_t(registry.generations[index]) << 24) | index };

            // Check if it also has a transform
            if (registry.transforms.Has(entity)) {
                auto& render = registry.renderables.Get(entity);
                auto& transform = registry.transforms.Get(entity);
                
                DrawCube(transform.position, render.size.x, render.size.y, render.size.z, render.color);
                DrawCubeWires(transform.position, render.size.x, render.size.y, render.size.z, DARKGRAY);
            }
        }
    }
}