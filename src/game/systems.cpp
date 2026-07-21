#include "game/systems.hpp"
#include "engine/input.hpp"
#include "engine/networking.hpp"
#include "engine/math/noise.hpp"
#include "engine/terrain/chunk_manager.hpp"
#include "raylib.h"
#include "raymath.h"
#include <cmath>
#include <cstdlib>
#include <unordered_map>

namespace game::systems {

    static constexpr float MOUSE_SENSITIVITY = 0.003f;
    static constexpr float MOVE_SPEED = 10.0f;
    static constexpr float EYE_HEIGHT = 2.0f;
    static constexpr float GRAVITY = 20.0f;
    static constexpr float JUMP_FORCE = 8.0f;
    static constexpr float PLAYER_RADIUS = 0.3f;
    static constexpr float SWORD_RANGE = 3.0f;
    static constexpr float SWORD_DAMAGE = 34.0f;
    static constexpr float SWING_DURATION = 0.35f;
    static float swingTimer = 0.0f;
    static constexpr float FIREBALL_COOLDOWN = 0.8f;
    static float fireballTimer = 0.0f;

    // Enemy spawn manager state
    static int totalSpawned = 0;
    static uint32_t nextNetId = 1;
    static constexpr int MAX_ACTIVE = 30;
    static constexpr int MAX_TOTAL = 100;
    static int syncTickCounter = 0;

    // Client-side mapping: netId -> local entity
    static std::unordered_map<uint32_t, engine::ecs::Entity> netIdToEntity;

    void PlayerMovementSystem(engine::ecs::Registry& reg) {
        float dt = GetFrameTime();

        for (size_t i = 0; i < reg.playerInputs.data.size(); i++) {
            engine::ecs::Entity e = {reg.playerInputs.indexToEntity[i]};
            if (!reg.transforms.Has(e) || !reg.cameras.Has(e)) continue;

            auto& input     = reg.playerInputs.data[i];
            auto& transform = reg.transforms.Get(e);
            auto& cam       = reg.cameras.Get(e);

            // --- Mouse look ---
            if (engine::input::IsCursorLocked()) {
                Vector2 mouseDelta = engine::input::GetMouseDelta();
                cam.yaw   -= mouseDelta.x * MOUSE_SENSITIVITY;
                cam.pitch -= mouseDelta.y * MOUSE_SENSITIVITY;

                if (cam.pitch >  1.5f) cam.pitch =  1.5f;
                if (cam.pitch < -1.5f) cam.pitch = -1.5f;
            }

            // --- WASD movement ---
            Vector3 forward = {sinf(cam.yaw), 0.0f, cosf(cam.yaw)};
            Vector3 right   = {-cosf(cam.yaw), 0.0f, sinf(cam.yaw)};

            Vector3 moveDir = {0.0f, 0.0f, 0.0f};
            if (engine::input::IsActionDown("MoveForward"))  moveDir = Vector3Add(moveDir, forward);
            if (engine::input::IsActionDown("MoveBackward")) moveDir = Vector3Subtract(moveDir, forward);
            if (engine::input::IsActionDown("MoveRight"))    moveDir = Vector3Add(moveDir, right);
            if (engine::input::IsActionDown("MoveLeft"))     moveDir = Vector3Subtract(moveDir, right);

            if (Vector3Length(moveDir) > 0.0f) {
                moveDir = Vector3Normalize(moveDir);
            }

            transform.position.x += moveDir.x * MOVE_SPEED * dt;
            transform.position.z += moveDir.z * MOVE_SPEED * dt;

            // --- Cube collision against all renderable entities ---
            for (size_t j = 0; j < reg.renderables.data.size(); j++) {
                engine::ecs::Entity other = {reg.renderables.indexToEntity[j]};
                if (other == e) continue;
                if (!reg.transforms.Has(other)) continue;

                auto& otherPos = reg.transforms.Get(other).position;
                auto& r        = reg.renderables.data[j];

                float halfW = r.width  * 0.5f;
                float halfD = r.depth  * 0.5f;
                float minX = otherPos.x - halfW - PLAYER_RADIUS;
                float maxX = otherPos.x + halfW + PLAYER_RADIUS;
                float minZ = otherPos.z - halfD - PLAYER_RADIUS;
                float maxZ = otherPos.z + halfD + PLAYER_RADIUS;
                float minY = otherPos.y - r.height * 0.5f;
                float maxY = otherPos.y + r.height * 0.5f;

                float feetY = transform.position.y - EYE_HEIGHT;

                if (feetY < maxY && transform.position.y > minY) {
                    bool insideX = transform.position.x > minX && transform.position.x < maxX;
                    bool insideZ = transform.position.z > minZ && transform.position.z < maxZ;

                    if (insideX && insideZ) {
                        float pushLeft  = transform.position.x - minX;
                        float pushRight = maxX - transform.position.x;
                        float pushFront = transform.position.z - minZ;
                        float pushBack  = maxZ - transform.position.z;
                        float minPush = pushLeft;
                        int axis = 0;

                        if (pushRight < minPush) { minPush = pushRight; axis = 1; }
                        if (pushFront < minPush) { minPush = pushFront; axis = 2; }
                        if (pushBack  < minPush) { minPush = pushBack;  axis = 3; }

                        if      (axis == 0) transform.position.x -= minPush;
                        else if (axis == 1) transform.position.x += minPush;
                        else if (axis == 2) transform.position.z -= minPush;
                        else                transform.position.z += minPush;
                    }
                }
            }

            // --- Jump ---
            if (engine::input::IsActionPressed("Jump") && input.grounded) {
                input.velocityY = JUMP_FORCE;
                input.grounded = false;
            }

            // --- Gravity ---
            input.velocityY -= GRAVITY * dt;
            transform.position.y += input.velocityY * dt;

            // --- Terrain floor collision (sample world height under player) ---
            float groundY = engine::math::WorldHeight(transform.position.x, transform.position.z);
            float eyeY    = groundY + EYE_HEIGHT;
            if (transform.position.y < eyeY) {
                transform.position.y = eyeY;
                input.velocityY = 0.0f;
                input.grounded = true;
            }

            // --- Sync camera to transform + angles ---
            cam.camera.position = transform.position;
            cam.camera.target = {
                transform.position.x + sinf(cam.yaw) * cosf(cam.pitch),
                transform.position.y + sinf(cam.pitch),
                transform.position.z + cosf(cam.yaw) * cosf(cam.pitch)
            };
            cam.camera.up = {0.0f, 1.0f, 0.0f};
        }
    }

    void CombatSystem(engine::ecs::Registry& reg) {
        float dt = GetFrameTime();
        swingTimer -= dt;
        if (swingTimer < 0.0f) swingTimer = 0.0f;

        // Find the player camera for the attack ray
        if (reg.playerInputs.data.empty()) return;
        engine::ecs::Entity playerE = {reg.playerInputs.indexToEntity[0]};
        if (!reg.cameras.Has(playerE)) return;
        auto& cam = reg.cameras.Get(playerE);

        // Left click = attack (only when cursor is locked and not mid-swing)
        if (engine::input::IsCursorLocked() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && swingTimer <= 0.0f) {
            swingTimer = SWING_DURATION;

            // Cast ray from camera center in look direction
            Vector3 dir = {
                sinf(cam.yaw) * cosf(cam.pitch),
                sinf(cam.pitch),
                cosf(cam.yaw) * cosf(cam.pitch)
            };
            Ray attackRay = {cam.camera.position, dir};

            // Find closest enemy hit (only entities with EnemyAIComponent)
            float closestDist = SWORD_RANGE;
            engine::ecs::Entity hitEntity = {UINT32_MAX};

            for (size_t i = 0; i < reg.enemyAIs.data.size(); i++) {
                engine::ecs::Entity e = {reg.enemyAIs.indexToEntity[i]};
                if (!reg.transforms.Has(e) || !reg.renderables.Has(e)) continue;

                auto& pos = reg.transforms.Get(e).position;
                auto& r   = reg.renderables.Get(e);

                BoundingBox box = {
                    {pos.x - r.width * 0.5f, pos.y - r.height * 0.5f, pos.z - r.depth * 0.5f},
                    {pos.x + r.width * 0.5f, pos.y + r.height * 0.5f, pos.z + r.depth * 0.5f}
                };

                RayCollision hit = GetRayCollisionBox(attackRay, box);
                if (hit.hit && hit.distance < closestDist) {
                    closestDist = hit.distance;
                    hitEntity = e;
                }
            }

            if (hitEntity.id != UINT32_MAX && reg.healths.Has(hitEntity)) {
                auto& hp = reg.healths.Get(hitEntity);
                hp.current -= SWORD_DAMAGE;
                if (reg.renderables.Has(hitEntity)) {
                    reg.renderables.Get(hitEntity).color = WHITE;
                }
                // Send damage to host if we're a client
                if (!engine::networking::IsHost() && reg.enemyAIs.Has(hitEntity)) {
                    engine::networking::SendDamageToHost(
                        reg.enemyAIs.Get(hitEntity).netId, SWORD_DAMAGE);
                }
            }
        }

        // --- M2: Fireball ---
        fireballTimer -= dt;
        if (fireballTimer < 0.0f) fireballTimer = 0.0f;

        if (engine::input::IsCursorLocked() && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && fireballTimer <= 0.0f) {
            fireballTimer = FIREBALL_COOLDOWN;

            Vector3 dir = {
                sinf(cam.yaw) * cosf(cam.pitch),
                sinf(cam.pitch),
                cosf(cam.yaw) * cosf(cam.pitch)
            };

            // Spawn fireball slightly in front of player
            Vector3 spawnPos = {
                cam.camera.position.x + dir.x * 1.0f,
                cam.camera.position.y + dir.y * 1.0f,
                cam.camera.position.z + dir.z * 1.0f
            };

            engine::ecs::Entity fb = engine::ecs::CreateEntity(reg);
            game::TransformComponent t;
            t.position = spawnPos;
            game::ProjectileComponent proj;
            proj.direction = dir;
            proj.speed = 25.0f;
            proj.damage = 50.0f;
            proj.aoeRadius = 4.0f;
            proj.lifetime = 3.0f;
            proj.radius = 0.25f;
            reg.transforms.Insert(fb, t);
            reg.projectiles.Insert(fb, proj);
        }

        // Destroy dead entities & restore colors (skip player from destruction)
        std::vector<engine::ecs::Entity> dead;
        for (size_t i = 0; i < reg.healths.data.size(); i++) {
            engine::ecs::Entity e = {reg.healths.indexToEntity[i]};
            if (reg.playerInputs.Has(e)) continue;  // Player never destroyed here
            if (reg.healths.data[i].current <= 0.0f) {
                dead.push_back(e);
            } else if (reg.renderables.Has(e)) {
                auto& r = reg.renderables.Get(e);
                // Restore hit flash (white from sword, orange from fireball) back to red
                bool isFlash = (r.color.r == WHITE.r && r.color.g == WHITE.g && r.color.b == WHITE.b)
                            || (r.color.r == ORANGE.r && r.color.g == ORANGE.g && r.color.b == ORANGE.b);
                if (isFlash) {
                    r.color = RED;
                }
            }
        }
        for (auto e : dead) {
            engine::ecs::DestroyEntity(reg, e);
        }
    }

    void ProjectileSystem(engine::ecs::Registry& reg) {
        float dt = GetFrameTime();

        std::vector<engine::ecs::Entity> toRemove;

        for (size_t i = 0; i < reg.projectiles.data.size(); i++) {
            engine::ecs::Entity e = {reg.projectiles.indexToEntity[i]};
            if (!reg.transforms.Has(e)) continue;

            auto& proj = reg.projectiles.data[i];
            auto& t = reg.transforms.Get(e);

            // Move projectile
            t.position.x += proj.direction.x * proj.speed * dt;
            t.position.y += proj.direction.y * proj.speed * dt;
            t.position.z += proj.direction.z * proj.speed * dt;

            proj.lifetime -= dt;
            if (proj.lifetime <= 0.0f) {
                toRemove.push_back(e);
                continue;
            }

            // Check collision against all enemies
            bool hit = false;
            for (size_t j = 0; j < reg.enemyAIs.data.size(); j++) {
                engine::ecs::Entity enemy = {reg.enemyAIs.indexToEntity[j]};
                if (!reg.transforms.Has(enemy)) continue;
                auto& ep = reg.transforms.Get(enemy).position;
                float dx = ep.x - t.position.x;
                float dy = ep.y - t.position.y;
                float dz = ep.z - t.position.z;
                float distSq = dx * dx + dy * dy + dz * dz;
                // Hit radius: projectile radius + half enemy width
                float hitDist = proj.radius + 0.5f;
                if (distSq < hitDist * hitDist) {
                    hit = true;
                    break;
                }
            }

            // Hit the ground (sample terrain height)
            if (t.position.y <= engine::math::WorldHeight(t.position.x, t.position.z)) hit = true;

            if (hit) {
                // AoE damage: hurt all enemies within aoeRadius
                for (size_t j = 0; j < reg.enemyAIs.data.size(); j++) {
                    engine::ecs::Entity enemy = {reg.enemyAIs.indexToEntity[j]};
                    if (!reg.transforms.Has(enemy) || !reg.healths.Has(enemy)) continue;
                    auto& ep = reg.transforms.Get(enemy).position;
                    float dx = ep.x - t.position.x;
                    float dz = ep.z - t.position.z;
                    float dist = sqrtf(dx * dx + dz * dz);
                    if (dist < proj.aoeRadius) {
                        float falloff = 1.0f - (dist / proj.aoeRadius);
                        float dmg = proj.damage * falloff;
                        reg.healths.Get(enemy).current -= dmg;
                        if (reg.renderables.Has(enemy)) {
                            reg.renderables.Get(enemy).color = ORANGE;
                        }
                        // Send damage to host if client
                        if (!engine::networking::IsHost()) {
                            engine::networking::SendDamageToHost(
                                reg.enemyAIs.data[j].netId, dmg);
                        }
                    }
                }
                toRemove.push_back(e);
            }
        }

        for (auto e : toRemove) {
            engine::ecs::DestroyEntity(reg, e);
        }
    }

    void EnemyAISystem(engine::ecs::Registry& reg) {
        float dt = GetFrameTime();
        if (reg.playerInputs.data.empty()) return;
        engine::ecs::Entity player = {reg.playerInputs.indexToEntity[0]};
        if (!reg.transforms.Has(player) || !reg.healths.Has(player)) return;
        Vector3 playerPos = reg.transforms.Get(player).position;
        auto& playerHP = reg.healths.Get(player);

        for (size_t i = 0; i < reg.enemyAIs.data.size(); i++) {
            engine::ecs::Entity e = {reg.enemyAIs.indexToEntity[i]};
            if (!reg.transforms.Has(e)) continue;
            auto& ai = reg.enemyAIs.data[i];
            auto& t = reg.transforms.Get(e);

            ai.attackTimer -= dt;
            if (ai.attackTimer < 0.0f) ai.attackTimer = 0.0f;

            // Horizontal direction to player
            Vector3 toPlayer = {playerPos.x - t.position.x, 0.0f, playerPos.z - t.position.z};
            float dist = sqrtf(toPlayer.x * toPlayer.x + toPlayer.z * toPlayer.z);
            if (dist < 0.001f) continue;

            if (dist > ai.attackRange) {
                // Move toward player
                toPlayer.x /= dist;
                toPlayer.z /= dist;
                t.position.x += toPlayer.x * ai.speed * dt;
                t.position.z += toPlayer.z * ai.speed * dt;

                // Simple separation from other enemies (avoid stacking)
                for (size_t j = 0; j < reg.enemyAIs.data.size(); j++) {
                    if (i == j) continue;
                    engine::ecs::Entity other = {reg.enemyAIs.indexToEntity[j]};
                    if (!reg.transforms.Has(other)) continue;
                    auto& op = reg.transforms.Get(other).position;
                    float dx = t.position.x - op.x;
                    float dz = t.position.z - op.z;
                    float d2 = dx * dx + dz * dz;
                    if (d2 > 0.01f && d2 < 1.5f * 1.5f) {
                        float d = sqrtf(d2);
                        float push = (1.5f - d) * 0.5f * dt * 5.0f;
                        t.position.x += (dx / d) * push;
                        t.position.z += (dz / d) * push;
                    }
                }
            } else if (ai.attackTimer <= 0.0f) {
                // Attack player
                playerHP.current -= ai.attackDamage;
                ai.attackTimer = ai.attackCooldown;
            }

            // Follow terrain (enemy body sits so its center is 1m above ground)
            t.position.y = engine::math::WorldHeight(t.position.x, t.position.z) + 1.0f;
        }
    }

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

            engine::ecs::Entity enemy = engine::ecs::CreateEntity(reg);
            game::TransformComponent t;
            t.position = pos;
            game::RenderComponent r;
            r.color = RED;
            r.width = 1.0f;
            r.height = 2.0f;
            r.depth = 1.0f;
            game::HealthComponent hp;
            hp.current = 100.0f;
            hp.max = 100.0f;
            game::EnemyAIComponent ai;
            ai.netId = nextNetId++;

            reg.transforms.Insert(enemy, t);
            reg.renderables.Insert(enemy, r);
            reg.healths.Insert(enemy, hp);
            reg.enemyAIs.Insert(enemy, ai);

            totalSpawned++;
            active++;
        }
    }

    void NetworkSyncSystem(engine::ecs::Registry& reg) {
        if (engine::networking::GetLobbyState() != engine::networking::LobbyState::InLobby) return;
        if (!engine::networking::HasRemotePeer()) return;

        if (engine::networking::IsHost()) {
            // HOST: broadcast enemy snapshot every 3 frames
            syncTickCounter++;
            if (syncTickCounter >= 3) {
                syncTickCounter = 0;
                std::vector<engine::networking::EnemyNetState> snapshot;
                for (size_t i = 0; i < reg.enemyAIs.data.size(); i++) {
                    engine::ecs::Entity e = {reg.enemyAIs.indexToEntity[i]};
                    if (!reg.transforms.Has(e) || !reg.healths.Has(e)) continue;
                    auto& t = reg.transforms.Get(e);
                    auto& hp = reg.healths.Get(e);
                    engine::networking::EnemyNetState s;
                    s.netId = reg.enemyAIs.data[i].netId;
                    s.x = t.position.x;
                    s.y = t.position.y;
                    s.z = t.position.z;
                    s.hpCurrent = hp.current;
                    s.hpMax = hp.max;
                    snapshot.push_back(s);
                }
                engine::networking::BroadcastEnemySnapshot(snapshot);
            }

            // HOST: apply damage from remote clients
            std::vector<engine::networking::DamageEvent> dmg;
            engine::networking::GetPendingDamage(dmg);
            for (auto& d : dmg) {
                for (size_t i = 0; i < reg.enemyAIs.data.size(); i++) {
                    if (reg.enemyAIs.data[i].netId == d.netId) {
                        engine::ecs::Entity e = {reg.enemyAIs.indexToEntity[i]};
                        if (reg.healths.Has(e)) {
                            reg.healths.Get(e).current -= d.damage;
                        }
                        break;
                    }
                }
            }
        } else {
            // CLIENT: receive enemy snapshot and sync local entities
            std::vector<engine::networking::EnemyNetState> snapshot;
            if (engine::networking::GetEnemySnapshot(snapshot)) {
                // Track which netIds are in this snapshot
                std::unordered_map<uint32_t, bool> alive;
                for (auto& s : snapshot) {
                    alive[s.netId] = true;
                    auto it = netIdToEntity.find(s.netId);
                    if (it != netIdToEntity.end()) {
                        // Update existing
                        engine::ecs::Entity e = it->second;
                        if (reg.transforms.Has(e)) {
                            auto& t = reg.transforms.Get(e);
                            t.position = {s.x, s.y, s.z};
                        }
                        if (reg.healths.Has(e)) {
                            auto& hp = reg.healths.Get(e);
                            hp.current = s.hpCurrent;
                            hp.max = s.hpMax;
                        }
                    } else {
                        // Create new enemy entity
                        engine::ecs::Entity enemy = engine::ecs::CreateEntity(reg);
                        game::TransformComponent t;
                        t.position = {s.x, s.y, s.z};
                        game::RenderComponent r;
                        r.color = RED;
                        r.width = 1.0f;
                        r.height = 2.0f;
                        r.depth = 1.0f;
                        game::HealthComponent hp;
                        hp.current = s.hpCurrent;
                        hp.max = s.hpMax;
                        game::EnemyAIComponent ai;
                        ai.netId = s.netId;
                        reg.transforms.Insert(enemy, t);
                        reg.renderables.Insert(enemy, r);
                        reg.healths.Insert(enemy, hp);
                        reg.enemyAIs.Insert(enemy, ai);
                        netIdToEntity[s.netId] = enemy;
                    }
                }
                // Destroy enemies no longer in snapshot
                std::vector<uint32_t> toRemove;
                for (auto& [netId, entity] : netIdToEntity) {
                    if (alive.find(netId) == alive.end()) {
                        engine::ecs::DestroyEntity(reg, entity);
                        toRemove.push_back(netId);
                    }
                }
                for (auto id : toRemove) {
                    netIdToEntity.erase(id);
                }
            }
        }
    }

    void SwordViewmodelSystem(engine::ecs::Registry& reg) {
        if (reg.cameras.data.empty()) return;
        auto& cam = reg.cameras.data[0].camera;

        Vector3 forward = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
        Vector3 right = Vector3Normalize(Vector3CrossProduct(forward, {0.0f, 1.0f, 0.0f}));
        Vector3 up = Vector3CrossProduct(right, forward);

        // Swing progress (0 = rest, 1 = end of swing)
        float t = 0.0f;
        if (swingTimer > 0.0f) t = 1.0f - (swingTimer / SWING_DURATION);
        // Curve: 0 -> 1 -> 0 across the swing
        float swing = sinf(t * PI);

        // Hilt position: right-forward of camera, sweeps left across screen when swinging
        float rightOffset   = 0.45f - swing * 0.7f;
        float upOffset      = -0.35f + swing * 0.1f;
        float forwardOffset = 0.55f;

        Vector3 hiltPos = Vector3Add(cam.position,
            Vector3Add(Vector3Scale(right, rightOffset),
            Vector3Add(Vector3Scale(up, upOffset),
                       Vector3Scale(forward, forwardOffset))));

        // Blade direction: up-forward at rest, tilts forward+down when swinging
        Vector3 bladeDir = Vector3Normalize(Vector3Add(
            Vector3Scale(up, 0.9f - swing * 1.3f),
            Vector3Scale(forward, 0.4f + swing * 0.9f)));

        float bladeLength = 0.95f;
        Vector3 tipPos = Vector3Add(hiltPos, Vector3Scale(bladeDir, bladeLength));

        // Blade (silver, tapered)
        DrawCylinderEx(hiltPos, tipPos, 0.035f, 0.015f, 8, LIGHTGRAY);

        // Handle / grip (dark brown, opposite of blade)
        Vector3 handleEnd = Vector3Subtract(hiltPos, Vector3Scale(bladeDir, 0.2f));
        DrawCylinderEx(hiltPos, handleEnd, 0.045f, 0.045f, 8, {80, 50, 20, 255});

        // Cross-guard (gold, perpendicular to blade)
        Vector3 guardAxis = Vector3Normalize(Vector3CrossProduct(bladeDir, forward));
        Vector3 guardStart = Vector3Subtract(hiltPos, Vector3Scale(guardAxis, 0.13f));
        Vector3 guardEnd   = Vector3Add(hiltPos, Vector3Scale(guardAxis, 0.13f));
        DrawCylinderEx(guardStart, guardEnd, 0.03f, 0.03f, 6, GOLD);

        // Pommel (gold ball at handle end)
        Vector3 pommelEnd = Vector3Subtract(handleEnd, Vector3Scale(bladeDir, 0.03f));
        DrawCylinderEx(handleEnd, pommelEnd, 0.055f, 0.055f, 8, GOLD);
    }

    void Render3DSystem(engine::ecs::Registry& reg) {
        // Streaming terrain (replaces the old flat plane).
        engine::terrain::chunks::Draw();

        // Draw all renderable entities as cubes
        for (size_t i = 0; i < reg.renderables.data.size(); i++) {
            engine::ecs::Entity e = {reg.renderables.indexToEntity[i]};
            if (!reg.transforms.Has(e)) continue;

            auto& pos = reg.transforms.Get(e).position;
            auto& r   = reg.renderables.data[i];

            DrawCube(pos, r.width, r.height, r.depth, r.color);
            DrawCubeWires(pos, r.width, r.height, r.depth, BLACK);
        }

        // Draw fireballs as orange spheres with a glow ring
        for (size_t i = 0; i < reg.projectiles.data.size(); i++) {
            engine::ecs::Entity e = {reg.projectiles.indexToEntity[i]};
            if (!reg.transforms.Has(e)) continue;
            auto& pos = reg.transforms.Get(e).position;
            auto& proj = reg.projectiles.data[i];
            DrawSphere(pos, proj.radius, ORANGE);
            DrawSphere(pos, proj.radius * 1.5f, {255, 160, 0, 80});  // outer glow
            DrawSphere(pos, proj.radius * 0.5f, YELLOW);              // hot core
        }
    }

    void HealthBarSystem(engine::ecs::Registry& reg) {
        // Find the player camera for projection
        if (reg.cameras.data.empty()) return;
        auto& cam = reg.cameras.data[0].camera;

        for (size_t i = 0; i < reg.healths.data.size(); i++) {
            engine::ecs::Entity e = {reg.healths.indexToEntity[i]};

            // Skip player (drawn on HUD separately)
            if (reg.playerInputs.Has(e)) continue;
            if (!reg.transforms.Has(e) || !reg.renderables.Has(e)) continue;

            auto& hp  = reg.healths.data[i];
            auto& pos = reg.transforms.Get(e).position;
            auto& r   = reg.renderables.Get(e);

            // Don't show bar if full HP
            if (hp.current >= hp.max) continue;

            float ratio = hp.current / hp.max;
            if (ratio < 0.0f) ratio = 0.0f;

            // Project world position above entity to screen
            Vector3 barWorldPos = {pos.x, pos.y + r.height * 0.5f + 0.4f, pos.z};
            Vector2 screenPos = GetWorldToScreen(barWorldPos, cam);

            // Skip if behind camera
            if (screenPos.x < -100 || screenPos.x > GetScreenWidth() + 100 ||
                screenPos.y < -100 || screenPos.y > GetScreenHeight() + 100) continue;

            int barW = 60;
            int barH = 8;
            int barX = (int)screenPos.x - barW / 2;
            int barY = (int)screenPos.y - barH / 2;

            Color barColor = (ratio > 0.5f) ? GREEN : (ratio > 0.25f) ? YELLOW : RED;

            DrawRectangle(barX, barY, barW, barH, DARKGRAY);
            DrawRectangle(barX, barY, (int)(barW * ratio), barH, barColor);
            DrawRectangleLines(barX, barY, barW, barH, BLACK);
        }
    }

}
