#include "game/systems.hpp"
#include "game/factories/entity_factory.hpp"
#include "game/spells.hpp"
#include "game/sfx.hpp"
#include "engine/input.hpp"
#include "engine/math/noise.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <cmath>
#include <cstdio>
#include <string>

namespace game::systems {

    // ---------------------------------------------------------------------------
    // Melee attack feel — FPS dagger viewmodel (procedural pose, not skeletal).
    // Phases: Anticipate (ease-in) → Strike (sharp, hitbox live) → optional HitStop
    //         → Recover (cancel window) → Idle / combo hold.
    // ---------------------------------------------------------------------------
    enum class MeleePhase : uint8_t {
        Idle = 0,
        Anticipate,
        Strike,
        HitStop,
        Recover
    };

    struct MeleeState {
        MeleePhase phase = MeleePhase::Idle;
        int   dir            = 0;   // 0 = left slash, 1 = right slash
        float phaseT         = 0.0f;
        float phaseDur       = 0.0f;
        int   queued         = -1;  // -1 = none
        float comboWindow    = 0.0f;
        bool  hitboxActive   = false;
        bool  hitLanded      = false;
        bool  strikeSfxFired = false;
        bool  impactHookFired = false;
        float frozenSlash    = 0.0f;
        float frozenRaise    = 0.0f;
        float punchTrauma    = 0.0f;
        float punchYawSign   = 1.0f;
    };

    static MeleeState g_melee;

    static constexpr float MELEE_ANTICIPATE_L = 0.11f;
    static constexpr float MELEE_STRIKE_L     = 0.085f;
    static constexpr float MELEE_RECOVER_L    = 0.17f;
    static constexpr float MELEE_ANTICIPATE_R = 0.08f;
    static constexpr float MELEE_STRIKE_R     = 0.095f;
    static constexpr float MELEE_RECOVER_R    = 0.20f;
    static constexpr float MELEE_HITSTOP      = 0.055f;
    static constexpr float MELEE_CANCEL_AFTER = 0.05f;
    static constexpr float MELEE_COMBO_WINDOW = 0.38f;
    static constexpr float MELEE_DAMAGE       = 35.0f;
    static constexpr float MELEE_RANGE        = 3.5f;
    static constexpr float MELEE_FOV_DOT      = 0.45f;

    static Model g_weaponModel = {};
    static Texture2D g_weaponTexture = {};
    static bool  g_weaponLoaded = false;
    static bool  g_weaponTriedLoad = false;

    static float EaseInCubic(float t) {
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        return t * t * t;
    }
    static float EaseOutCubic(float t) {
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        float u = 1.0f - t;
        return 1.0f - u * u * u;
    }
    static float EaseOutQuad(float t) {
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        float u = 1.0f - t;
        return 1.0f - u * u;
    }

    static void HookMeleeWindup(int dir) { (void)dir; }
    static void HookMeleeStrikeFrame(int dir) { game::sfx::PlayMeleeSwing(dir); }
    static void HookMeleeImpact(Vector3 pos, bool didHit) {
        if (didHit) game::sfx::PlayMeleeImpact(pos);
    }
    static void HookMeleeCameraPunch(float intensity, float yawSign) {
        g_melee.punchTrauma = fmaxf(g_melee.punchTrauma, intensity);
        g_melee.punchYawSign = yawSign;
    }
    static void HookMeleeRecover(int dir) { (void)dir; }

    static bool FileReadable(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::fclose(f);
        return true;
    }

    static std::string ResolveWeaponGlbPath() {
        std::string appDir = GetApplicationDirectory();
        if (!appDir.empty() && appDir.back() != '/' && appDir.back() != '\\')
            appDir.push_back('/');
        const char* names[] = {
            "assets/models/necro-dagger/model_lowpoly.glb",
            "assets/models/necro-dagger/necro-dagger_lowpoly.glb",
        };
        for (const char* rel : names) {
            std::string p = appDir + rel;
            if (FileReadable(p)) return p;
            if (FileReadable(rel)) return rel;
        }
        return appDir + names[0];
    }

    static std::string WeaponDirFromGlb(const std::string& glbPath) {
        auto slash = glbPath.find_last_of("/\\");
        if (slash == std::string::npos) return {};
        return glbPath.substr(0, slash + 1);
    }

    void WeaponViewmodelInit() {}

    static void WeaponViewmodelEnsureLoaded() {
        if (g_weaponTriedLoad) return;
        g_weaponTriedLoad = true;

        std::string path = ResolveWeaponGlbPath();
        if (!FileReadable(path)) {
            TraceLog(LOG_WARNING, "WEAPON: missing low-poly GLB at %s — procedural fallback", path.c_str());
            return;
        }

        TraceLog(LOG_INFO, "WEAPON: loading %s", path.c_str());
        Model model = LoadModel(path.c_str());
        if (model.meshCount <= 0 || model.meshes == nullptr ||
            model.meshes[0].vertexCount <= 0) {
            TraceLog(LOG_WARNING, "WEAPON: LoadModel failed");
            if (model.meshCount > 0) UnloadModel(model);
            return;
        }

        std::string texPath = WeaponDirFromGlb(path) + "diffuse_0.png";
        if (FileReadable(texPath)) {
            g_weaponTexture = LoadTexture(texPath.c_str());
            if (g_weaponTexture.id > 0) {
                GenTextureMipmaps(&g_weaponTexture);
                SetTextureFilter(g_weaponTexture, TEXTURE_FILTER_BILINEAR);
                for (int i = 0; i < model.materialCount; i++) {
                    model.materials[i].maps[MATERIAL_MAP_ALBEDO].texture = g_weaponTexture;
                    model.materials[i].maps[MATERIAL_MAP_ALBEDO].color = WHITE;
                }
            }
        }

        g_weaponModel = model;
        g_weaponLoaded = true;
        TraceLog(LOG_INFO, "WEAPON: ready (%d meshes, %d verts, %d tris)",
                 model.meshCount, model.meshes[0].vertexCount, model.meshes[0].triangleCount);
    }

    void WeaponViewmodelShutdown() {
        if (g_weaponLoaded) {
            for (int i = 0; i < g_weaponModel.materialCount; i++) {
                g_weaponModel.materials[i].maps[MATERIAL_MAP_ALBEDO].texture = {};
            }
            UnloadModel(g_weaponModel);
            if (g_weaponTexture.id > 0) UnloadTexture(g_weaponTexture);
            g_weaponModel = {};
            g_weaponTexture = {};
            g_weaponLoaded = false;
        }
        g_weaponTriedLoad = false;
    }

    static Matrix MatrixFromAxes(Vector3 xAxis, Vector3 yAxis, Vector3 zAxis, Vector3 translation) {
        Matrix m = MatrixIdentity();
        m.m0 = xAxis.x; m.m1 = xAxis.y; m.m2 = xAxis.z;
        m.m4 = yAxis.x; m.m5 = yAxis.y; m.m6 = yAxis.z;
        m.m8 = zAxis.x; m.m9 = zAxis.y; m.m10 = zAxis.z;
        m.m12 = translation.x; m.m13 = translation.y; m.m14 = translation.z;
        return m;
    }

    bool IsSwinging() {
        return g_melee.phase != MeleePhase::Idle || g_melee.comboWindow > 0.0f;
    }

    static float PhaseDuration(MeleePhase phase, int dir) {
        const bool left = (dir == 0);
        switch (phase) {
            case MeleePhase::Anticipate: return left ? MELEE_ANTICIPATE_L : MELEE_ANTICIPATE_R;
            case MeleePhase::Strike:     return left ? MELEE_STRIKE_L     : MELEE_STRIKE_R;
            case MeleePhase::HitStop:    return MELEE_HITSTOP;
            case MeleePhase::Recover:    return left ? MELEE_RECOVER_L    : MELEE_RECOVER_R;
            default: return 0.0f;
        }
    }

    static void EnterPhase(MeleePhase phase) {
        g_melee.phase = phase;
        g_melee.phaseT = 0.0f;
        g_melee.phaseDur = PhaseDuration(phase, g_melee.dir);

        if (phase == MeleePhase::Anticipate) {
            g_melee.hitboxActive = false;
            g_melee.hitLanded = false;
            g_melee.strikeSfxFired = false;
            g_melee.impactHookFired = false;
            HookMeleeWindup(g_melee.dir);
        } else if (phase == MeleePhase::Strike) {
            g_melee.hitboxActive = true;
            if (!g_melee.strikeSfxFired) {
                g_melee.strikeSfxFired = true;
                HookMeleeStrikeFrame(g_melee.dir);
            }
        } else if (phase == MeleePhase::Recover) {
            g_melee.hitboxActive = false;
            HookMeleeRecover(g_melee.dir);
        } else {
            g_melee.hitboxActive = false;
        }
    }

    static void StartSwing(int dir) {
        g_melee.dir = dir;
        g_melee.queued = -1;
        g_melee.comboWindow = 0.0f;
        EnterPhase(MeleePhase::Anticipate);
    }

    static bool InCancelWindow() {
        return g_melee.phase == MeleePhase::Recover &&
               g_melee.phaseT >= MELEE_CANCEL_AFTER;
    }

    static void EvalMeleePose(float& outSlash, float& outRaise) {
        outSlash = 0.0f;
        outRaise = 0.0f;

        if (g_melee.phase == MeleePhase::Idle) {
            if (g_melee.comboWindow > 0.0f && g_melee.dir == 0) outSlash = 1.0f;
            return;
        }
        if (g_melee.phase == MeleePhase::HitStop) {
            outSlash = g_melee.frozenSlash;
            outRaise = g_melee.frozenRaise;
            return;
        }

        float u = (g_melee.phaseDur > 1e-5f) ? (g_melee.phaseT / g_melee.phaseDur) : 1.0f;
        if (u < 0.0f) u = 0.0f;
        if (u > 1.0f) u = 1.0f;

        if (g_melee.phase == MeleePhase::Anticipate) {
            float e = EaseInCubic(u);
            outRaise = e;
            outSlash = e * 0.10f;
        } else if (g_melee.phase == MeleePhase::Strike) {
            float e = EaseOutCubic(u);
            outRaise = 1.0f - e;
            outSlash = 0.10f + e * 0.90f;
        } else if (g_melee.phase == MeleePhase::Recover) {
            if (g_melee.dir == 0) {
                outSlash = 1.0f;
                outRaise = 0.0f;
            } else {
                outSlash = 1.0f - EaseOutQuad(u);
                outRaise = 0.0f;
            }
        }
    }

    static bool TryMeleeHitbox(engine::ecs::Registry& reg,
                               const game::TransformComponent& pTrans,
                               const game::CameraComponent& pCam) {
        if (!g_melee.hitboxActive || g_melee.hitLanded) return false;

        Vector3 fwd;
        fwd.x = cosf(pCam.pitch) * sinf(pCam.yaw);
        fwd.y = sinf(pCam.pitch);
        fwd.z = cosf(pCam.pitch) * cosf(pCam.yaw);

        bool any = false;
        Vector3 hitPos = {};
        for (size_t i = 0; i < reg.enemyAIs.data.size(); i++) {
            engine::ecs::Entity enemy = {reg.enemyAIs.indexToEntity[i]};
            if (!reg.transforms.Has(enemy) || !reg.healths.Has(enemy)) continue;

            auto& eTrans = reg.transforms.Get(enemy);
            auto& eHP    = reg.healths.Get(enemy);

            Vector3 toEnemy = Vector3Subtract(eTrans.position, pTrans.position);
            float dist = Vector3Length(toEnemy);
            if (dist > MELEE_RANGE || dist < 0.1f) continue;

            Vector3 dirToEnemy = Vector3Scale(toEnemy, 1.0f / dist);
            if (Vector3DotProduct(fwd, dirToEnemy) < MELEE_FOV_DOT) continue;

            float dmg = MELEE_DAMAGE * (g_melee.dir == 1 ? 1.15f : 1.0f);
            eHP.current -= dmg;
            if (engine::networking::GetLobbyState() == engine::networking::LobbyState::InLobby &&
                !engine::networking::IsHost()) {
                engine::networking::SendDamageToHost(reg.enemyAIs.data[i].netId, dmg);
            }
            hitPos = eTrans.position;
            any = true;
        }

        if (any) {
            g_melee.hitLanded = true;
            g_melee.hitboxActive = false;
            if (!g_melee.impactHookFired) {
                g_melee.impactHookFired = true;
                HookMeleeImpact(hitPos, true);
                HookMeleeCameraPunch(0.65f, g_melee.dir == 0 ? -1.0f : 1.0f);
            }
        }
        return any;
    }

    static void ApplyCameraPunch(game::CameraComponent& cam, float dt) {
        if (g_melee.punchTrauma <= 0.0f) return;

        float t = g_melee.punchTrauma;
        float shake = t * t;
        float time = (float)GetTime();
        float yawKick   = g_melee.punchYawSign * shake * 0.045f;
        float pitchKick = -shake * 0.028f;
        float ox = sinf(time * 67.0f) * shake * 0.012f;
        float oy = cosf(time * 53.0f) * shake * 0.010f;

        Vector3 forward;
        forward.x = cosf(cam.pitch + pitchKick) * sinf(cam.yaw + yawKick);
        forward.y = sinf(cam.pitch + pitchKick);
        forward.z = cosf(cam.pitch + pitchKick) * cosf(cam.yaw + yawKick);

        cam.camera.position.x += ox;
        cam.camera.position.y += oy;
        cam.camera.target = Vector3Add(cam.camera.position, forward);

        g_melee.punchTrauma = fmaxf(0.0f, g_melee.punchTrauma - dt * 5.5f);
    }

    void CombatSystem(engine::ecs::Registry& reg) {
        float dt = GetFrameTime();
        if (dt > 0.1f) dt = 0.1f;

        if (reg.playerInputs.data.empty()) return;
        engine::ecs::Entity player = {reg.playerInputs.indexToEntity[0]};
        if (!reg.transforms.Has(player) || !reg.cameras.Has(player)) return;

        auto& pTrans = reg.transforms.Get(player);
        auto& pCam   = reg.cameras.Get(player);

        if (g_melee.phase != MeleePhase::Idle) {
            g_melee.phaseT += dt;

            if (g_melee.phase == MeleePhase::Strike) {
                if (TryMeleeHitbox(reg, pTrans, pCam)) {
                    EvalMeleePose(g_melee.frozenSlash, g_melee.frozenRaise);
                    EnterPhase(MeleePhase::HitStop);
                }
            }

            if (g_melee.phaseT >= g_melee.phaseDur) {
                switch (g_melee.phase) {
                    case MeleePhase::Anticipate:
                        EnterPhase(MeleePhase::Strike);
                        break;
                    case MeleePhase::Strike:
                        if (!g_melee.impactHookFired) {
                            g_melee.impactHookFired = true;
                            HookMeleeImpact(pTrans.position, false);
                        }
                        EnterPhase(MeleePhase::Recover);
                        break;
                    case MeleePhase::HitStop:
                        EnterPhase(MeleePhase::Recover);
                        break;
                    case MeleePhase::Recover:
                        if (g_melee.queued >= 0) {
                            int next = g_melee.queued;
                            g_melee.queued = -1;
                            StartSwing(next);
                        } else if (g_melee.dir == 0) {
                            g_melee.comboWindow = MELEE_COMBO_WINDOW;
                            EnterPhase(MeleePhase::Idle);
                        } else {
                            g_melee.dir = 0;
                            g_melee.comboWindow = 0.0f;
                            EnterPhase(MeleePhase::Idle);
                        }
                        break;
                    default:
                        EnterPhase(MeleePhase::Idle);
                        break;
                }
            }
        }

        if (g_melee.phase == MeleePhase::Idle && g_melee.comboWindow > 0.0f) {
            g_melee.comboWindow -= dt;
            if (g_melee.comboWindow <= 0.0f) {
                g_melee.comboWindow = 0.0f;
                g_melee.dir = 0;
            }
        }

        if (engine::input::IsCursorLocked() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (g_melee.phase == MeleePhase::Idle) {
                if (g_melee.comboWindow > 0.0f && g_melee.dir == 0) StartSwing(1);
                else StartSwing(0);
            } else if (InCancelWindow() && g_melee.queued < 0) {
                g_melee.queued = (g_melee.dir == 0) ? 1 : 0;
            } else if (g_melee.phase == MeleePhase::Strike && g_melee.dir == 0 &&
                       g_melee.phaseT > g_melee.phaseDur * 0.4f && g_melee.queued < 0) {
                g_melee.queued = 1;
            }
        }

        if (engine::input::IsCursorLocked() && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            if (reg.spellCasters.Has(player)) {
                auto& caster = reg.spellCasters.Get(player);
                SpellId list[16];
                int n = game::GetSpellsForElement((game::SpellElement)caster.selectedElement, list, 16);
                if (n > 0) {
                    int slot = caster.selectedSlot;
                    if (slot < 0) slot = 0;
                    if (slot >= n) slot = n - 1;
                    TryCastSpell(reg, (int)list[slot]);
                }
            }
        }

        ApplyCameraPunch(pCam, dt);
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

            bool hitTerrain = false;
            float groundY = engine::math::WorldHeight(t.position.x, t.position.z);
            if (t.position.y <= groundY + p.radius) {
                hitTerrain = true;
            }

            auto alreadyPierced = [&](uint32_t id) -> bool {
                for (uint8_t k = 0; k < p.pierceCount; k++) {
                    if (p.piercedIds[k] == id) return true;
                }
                return false;
            };

            if (p.piercing) {
                // Damage each new enemy once; survive until terrain or timeout.
                for (size_t j = 0; j < reg.enemyAIs.data.size(); j++) {
                    engine::ecs::Entity enemy = {reg.enemyAIs.indexToEntity[j]};
                    if (!reg.transforms.Has(enemy) || !reg.healths.Has(enemy)) continue;
                    if (alreadyPierced(enemy.id)) continue;
                    auto& eTrans = reg.transforms.Get(enemy);
                    if (Vector3Distance(t.position, eTrans.position) < (p.radius + 0.9f)) {
                        float dmg = p.damage;
                        reg.healths.Get(enemy).current -= dmg;
                        if (engine::networking::GetLobbyState() == engine::networking::LobbyState::InLobby &&
                            !engine::networking::IsHost()) {
                            engine::networking::SendDamageToHost(reg.enemyAIs.data[j].netId, dmg);
                        }
                        if (p.pierceCount < 8) {
                            p.piercedIds[p.pierceCount++] = enemy.id;
                        }
                        NotifyProjectileImpact(reg, p.spellId, eTrans.position);
                    }
                }
                if (hitTerrain || p.lifetime <= 0.0f) {
                    if (hitTerrain) NotifyProjectileImpact(reg, p.spellId, t.position);
                    engine::ecs::DestroyEntity(reg, proj);
                }
                continue;
            }

            bool hit = hitTerrain;
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
                    NotifyProjectileImpact(reg, p.spellId, t.position);
                }
                engine::ecs::DestroyEntity(reg, proj);
            }
        }
    }

    void SwordViewmodelSystem(engine::ecs::Registry& reg) {
        WeaponViewmodelEnsureLoaded();

        if (reg.playerInputs.data.empty()) return;
        engine::ecs::Entity player = {reg.playerInputs.indexToEntity[0]};
        if (!reg.cameras.Has(player)) return;

        auto& cam = reg.cameras.Get(player).camera;

        Vector3 forward = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
        Vector3 right   = Vector3Normalize(Vector3CrossProduct(forward, cam.up));
        Vector3 up      = Vector3CrossProduct(right, forward);

        float slash = 0.0f;
        float raise = 0.0f;
        EvalMeleePose(slash, raise);

        Vector3 restDir = Vector3Normalize(Vector3Add(
            Vector3Add(Vector3Scale(up, 0.78f), Vector3Scale(forward, 0.22f)),
            Vector3Scale(right, 0.40f)));

        float rightOffset;
        float downOffset;
        float forwardOffset;
        float slashRoll;
        float slashPitch;

        if (g_melee.dir == 1 && slash > 0.0f) {
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

        if (g_weaponLoaded) {
            // Mesh is Y-up after glTF node bake: blade along Y (~1), width X (~0.26),
            // thickness Z (~0.15). Uniform scale reads as horizontally stretched in
            // first-person, so keep length and slim the lateral axes.
            const float scaleLen   = 0.70f; // along blade (model Y)
            const float scaleWidth = 0.48f; // left/right (model X)
            const float scaleThick = 0.55f; // edge-on depth (model Z)
            const float gripAlongBlade = 0.20f;

            Vector3 yAxis = bladeDir;
            Vector3 xAxis = side;
            Vector3 zAxis = Vector3Normalize(Vector3CrossProduct(xAxis, yAxis));
            xAxis = Vector3Normalize(Vector3CrossProduct(yAxis, zAxis));

            Vector3 modelCenter = Vector3Add(swordPos, Vector3Scale(bladeDir, gripAlongBlade));

            Matrix transform = MatrixFromAxes(
                Vector3Scale(xAxis, scaleWidth),
                Vector3Scale(yAxis, scaleLen),
                Vector3Scale(zAxis, scaleThick),
                modelCenter);

            // Sky/terrain shaders can leave GL state that culls this mesh.
            rlDisableBackfaceCulling();
            for (int i = 0; i < g_weaponModel.meshCount; i++) {
                int mi = g_weaponModel.meshMaterial ? g_weaponModel.meshMaterial[i] : 0;
                if (mi < 0 || mi >= g_weaponModel.materialCount) mi = 0;
                DrawMesh(g_weaponModel.meshes[i], g_weaponModel.materials[mi], transform);
            }
            rlEnableBackfaceCulling();
            return;
        }

        // Procedural fallback sword
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
