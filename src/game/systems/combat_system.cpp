#include "game/systems.hpp"
#include "game/factories/entity_factory.hpp"
#include "engine/input.hpp"
#include "engine/math/noise.hpp"
#include "raymath.h"
#include <cmath>

namespace game::systems {

    static float swingTimer    = 0.0f;
    static float fireballTimer = 0.0f;
    static const float SWING_DURATION = 0.25f;
    static const float FIREBALL_COOLDOWN = 0.4f;

    bool IsSwinging() { return swingTimer > 0.0f; }

    void CombatSystem(engine::ecs::Registry& reg) {
        float dt = GetFrameTime();
        if (swingTimer > 0.0f)    swingTimer    -= dt;
        if (fireballTimer > 0.0f) fireballTimer -= dt;

        if (reg.playerInputs.data.empty()) return;
        engine::ecs::Entity player = {reg.playerInputs.indexToEntity[0]};
        if (!reg.transforms.Has(player) || !reg.cameras.Has(player)) return;

        auto& pTrans = reg.transforms.Get(player);
        auto& pCam   = reg.cameras.Get(player);

        // --- Melee Attack (LMB) ---
        if (engine::input::IsCursorLocked() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && swingTimer <= 0.0f) {
            swingTimer = SWING_DURATION;

            Vector3 fwd;
            fwd.x = cosf(pCam.pitch) * sinf(pCam.yaw);
            fwd.y = sinf(pCam.pitch);
            fwd.z = cosf(pCam.pitch) * cosf(pCam.yaw);

            // Hit all enemies in cone (range 3.0, angle threshold)
            for (size_t i = 0; i < reg.enemyAIs.data.size(); i++) {
                engine::ecs::Entity enemy = {reg.enemyAIs.indexToEntity[i]};
                if (!reg.transforms.Has(enemy) || !reg.healths.Has(enemy)) continue;

                auto& eTrans = reg.transforms.Get(enemy);
                auto& eHP    = reg.healths.Get(enemy);

                Vector3 toEnemy = Vector3Subtract(eTrans.position, pTrans.position);
                float dist = Vector3Length(toEnemy);
                if (dist > 3.5f || dist < 0.1f) continue;

                Vector3 dirToEnemy = Vector3Scale(toEnemy, 1.0f / dist);
                float dot = Vector3DotProduct(fwd, dirToEnemy);

                if (dot > 0.5f) { // ~60 deg cone
                    eHP.current -= 35.0f;
                    // Send damage event to host if we are client
                    if (engine::networking::GetLobbyState() == engine::networking::LobbyState::InLobby &&
                        !engine::networking::IsHost()) {
                        engine::networking::SendDamageToHost(reg.enemyAIs.data[i].netId, 35.0f);
                    }
                }
            }
        }

        // --- Fireball (RMB) ---
        if (engine::input::IsCursorLocked() && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && fireballTimer <= 0.0f) {
            fireballTimer = FIREBALL_COOLDOWN;

            Vector3 fwd;
            fwd.x = cosf(pCam.pitch) * sinf(pCam.yaw);
            fwd.y = sinf(pCam.pitch);
            fwd.z = cosf(pCam.pitch) * cosf(pCam.yaw);

            Vector3 spawnPos = Vector3Add(pTrans.position, Vector3Scale(fwd, 0.8f));
            factories::EntityFactory::CreateProjectile(reg, spawnPos, fwd);
        }
    }

    void ProjectileSystem(engine::ecs::Registry& reg) {
        float dt = GetFrameTime();

        for (int i = (int)reg.projectiles.data.size() - 1; i >= 0; i--) {
            engine::ecs::Entity proj = {reg.projectiles.indexToEntity[i]};
            if (!reg.transforms.Has(proj)) continue;

            auto& p = reg.projectiles.data[i];
            auto& t = reg.transforms.Get(proj);

            p.lifetime -= dt;
            t.position = Vector3Add(t.position, Vector3Scale(p.direction, p.speed * dt));

            bool hit = false;
            // Hit check vs terrain
            float groundY = engine::math::WorldHeight(t.position.x, t.position.z);
            if (t.position.y <= groundY + p.radius) {
                hit = true;
            }

            // Hit check vs enemies
            if (!hit) {
                for (size_t j = 0; j < reg.enemyAIs.data.size(); j++) {
                    engine::ecs::Entity enemy = {reg.enemyAIs.indexToEntity[j]};
                    if (!reg.transforms.Has(enemy)) continue;
                    auto& eTrans = reg.transforms.Get(enemy);
                    if (Vector3Distance(t.position, eTrans.position) < (p.radius + 0.8f)) {
                        hit = true;
                        break;
                    }
                }
            }

            if (hit || p.lifetime <= 0.0f) {
                // Apply AOE damage on hit
                if (hit) {
                    for (size_t j = 0; j < reg.enemyAIs.data.size(); j++) {
                        engine::ecs::Entity enemy = {reg.enemyAIs.indexToEntity[j]};
                        if (!reg.transforms.Has(enemy) || !reg.healths.Has(enemy)) continue;
                        auto& eTrans = reg.transforms.Get(enemy);
                        auto& eHP    = reg.healths.Get(enemy);
                        float dist = Vector3Distance(t.position, eTrans.position);
                        if (dist <= p.aoeRadius) {
                            float falloff = 1.0f - (dist / p.aoeRadius);
                            float dmg = p.damage * falloff;
                            eHP.current -= dmg;
                            if (engine::networking::GetLobbyState() == engine::networking::LobbyState::InLobby &&
                                !engine::networking::IsHost()) {
                                engine::networking::SendDamageToHost(reg.enemyAIs.data[j].netId, dmg);
                            }
                        }
                    }
                }
                engine::ecs::DestroyEntity(reg, proj);
            }
        }
    }

    void SwordViewmodelSystem(engine::ecs::Registry& reg) {
        if (reg.playerInputs.data.empty()) return;
        engine::ecs::Entity player = {reg.playerInputs.indexToEntity[0]};
        if (!reg.cameras.Has(player)) return;

        auto& cam = reg.cameras.Get(player).camera;

        // Viewmodel math: position relative to camera right/up/forward vectors
        Vector3 forward = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
        Vector3 right   = Vector3Normalize(Vector3CrossProduct(forward, cam.up));
        Vector3 up      = Vector3CrossProduct(right, forward);

        float swingProgress = 0.0f;
        if (swingTimer > 0.0f) {
            swingProgress = 1.0f - (swingTimer / SWING_DURATION);
        }

        // Sword resting offset
        float rightOffset   = 0.35f;
        float downOffset    = -0.30f;
        float forwardOffset = 0.60f;

        // Swing animation curve
        float yawAnim   = 0.0f;
        float pitchAnim = 0.0f;
        if (swingProgress > 0.0f) {
            float t = sinf(swingProgress * PI);
            rightOffset   -= t * 0.40f;
            forwardOffset += t * 0.20f;
            yawAnim        = -t * 0.8f;
            pitchAnim      =  t * 0.5f;
        }

        Vector3 swordPos = Vector3Add(cam.position, Vector3Scale(right, rightOffset));
        swordPos         = Vector3Add(swordPos, Vector3Scale(up, downOffset));
        swordPos         = Vector3Add(swordPos, Vector3Scale(forward, forwardOffset));

        // Draw simple blockout sword
        float bladeLength = 0.9f;
        float hiltWidth   = 0.25f;

        // Blade direction
        Vector3 bladeDir = forward;
        bladeDir = Vector3RotateByAxisAngle(bladeDir, up, yawAnim);
        bladeDir = Vector3RotateByAxisAngle(bladeDir, right, pitchAnim);
        Vector3 bladeTip = Vector3Add(swordPos, Vector3Scale(bladeDir, bladeLength));

        // Draw hilt (cylinder)
        DrawCylinderEx(Vector3Subtract(swordPos, Vector3Scale(right, hiltWidth*0.5f)),
                       Vector3Add(swordPos, Vector3Scale(right, hiltWidth*0.5f)),
                       0.02f, 0.02f, 8, GOLD);
        // Draw blade (cylinder)
        DrawCylinderEx(swordPos, bladeTip, 0.03f, 0.0f, 8, LIGHTGRAY);
    }

}
