#include "game/systems.hpp"
#include "game/factories/entity_factory.hpp"
#include "engine/input.hpp"
#include "engine/math/noise.hpp"
#include "raymath.h"
#include <cmath>

namespace game::systems {

    static float swingTimer    = 0.0f;
    static float fireballTimer = 0.0f;
    static float comboTimer    = 0.0f;
    static int   swingDir      = 0;   // 0 = slash left, 1 = slash right
    static int   queuedSwing   = -1;  // -1 = none

    static const float SWING_DURATION = 0.34f;
    static const float COMBO_WINDOW   = 0.42f;
    static const float COMBO_BUFFER   = 0.14f; // queue follow-up in last part of swing
    static const float FIREBALL_COOLDOWN = 0.4f;
    static const float MELEE_DAMAGE = 35.0f;

    bool IsSwinging() { return swingTimer > 0.0f; }

    static void ApplyMeleeHit(engine::ecs::Registry& reg,
                              const game::TransformComponent& pTrans,
                              const game::CameraComponent& pCam) {
        Vector3 fwd;
        fwd.x = cosf(pCam.pitch) * sinf(pCam.yaw);
        fwd.y = sinf(pCam.pitch);
        fwd.z = cosf(pCam.pitch) * cosf(pCam.yaw);

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

            if (dot > 0.5f) {
                float dmg = MELEE_DAMAGE * (swingDir == 1 ? 1.15f : 1.0f);
                eHP.current -= dmg;
                if (engine::networking::GetLobbyState() == engine::networking::LobbyState::InLobby &&
                    !engine::networking::IsHost()) {
                    engine::networking::SendDamageToHost(reg.enemyAIs.data[i].netId, dmg);
                }
            }
        }
    }

    static void StartSwing(engine::ecs::Registry& reg,
                           const game::TransformComponent& pTrans,
                           const game::CameraComponent& pCam,
                           int dir) {
        swingDir    = dir;
        swingTimer  = SWING_DURATION;
        comboTimer  = 0.0f;
        queuedSwing = -1;
        ApplyMeleeHit(reg, pTrans, pCam);
    }

    void CombatSystem(engine::ecs::Registry& reg) {
        float dt = GetFrameTime();
        if (fireballTimer > 0.0f) fireballTimer -= dt;

        bool wasSwinging = swingTimer > 0.0f;
        if (swingTimer > 0.0f) swingTimer -= dt;

        if (reg.playerInputs.data.empty()) return;
        engine::ecs::Entity player = {reg.playerInputs.indexToEntity[0]};
        if (!reg.transforms.Has(player) || !reg.cameras.Has(player)) return;

        auto& pTrans = reg.transforms.Get(player);
        auto& pCam   = reg.cameras.Get(player);

        // Finish current swing → either queued follow-up or open combo window
        if (wasSwinging && swingTimer <= 0.0f) {
            swingTimer = 0.0f;
            if (queuedSwing >= 0) {
                StartSwing(reg, pTrans, pCam, queuedSwing);
            } else if (swingDir == 0) {
                comboTimer = COMBO_WINDOW; // press again for right slash
            } else {
                swingDir = 0; // combo finished, next click is left again
                comboTimer = 0.0f;
            }
        }

        if (swingTimer <= 0.0f && comboTimer > 0.0f) {
            comboTimer -= dt;
            if (comboTimer <= 0.0f) {
                comboTimer = 0.0f;
                swingDir = 0;
            }
        }

        // --- Melee Attack (LMB) — left slash, then combo into right slash ---
        if (engine::input::IsCursorLocked() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (swingTimer > 0.0f) {
                // Queue the right follow-up near the end of a left slash
                if (swingDir == 0 && swingTimer <= COMBO_BUFFER && queuedSwing < 0) {
                    queuedSwing = 1;
                }
            } else if (comboTimer > 0.0f && swingDir == 0) {
                StartSwing(reg, pTrans, pCam, 1);
            } else {
                StartSwing(reg, pTrans, pCam, 0);
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

        Vector3 forward = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
        Vector3 right   = Vector3Normalize(Vector3CrossProduct(forward, cam.up));
        Vector3 up      = Vector3CrossProduct(right, forward);

        float progress = 0.0f;
        if (swingTimer > 0.0f) {
            progress = 1.0f - (swingTimer / SWING_DURATION);
        }

        float slash = 0.0f;
        float raise = 0.0f;

        if (swingTimer > 0.0f) {
            if (swingDir == 0) {
                // Left slash: wind-up → cut, hold follow-through for the combo
                if (progress < 0.18f) {
                    float u = progress / 0.18f;
                    raise = u;
                    slash = u * 0.08f;
                } else {
                    float u = (progress - 0.18f) / 0.82f;
                    float e = u * u * (3.0f - 2.0f * u);
                    raise = 1.0f - e;
                    slash = 0.08f + e * 0.92f;
                }
            } else {
                // Right slash: cut across, then recover to rest
                if (progress < 0.15f) {
                    float u = progress / 0.15f;
                    raise = u * 0.5f;
                    slash = u * 0.1f;
                } else if (progress < 0.70f) {
                    float u = (progress - 0.15f) / 0.55f;
                    float e = u * u * (3.0f - 2.0f * u);
                    raise = 0.5f * (1.0f - e);
                    slash = 0.1f + e * 0.9f;
                } else {
                    float u = (progress - 0.70f) / 0.30f;
                    float e = u * u * (3.0f - 2.0f * u);
                    slash = 1.0f - e;
                    raise = 0.0f;
                }
            }
        } else if (comboTimer > 0.0f && swingDir == 0) {
            // Hold left follow-through while waiting for the right slash
            slash = 1.0f;
        }

        Vector3 restDir = Vector3Normalize(Vector3Add(
            Vector3Add(Vector3Scale(up, 0.78f), Vector3Scale(forward, 0.22f)),
            Vector3Scale(right, 0.40f)));

        float rightOffset;
        float downOffset;
        float forwardOffset;
        float slashRoll;
        float slashPitch;

        if (swingDir == 1 && (swingTimer > 0.0f || slash > 0.0f)) {
            // Hit 2: left → right follow-up
            rightOffset   = Lerp(-0.36f, 0.42f, slash);
            downOffset    = Lerp(-0.40f, -0.28f, slash) + raise * 0.12f;
            forwardOffset = Lerp(0.54f, 0.50f, slash);
            slashRoll     = Lerp(-2.20f, 0.35f, slash);
            slashPitch    = Lerp(-0.40f, -0.10f, slash);
        } else {
            // Hit 1 / idle: right → left across-body (or rest when slash == 0)
            rightOffset   = Lerp(0.40f, -0.38f, slash);
            downOffset    = Lerp(-0.28f, -0.40f, slash) + raise * 0.16f;
            forwardOffset = Lerp(0.48f, 0.56f, slash);
            slashRoll     = slash * (-2.35f);
            slashPitch    = slash * (-0.40f);
        }

        Vector3 swordPos = Vector3Add(cam.position, Vector3Scale(right, rightOffset));
        swordPos         = Vector3Add(swordPos, Vector3Scale(up, downOffset));
        swordPos         = Vector3Add(swordPos, Vector3Scale(forward, forwardOffset));

        Vector3 bladeDir = Vector3RotateByAxisAngle(restDir, forward, slashRoll);
        bladeDir = Vector3RotateByAxisAngle(bladeDir, right, slashPitch);
        bladeDir = Vector3Normalize(bladeDir);

        Vector3 side = Vector3CrossProduct(bladeDir, forward);
        if (Vector3LengthSqr(side) < 0.001f) {
            side = right;
        } else {
            side = Vector3Normalize(side);
        }

        const float bladeLength  = 0.78f;
        const float gripLength   = 0.24f;
        const float guardWidth   = 0.28f;
        const float bladeRadius  = 0.048f;
        const float tipRadius    = 0.014f;
        const float gripRadius   = 0.038f;
        const float guardRadius  = 0.032f;
        const float pommelRadius = 0.048f;

        Vector3 bladeTip = Vector3Add(swordPos, Vector3Scale(bladeDir, bladeLength));
        Vector3 gripEnd  = Vector3Subtract(swordPos, Vector3Scale(bladeDir, gripLength));
        Vector3 guardA   = Vector3Subtract(swordPos, Vector3Scale(side, guardWidth * 0.5f));
        Vector3 guardB   = Vector3Add(swordPos, Vector3Scale(side, guardWidth * 0.5f));

        Vector3 bladeMid = Vector3Lerp(swordPos, bladeTip, 0.55f);
        DrawCylinderEx(swordPos, bladeMid, bladeRadius, bladeRadius * 0.85f, 10, LIGHTGRAY);
        DrawCylinderEx(bladeMid, bladeTip, bladeRadius * 0.85f, tipRadius, 8, LIGHTGRAY);

        DrawCylinderEx(swordPos, gripEnd, gripRadius, gripRadius * 0.9f, 10, BROWN);
        DrawSphere(gripEnd, pommelRadius, DARKBROWN);
        DrawCylinderEx(guardA, guardB, guardRadius, guardRadius, 10, GOLD);
        DrawSphere(guardA, guardRadius * 1.15f, GOLD);
        DrawSphere(guardB, guardRadius * 1.15f, GOLD);
    }

}
