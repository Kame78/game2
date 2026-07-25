#include "game/systems.hpp"
#include "game/spells.hpp"
#include "engine/input.hpp"
#include "engine/math/noise.hpp"
#include "raymath.h"
#include <cmath>

namespace game::systems {

    static constexpr float MOUSE_SENSITIVITY = 0.003f;
    static constexpr float EYE_HEIGHT = 2.0f;
    static constexpr float GRAVITY = 20.0f;
    static constexpr float JUMP_FORCE = 8.0f;
    static constexpr float PLAYER_RADIUS = 0.3f;

    void PlayerMovementSystem(engine::ecs::Registry& reg) {
        float dt = GetFrameTime();
        if (dt > 0.1f) dt = 0.1f; // Cap delta time to prevent physics tunneling

        for (size_t i = 0; i < reg.playerInputs.data.size(); i++) {
            engine::ecs::Entity e = {reg.playerInputs.indexToEntity[i]};
            if (!reg.transforms.Has(e) || !reg.cameras.Has(e)) continue;

            auto& input     = reg.playerInputs.data[i];
            auto& transform = reg.transforms.Get(e);
            auto& cam       = reg.cameras.Get(e);

            bool rangerClass = false;
            if (reg.spellCasters.Has(e)) {
                rangerClass = (reg.spellCasters.Get(e).selectedElement ==
                               (uint8_t)game::SpellElement::Ranger);
            }
            if (!rangerClass) input.canDoubleJump = false;

            // --- Mouse look ---
            if (engine::input::IsCursorLocked()) {
                Vector2 mouseDelta = engine::input::GetMouseDelta();
                cam.yaw   -= mouseDelta.x * MOUSE_SENSITIVITY;
                cam.pitch -= mouseDelta.y * MOUSE_SENSITIVITY;

                // Clamp pitch to [-89 deg, 89 deg]
                if (cam.pitch >  1.55f) cam.pitch =  1.55f;
                if (cam.pitch < -1.55f) cam.pitch = -1.55f;
            }

            // --- WASD movement ---
            Vector3 forward = {sinf(cam.yaw), 0.0f, cosf(cam.yaw)};
            Vector3 right   = {-cosf(cam.yaw), 0.0f, sinf(cam.yaw)};

            Vector3 moveDir = {0.0f, 0.0f, 0.0f};
            if (engine::input::IsCursorLocked()) {
                if (engine::input::IsActionDown("MoveForward"))  moveDir = Vector3Add(moveDir, forward);
                if (engine::input::IsActionDown("MoveBackward")) moveDir = Vector3Subtract(moveDir, forward);
                if (engine::input::IsActionDown("MoveRight"))    moveDir = Vector3Add(moveDir, right);
                if (engine::input::IsActionDown("MoveLeft"))     moveDir = Vector3Subtract(moveDir, right);
            }

            if (Vector3Length(moveDir) > 0.0f) {
                moveDir = Vector3Normalize(moveDir);
            }

            // Ranger dash overrides normal move briefly
            if (input.dashTimer > 0.0f) {
                transform.position.x += input.dashVelX * dt;
                transform.position.z += input.dashVelZ * dt;
                input.dashTimer -= dt;
                if (input.dashTimer < 0.0f) input.dashTimer = 0.0f;
            } else {
                transform.position.x += moveDir.x * g_playerMoveSpeed * dt;
                transform.position.z += moveDir.z * g_playerMoveSpeed * dt;
            }

            // --- Cube collision against all renderable entities ---
            if (!input.noClip) {
                for (size_t j = 0; j < reg.renderables.data.size(); j++) {
                    engine::ecs::Entity other = {reg.renderables.indexToEntity[j]};
                    if (other == e) continue;
                    if (!reg.transforms.Has(other)) continue;

                    auto& otherT = reg.transforms.Get(other);
                    auto& otherR = reg.renderables.data[j];

                    float halfW = otherR.width  * 0.5f;
                    float halfH = otherR.height * 0.5f;
                    float halfD = otherR.depth  * 0.5f;

                    float pMinX = transform.position.x - PLAYER_RADIUS;
                    float pMaxX = transform.position.x + PLAYER_RADIUS;
                    float pMinY = transform.position.y;
                    float pMaxY = transform.position.y + EYE_HEIGHT;
                    float pMinZ = transform.position.z - PLAYER_RADIUS;
                    float pMaxZ = transform.position.z + PLAYER_RADIUS;

                    float oMinX = otherT.position.x - halfW;
                    float oMaxX = otherT.position.x + halfW;
                    float oMinY = otherT.position.y - halfH;
                    float oMaxY = otherT.position.y + halfH;
                    float oMinZ = otherT.position.z - halfD;
                    float oMaxZ = otherT.position.z + halfD;

                    if (pMaxX > oMinX && pMinX < oMaxX &&
                        pMaxY > oMinY && pMinY < oMaxY &&
                        pMaxZ > oMinZ && pMinZ < oMaxZ) {
                        
                        float pushL = pMaxX - oMinX;
                        float pushR = oMaxX - pMinX;
                        float pushB = pMaxZ - oMinZ;
                        float pushF = oMaxZ - pMinZ;

                        float minPush = pushL;
                        int axis = 0;
                        if (pushR < minPush) { minPush = pushR; axis = 1; }
                        if (pushB < minPush) { minPush = pushB; axis = 2; }
                        if (pushF < minPush) { minPush = pushF; axis = 3; }

                        if      (axis == 0) transform.position.x -= minPush;
                        else if (axis == 1) transform.position.x += minPush;
                        else if (axis == 2) transform.position.z -= minPush;
                        else                transform.position.z += minPush;
                    }
                }
            } // end !input.noClip

            // --- Jump / Ranger double jump ---
            if (engine::input::IsCursorLocked() && engine::input::IsActionPressed("Jump")) {
                if (input.grounded) {
                    input.velocityY = JUMP_FORCE;
                    input.grounded = false;
                    input.canDoubleJump = rangerClass;
                } else if (input.canDoubleJump && rangerClass && !input.isFlying) {
                    input.velocityY = JUMP_FORCE;
                    input.canDoubleJump = false;
                }
            }

            // --- Gravity / Flight ---
            if (!input.isFlying) {
                input.velocityY -= GRAVITY * dt;
                transform.position.y += input.velocityY * dt;
            } else {
                input.velocityY = 0.0f;
                if (engine::input::IsCursorLocked()) {
                    if (engine::input::IsActionDown("Jump")) transform.position.y += g_playerMoveSpeed * dt;
                    if (IsKeyDown(KEY_LEFT_SHIFT))           transform.position.y -= g_playerMoveSpeed * dt;
                }
            }

            // --- Terrain floor collision (sample world height under player) ---
            if (!input.noClip) {
                float groundY = engine::math::WorldHeight(transform.position.x, transform.position.z);
                float eyeY    = groundY + EYE_HEIGHT;
                if (transform.position.y < eyeY) {
                    transform.position.y = eyeY;
                    input.velocityY = 0.0f;
                    input.grounded = true;
                }
            } else {
                input.grounded = false;
            }

            // --- Sync camera to transform + angles ---
            cam.camera.position = transform.position;

            Vector3 forward3D;
            forward3D.x = cosf(cam.pitch) * sinf(cam.yaw);
            forward3D.y = sinf(cam.pitch);
            forward3D.z = cosf(cam.pitch) * cosf(cam.yaw);

            cam.camera.target = Vector3Add(cam.camera.position, forward3D);
        }
    }

}
