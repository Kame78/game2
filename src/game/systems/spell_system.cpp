#include "game/systems.hpp"
#include "game/factories/entity_factory.hpp"
#include "game/spells.hpp"
#include "game/sfx.hpp"
#include "engine/input.hpp"
#include "engine/math/noise.hpp"
#include "engine/networking.hpp"
#include "raymath.h"
#include <cmath>
#include <cstdint>

namespace game::systems {

namespace {

// ---------------------------------------------------------------------------
// Lightweight particle pool
// ---------------------------------------------------------------------------
enum class ParticleKind : uint8_t { Ember, Smoke, Spark, Droplet, Mist };

struct Particle {
    Vector3 pos;
    Vector3 vel;
    Color color;
    float life;
    float maxLife;
    float size;
    ParticleKind kind;
};

static constexpr int MAX_PARTICLES = 448;
static Particle g_particles[MAX_PARTICLES];
static int g_particleCount = 0;

static float frand01() {
    return (float)GetRandomValue(0, 10000) / 10000.0f;
}

static void SpawnParticle(Vector3 pos, Vector3 vel, Color color, float life, float size, ParticleKind kind) {
    if (g_particleCount >= MAX_PARTICLES) {
        int slot = GetRandomValue(0, MAX_PARTICLES - 1);
        g_particles[slot] = {pos, vel, color, life, life, size, kind};
        return;
    }
    g_particles[g_particleCount++] = {pos, vel, color, life, life, size, kind};
}

static Color LerpColor(Color a, Color b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return Color{
        (unsigned char)(a.r + (b.r - a.r) * t),
        (unsigned char)(a.g + (b.g - a.g) * t),
        (unsigned char)(a.b + (b.b - a.b) * t),
        (unsigned char)(a.a + (b.a - a.a) * t)
    };
}

static void SpawnEmberBurst(Vector3 center, int count, float speed, float sizeScale) {
    for (int i = 0; i < count; i++) {
        float yaw = frand01() * 6.28318f;
        float pitch = frand01() * 1.4f;
        Vector3 vel = {
            cosf(pitch) * sinf(yaw) * speed * (0.4f + frand01()),
            sinf(pitch) * speed * (0.5f + frand01()),
            cosf(pitch) * cosf(yaw) * speed * (0.4f + frand01())
        };
        Color c = LerpColor(Color{255, 220, 60, 255}, Color{255, 80, 20, 255}, frand01());
        SpawnParticle(center, vel, c, 0.35f + frand01() * 0.55f, (0.06f + frand01() * 0.12f) * sizeScale,
                      ParticleKind::Ember);
    }
}

static void SpawnSmokePuff(Vector3 center, int count, float rise) {
    for (int i = 0; i < count; i++) {
        Vector3 vel = {
            (frand01() - 0.5f) * 1.2f,
            rise * (0.5f + frand01()),
            (frand01() - 0.5f) * 1.2f
        };
        Color c = Color{40, 40, 40, (unsigned char)(90 + GetRandomValue(0, 80))};
        Vector3 p = {
            center.x + (frand01() - 0.5f) * 0.8f,
            center.y + frand01() * 0.4f,
            center.z + (frand01() - 0.5f) * 0.8f
        };
        SpawnParticle(p, vel, c, 0.8f + frand01() * 1.0f, 0.25f + frand01() * 0.45f, ParticleKind::Smoke);
    }
}

static void SpawnSparks(Vector3 center, int count, float speed) {
    for (int i = 0; i < count; i++) {
        float yaw = frand01() * 6.28318f;
        Vector3 vel = {
            sinf(yaw) * speed * (0.6f + frand01()),
            2.0f + frand01() * 6.0f,
            cosf(yaw) * speed * (0.6f + frand01())
        };
        SpawnParticle(center, vel, Color{255, 240, 160, 255}, 0.2f + frand01() * 0.35f,
                      0.04f + frand01() * 0.05f, ParticleKind::Spark);
    }
}

static void SpawnWaterBurst(Vector3 center, int count, float speed, float sizeScale) {
    for (int i = 0; i < count; i++) {
        float yaw = frand01() * 6.28318f;
        float pitch = frand01() * 1.6f;
        Vector3 vel = {
            cosf(pitch) * sinf(yaw) * speed * (0.3f + frand01()),
            sinf(pitch) * speed * (0.6f + frand01()),
            cosf(pitch) * cosf(yaw) * speed * (0.3f + frand01())
        };
        Color c = LerpColor(Color{160, 220, 255, 230}, Color{40, 120, 220, 200}, frand01());
        SpawnParticle(center, vel, c, 0.3f + frand01() * 0.5f, (0.05f + frand01() * 0.1f) * sizeScale,
                      ParticleKind::Droplet);
    }
}

static void SpawnMistPuff(Vector3 center, int count, float rise) {
    for (int i = 0; i < count; i++) {
        Vector3 vel = {
            (frand01() - 0.5f) * 1.5f,
            rise * (0.4f + frand01()),
            (frand01() - 0.5f) * 1.5f
        };
        Color c = Color{180, 210, 230, (unsigned char)(70 + GetRandomValue(0, 60))};
        Vector3 p = {
            center.x + (frand01() - 0.5f) * 1.0f,
            center.y + frand01() * 0.5f,
            center.z + (frand01() - 0.5f) * 1.0f
        };
        SpawnParticle(p, vel, c, 0.7f + frand01() * 0.9f, 0.3f + frand01() * 0.5f, ParticleKind::Mist);
    }
}

static void SpawnHellRiftBurst(Vector3 ground, float radius) {
    for (int i = 0; i < 28; i++) {
        float a = frand01() * 6.28318f;
        float r = frand01() * radius;
        Vector3 p = {ground.x + cosf(a) * r, ground.y + 0.1f, ground.z + sinf(a) * r};
        Vector3 vel = {(frand01() - 0.5f) * 1.5f, 3.0f + frand01() * 7.0f, (frand01() - 0.5f) * 1.5f};
        Color c = LerpColor(Color{255, 60, 10, 230}, Color{80, 10, 20, 180}, frand01());
        SpawnParticle(p, vel, c, 0.7f + frand01() * 0.9f, 0.18f + frand01() * 0.35f, ParticleKind::Smoke);
    }
    SpawnEmberBurst({ground.x, ground.y + 0.4f, ground.z}, 22, 7.0f, 1.8f);
    SpawnSparks({ground.x, ground.y + 0.3f, ground.z}, 16, 6.0f);
}

static void SpawnHolyBeamBurst(Vector3 ground, float height) {
    for (int i = 0; i < 24; i++) {
        float a = frand01() * 6.28318f;
        float r = frand01() * 1.8f;
        float y = ground.y + frand01() * height;
        Vector3 p = {ground.x + cosf(a) * r, y, ground.z + sinf(a) * r};
        Vector3 vel = {(frand01() - 0.5f) * 0.8f, -2.0f - frand01() * 4.0f, (frand01() - 0.5f) * 0.8f};
        Color c = LerpColor(Color{255, 255, 220, 230}, Color{255, 220, 120, 180}, frand01());
        SpawnParticle(p, vel, c, 0.6f + frand01() * 0.8f, 0.12f + frand01() * 0.22f, ParticleKind::Mist);
    }
    SpawnSparks({ground.x, ground.y + height * 0.5f, ground.z}, 18, 5.0f);
}

static float Smoothstep01(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static void UpdateParticles(float dt) {
    for (int i = g_particleCount - 1; i >= 0; i--) {
        Particle& p = g_particles[i];
        p.life -= dt;
        if (p.life <= 0.0f) {
            g_particles[i] = g_particles[--g_particleCount];
            continue;
        }
        p.pos = Vector3Add(p.pos, Vector3Scale(p.vel, dt));
        if (p.kind == ParticleKind::Ember) {
            p.vel.y -= 6.0f * dt;
            p.vel = Vector3Scale(p.vel, 1.0f - 1.5f * dt);
        } else if (p.kind == ParticleKind::Smoke || p.kind == ParticleKind::Mist) {
            p.vel.y += 0.6f * dt;
            p.vel.x *= (1.0f - 0.6f * dt);
            p.vel.z *= (1.0f - 0.6f * dt);
            p.size += dt * 0.3f;
        } else if (p.kind == ParticleKind::Spark) {
            p.vel.y -= 14.0f * dt;
        } else if (p.kind == ParticleKind::Droplet) {
            p.vel.y -= 18.0f * dt;
            p.vel.x *= (1.0f - 0.8f * dt);
            p.vel.z *= (1.0f - 0.8f * dt);
        }
    }
}

static void DrawParticles() {
    for (int i = 0; i < g_particleCount; i++) {
        const Particle& p = g_particles[i];
        float t = 1.0f - (p.life / p.maxLife);
        Color c = p.color;
        if (p.kind == ParticleKind::Ember) {
            c = LerpColor(p.color, Color{80, 20, 10, 0}, t);
            DrawSphere(p.pos, p.size * (1.0f - t * 0.5f), c);
        } else if (p.kind == ParticleKind::Smoke || p.kind == ParticleKind::Mist) {
            c.a = (unsigned char)((1.0f - t) * p.color.a);
            DrawSphere(p.pos, p.size, c);
        } else if (p.kind == ParticleKind::Spark || p.kind == ParticleKind::Droplet) {
            c.a = (unsigned char)((1.0f - t) * p.color.a);
            DrawSphere(p.pos, p.size, c);
        }
    }
}

// ---------------------------------------------------------------------------
// Damage / force helpers
// ---------------------------------------------------------------------------
static void ApplyDamageToEnemy(engine::ecs::Registry& reg, size_t enemyDenseIdx, float dmg) {
    engine::ecs::Entity enemy = {reg.enemyAIs.indexToEntity[enemyDenseIdx]};
    if (!reg.healths.Has(enemy)) return;
    reg.healths.Get(enemy).current -= dmg;
    if (engine::networking::GetLobbyState() == engine::networking::LobbyState::InLobby &&
        !engine::networking::IsHost()) {
        engine::networking::SendDamageToHost(reg.enemyAIs.data[enemyDenseIdx].netId, dmg);
    }
}

static void ApplyAoEDamage(engine::ecs::Registry& reg, Vector3 center, float radius, float damage,
                           bool falloff) {
    for (size_t j = 0; j < reg.enemyAIs.data.size(); j++) {
        engine::ecs::Entity enemy = {reg.enemyAIs.indexToEntity[j]};
        if (!reg.transforms.Has(enemy) || !reg.healths.Has(enemy)) continue;
        float dist = Vector3Distance(center, reg.transforms.Get(enemy).position);
        if (dist > radius) continue;
        float mul = falloff ? (1.0f - dist / radius) : 1.0f;
        ApplyDamageToEnemy(reg, j, damage * mul);
    }
}

static void ApplyOrRefreshBurn(engine::ecs::Registry& reg, engine::ecs::Entity enemy,
                               float burnDps, float duration) {
    if (burnDps <= 0.0f || duration <= 0.0f) return;
    if (reg.statusEffects.Has(enemy)) {
        auto& s = reg.statusEffects.Get(enemy);
        if (burnDps >= s.burnDps) s.burnDps = burnDps;
        if (duration > s.burnRemaining) s.burnRemaining = duration;
    } else {
        game::StatusEffectComponent s;
        s.burnDps = burnDps;
        s.burnRemaining = duration;
        reg.statusEffects.Insert(enemy, s);
    }
}

static void ApplyOrRefreshHeal(engine::ecs::Registry& reg, engine::ecs::Entity e,
                               float healPerSec, float duration) {
    if (healPerSec <= 0.0f || duration <= 0.0f) return;
    if (reg.statusEffects.Has(e)) {
        auto& s = reg.statusEffects.Get(e);
        s.healPerSec = healPerSec;
        s.healRemaining = fmaxf(s.healRemaining, duration);
    } else {
        game::StatusEffectComponent s;
        s.healPerSec = healPerSec;
        s.healRemaining = duration;
        reg.statusEffects.Insert(e, s);
    }
}

static void ApplyOrRefreshDrain(engine::ecs::Registry& reg, engine::ecs::Entity e,
                                float drainPerSec, float duration, float range) {
    if (drainPerSec <= 0.0f || duration <= 0.0f) return;
    if (reg.statusEffects.Has(e)) {
        auto& s = reg.statusEffects.Get(e);
        s.drainPerSec = drainPerSec;
        s.drainRemaining = fmaxf(s.drainRemaining, duration);
        s.drainRange = range;
    } else {
        game::StatusEffectComponent s;
        s.drainPerSec = drainPerSec;
        s.drainRemaining = duration;
        s.drainRange = range;
        reg.statusEffects.Insert(e, s);
    }
}

static void HealEntity(engine::ecs::Registry& reg, engine::ecs::Entity e, float amount) {
    if (!reg.healths.Has(e) || amount <= 0.0f) return;
    auto& hp = reg.healths.Get(e);
    hp.current += amount;
    if (hp.current > hp.max) hp.current = hp.max;
}

static engine::ecs::Entity SpawnSummon(engine::ecs::Registry& reg,
                                       engine::ecs::Entity owner,
                                       Vector3 pos,
                                       const SpellDef& def) {
    using game::SummonKind;
    engine::ecs::Entity e = engine::ecs::CreateEntity(reg);

    game::TransformComponent t;
    t.position = pos;

    game::RenderComponent r;
    game::SummonComponent s;
    s.ownerId = owner.id;
    s.lifetime = fmaxf(def.lifetime, def.extra);
    s.age = 0.0f;
    s.attackDamage = def.damage;
    s.attackRange = fmaxf(def.aoeRadius, 12.0f);
    s.strikeRange = fminf(s.attackRange, 5.0f);
    s.attackTimer = 0.15f;
    s.orbitAngle = frand01() * 6.28318f;

    if (def.id == SpellId::SummonGargoyle) {
        s.kind = SummonKind::Gargoyle;
        s.combatPet = true;
        s.hoverHeight = 1.8f;
        s.attackCooldown = 0.70f;
        s.strikeRange = 5.0f;
        r.color = Color{90, 95, 105, 255};
        r.width = 1.1f; r.height = 1.6f; r.depth = 1.1f;
        r.visual = game::CharacterVisual::Gargoyle;
    } else if (def.id == SpellId::SummonBattleAngel) {
        s.kind = SummonKind::BattleAngel;
        s.combatPet = true;
        s.hoverHeight = 2.2f;
        s.attackCooldown = 0.60f;
        s.strikeRange = 5.5f;
        r.color = Color{255, 230, 140, 255};
        r.width = 1.0f; r.height = 2.0f; r.depth = 0.7f;
        r.visual = game::CharacterVisual::BattleAngel;
    } else if (def.id == SpellId::SummonReaper) {
        s.kind = SummonKind::Reaper;
        s.combatPet = true;
        s.auraAttack = true;
        s.hoverHeight = 4.0f;
        s.attackCooldown = 0.55f;
        s.strikeRange = s.attackRange;
        r.color = Color{40, 10, 50, 255};
        r.width = 3.2f; r.height = 5.5f; r.depth = 2.4f;
        r.visual = game::CharacterVisual::Reaper;
        // Rise from hell: start buried, climb to standing height
        float gy = engine::math::WorldHeight(pos.x, pos.z);
        s.spawnAnimDuration = 2.4f;
        s.spawnHomeX = pos.x;
        s.spawnHomeZ = pos.z;
        s.spawnGroundY = gy;
        s.bodyHeight = r.height;
        s.spawnStartY = gy - r.height * 0.55f;
        s.spawnEndY = gy + r.height * 0.5f + 0.6f;
        t.position = {pos.x, s.spawnStartY, pos.z};
    } else if (def.id == SpellId::SummonArchAngel) {
        s.kind = SummonKind::ArchAngel;
        s.combatPet = true;
        s.auraAttack = true;
        s.hoverHeight = 5.0f;
        s.attackCooldown = 0.55f;
        s.strikeRange = s.attackRange;
        r.color = Color{255, 245, 200, 255};
        r.width = 3.0f; r.height = 6.0f; r.depth = 2.0f;
        r.visual = game::CharacterVisual::ArchAngel;
        // Descend from the heavens along a light beam
        float gy = engine::math::WorldHeight(pos.x, pos.z);
        s.spawnAnimDuration = 2.6f;
        s.spawnHomeX = pos.x;
        s.spawnHomeZ = pos.z;
        s.spawnGroundY = gy;
        s.bodyHeight = r.height;
        s.spawnStartY = gy + 48.0f;
        s.spawnEndY = gy + r.height * 0.5f + 1.2f;
        t.position = {pos.x, s.spawnStartY, pos.z};
    } else if (def.id == SpellId::SummonPixie) {
        s.kind = SummonKind::Pixie;
        s.combatPet = false;
        s.hoverHeight = 1.4f;
        r.color = Color{180, 80, 220, 230};
        r.width = 0.35f; r.height = 0.35f; r.depth = 0.35f;
        r.visual = game::CharacterVisual::Pixie;
    } else { // Sprite
        s.kind = SummonKind::Sprite;
        s.combatPet = false;
        s.hoverHeight = 1.5f;
        r.color = Color{255, 245, 180, 230};
        r.width = 0.35f; r.height = 0.35f; r.depth = 0.35f;
        r.visual = game::CharacterVisual::Sprite;
    }

    reg.transforms.Insert(e, t);
    reg.renderables.Insert(e, r);
    reg.summons.Insert(e, s);
    return e;
}

static void SnapEnemyToGround(game::TransformComponent& t) {
    float gy = engine::math::WorldHeight(t.position.x, t.position.z);
    t.position.y = gy + 1.0f;
}

static void PushEnemyXZ(engine::ecs::Registry& reg, engine::ecs::Entity enemy, Vector3 deltaXZ) {
    if (!reg.transforms.Has(enemy)) return;
    auto& t = reg.transforms.Get(enemy);
    t.position.x += deltaXZ.x;
    t.position.z += deltaXZ.z;
    SnapEnemyToGround(t);
}

static bool ZoneAlreadyHit(const game::SpellZoneComponent& z, uint32_t id) {
    for (uint8_t i = 0; i < z.hitCount; i++) {
        if (z.hitEntityIds[i] == id) return true;
    }
    return false;
}

static void ZoneMarkHit(game::SpellZoneComponent& z, uint32_t id) {
    if (z.hitCount >= 32) return;
    z.hitEntityIds[z.hitCount++] = id;
}

static bool EnemyInWave(Vector3 enemyPos, Vector3 wavePos, Vector3 moveDir, float halfDepth,
                        float halfWidth) {
    Vector3 to = Vector3Subtract(enemyPos, wavePos);
    to.y = 0.0f;
    float along = Vector3DotProduct(to, moveDir);
    if (fabsf(along) > halfDepth) return false;
    Vector3 lateral = Vector3Subtract(to, Vector3Scale(moveDir, along));
    return Vector3Length(lateral) <= halfWidth;
}

// ---------------------------------------------------------------------------
// Aim helpers
// ---------------------------------------------------------------------------
static Vector3 LookDirection(const game::CameraComponent& cam) {
    return {
        cosf(cam.pitch) * sinf(cam.yaw),
        sinf(cam.pitch),
        cosf(cam.pitch) * cosf(cam.yaw)
    };
}

static Vector3 AimPointFromLook(const game::TransformComponent& pTrans,
                                const game::CameraComponent& pCam,
                                engine::ecs::Registry& reg,
                                float maxDist) {
    Vector3 origin = pTrans.position;
    Vector3 dir = Vector3Normalize(LookDirection(pCam));

    float bestT = maxDist;
    Vector3 best = Vector3Add(origin, Vector3Scale(dir, maxDist));
    for (size_t i = 0; i < reg.enemyAIs.data.size(); i++) {
        engine::ecs::Entity enemy = {reg.enemyAIs.indexToEntity[i]};
        if (!reg.transforms.Has(enemy)) continue;
        Vector3 ep = reg.transforms.Get(enemy).position;
        Vector3 to = Vector3Subtract(ep, origin);
        float t = Vector3DotProduct(to, dir);
        if (t < 0.5f || t > bestT) continue;
        Vector3 closest = Vector3Add(origin, Vector3Scale(dir, t));
        if (Vector3Distance(closest, ep) < 1.2f) {
            bestT = t;
            best = ep;
        }
    }

    for (float t = 1.0f; t < bestT; t += 0.5f) {
        Vector3 p = Vector3Add(origin, Vector3Scale(dir, t));
        float gy = engine::math::WorldHeight(p.x, p.z);
        if (p.y <= gy + 0.2f) {
            best = {p.x, gy + 0.15f, p.z};
            break;
        }
    }
    return best;
}

static bool FindHitscanTarget(engine::ecs::Registry& reg,
                              const game::TransformComponent& pTrans,
                              const game::CameraComponent& pCam,
                              float maxDist,
                              engine::ecs::Entity& outEnemy) {
    Vector3 origin = pTrans.position;
    Vector3 dir = Vector3Normalize(LookDirection(pCam));
    float bestT = maxDist;
    bool found = false;

    for (size_t i = 0; i < reg.enemyAIs.data.size(); i++) {
        engine::ecs::Entity enemy = {reg.enemyAIs.indexToEntity[i]};
        if (!reg.transforms.Has(enemy)) continue;
        Vector3 ep = reg.transforms.Get(enemy).position;
        Vector3 to = Vector3Subtract(ep, origin);
        float t = Vector3DotProduct(to, dir);
        if (t < 0.8f || t > bestT) continue;
        Vector3 closest = Vector3Add(origin, Vector3Scale(dir, t));
        if (Vector3Distance(closest, ep) < 1.4f) {
            bestT = t;
            outEnemy = enemy;
            found = true;
        }
    }
    return found;
}

// ---------------------------------------------------------------------------
// Spell effect factories
// ---------------------------------------------------------------------------
static void SpawnImpactVfx(SpellId id, Vector3 pos) {
    const SpellDef& def = GetSpellDef(id);
    if (def.element == SpellElement::Water) {
        switch (id) {
            case SpellId::Splash:
                SpawnWaterBurst(pos, 18, 7.0f, 1.1f);
                SpawnMistPuff(pos, 5, 1.8f);
                break;
            case SpellId::Waterjet:
                SpawnWaterBurst(pos, 10, 5.0f, 0.8f);
                SpawnMistPuff(pos, 3, 1.2f);
                break;
            case SpellId::Geyser:
                SpawnWaterBurst(pos, 28, 14.0f, 1.5f);
                SpawnMistPuff(pos, 12, 4.0f);
                break;
            case SpellId::Surge:
                SpawnWaterBurst(pos, 20, 10.0f, 1.8f);
                SpawnMistPuff(pos, 8, 2.5f);
                break;
            case SpellId::Tsunami:
                SpawnWaterBurst(pos, 40, 16.0f, 2.8f);
                SpawnMistPuff(pos, 16, 4.0f);
                break;
            default:
                SpawnWaterBurst(pos, 12, 6.0f, 1.0f);
                SpawnMistPuff(pos, 4, 1.5f);
                break;
        }
    } else {
        switch (id) {
            case SpellId::Fireball:
                SpawnEmberBurst(pos, 14, 6.0f, 1.0f);
                SpawnSmokePuff(pos, 4, 1.5f);
                break;
            case SpellId::Fireblast:
                SpawnEmberBurst(pos, 28, 9.0f, 1.4f);
                SpawnSparks(pos, 12, 8.0f);
                SpawnSmokePuff(pos, 8, 2.2f);
                break;
            case SpellId::SuperNova:
                SpawnEmberBurst(pos, 36, 12.0f, 1.6f);
                SpawnSparks(pos, 24, 14.0f);
                SpawnSmokePuff(pos, 10, 3.0f);
                break;
            case SpellId::Inferno:
                SpawnEmberBurst(pos, 48, 16.0f, 2.2f);
                SpawnSparks(pos, 32, 18.0f);
                SpawnSmokePuff(pos, 16, 4.0f);
                break;
            case SpellId::LavaPlume:
                SpawnEmberBurst(pos, 36, 14.0f, 1.8f);
                SpawnSparks(pos, 20, 12.0f);
                SpawnSmokePuff(pos, 14, 3.5f);
                break;
            case SpellId::HellOnEarth:
                SpawnEmberBurst(pos, 56, 20.0f, 2.6f);
                SpawnSparks(pos, 40, 22.0f);
                SpawnSmokePuff(pos, 22, 5.0f);
                break;
            case SpellId::Ignite:
                SpawnEmberBurst(pos, 22, 7.0f, 1.8f);
                SpawnSparks(pos, 18, 10.0f);
                SpawnSmokePuff(pos, 6, 2.5f);
                break;
            default:
                SpawnEmberBurst(pos, 10, 5.0f, 1.0f);
                break;
        }
    }
    game::sfx::PlaySpellImpact(id, pos);
}

static engine::ecs::Entity SpawnSpellZone(engine::ecs::Registry& reg, Vector3 pos, const SpellDef& def,
                                          bool damagingTicks, bool expandingVisual,
                                          Vector3 moveDir = {0, 0, 0}) {
    engine::ecs::Entity e = engine::ecs::CreateEntity(reg);
    game::TransformComponent t;
    t.position = pos;

    game::SpellZoneComponent z;
    z.spellId = (uint8_t)def.id;
    z.damage = def.damage;
    z.burnDps = def.burnDps;
    z.burnDuration = def.burnDuration;
    z.radiusMax = def.aoeRadius;
    z.radius = expandingVisual ? 0.4f : def.aoeRadius;
    z.expandSpeed = expandingVisual ? (def.aoeRadius / fmaxf(def.lifetime * 0.55f, 0.15f)) : 0.0f;
    z.lifetime = def.lifetime;
    z.age = 0.0f;
    z.tickTimer = 0.0f;
    z.tickRate = (def.id == SpellId::Hurricane || def.id == SpellId::Maelstrom) ? 0.15f
               : (def.id == SpellId::CallOfTheDead || def.id == SpellId::HellOnEarth) ? 0.18f
               : (def.id == SpellId::LavaPlume) ? 0.20f
               : 0.25f;
    z.height = (def.id == SpellId::Firewall || def.id == SpellId::Waterwall) ? 2.8f
             : (def.id == SpellId::Geyser) ? fmaxf(def.extra, 6.0f)
             : (def.id == SpellId::LavaPlume) ? fmaxf(def.extra, 12.0f)
             : (def.id == SpellId::HellOnEarth) ? fmaxf(def.extra, 18.0f)
             : (def.id == SpellId::Hurricane) ? 6.0f
             : (def.id == SpellId::Maelstrom) ? 28.0f
             : (def.id == SpellId::CallOfTheDead) ? 2.2f
             : (def.id == SpellId::Surge) ? 4.5f
             : (def.id == SpellId::Tsunami) ? 14.0f // ~45 ft crest
             : 1.0f;
    z.force = def.force;
    z.travelSpeed = def.travelSpeed;
    z.moveDirX = moveDir.x;
    z.moveDirY = moveDir.y;
    z.moveDirZ = moveDir.z;
    z.waveHalfWidth = (def.delivery == SpellDelivery::MovingWave) ? def.extra : 0.0f;
    z.damages = damagingTicks;
    z.expandingVisual = expandingVisual;
    z.damageOnce = def.damageOnce;
    z.hitCount = 0;

    reg.transforms.Insert(e, t);
    reg.spellZones.Insert(e, z);
    return e;
}

static void ExecuteHitscan(engine::ecs::Registry& reg,
                           const game::TransformComponent& pTrans,
                           const game::CameraComponent& pCam,
                           const SpellDef& def) {
    game::sfx::PlaySpellCast(def.id, pTrans.position);
    engine::ecs::Entity target{};
    Vector3 lookDir = Vector3Normalize(LookDirection(pCam));

    if (FindHitscanTarget(reg, pTrans, pCam, 55.0f, target) &&
        reg.transforms.Has(target) && reg.healths.Has(target)) {
        Vector3 hitPos = reg.transforms.Get(target).position;
        hitPos.y += 0.8f;
        for (size_t j = 0; j < reg.enemyAIs.data.size(); j++) {
            if (reg.enemyAIs.indexToEntity[j] == target.id) {
                ApplyDamageToEnemy(reg, j, def.damage);
                break;
            }
        }
        SpawnImpactVfx(def.id, hitPos);
        SpellDef burst = def;
        burst.lifetime = 0.45f;
        burst.aoeRadius = 1.6f;
        SpawnSpellZone(reg, hitPos, burst, false, true);
    } else {
        Vector3 aim = AimPointFromLook(pTrans, pCam, reg, 40.0f);
        if (def.element == SpellElement::Water) {
            SpawnWaterBurst(aim, 8, 5.0f, 0.8f);
            SpawnMistPuff(aim, 3, 1.2f);
        } else {
            SpawnSparks(aim, 8, 6.0f);
            SpawnSmokePuff(aim, 3, 1.5f);
        }
        Vector3 spawnPos = Vector3Add(pTrans.position, Vector3Scale(lookDir, 0.8f));
        factories::EntityFactory::CreateProjectileFromSpell(reg, spawnPos, lookDir, (int)def.id);
    }
}

static void FinishCast(engine::ecs::Registry& reg, engine::ecs::Entity player,
                       game::SpellCasterComponent& caster) {
    if (caster.castingSpell < 0) return;
    const SpellDef& def = GetSpellDef(caster.castingSpell);
    if (!reg.transforms.Has(player) || !reg.cameras.Has(player)) {
        caster.castingSpell = -1;
        caster.castTimer = 0.0f;
        return;
    }
    auto& pTrans = reg.transforms.Get(player);
    auto& pCam = reg.cameras.Get(player);
    Vector3 lookDir = Vector3Normalize(LookDirection(pCam));
    Vector3 aimPoint = {caster.castAimX, caster.castAimY, caster.castAimZ};
    Vector3 dir = {caster.castDirX, caster.castDirY, caster.castDirZ};
    if (Vector3LengthSqr(dir) < 0.01f) dir = lookDir;
    dir = Vector3Normalize(dir);

    if (def.delivery == SpellDelivery::Hitscan) {
        ExecuteHitscan(reg, pTrans, pCam, def);
    } else if (def.delivery == SpellDelivery::SelfBuff) {
        game::sfx::PlaySpellCast(def.id, pTrans.position);
        if (def.id == SpellId::SummonPixie) {
            ApplyOrRefreshDrain(reg, player, def.damage, def.lifetime, def.aoeRadius);
            SpawnSummon(reg, player, pTrans.position, def);
            SpawnSparks(pTrans.position, 10, 4.0f);
        } else if (def.id == SpellId::SummonSprite) {
            ApplyOrRefreshHeal(reg, player, def.damage, def.lifetime);
            SpawnSummon(reg, player, pTrans.position, def);
            SpawnMistPuff(pTrans.position, 6, 1.5f);
        }
    } else if (def.delivery == SpellDelivery::SummonPet) {
        game::sfx::PlaySpellCast(def.id, pTrans.position);
        Vector3 spawn = aimPoint;
        spawn.y = engine::math::WorldHeight(spawn.x, spawn.z) + 1.5f;
        // Prefer near player if aim is too far
        if (Vector3Distance(spawn, pTrans.position) > 18.0f) {
            spawn = Vector3Add(pTrans.position, Vector3Scale(lookDir, 3.0f));
            spawn.y = engine::math::WorldHeight(spawn.x, spawn.z) + 1.5f;
        }
        SpawnSummon(reg, player, spawn, def);
        if (def.id == SpellId::SummonReaper) {
            Vector3 ground = {spawn.x, engine::math::WorldHeight(spawn.x, spawn.z), spawn.z};
            SpawnHellRiftBurst(ground, 4.5f);
        } else if (def.id == SpellId::SummonArchAngel) {
            Vector3 ground = {spawn.x, engine::math::WorldHeight(spawn.x, spawn.z), spawn.z};
            SpawnHolyBeamBurst(ground, 48.0f);
        } else if (def.element == SpellElement::Necromancer) {
            SpawnSmokePuff(spawn, 8, 2.0f);
            SpawnSparks(spawn, 6, 3.0f);
        } else {
            SpawnMistPuff(spawn, 8, 2.0f);
            SpawnSparks(spawn, 8, 4.0f);
        }
    } else if (def.delivery == SpellDelivery::Mobility) {
        game::sfx::PlaySpellCast(def.id, pTrans.position);
        if (def.id == SpellId::Dash && reg.playerInputs.Has(player)) {
            auto& pin = reg.playerInputs.Get(player);
            Vector3 dashDir = {dir.x, 0.0f, dir.z};
            // Prefer WASD direction if moving
            if (reg.cameras.Has(player)) {
                auto& cam = reg.cameras.Get(player);
                Vector3 forward = {sinf(cam.yaw), 0.0f, cosf(cam.yaw)};
                Vector3 right   = {-cosf(cam.yaw), 0.0f, sinf(cam.yaw)};
                Vector3 moveDir = {0, 0, 0};
                if (engine::input::IsActionDown("MoveForward"))  moveDir = Vector3Add(moveDir, forward);
                if (engine::input::IsActionDown("MoveBackward")) moveDir = Vector3Subtract(moveDir, forward);
                if (engine::input::IsActionDown("MoveRight"))    moveDir = Vector3Add(moveDir, right);
                if (engine::input::IsActionDown("MoveLeft"))     moveDir = Vector3Subtract(moveDir, right);
                if (Vector3LengthSqr(moveDir) > 0.01f) dashDir = moveDir;
            }
            if (Vector3LengthSqr(dashDir) < 0.01f) dashDir = {0, 0, 1};
            dashDir = Vector3Normalize(dashDir);
            float dist = fmaxf(def.extra, 1.0f);
            float dur  = fmaxf(def.lifetime, 0.08f);
            float speed = dist / dur;
            pin.dashTimer = dur;
            pin.dashVelX = dashDir.x * speed;
            pin.dashVelZ = dashDir.z * speed;
            SpawnMistPuff(pTrans.position, 6, 1.2f);
            SpawnSparks(pTrans.position, 8, 5.0f);
        } else if (def.id == SpellId::Teleport) {
            Vector3 dest = aimPoint;
            Vector3 from = pTrans.position;
            Vector3 delta = Vector3Subtract(dest, from);
            delta.y = 0.0f;
            float maxR = fmaxf(def.aoeRadius, 5.0f);
            float d = Vector3Length(delta);
            if (d > maxR && d > 0.01f) {
                delta = Vector3Scale(Vector3Normalize(delta), maxR);
                dest = Vector3Add(from, delta);
            }
            dest.y = engine::math::WorldHeight(dest.x, dest.z) + 2.0f; // eye height
            SpawnMistPuff(from, 10, 2.0f);
            SpawnSparks(from, 6, 4.0f);
            pTrans.position = dest;
            if (reg.cameras.Has(player)) {
                reg.cameras.Get(player).camera.position = dest;
            }
            if (reg.playerInputs.Has(player)) {
                reg.playerInputs.Get(player).velocityY = 0.0f;
                reg.playerInputs.Get(player).grounded = true;
            }
            SpawnMistPuff(dest, 10, 2.0f);
            SpawnSparks(dest, 10, 5.0f);
        }
    } else if (def.delivery == SpellDelivery::Projectile) {
        game::sfx::PlaySpellCast(def.id, pTrans.position);
        Vector3 spawnPos = Vector3Add(pTrans.position, Vector3Scale(lookDir, 0.8f));
        factories::EntityFactory::CreateProjectileFromSpell(reg, spawnPos, dir, (int)def.id);
    } else if (def.delivery == SpellDelivery::MovingWave) {
        game::sfx::PlaySpellCast(def.id, pTrans.position);
        Vector3 horiz = {dir.x, 0.0f, dir.z};
        if (Vector3LengthSqr(horiz) < 0.01f) horiz = {0, 0, 1};
        horiz = Vector3Normalize(horiz);
        Vector3 spawn = Vector3Add(pTrans.position, Vector3Scale(horiz, 3.0f));
        float gy = engine::math::WorldHeight(spawn.x, spawn.z);
        spawn.y = gy + 0.2f;
        SpawnSpellZone(reg, spawn, def, true, false, horiz);
        SpawnWaterBurst(spawn, 24, 8.0f, 1.6f);
        SpawnMistPuff(spawn, 10, 3.0f);
    } else {
        game::sfx::PlaySpellCast(def.id, pTrans.position);
        if (def.delivery == SpellDelivery::InstantAoE) {
            ApplyAoEDamage(reg, aimPoint, def.aoeRadius, def.damage, true);
            SpawnImpactVfx(def.id, aimPoint);

            if (def.id == SpellId::LavaPlume) {
                // Lingering burning column (DPS zone, not just a visual)
                SpellDef residual = def;
                residual.damage = 45.0f; // DPS while standing in the plume
                SpawnSpellZone(reg, aimPoint, residual, true, false);
            } else if (def.id == SpellId::HellOnEarth) {
                // Inferno-style expanding shockwave (visual)
                SpellDef blast = def;
                blast.lifetime = 1.50f;
                SpawnSpellZone(reg, aimPoint, blast, false, true);
                // Persistent hellfire ring + plume (Firewall + Lava Plume, same AoE as Maelstrom)
                SpellDef hell = def;
                hell.damage = 95.0f;     // ring/plume DPS
                hell.aoeRadius = def.aoeRadius; // 120 — match Maelstrom
                hell.lifetime = 10.0f;
                hell.burnDps = 40.0f;
                hell.burnDuration = 3.5f;
                SpawnSpellZone(reg, aimPoint, hell, true, false);
            } else {
                // Geyser uses a tall residual column; other AoEs use expanding blast rings.
                bool expand = (def.id != SpellId::Geyser);
                SpawnSpellZone(reg, aimPoint, def, false, expand);
            }
        } else if (def.delivery == SpellDelivery::PersistentZone) {
            SpawnSpellZone(reg, aimPoint, def, true, false);
            if (def.element == SpellElement::Water) {
                SpawnWaterBurst(aimPoint, 16, 5.0f, 1.2f);
                SpawnMistPuff(aimPoint, 8, 2.2f);
            } else if (def.element == SpellElement::Necromancer) {
                SpawnSmokePuff(aimPoint, 10, 2.5f);
                SpawnSparks(aimPoint, 12, 4.0f);
            } else if (def.element == SpellElement::Priest) {
                SpawnMistPuff(aimPoint, 8, 2.0f);
                SpawnSparks(aimPoint, 10, 5.0f);
            } else {
                SpawnEmberBurst(aimPoint, 16, 4.0f, 1.2f);
                SpawnSmokePuff(aimPoint, 6, 2.0f);
            }
        }
    }

    caster.castingSpell = -1;
    caster.castTimer = 0.0f;
}

static void SpawnZoneAmbientParticles(const game::SpellZoneComponent& z, Vector3 center) {
    SpellId id = (SpellId)z.spellId;
    const SpellDef& def = GetSpellDef(id);

    if (def.element == SpellElement::Water) {
        int chance = 40;
        if (id == SpellId::Maelstrom) chance = 85;
        else if (id == SpellId::Tsunami) chance = 80;
        else if (id == SpellId::Surge) chance = 55;
        else if (id == SpellId::Whirlpool || id == SpellId::Hurricane) chance = 65;
        else if (id == SpellId::Waterwall) chance = 60;
        if (GetRandomValue(0, 100) < chance) {
            float ang = frand01() * 6.28318f;
            float r = (z.waveHalfWidth > 0.0f)
                ? (frand01() * z.waveHalfWidth)
                : (z.radius * (0.3f + frand01() * 0.7f));
            Vector3 p = {
                center.x + cosf(ang) * r,
                center.y + frand01() * z.height,
                center.z + sinf(ang) * r
            };
            if (id == SpellId::Whirlpool || id == SpellId::Hurricane) {
                float spin = 3.0f;
                Vector3 tang = {-sinf(ang) * spin, 1.0f + frand01() * 2.0f, cosf(ang) * spin};
                SpawnParticle(p, tang, Color{120, 190, 255, 210}, 0.45f, 0.1f, ParticleKind::Droplet);
            } else if (id == SpellId::Maelstrom) {
                // Dense spiraling spray up the funnel
                float spin = 12.0f;
                float rr = z.radius * (0.15f + frand01() * 0.85f);
                Vector3 p2 = {
                    center.x + cosf(ang) * rr,
                    center.y + frand01() * z.height,
                    center.z + sinf(ang) * rr
                };
                Vector3 tang = {-sinf(ang) * spin, 2.0f + frand01() * 6.0f, cosf(ang) * spin};
                SpawnParticle(p2, tang, Color{140, 200, 255, 220}, 0.6f, 0.16f, ParticleKind::Droplet);
                if (GetRandomValue(0, 100) < 55) {
                    SpawnParticle(p2, Vector3Scale(tang, 0.45f),
                                  Color{190, 220, 240, 120}, 1.0f, 0.45f, ParticleKind::Mist);
                }
            } else if (id == SpellId::Surge || id == SpellId::Tsunami) {
                Vector3 md = {z.moveDirX, 0, z.moveDirZ};
                if (Vector3LengthSqr(md) > 0.01f) md = Vector3Normalize(md);
                bool mega = (id == SpellId::Tsunami);
                float boost = mega ? 10.0f : 4.0f;
                // Spawn along the wave face, not a circle
                float u = (frand01() * 2.0f - 1.0f) * z.waveHalfWidth;
                Vector3 right = {-md.z, 0, md.x};
                Vector3 p2 = Vector3Add(center, Vector3Add(Vector3Scale(right, u),
                                                           Vector3Scale(md, (frand01() - 0.3f) * z.radius)));
                p2.y = center.y + frand01() * z.height;
                SpawnParticle(p2, Vector3Add(Vector3Scale(md, boost), {0, 4.0f + frand01() * 6.0f, 0}),
                              Color{200, 230, 255, 230}, 0.55f, mega ? 0.28f : 0.14f,
                              ParticleKind::Droplet);
                if (mega && GetRandomValue(0, 100) < 60) {
                    SpawnParticle(p2, Vector3Add(Vector3Scale(md, 3.0f), {0, 1.5f, 0}),
                                  Color{180, 210, 230, 110}, 1.1f, 0.55f, ParticleKind::Mist);
                }
            } else {
                SpawnParticle(p, {0, 2.0f + frand01() * 3.0f, 0},
                              Color{100, 180, 255, 210}, 0.5f, 0.1f, ParticleKind::Droplet);
            }
            if (GetRandomValue(0, 100) < 35) {
                SpawnParticle(p, {(frand01() - 0.5f), 1.0f, (frand01() - 0.5f)},
                              Color{170, 200, 220, 100}, 0.8f, 0.35f, ParticleKind::Mist);
            }
        }
    } else if (def.element == SpellElement::Necromancer && z.damages && GetRandomValue(0, 100) < 40) {
        float ang = frand01() * 6.28318f;
        float r = z.radius * frand01();
        Vector3 p = {
            center.x + cosf(ang) * r,
            center.y + frand01() * z.height,
            center.z + sinf(ang) * r
        };
        SpawnParticle(p, {0, 2.0f + frand01() * 3.0f, 0},
                      Color{140, 50, 180, 200}, 0.5f, 0.15f, ParticleKind::Smoke);
    } else if ((id == SpellId::LavaPlume || id == SpellId::HellOnEarth ||
                id == SpellId::Firewall) && z.damages) {
        int chance = (id == SpellId::HellOnEarth) ? 90 : (id == SpellId::LavaPlume) ? 78 : 60;
        if (GetRandomValue(0, 100) >= chance) return;
        bool hell = (id == SpellId::HellOnEarth);
        bool wall = (id == SpellId::Firewall);
        float ang = frand01() * 6.28318f;
        bool fromColumn = hell ? (GetRandomValue(0, 100) < 55) : !wall;
        float r = wall ? z.radius * (0.85f + frand01() * 0.15f)
               : fromColumn ? (z.radius * (hell ? 0.12f : 0.35f) * frand01())
                            : (z.radius * (0.55f + frand01() * 0.45f));
        Vector3 p = {
            center.x + cosf(ang) * r,
            center.y + (fromColumn ? frand01() * z.height : frand01() * (wall ? z.height : 3.5f)),
            center.z + sinf(ang) * r
        };
        Vector3 vel = {
            (frand01() - 0.5f) * (hell ? 6.0f : 3.0f),
            (fromColumn || wall ? 6.0f : 3.0f) + frand01() * (hell ? 14.0f : 8.0f),
            (frand01() - 0.5f) * (hell ? 6.0f : 3.0f)
        };
        SpawnParticle(p, vel, Color{255, (unsigned char)(80 + GetRandomValue(0, 100)), 20, 240},
                      0.5f + frand01() * 0.5f, hell ? 0.2f : 0.12f, ParticleKind::Ember);
        if (GetRandomValue(0, 100) < (hell ? 55 : 40)) {
            SpawnParticle(p, {vel.x * 0.3f, vel.y * 0.4f, vel.z * 0.3f},
                          Color{40, 30, 25, 140}, 1.1f, hell ? 0.55f : 0.35f, ParticleKind::Smoke);
        }
        if (hell && GetRandomValue(0, 100) < 35) {
            SpawnSparks(p, 2, 8.0f);
        }
    } else if (z.damages && GetRandomValue(0, 100) < 35) {
        float ang = frand01() * 6.28318f;
        float r = z.radius * (0.7f + frand01() * 0.3f);
        Vector3 p = {
            center.x + cosf(ang) * r,
            center.y + frand01() * z.height,
            center.z + sinf(ang) * r
        };
        SpawnParticle(p, {0, 1.5f + frand01() * 2.5f, 0},
                      Color{255, 140, 40, 230}, 0.5f, 0.1f, ParticleKind::Ember);
        if (GetRandomValue(0, 100) < 40) {
            SpawnParticle(p, {(frand01() - 0.5f) * 0.5f, 1.2f, (frand01() - 0.5f) * 0.5f},
                          Color{50, 50, 50, 120}, 0.9f, 0.3f, ParticleKind::Smoke);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool TryCastSpell(engine::ecs::Registry& reg, int spellId, bool freeCast) {
    if (spellId < 0 || spellId >= (int)SpellId::Count) return false;
    if (reg.playerInputs.data.empty()) return false;

    engine::ecs::Entity player = {reg.playerInputs.indexToEntity[0]};
    if (!reg.spellCasters.Has(player) || !reg.transforms.Has(player) || !reg.cameras.Has(player)) {
        return false;
    }

    auto& caster = reg.spellCasters.Get(player);
    if (caster.castingSpell >= 0) return false;

    const SpellDef& def = GetSpellDef(spellId);
    if (def.delivery == SpellDelivery::Passive) return false; // always-on; not castable
    if (!freeCast) {
        if (caster.cooldowns[spellId] > 0.0f) return false;
        if (caster.mana < def.manaCost) return false;
        caster.mana -= def.manaCost;
        caster.cooldowns[spellId] = def.cooldown;
    }

    auto& pTrans = reg.transforms.Get(player);
    auto& pCam = reg.cameras.Get(player);
    Vector3 lookDir = Vector3Normalize(LookDirection(pCam));
    Vector3 aim = AimPointFromLook(pTrans, pCam, reg, 60.0f);

    caster.castAimX = aim.x;
    caster.castAimY = aim.y;
    caster.castAimZ = aim.z;
    caster.castDirX = lookDir.x;
    caster.castDirY = lookDir.y;
    caster.castDirZ = lookDir.z;
    caster.castingSpell = spellId;
    caster.castTimer = def.castTime;

    if (def.castTime <= 0.0f) {
        FinishCast(reg, player, caster);
    }
    return true;
}

void SpellSystem(engine::ecs::Registry& reg) {
    float dt = GetFrameTime();
    if (dt > 0.1f) dt = 0.1f;

    UpdateParticles(dt);

    for (size_t i = 0; i < reg.spellCasters.data.size(); i++) {
        auto& caster = reg.spellCasters.data[i];
        engine::ecs::Entity owner = {reg.spellCasters.indexToEntity[i]};

        caster.mana += caster.manaRegen * dt;
        if (caster.mana > caster.manaMax) caster.mana = caster.manaMax;

        for (int s = 0; s < (int)SpellId::Count; s++) {
            if (caster.cooldowns[s] > 0.0f) {
                caster.cooldowns[s] -= dt;
                if (caster.cooldowns[s] < 0.0f) caster.cooldowns[s] = 0.0f;
            }
        }

        if (caster.castingSpell >= 0) {
            caster.castTimer -= dt;
            if (caster.castTimer <= 0.0f) {
                FinishCast(reg, owner, caster);
            }
        }
    }

    if (engine::input::IsCursorLocked() && !reg.playerInputs.data.empty()) {
        engine::ecs::Entity player = {reg.playerInputs.indexToEntity[0]};
        if (reg.spellCasters.Has(player)) {
            auto& caster = reg.spellCasters.Get(player);

            // Tab: cycle Fire → Water → Necro → Priest → Ranger
            if (IsKeyPressed(KEY_TAB)) {
                caster.selectedElement =
                    (uint8_t)NextElement((SpellElement)caster.selectedElement);
                caster.selectedSlot = 0;
            }

            // 1-9 / 0: select spell slot within the current class (does not cast)
            SpellId list[16];
            int n = GetSpellsForElement((SpellElement)caster.selectedElement, list, 16);
            auto selectSlot = [&](int slot) {
                if (slot >= 0 && slot < n) caster.selectedSlot = (uint8_t)slot;
            };
            if (IsKeyPressed(KEY_ONE))   selectSlot(0);
            if (IsKeyPressed(KEY_TWO))   selectSlot(1);
            if (IsKeyPressed(KEY_THREE)) selectSlot(2);
            if (IsKeyPressed(KEY_FOUR))  selectSlot(3);
            if (IsKeyPressed(KEY_FIVE))  selectSlot(4);
            if (IsKeyPressed(KEY_SIX))   selectSlot(5);
            if (IsKeyPressed(KEY_SEVEN)) selectSlot(6);
            if (IsKeyPressed(KEY_EIGHT)) selectSlot(7);
            if (IsKeyPressed(KEY_NINE))  selectSlot(8);
            if (IsKeyPressed(KEY_ZERO))  selectSlot(9);
            if (caster.selectedSlot >= n && n > 0) caster.selectedSlot = (uint8_t)(n - 1);
        }
    }

    // Status effects: burn / heal / pixie drain
    for (int i = (int)reg.statusEffects.data.size() - 1; i >= 0; i--) {
        engine::ecs::Entity e = {reg.statusEffects.indexToEntity[i]};
        auto& s = reg.statusEffects.data[i];
        if (!reg.healths.Has(e)) {
            reg.statusEffects.Remove(e);
            continue;
        }

        if (s.burnRemaining > 0.0f && s.burnDps > 0.0f) {
            float dmg = s.burnDps * dt;
            reg.healths.Get(e).current -= dmg;
            if (reg.enemyAIs.Has(e) &&
                engine::networking::GetLobbyState() == engine::networking::LobbyState::InLobby &&
                !engine::networking::IsHost()) {
                engine::networking::SendDamageToHost(reg.enemyAIs.Get(e).netId, dmg);
            }
            if (reg.transforms.Has(e) && GetRandomValue(0, 100) < 8) {
                Vector3 p = reg.transforms.Get(e).position;
                p.y += 1.0f;
                SpawnParticle(p, {0, 2.0f + frand01() * 2.0f, 0},
                              Color{255, 120, 40, 220}, 0.4f, 0.08f, ParticleKind::Ember);
            }
            s.burnRemaining -= dt;
            if (s.burnRemaining < 0.0f) { s.burnRemaining = 0.0f; s.burnDps = 0.0f; }
        }

        if (s.healRemaining > 0.0f && s.healPerSec > 0.0f) {
            HealEntity(reg, e, s.healPerSec * dt);
            if (reg.transforms.Has(e) && GetRandomValue(0, 100) < 12) {
                Vector3 p = reg.transforms.Get(e).position;
                p.y += 1.2f;
                SpawnParticle(p, {0, 1.5f + frand01(), 0},
                              Color{255, 240, 160, 220}, 0.35f, 0.08f, ParticleKind::Spark);
            }
            s.healRemaining -= dt;
            if (s.healRemaining < 0.0f) { s.healRemaining = 0.0f; s.healPerSec = 0.0f; }
        }

        if (s.drainRemaining > 0.0f && s.drainPerSec > 0.0f && reg.transforms.Has(e)) {
            Vector3 origin = reg.transforms.Get(e).position;
            float bestDist = s.drainRange;
            int bestIdx = -1;
            for (size_t j = 0; j < reg.enemyAIs.data.size(); j++) {
                engine::ecs::Entity enemy = {reg.enemyAIs.indexToEntity[j]};
                if (!reg.transforms.Has(enemy) || !reg.healths.Has(enemy)) continue;
                float d = Vector3Distance(origin, reg.transforms.Get(enemy).position);
                if (d < bestDist) { bestDist = d; bestIdx = (int)j; }
            }
            if (bestIdx >= 0) {
                float stolen = s.drainPerSec * dt;
                ApplyDamageToEnemy(reg, (size_t)bestIdx, stolen);
                HealEntity(reg, e, stolen);
                if (GetRandomValue(0, 100) < 20) {
                    engine::ecs::Entity victim = {reg.enemyAIs.indexToEntity[bestIdx]};
                    Vector3 vp = reg.transforms.Get(victim).position;
                    vp.y += 1.0f;
                    SpawnParticle(vp, Vector3Scale(Vector3Normalize(Vector3Subtract(origin, vp)), 4.0f),
                                  Color{180, 60, 220, 230}, 0.3f, 0.07f, ParticleKind::Spark);
                }
            }
            s.drainRemaining -= dt;
            if (s.drainRemaining < 0.0f) { s.drainRemaining = 0.0f; s.drainPerSec = 0.0f; }
        }

        if (s.burnRemaining <= 0.0f && s.healRemaining <= 0.0f && s.drainRemaining <= 0.0f) {
            reg.statusEffects.Remove(e);
        }
    }

    // Summons: orbit familiars + combat pets
    for (int i = (int)reg.summons.data.size() - 1; i >= 0; i--) {
        engine::ecs::Entity e = {reg.summons.indexToEntity[i]};
        if (!reg.transforms.Has(e)) {
            engine::ecs::DestroyEntity(reg, e);
            continue;
        }
        auto& s = reg.summons.data[i];
        auto& t = reg.transforms.Get(e);
        s.age += dt;
        if (s.age >= s.lifetime) {
            engine::ecs::DestroyEntity(reg, e);
            continue;
        }

        // Intro: Reaper rises from hell / Arch Angel descends in light — lock XZ, ease Y.
        if (s.spawnAnimDuration > 0.0f && s.age < s.spawnAnimDuration) {
            float u = Smoothstep01(s.age / s.spawnAnimDuration);
            t.position.x = s.spawnHomeX;
            t.position.z = s.spawnHomeZ;
            t.position.y = s.spawnStartY + (s.spawnEndY - s.spawnStartY) * u;

            if (s.kind == game::SummonKind::Reaper) {
                Vector3 ground = {s.spawnHomeX, s.spawnGroundY, s.spawnHomeZ};
                if (GetRandomValue(0, 100) < 55) {
                    float a = frand01() * 6.28318f;
                    float r = frand01() * 3.2f;
                    Vector3 p = {ground.x + cosf(a) * r, ground.y + 0.15f, ground.z + sinf(a) * r};
                    SpawnParticle(p,
                                  {(frand01() - 0.5f) * 1.2f, 2.5f + frand01() * 5.0f, (frand01() - 0.5f) * 1.2f},
                                  LerpColor(Color{255, 70, 20, 200}, Color{60, 10, 30, 160}, frand01()),
                                  0.5f + frand01() * 0.6f, 0.2f + frand01() * 0.35f, ParticleKind::Smoke);
                }
                if (GetRandomValue(0, 100) < 35) {
                    SpawnEmberBurst({t.position.x, s.spawnGroundY + 0.5f, t.position.z}, 3, 4.0f, 1.2f);
                }
            } else if (s.kind == game::SummonKind::ArchAngel) {
                if (GetRandomValue(0, 100) < 50) {
                    float a = frand01() * 6.28318f;
                    float r = frand01() * 1.6f;
                    float y = s.spawnGroundY + frand01() * (t.position.y - s.spawnGroundY + 8.0f);
                    Vector3 p = {s.spawnHomeX + cosf(a) * r, y, s.spawnHomeZ + sinf(a) * r};
                    SpawnParticle(p,
                                  {(frand01() - 0.5f) * 0.6f, -3.0f - frand01() * 5.0f, (frand01() - 0.5f) * 0.6f},
                                  Color{255, 245, 190, 210}, 0.45f + frand01() * 0.5f,
                                  0.1f + frand01() * 0.18f, ParticleKind::Mist);
                }
            }
            continue; // no combat / chase during intro
        }

        engine::ecs::Entity owner{s.ownerId};
        Vector3 ownerPos = t.position;
        if (engine::ecs::IsValid(reg, owner) && reg.transforms.Has(owner)) {
            ownerPos = reg.transforms.Get(owner).position;
        }

        // Horizontal (XZ) distance — pets hover above ground, so 3D range was never hitting.
        auto xzDist = [](Vector3 a, Vector3 b) {
            float dx = a.x - b.x, dz = a.z - b.z;
            return sqrtf(dx * dx + dz * dz);
        };

        float bestDist = s.attackRange;
        int bestIdx = -1;
        if (s.combatPet) {
            for (size_t j = 0; j < reg.enemyAIs.data.size(); j++) {
                engine::ecs::Entity enemy = {reg.enemyAIs.indexToEntity[j]};
                if (!reg.transforms.Has(enemy) || !reg.healths.Has(enemy)) continue;
                if (reg.healths.Get(enemy).current <= 0.0f) continue;
                float d = xzDist(t.position, reg.transforms.Get(enemy).position);
                if (d < bestDist) { bestDist = d; bestIdx = (int)j; }
            }
        }

        s.orbitAngle += dt * (s.combatPet ? 1.2f : 3.5f);
        float orbitR = s.auraAttack ? 6.0f : (s.combatPet ? 2.2f : 1.1f);
        Vector3 desired = {
            ownerPos.x + cosf(s.orbitAngle) * orbitR,
            ownerPos.y + s.hoverHeight,
            ownerPos.z + sinf(s.orbitAngle) * orbitR
        };

        // Chase nearest enemy in seek range; otherwise orbit owner.
        if (s.combatPet && bestIdx >= 0) {
            Vector3 ep = reg.transforms.Get({reg.enemyAIs.indexToEntity[bestIdx]}).position;
            float chaseR = s.auraAttack ? 4.0f : 1.6f;
            desired = {
                ep.x + cosf(s.orbitAngle) * chaseR,
                ep.y + s.hoverHeight * 0.55f,
                ep.z + sinf(s.orbitAngle) * chaseR
            };
        }
        float chaseSpeed = s.auraAttack ? 4.0f : 8.0f;
        t.position = Vector3Lerp(t.position, desired, fminf(1.0f, dt * chaseSpeed));

        if (reg.renderables.Has(e)) {
            Vector3 face = Vector3Subtract(desired, t.position);
            face.y = 0.0f;
            if (s.combatPet && bestIdx >= 0) {
                Vector3 ep = reg.transforms.Get({reg.enemyAIs.indexToEntity[bestIdx]}).position;
                face = Vector3Subtract(ep, t.position);
                face.y = 0.0f;
            }
            if (Vector3LengthSqr(face) > 0.001f) {
                reg.renderables.Get(e).facingYaw = atan2f(face.x, face.z);
            }
        }

        if (!s.combatPet) continue;

        s.attackTimer -= dt;
        if (s.attackTimer > 0.0f) continue;
        s.attackTimer = s.attackCooldown;

        if (s.auraAttack) {
            // Ultimate pets: damage every living enemy inside the aura radius.
            bool anyHit = false;
            for (size_t j = 0; j < reg.enemyAIs.data.size(); j++) {
                engine::ecs::Entity enemy = {reg.enemyAIs.indexToEntity[j]};
                if (!reg.transforms.Has(enemy) || !reg.healths.Has(enemy)) continue;
                if (reg.healths.Get(enemy).current <= 0.0f) continue;
                float d = xzDist(t.position, reg.transforms.Get(enemy).position);
                if (d > s.attackRange) continue;
                ApplyDamageToEnemy(reg, j, s.attackDamage);
                Vector3 vp = reg.transforms.Get(enemy).position;
                vp.y += 1.0f;
                anyHit = true;
                if (s.kind == game::SummonKind::ArchAngel) {
                    SpawnSparks(vp, 4, 6.0f);
                } else {
                    SpawnSmokePuff(vp, 2, 1.4f);
                    SpawnSparks(vp, 2, 3.0f);
                }
            }
            if (anyHit && s.kind == game::SummonKind::ArchAngel && engine::ecs::IsValid(reg, owner)) {
                HealEntity(reg, owner, 12.0f);
            }
            continue;
        }

        if (bestIdx < 0 || bestDist > s.strikeRange) continue;

        ApplyDamageToEnemy(reg, (size_t)bestIdx, s.attackDamage);
        engine::ecs::Entity victim = {reg.enemyAIs.indexToEntity[bestIdx]};
        Vector3 vp = reg.transforms.Get(victim).position;
        vp.y += 1.0f;
        if (s.kind == game::SummonKind::BattleAngel) {
            SpawnSparks(vp, 6, 5.0f);
            if (engine::ecs::IsValid(reg, owner)) HealEntity(reg, owner, 4.0f);
        } else {
            SpawnSmokePuff(vp, 2, 1.0f);
            SpawnSparks(vp, 3, 3.0f);
        }
    }

    // Spell zones
    for (int i = (int)reg.spellZones.data.size() - 1; i >= 0; i--) {
        engine::ecs::Entity e = {reg.spellZones.indexToEntity[i]};
        if (!reg.transforms.Has(e)) {
            engine::ecs::DestroyEntity(reg, e);
            continue;
        }
        auto& z = reg.spellZones.data[i];
        auto& t = reg.transforms.Get(e);
        z.age += dt;

        // Moving wave (Surge / Tsunami)
        if (z.travelSpeed > 0.0f) {
            Vector3 md = {z.moveDirX, z.moveDirY, z.moveDirZ};
            t.position = Vector3Add(t.position, Vector3Scale(md, z.travelSpeed * dt));
            float gy = engine::math::WorldHeight(t.position.x, t.position.z);
            t.position.y = gy + 0.2f;
        }

        if (z.expandSpeed > 0.0f && z.radius < z.radiusMax) {
            z.radius += z.expandSpeed * dt;
            if (z.radius > z.radiusMax) z.radius = z.radiusMax;
        }

        if (z.damages) {
            Vector3 moveDir = {z.moveDirX, 0.0f, z.moveDirZ};
            bool isWave = z.travelSpeed > 0.0f && Vector3LengthSqr(moveDir) > 0.01f;
            if (isWave) moveDir = Vector3Normalize(moveDir);

            // Continuous force every frame while inside
            if (fabsf(z.force) > 0.01f) {
                for (size_t j = 0; j < reg.enemyAIs.data.size(); j++) {
                    engine::ecs::Entity enemy = {reg.enemyAIs.indexToEntity[j]};
                    if (!reg.transforms.Has(enemy)) continue;
                    Vector3 ep = reg.transforms.Get(enemy).position;
                    bool inside = false;
                    Vector3 forceDir = {0, 0, 0};

                    if (isWave) {
                        inside = EnemyInWave(ep, t.position, moveDir, z.radius, z.waveHalfWidth);
                        if (inside) forceDir = moveDir; // push along wave
                    } else {
                        float dist = Vector3Distance(
                            Vector3{t.position.x, 0, t.position.z},
                            Vector3{ep.x, 0, ep.z});
                        if (dist <= z.radius && dist > 0.15f) {
                            inside = true;
                            forceDir = Vector3Normalize(Vector3{
                                ep.x - t.position.x, 0, ep.z - t.position.z});
                            // force >0 push out, <0 pull in
                            if (z.force < 0.0f) forceDir = Vector3Scale(forceDir, -1.0f);
                        }
                    }

                    if (inside) {
                        float strength = fabsf(z.force) * dt;
                        PushEnemyXZ(reg, enemy, Vector3Scale(forceDir, strength));
                    }
                }
            }

            z.tickTimer -= dt;
            if (z.tickTimer <= 0.0f) {
                z.tickTimer = z.tickRate;
                float tickDmg = z.damageOnce ? 0.0f : (z.damage * z.tickRate);

                for (size_t j = 0; j < reg.enemyAIs.data.size(); j++) {
                    engine::ecs::Entity enemy = {reg.enemyAIs.indexToEntity[j]};
                    if (!reg.transforms.Has(enemy)) continue;
                    Vector3 ep = reg.transforms.Get(enemy).position;
                    bool inside = false;

                    if (isWave) {
                        inside = EnemyInWave(ep, t.position, moveDir, z.radius, z.waveHalfWidth);
                    } else {
                        float dist = Vector3Distance(
                            Vector3{t.position.x, 0, t.position.z},
                            Vector3{ep.x, 0, ep.z});
                        inside = dist <= z.radius;
                    }
                    if (!inside) continue;

                    if (z.damageOnce) {
                        if (!ZoneAlreadyHit(z, enemy.id)) {
                            ZoneMarkHit(z, enemy.id);
                            ApplyDamageToEnemy(reg, j, z.damage);
                            SpawnImpactVfx((SpellId)z.spellId, ep);
                        }
                    } else if (tickDmg > 0.0f) {
                        ApplyDamageToEnemy(reg, j, tickDmg);
                        ApplyOrRefreshBurn(reg, enemy, z.burnDps, z.tickRate + 0.15f);
                    }
                }
            }

            SpawnZoneAmbientParticles(z, t.position);
        } else if (z.expandingVisual && GetRandomValue(0, 100) < 25) {
            float ang = frand01() * 6.28318f;
            Vector3 p = {
                t.position.x + cosf(ang) * z.radius,
                t.position.y + 0.4f + frand01(),
                t.position.z + sinf(ang) * z.radius
            };
            const SpellDef& def = GetSpellDef((int)z.spellId);
            if (def.element == SpellElement::Water) {
                SpawnParticle(p, {0, 2.0f, 0}, Color{140, 200, 255, 200}, 0.35f, 0.12f, ParticleKind::Droplet);
            } else {
                SpawnParticle(p, {0, 2.0f, 0}, Color{255, 180, 50, 200}, 0.35f, 0.12f, ParticleKind::Ember);
            }
        } else if (!z.damages && !z.expandingVisual) {
            // Geyser residual column spray
            if ((SpellId)z.spellId == SpellId::Geyser && GetRandomValue(0, 100) < 45) {
                Vector3 p = {
                    t.position.x + (frand01() - 0.5f) * 1.2f,
                    t.position.y + frand01() * z.height,
                    t.position.z + (frand01() - 0.5f) * 1.2f
                };
                SpawnParticle(p, {(frand01() - 0.5f) * 2.0f, 6.0f + frand01() * 8.0f, (frand01() - 0.5f) * 2.0f},
                              Color{150, 210, 255, 220}, 0.5f, 0.12f, ParticleKind::Droplet);
                if (GetRandomValue(0, 100) < 30) {
                    SpawnParticle(p, {0, 2.0f, 0}, Color{180, 210, 230, 90}, 0.7f, 0.4f, ParticleKind::Mist);
                }
            }
        }

        if (z.age >= z.lifetime) {
            engine::ecs::DestroyEntity(reg, e);
        }
    }
}

void SpellVfxRenderSystem(engine::ecs::Registry& reg) {
    for (size_t i = 0; i < reg.projectiles.data.size(); i++) {
        engine::ecs::Entity e = {reg.projectiles.indexToEntity[i]};
        if (!reg.transforms.Has(e)) continue;
        auto& p = reg.projectiles.data[i];
        auto& t = reg.transforms.Get(e);
        SpellId id = (SpellId)p.spellId;

        if (id == SpellId::Fireball) {
            float flick = 0.9f + 0.1f * sinf((float)GetTime() * 18.0f);
            DrawSphere(t.position, p.radius * 1.15f * flick, Color{255, 80, 20, 180});
            DrawSphere(t.position, p.radius, Color{255, 140, 40, 255});
            DrawSphere(t.position, p.radius * 0.5f, Color{255, 230, 100, 255});
            DrawSphere(t.position, p.radius * 0.22f, Color{255, 255, 220, 255});
            // Trail tongues
            Vector3 back = Vector3Subtract(t.position, Vector3Scale(p.direction, p.radius * 1.8f));
            DrawCylinderEx(back, t.position, p.radius * 0.55f, p.radius * 0.15f, 6,
                           Color{255, 100, 30, 200});
            if (GetRandomValue(0, 100) < 70) {
                SpawnParticle(back, Vector3Scale(p.direction, -3.0f),
                              Color{255, 120, 30, 220}, 0.35f, 0.08f, ParticleKind::Ember);
            }
        } else if (id == SpellId::Fireblast) {
            float flick = 0.85f + 0.15f * sinf((float)GetTime() * 22.0f);
            DrawSphere(t.position, p.radius * 1.35f * flick, Color{255, 50, 10, 120});
            DrawSphere(t.position, p.radius, Color{255, 160, 40, 255});
            DrawSphere(t.position, p.radius * 0.55f, Color{255, 240, 120, 255});
            DrawSphere(t.position, p.radius * 0.25f, WHITE);
            DrawSphereWires(t.position, p.radius + 0.15f, 8, 8, Color{255, 60, 10, 220});
            for (int k = 0; k < 3; k++) {
                float a = (float)GetTime() * 8.0f + k * 2.1f;
                Vector3 wing = {
                    t.position.x + cosf(a) * p.radius * 0.9f,
                    t.position.y + sinf(a * 1.3f) * p.radius * 0.4f,
                    t.position.z + sinf(a) * p.radius * 0.9f
                };
                DrawSphere(wing, p.radius * 0.22f, Color{255, 180, 50, 200});
            }
            Vector3 back = Vector3Subtract(t.position, Vector3Scale(p.direction, p.radius * 2.2f));
            DrawCylinderEx(back, t.position, p.radius * 0.7f, p.radius * 0.2f, 7,
                           Color{255, 90, 20, 210});
            if (GetRandomValue(0, 100) < 75) {
                SpawnParticle(back, Vector3Scale(p.direction, -4.0f),
                              Color{255, 100, 30, 200}, 0.35f, 0.1f, ParticleKind::Ember);
                SpawnParticle(t.position, {(frand01()-0.5f)*2, 1.0f, (frand01()-0.5f)*2},
                              Color{40, 30, 25, 100}, 0.5f, 0.25f, ParticleKind::Smoke);
            }
        } else if (id == SpellId::Ignite) {
            float flick = 0.8f + 0.2f * sinf((float)GetTime() * 25.0f);
            DrawSphere(t.position, p.radius * 1.1f * flick, Color{255, 40, 5, 140});
            DrawSphere(t.position, p.radius * 0.8f, Color{255, 80, 20, 255});
            DrawSphere(t.position, p.radius * 0.35f, YELLOW);
            DrawSphere(t.position, p.radius * 0.15f, Color{255, 255, 200, 255});
            if (GetRandomValue(0, 100) < 65) {
                SpawnParticle(t.position, Vector3Scale(p.direction, -2.5f),
                              Color{255, 140, 40, 220}, 0.3f, 0.07f, ParticleKind::Ember);
            }
        } else if (id == SpellId::Splash) {
            float pulse = 0.9f + 0.1f * sinf((float)GetTime() * 14.0f);
            DrawSphere(t.position, p.radius * 1.2f * pulse, Color{40, 120, 220, 100});
            DrawSphere(t.position, p.radius, Color{80, 170, 255, 230});
            DrawSphere(t.position, p.radius * 0.5f, Color{200, 240, 255, 240});
            DrawSphereWires(t.position, p.radius + 0.08f, 6, 6, Color{40, 100, 220, 200});
            for (int k = 0; k < 4; k++) {
                float a = (float)GetTime() * 6.0f + k * 1.57f;
                Vector3 drop = {
                    t.position.x + cosf(a) * p.radius * 0.85f,
                    t.position.y + sinf(a * 2.0f) * p.radius * 0.3f,
                    t.position.z + sinf(a) * p.radius * 0.85f
                };
                DrawSphere(drop, p.radius * 0.18f, Color{160, 220, 255, 200});
            }
            if (GetRandomValue(0, 100) < 55) {
                SpawnParticle(t.position, Vector3Scale(p.direction, -2.0f),
                              Color{120, 200, 255, 200}, 0.3f, 0.08f, ParticleKind::Droplet);
            }
        } else if (id == SpellId::Waterjet) {
            Vector3 tip = Vector3Add(t.position, Vector3Scale(p.direction, 0.7f));
            Vector3 mid = t.position;
            Vector3 tail = Vector3Subtract(t.position, Vector3Scale(p.direction, 1.2f));
            DrawCylinderEx(tail, mid, 0.18f, 0.12f, 8, Color{40, 120, 220, 200});
            DrawCylinderEx(mid, tip, 0.12f, 0.05f, 8, Color{100, 190, 255, 240});
            DrawSphere(tip, 0.14f, Color{200, 240, 255, 255});
            DrawSphere(mid, 0.1f, Color{160, 220, 255, 220});
            if (GetRandomValue(0, 100) < 80) {
                SpawnParticle(tail, Vector3Scale(p.direction, -2.0f),
                              Color{120, 200, 255, 180}, 0.3f, 0.08f, ParticleKind::Droplet);
                SpawnParticle(mid, {(frand01()-0.5f), 1.0f, (frand01()-0.5f)},
                              Color{180, 210, 230, 90}, 0.4f, 0.2f, ParticleKind::Mist);
            }
        } else {
            // Default fireball-like
            DrawSphere(t.position, p.radius, ORANGE);
            DrawSphere(t.position, p.radius * 0.45f, Color{255, 220, 80, 255});
            DrawSphereWires(t.position, p.radius + 0.05f, 6, 6, RED);
        }
    }

    for (size_t i = 0; i < reg.spellZones.data.size(); i++) {
        engine::ecs::Entity e = {reg.spellZones.indexToEntity[i]};
        if (!reg.transforms.Has(e)) continue;
        auto& z = reg.spellZones.data[i];
        auto& t = reg.transforms.Get(e);
        float lifeT = z.age / fmaxf(z.lifetime, 0.01f);
        float fade = 1.0f - lifeT;
        if (fade < 0.0f) fade = 0.0f;

        SpellId id = (SpellId)z.spellId;

        if (id == SpellId::Firewall && z.damages) {
            float pulse = 0.85f + 0.15f * sinf(z.age * 9.0f);
            DrawCircle3D({t.position.x, t.position.y + 0.06f, t.position.z}, z.radius,
                         {1, 0, 0}, 90.0f, Color{255, 60, 10, (unsigned char)(120 * fade)});
            DrawCylinder(t.position, z.radius * 0.85f, z.radius * 0.98f, 0.2f, 28,
                         Color{80, 20, 5, (unsigned char)(90 * fade)});
            const int segments = 32;
            for (int s = 0; s < segments; s++) {
                float a0 = (s / (float)segments) * 6.28318f + z.age * 0.85f;
                float wobble = 0.65f + 0.35f * sinf(z.age * 12.0f + s * 1.4f);
                Vector3 p0 = {t.position.x + cosf(a0) * z.radius, t.position.y,
                              t.position.z + sinf(a0) * z.radius};
                float h = z.height * pulse * wobble;
                DrawCylinderEx(p0, {p0.x, p0.y + h, p0.z}, 0.42f, 0.1f, 6,
                               Color{200, 40, 5, (unsigned char)(200 * fade)});
                DrawCylinderEx(p0, {p0.x, p0.y + h * 0.65f, p0.z}, 0.22f, 0.05f, 5,
                               Color{255, 180, 40, (unsigned char)(220 * fade)});
                DrawSphere({p0.x, p0.y + h, p0.z}, 0.18f,
                           Color{255, 230, 120, (unsigned char)(190 * fade)});
                if (s % 3 == 0) {
                    float ri = z.radius * 0.82f;
                    Vector3 pi = {t.position.x + cosf(a0) * ri, t.position.y,
                                  t.position.z + sinf(a0) * ri};
                    DrawCylinderEx(pi, {pi.x, pi.y + h * 0.45f, pi.z}, 0.25f, 0.06f, 5,
                                   Color{255, 100, 20, (unsigned char)(160 * fade)});
                }
            }
        } else if (id == SpellId::HellOnEarth && z.damages) {
            // ============================================================
            // Hell on Earth — infernal ring wall + central magma geyser
            // ============================================================
            float pulse = 0.88f + 0.12f * sinf(z.age * 8.0f);
            Vector3 eye = t.position;

            // Scorched ground disk
            DrawCylinder(eye, z.radius * 0.15f, z.radius * 0.98f, 0.35f, 36,
                         Color{60, 15, 5, (unsigned char)(100 * fade)});
            DrawCylinder(eye, 1.0f, z.radius * 0.45f, 0.55f, 28,
                         Color{120, 30, 5, (unsigned char)(120 * fade)});
            DrawCircle3D({eye.x, eye.y + 0.12f, eye.z}, z.radius,
                         {1, 0, 0}, 90.0f, Color{255, 40, 0, (unsigned char)(100 * fade)});
            DrawCircle3D({eye.x, eye.y + 0.28f, eye.z}, z.radius * 0.62f,
                         {1, 0, 0}, 90.0f, Color{255, 100, 20, (unsigned char)(80 * fade)});

            // Outer hellfire curtain — tall, uneven, rotating
            const int segments = 48;
            float ringH = fminf(7.5f, z.height * 0.32f);
            for (int s = 0; s < segments; s++) {
                float a0 = (s / (float)segments) * 6.28318f + z.age * 0.7f;
                float wobble = 0.65f + 0.35f * sinf(z.age * 11.0f + s * 1.3f);
                float r = z.radius * (0.92f + 0.06f * sinf(z.age * 3.0f + s));
                Vector3 p0 = {eye.x + cosf(a0) * r, eye.y, eye.z + sinf(a0) * r};
                float h = ringH * pulse * wobble;
                // Dark outer flame
                DrawCylinderEx(p0, {p0.x, p0.y + h, p0.z}, 1.15f, 0.28f, 6,
                               Color{180, 30, 5, (unsigned char)(190 * fade)});
                // Bright core tongue
                DrawCylinderEx(p0, {p0.x, p0.y + h * 0.7f, p0.z}, 0.55f, 0.1f, 5,
                               Color{255, 160, 30, (unsigned char)(210 * fade)});
                // White-hot tip
                DrawSphere({p0.x, p0.y + h, p0.z}, 0.45f,
                           Color{255, 230, 120, (unsigned char)(180 * fade)});
                // Secondary inner ring of shorter flames
                if (s % 2 == 0) {
                    float ri = z.radius * 0.72f;
                    Vector3 pi = {eye.x + cosf(a0 + 0.08f) * ri, eye.y, eye.z + sinf(a0 + 0.08f) * ri};
                    float hi = h * 0.55f;
                    DrawCylinderEx(pi, {pi.x, pi.y + hi, pi.z}, 0.7f, 0.15f, 5,
                                   Color{255, 70, 10, (unsigned char)(160 * fade)});
                }
            }

            // Central mega plume — layered eruption
            float surge = 0.75f + 0.25f * sinf(z.age * 9.5f);
            float ph = z.height * surge * (1.0f - lifeT * 0.2f) * pulse;
            // Magma stem (dark → bright)
            DrawCylinderEx(eye, {eye.x, eye.y + ph * 0.55f, eye.z}, 3.4f, 1.6f, 14,
                           Color{80, 15, 5, (unsigned char)(180 * fade)});
            DrawCylinderEx({eye.x, eye.y + ph * 0.2f, eye.z},
                           {eye.x, eye.y + ph * 0.85f, eye.z}, 2.2f, 0.9f, 12,
                           Color{255, 70, 10, (unsigned char)(200 * fade)});
            DrawCylinderEx({eye.x, eye.y + ph * 0.5f, eye.z},
                           {eye.x, eye.y + ph, eye.z}, 1.2f, 0.35f, 10,
                           Color{255, 200, 50, (unsigned char)(220 * fade)});
            // Cap / mushroom of fire
            DrawSphere({eye.x, eye.y + ph, eye.z}, 3.2f,
                       Color{255, 90, 15, (unsigned char)(150 * fade)});
            DrawSphere({eye.x, eye.y + ph * 1.05f, eye.z}, 1.8f,
                       Color{255, 220, 80, (unsigned char)(180 * fade)});
            DrawSphere({eye.x, eye.y + ph * 1.08f, eye.z}, 0.9f,
                       Color{255, 255, 200, (unsigned char)(200 * fade)});

            // Ejected lava arcs from the plume
            const int blobs = 20;
            for (int s = 0; s < blobs; s++) {
                float a = z.age * 1.8f + s * (6.28318f / blobs);
                float arcT = fmodf(z.age * 0.7f + s * 0.17f, 1.0f);
                float rr = z.radius * (0.12f + arcT * 0.42f);
                float yy = eye.y + ph * (0.5f + 0.5f * sinf(arcT * 3.14159f));
                Vector3 bp = {eye.x + cosf(a) * rr, yy, eye.z + sinf(a) * rr};
                float br = 0.5f + 0.4f * (1.0f - arcT);
                DrawSphere(bp, br, Color{255, (unsigned char)(70 + s * 6), 15,
                                         (unsigned char)(185 * fade)});
            }

            // Rising smoke pillars around the ring
            for (int s = 0; s < 10; s++) {
                float a = z.age * 0.4f + s * (6.28318f / 10.0f);
                float r = z.radius * (0.55f + 0.2f * ((s % 3) / 2.0f));
                Vector3 base = {eye.x + cosf(a) * r, eye.y + 0.5f, eye.z + sinf(a) * r};
                float sh = 4.0f + 3.0f * sinf(z.age * 2.0f + s);
                DrawCylinderEx(base, {base.x, base.y + sh, base.z}, 1.2f, 2.0f, 6,
                               Color{35, 25, 20, (unsigned char)(55 * fade)});
            }

            // Extra mid-radius fire tongues
            for (int s = 0; s < 16; s++) {
                float a = -z.age * 1.1f + s * (6.28318f / 16.0f);
                float r = z.radius * 0.4f;
                Vector3 p0 = {eye.x + cosf(a) * r, eye.y, eye.z + sinf(a) * r};
                float hh = 3.5f * pulse * (0.6f + 0.4f * sinf(z.age * 10.0f + s));
                DrawCylinderEx(p0, {p0.x, p0.y + hh, p0.z}, 0.6f, 0.12f, 5,
                               Color{255, 90, 15, (unsigned char)(150 * fade)});
            }

            // Heat haze glow
            DrawSphere({eye.x, eye.y + ph * 0.4f, eye.z}, z.radius * 0.28f,
                       Color{255, 80, 10, (unsigned char)(28 * fade)});
            DrawSphere({eye.x, eye.y + 1.0f, eye.z}, z.radius * 0.5f,
                       Color{255, 40, 5, (unsigned char)(18 * fade)});
        } else if (id == SpellId::LavaPlume && z.damages) {
            // ============================================================
            // Lava Plume — erupting magma column with falling spatters
            // ============================================================
            float pulse = 0.82f + 0.18f * sinf(z.age * 12.0f);
            float surge = 0.7f + 0.3f * sinf(z.age * 7.0f);
            float h = z.height * (1.0f - lifeT * 0.3f) * pulse * surge;
            Vector3 eye = t.position;

            // Crater / glowing pool
            DrawCylinder(eye, 0.6f, z.radius * 0.95f, 0.4f, 20,
                         Color{70, 15, 5, (unsigned char)(130 * fade)});
            DrawCylinder(eye, 0.4f, z.radius * 0.55f, 0.55f, 16,
                         Color{180, 40, 5, (unsigned char)(150 * fade)});
            DrawCircle3D({eye.x, eye.y + 0.1f, eye.z}, z.radius,
                         {1, 0, 0}, 90.0f, Color{255, 80, 15, (unsigned char)(110 * fade)});
            DrawCircle3D({eye.x, eye.y + 0.22f, eye.z}, z.radius * 0.5f,
                         {1, 0, 0}, 90.0f, Color{255, 160, 30, (unsigned char)(90 * fade)});

            // Layered eruption column
            DrawCylinderEx(eye, {eye.x, eye.y + h * 0.4f, eye.z}, 2.0f, 1.2f, 12,
                           Color{90, 20, 5, (unsigned char)(190 * fade)});
            DrawCylinderEx({eye.x, eye.y + h * 0.15f, eye.z},
                           {eye.x, eye.y + h * 0.75f, eye.z}, 1.5f, 0.7f, 12,
                           Color{255, 70, 10, (unsigned char)(200 * fade)});
            DrawCylinderEx({eye.x, eye.y + h * 0.45f, eye.z},
                           {eye.x, eye.y + h, eye.z}, 0.85f, 0.28f, 10,
                           Color{255, 190, 40, (unsigned char)(220 * fade)});
            // Hot tip bloom
            DrawSphere({eye.x, eye.y + h, eye.z}, 1.8f,
                       Color{255, 110, 20, (unsigned char)(170 * fade)});
            DrawSphere({eye.x, eye.y + h * 1.05f, eye.z}, 1.0f,
                       Color{255, 220, 80, (unsigned char)(190 * fade)});
            DrawSphere({eye.x, eye.y + h * 1.08f, eye.z}, 0.45f,
                       Color{255, 255, 210, (unsigned char)(210 * fade)});

            // Twisting side jets
            for (int s = 0; s < 10; s++) {
                float a = z.age * 2.8f + s * (6.28318f / 10.0f);
                float jh = h * (0.4f + 0.35f * sinf(z.age * 9.0f + s));
                float out = 1.0f + 0.8f * ((s % 3) / 2.0f);
                Vector3 base = {eye.x + cosf(a) * 1.1f, eye.y + 0.2f, eye.z + sinf(a) * 1.1f};
                Vector3 tip = {eye.x + cosf(a) * (1.8f + out), eye.y + jh, eye.z + sinf(a) * (1.8f + out)};
                DrawCylinderEx(base, tip, 0.4f, 0.08f, 5,
                               Color{255, 100, 20, (unsigned char)(175 * fade)});
                DrawSphere(tip, 0.22f, Color{255, 200, 60, (unsigned char)(180 * fade)});
            }

            // Falling / orbiting lava spatters
            for (int s = 0; s < 22; s++) {
                float a = z.age * 2.4f + s * (6.28318f / 22.0f);
                float arcT = fmodf(z.age * 0.95f + s * 0.11f, 1.0f);
                float rr = z.radius * (0.2f + arcT * 0.75f);
                float yy = eye.y + h * (0.9f * sinf(arcT * 3.14159f)) + 0.25f;
                Vector3 p = {eye.x + cosf(a) * rr, yy, eye.z + sinf(a) * rr};
                float br = 0.2f + 0.28f * (1.0f - arcT);
                DrawSphere(p, br, Color{255, (unsigned char)(60 + (s % 6) * 22), 12,
                                        (unsigned char)(175 * fade)});
            }

            // Ground crack glow spokes
            for (int s = 0; s < 8; s++) {
                float a = s * (6.28318f / 8.0f) + z.age * 0.2f;
                Vector3 tip = {eye.x + cosf(a) * z.radius * 0.95f, eye.y + 0.15f,
                               eye.z + sinf(a) * z.radius * 0.95f};
                DrawCylinderEx({eye.x, eye.y + 0.12f, eye.z}, tip, 0.18f, 0.05f, 4,
                               Color{255, 80, 10, (unsigned char)(110 * fade)});
            }

            // Soft heat glow
            DrawSphere({eye.x, eye.y + h * 0.35f, eye.z}, z.radius * 0.6f,
                       Color{255, 60, 10, (unsigned char)(32 * fade)});
            DrawSphere({eye.x, eye.y + h * 0.7f, eye.z}, 2.5f,
                       Color{255, 140, 30, (unsigned char)(40 * fade)});
        } else if (id == SpellId::Waterwall && z.damages) {
            float pulse = 0.85f + 0.15f * sinf(z.age * 7.0f);
            DrawCylinder(t.position, z.radius * 0.8f, z.radius * 0.98f, 0.25f, 28,
                         Color{20, 60, 140, (unsigned char)(70 * fade)});
            DrawCircle3D({t.position.x, t.position.y + 0.08f, t.position.z}, z.radius,
                         {1, 0, 0}, 90.0f, Color{80, 160, 255, (unsigned char)(130 * fade)});
            const int segments = 36;
            for (int s = 0; s < segments; s++) {
                float a0 = (s / (float)segments) * 6.28318f + z.age * 1.1f;
                float wobble = 0.7f + 0.3f * sinf(z.age * 10.0f + s * 0.8f);
                Vector3 p0 = {t.position.x + cosf(a0) * z.radius, t.position.y,
                              t.position.z + sinf(a0) * z.radius};
                float hh = z.height * pulse * wobble;
                DrawCylinderEx(p0, {p0.x, p0.y + hh, p0.z}, 0.4f, 0.1f, 6,
                               Color{40, 120, 220, (unsigned char)(185 * fade)});
                DrawCylinderEx(p0, {p0.x, p0.y + hh * 0.6f, p0.z}, 0.2f, 0.05f, 5,
                               Color{180, 230, 255, (unsigned char)(210 * fade)});
                DrawSphere({p0.x, p0.y + hh, p0.z}, 0.2f,
                           Color{220, 245, 255, (unsigned char)(180 * fade)});
                if (s % 2 == 0) {
                    float ri = z.radius * 0.78f;
                    Vector3 pi = {t.position.x + cosf(a0 + 0.05f) * ri, t.position.y,
                                  t.position.z + sinf(a0 + 0.05f) * ri};
                    DrawCylinderEx(pi, {pi.x, pi.y + hh * 0.5f, pi.z}, 0.22f, 0.06f, 5,
                                   Color{100, 180, 255, (unsigned char)(150 * fade)});
                }
            }
        } else if (id == SpellId::Whirlpool && z.damages) {
            float spin = z.age * 4.2f;
            float pulse = 0.9f + 0.1f * sinf(z.age * 6.0f);
            DrawCylinder(t.position, 0.3f, z.radius * 0.95f, 0.35f, 28,
                         Color{20, 60, 130, (unsigned char)(90 * fade)});
            for (int ring = 1; ring <= 5; ring++) {
                float r = z.radius * (ring / 5.0f) * pulse;
                DrawCircle3D({t.position.x, t.position.y + 0.1f * ring, t.position.z}, r,
                             {1, 0, 0}, 90.0f,
                             Color{50, 140, 230, (unsigned char)((130 - ring * 15) * fade)});
            }
            // Spiraling droplets climbing inward
            for (int s = 0; s < 20; s++) {
                float a = spin + s * (6.28318f / 20.0f);
                float u = (s % 5) / 4.0f;
                float r = z.radius * (0.95f - u * 0.75f);
                Vector3 p = {t.position.x + cosf(a) * r,
                             t.position.y + 0.2f + u * 1.8f,
                             t.position.z + sinf(a) * r};
                DrawSphere(p, 0.2f + (1.0f - u) * 0.12f,
                           Color{120, 200, 255, (unsigned char)(190 * fade)});
            }
            // Funnel ribbons
            for (int s = 0; s < 5; s++) {
                float baseA = spin * 0.8f + s * (6.28318f / 5.0f);
                for (int seg = 0; seg < 8; seg++) {
                    float u0 = seg / 8.0f, u1 = (seg + 1) / 8.0f;
                    float a0 = baseA + u0 * 3.0f, a1 = baseA + u1 * 3.0f;
                    float r0 = z.radius * (0.9f - u0 * 0.7f);
                    float r1 = z.radius * (0.9f - u1 * 0.7f);
                    Vector3 p0 = {t.position.x + cosf(a0) * r0, t.position.y + u0 * 1.6f,
                                  t.position.z + sinf(a0) * r0};
                    Vector3 p1 = {t.position.x + cosf(a1) * r1, t.position.y + u1 * 1.6f,
                                  t.position.z + sinf(a1) * r1};
                    DrawCylinderEx(p0, p1, 0.18f, 0.1f, 4,
                                   Color{90, 170, 240, (unsigned char)(140 * fade)});
                }
            }
            DrawSphere({t.position.x, t.position.y + 0.3f, t.position.z}, 0.6f,
                       Color{10, 40, 90, (unsigned char)(120 * fade)});
        } else if (id == SpellId::Hurricane && z.damages) {
            float spin = z.age * 4.5f;
            float pulse = 0.92f + 0.08f * sinf(z.age * 5.0f);
            DrawCylinder(t.position, z.radius * 0.12f, z.radius, z.height * 0.15f, 28,
                         Color{90, 130, 180, (unsigned char)(50 * fade)});
            // Stacked swirling bands
            for (int band = 0; band < 4; band++) {
                float yh = t.position.y + z.height * (0.15f + band * 0.2f);
                float rr = z.radius * (0.85f - band * 0.12f) * pulse;
                DrawCircle3D({t.position.x, yh, t.position.z}, rr, {1, 0, 0}, 90.0f,
                             Color{100, 170, 230, (unsigned char)((100 - band * 15) * fade)});
            }
            int arms = 22;
            for (int s = 0; s < arms; s++) {
                float a = spin + s * (6.28318f / (float)arms);
                float r = z.radius * (0.3f + 0.6f * sinf(z.age * 1.8f + s * 0.35f));
                if (r < 1.5f) r = 1.5f;
                Vector3 p0 = {t.position.x + cosf(a) * r, t.position.y + 0.4f,
                              t.position.z + sinf(a) * r};
                Vector3 p1 = {t.position.x + cosf(a + 0.4f) * (r * 0.55f),
                              t.position.y + z.height * 0.85f,
                              t.position.z + sinf(a + 0.4f) * (r * 0.55f)};
                DrawCylinderEx(p0, p1, 0.35f, 0.1f, 5,
                               Color{150, 200, 240, (unsigned char)(145 * fade)});
                if (s % 2 == 0) DrawSphere(p1, 0.25f, Color{200, 230, 255, (unsigned char)(150 * fade)});
            }
            // Outer rain curtain
            for (int s = 0; s < 14; s++) {
                float a = -spin * 0.5f + s * (6.28318f / 14.0f);
                Vector3 base = {t.position.x + cosf(a) * z.radius * 0.9f, t.position.y,
                                t.position.z + sinf(a) * z.radius * 0.9f};
                float hh = z.height * 0.4f * (0.7f + 0.3f * sinf(z.age * 8.0f + s));
                DrawCylinderEx(base, {base.x, base.y + hh, base.z}, 0.2f, 0.05f, 4,
                               Color{80, 150, 220, (unsigned char)(120 * fade)});
            }
            DrawCircle3D({t.position.x, t.position.y + 0.25f, t.position.z}, z.radius,
                         {1, 0, 0}, 90.0f, Color{80, 150, 255, (unsigned char)(100 * fade)});
        } else if (id == SpellId::Maelstrom && z.damages) {
            // ============================================================
            // Maelstrom — towering spinning vortex funnel + storm wall
            // ============================================================
            float spin = z.age * 2.8f;
            float pulse = 0.92f + 0.08f * sinf(z.age * 5.5f);
            Vector3 eye = {t.position.x, t.position.y, t.position.z};

            // Dark ocean disk / churning base
            DrawCylinder(eye, z.radius * 0.08f, z.radius * 0.98f, 0.55f * pulse, 36,
                         Color{15, 45, 110, (unsigned char)(90 * fade)});
            DrawCylinder(eye, z.radius * 0.05f, z.radius * 0.55f, 0.35f, 28,
                         Color{10, 30, 80, (unsigned char)(110 * fade)});

            // Funnel layers — radius shrinks as height rises (vortex silhouette)
            const int layers = 10;
            for (int L = 0; L < layers; L++) {
                float t0 = L / (float)layers;
                float t1 = (L + 1) / (float)layers;
                // Wider at bottom, narrow eye at top
                float r0 = z.radius * (0.92f - t0 * 0.78f) * pulse;
                float r1 = z.radius * (0.92f - t1 * 0.78f) * pulse;
                float y0 = eye.y + z.height * t0 * 0.95f;
                float y1 = eye.y + z.height * t1 * 0.95f;
                unsigned char a = (unsigned char)((55 + L * 8) * fade);
                Color band = Color{(unsigned char)(30 + L * 8), (unsigned char)(90 + L * 10),
                                   (unsigned char)(180 + L * 4), a};
                DrawCylinderEx({eye.x, y0, eye.z}, {eye.x, y1, eye.z}, r0, r1, 28, band);
            }

            // Hollow dark eye column
            DrawCylinderEx({eye.x, eye.y + 0.2f, eye.z},
                           {eye.x, eye.y + z.height * 0.85f, eye.z},
                           z.radius * 0.12f, z.radius * 0.04f, 16,
                           Color{5, 15, 40, (unsigned char)(100 * fade)});

            // Spiraling water ribbons climbing the funnel
            const int ribbons = 8;
            for (int s = 0; s < ribbons; s++) {
                float baseA = spin * (1.0f + 0.08f * s) + s * (6.28318f / ribbons);
                for (int seg = 0; seg < 14; seg++) {
                    float u0 = seg / 14.0f;
                    float u1 = (seg + 1) / 14.0f;
                    float a0 = baseA + u0 * 4.5f;
                    float a1 = baseA + u1 * 4.5f;
                    float r0 = z.radius * (0.88f - u0 * 0.72f) * pulse;
                    float r1 = z.radius * (0.88f - u1 * 0.72f) * pulse;
                    Vector3 p0 = {eye.x + cosf(a0) * r0, eye.y + z.height * u0 * 0.92f,
                                  eye.z + sinf(a0) * r0};
                    Vector3 p1 = {eye.x + cosf(a1) * r1, eye.y + z.height * u1 * 0.92f,
                                  eye.z + sinf(a1) * r1};
                    float thick = 0.9f - u0 * 0.55f;
                    DrawCylinderEx(p0, p1, thick, thick * 0.7f, 5,
                                   Color{160, 210, 255, (unsigned char)((150 - seg * 4) * fade)});
                    if (seg % 3 == 0) {
                        DrawSphere(p0, thick * 0.55f,
                                   Color{220, 240, 255, (unsigned char)(120 * fade)});
                    }
                }
            }

            // Outer storm wall — rotating vertical geysers
            const int wall = 20;
            for (int s = 0; s < wall; s++) {
                float a = spin * 0.55f + s * (6.28318f / wall);
                float wobble = 0.85f + 0.15f * sinf(z.age * 7.0f + s);
                float r = z.radius * (0.82f + 0.12f * sinf(z.age * 2.0f + s * 0.7f));
                Vector3 base = {eye.x + cosf(a) * r, eye.y, eye.z + sinf(a) * r};
                float h = z.height * 0.35f * wobble * (0.7f + 0.3f * ((s % 4) / 3.0f));
                DrawCylinderEx(base, {base.x, base.y + h, base.z}, 1.1f, 0.25f, 6,
                               Color{50, 130, 220, (unsigned char)(140 * fade)});
                DrawCylinderEx(base, {base.x, base.y + h * 0.55f, base.z}, 0.45f, 0.1f, 5,
                               Color{180, 225, 255, (unsigned char)(160 * fade)});
            }

            // Horizontal spray rings at mid / upper heights
            for (int ring = 0; ring < 4; ring++) {
                float yh = eye.y + z.height * (0.2f + ring * 0.2f);
                float rr = z.radius * (0.75f - ring * 0.14f) * pulse;
                DrawCircle3D({eye.x, yh, eye.z}, rr, {1, 0, 0}, 90.0f,
                             Color{100, 180, 255, (unsigned char)((90 - ring * 12) * fade)});
            }

            // Bright foam at the lip
            DrawCircle3D({eye.x, eye.y + z.height * 0.92f, eye.z}, z.radius * 0.18f * pulse,
                         {1, 0, 0}, 90.0f, Color{230, 245, 255, (unsigned char)(160 * fade)});
            DrawSphere({eye.x, eye.y + z.height * 0.95f, eye.z}, 2.2f,
                       Color{200, 230, 255, (unsigned char)(70 * fade)});
        } else if ((id == SpellId::Surge || id == SpellId::Tsunami) && z.damages) {
            // ============================================================
            // Surge / Tsunami — continuous breaking wall of water
            // ============================================================
            Vector3 md = Vector3Normalize(Vector3{z.moveDirX, 0, z.moveDirZ});
            Vector3 right = {-md.z, 0, md.x};
            float hw = z.waveHalfWidth;
            float hd = z.radius;
            Vector3 c = t.position;
            bool mega = (id == SpellId::Tsunami);
            float pulse = 0.9f + 0.1f * sinf(z.age * 6.0f);

            Color deep  = Color{20, 70, 160, (unsigned char)((mega ? 170 : 140) * fade)};
            Color mid   = Color{40, 130, 220, (unsigned char)((mega ? 190 : 160) * fade)};
            Color foam  = Color{230, 245, 255, (unsigned char)((mega ? 230 : 200) * fade)};
            Color spray = Color{180, 215, 245, (unsigned char)((mega ? 120 : 90) * fade)};

            int slices = mega ? 36 : 16;
            float wallThick = mega ? hd * 1.1f : hd * 0.9f;
            float crestLean = mega ? hd * 0.85f : hd * 0.45f; // foam tips lean forward

            for (int s = 0; s < slices; s++) {
                float u = (slices <= 1) ? 0.0f : (s / (float)(slices - 1)) * 2.0f - 1.0f;
                // Undulating crest along the width
                float undulate = 0.72f + 0.28f * sinf(u * 3.2f + z.age * 5.0f);
                float edgeTaper = 1.0f - fabsf(u) * (mega ? 0.25f : 0.35f);
                float h = z.height * undulate * edgeTaper * pulse;

                Vector3 base = Vector3Add(c, Vector3Scale(right, u * hw));
                base = Vector3Add(base, Vector3Scale(md, -hd * 0.15f));
                base.y = engine::math::WorldHeight(base.x, base.z) + 0.05f;

                // Deep body of the wave (rear → front bulk)
                Vector3 rear = Vector3Subtract(base, Vector3Scale(md, wallThick * 0.55f));
                rear.y = engine::math::WorldHeight(rear.x, rear.z) + 0.05f;
                Vector3 face = Vector3Add(base, Vector3Scale(md, wallThick * 0.25f));
                face.y = base.y;

                float bodyR = mega ? 2.2f : 0.85f;
                // Rear churn
                DrawCylinderEx(rear, {rear.x, rear.y + h * 0.55f, rear.z},
                               bodyR * 1.3f, bodyR * 0.5f, 6, deep);
                // Main face wall
                DrawCylinderEx(face, {face.x, face.y + h, face.z},
                               bodyR, bodyR * 0.35f, 7, mid);
                // Forward-leaning crest / breakers
                Vector3 tip = Vector3Add({face.x, face.y + h, face.z},
                                         Vector3Scale(md, crestLean * undulate));
                DrawCylinderEx({face.x, face.y + h * 0.75f, face.z}, tip,
                               bodyR * 0.55f, bodyR * 0.12f, 6, foam);
                DrawSphere(tip, mega ? 1.1f : 0.4f, foam);

                // Spray curls above the crest
                if (s % (mega ? 2 : 3) == 0) {
                    Vector3 curl = Vector3Add(tip, Vector3Scale(md, crestLean * 0.25f));
                    curl.y += h * 0.08f;
                    DrawSphere(curl, mega ? 0.7f : 0.28f, spray);
                }
            }

            // Continuous foam lip along the crest (connecting spheres)
            int lip = mega ? 40 : 14;
            for (int s = 0; s < lip; s++) {
                float u = (lip <= 1) ? 0.0f : (s / (float)(lip - 1)) * 2.0f - 1.0f;
                float undulate = 0.72f + 0.28f * sinf(u * 3.2f + z.age * 5.0f);
                float edgeTaper = 1.0f - fabsf(u) * 0.25f;
                float h = z.height * undulate * edgeTaper * pulse;
                Vector3 base = Vector3Add(c, Vector3Scale(right, u * hw));
                base = Vector3Add(base, Vector3Scale(md, wallThick * 0.25f));
                base.y = engine::math::WorldHeight(base.x, base.z) + 0.05f;
                Vector3 tip = Vector3Add({base.x, base.y + h, base.z},
                                         Vector3Scale(md, crestLean * undulate));
                DrawSphere(tip, mega ? 0.85f : 0.32f, foam);
            }

            // Base surge sheet under the wave
            Vector3 sheetC = Vector3Add(c, Vector3Scale(md, hd * 0.1f));
            sheetC.y = engine::math::WorldHeight(sheetC.x, sheetC.z) + 0.12f;
            DrawCylinder(sheetC, mega ? 3.0f : 1.0f, hw * 0.95f, mega ? 0.8f : 0.35f, 24,
                         Color{30, 90, 180, (unsigned char)(80 * fade)});

            // Mist curtain behind the wall
            if (mega) {
                Vector3 mist = Vector3Subtract(c, Vector3Scale(md, hd * 0.7f));
                mist.y = engine::math::WorldHeight(mist.x, mist.z) + z.height * 0.35f;
                DrawSphere(mist, hw * 0.35f, Color{160, 200, 230, (unsigned char)(35 * fade)});
                DrawSphere(Vector3Add(mist, Vector3Scale(right, hw * 0.4f)), hw * 0.22f,
                           Color{160, 200, 230, (unsigned char)(28 * fade)});
                DrawSphere(Vector3Subtract(mist, Vector3Scale(right, hw * 0.4f)), hw * 0.22f,
                           Color{160, 200, 230, (unsigned char)(28 * fade)});
            }
        } else if (id == SpellId::CallOfTheDead && z.damages) {
            float pulse = 0.75f + 0.25f * sinf(z.age * 7.0f);
            DrawCircle3D({t.position.x, t.position.y + 0.05f, t.position.z}, z.radius,
                         {1, 0, 0}, 90.0f, Color{80, 20, 100, (unsigned char)(120 * fade)});
            // Undead arms reaching up from the ground
            const int hands = 14;
            for (int s = 0; s < hands; s++) {
                float a = (s / (float)hands) * 6.28318f + z.age * 0.4f;
                float r = z.radius * (0.25f + 0.7f * ((s % 5) / 4.0f));
                Vector3 base = {
                    t.position.x + cosf(a) * r,
                    t.position.y,
                    t.position.z + sinf(a) * r
                };
                base.y = engine::math::WorldHeight(base.x, base.z);
                float h = z.height * pulse * (0.55f + 0.45f * sinf(z.age * 9.0f + s));
                Color bone = Color{160, 150, 140, (unsigned char)(200 * fade)};
                Color dark = Color{60, 20, 80, (unsigned char)(180 * fade)};
                DrawCylinderEx(base, {base.x, base.y + h, base.z}, 0.18f, 0.08f, 5, bone);
                DrawSphere({base.x, base.y + h, base.z}, 0.22f, dark);
                // Clawed fingers
                for (int f = 0; f < 3; f++) {
                    float fa = a + (f - 1) * 0.35f;
                    Vector3 tip = {
                        base.x + cosf(fa) * 0.35f,
                        base.y + h + 0.15f,
                        base.z + sinf(fa) * 0.35f
                    };
                    DrawCylinderEx({base.x, base.y + h, base.z}, tip, 0.05f, 0.02f, 3, bone);
                }
            }
        } else if (id == SpellId::Geyser && !z.damages) {
            float pulse = 0.8f + 0.2f * sinf(z.age * 14.0f);
            float surge = 0.75f + 0.25f * sinf(z.age * 8.0f);
            float h = z.height * (1.0f - lifeT * 0.45f) * pulse * surge;
            Vector3 eye = t.position;
            DrawCylinder(eye, 0.5f, z.radius > 0.1f ? z.radius * 0.4f : 2.0f, 0.35f, 16,
                         Color{30, 80, 160, (unsigned char)(100 * fade)});
            DrawCylinderEx(eye, {eye.x, eye.y + h * 0.4f, eye.z}, 1.3f, 0.7f, 12,
                           Color{40, 110, 200, (unsigned char)(170 * fade)});
            DrawCylinderEx({eye.x, eye.y + h * 0.25f, eye.z},
                           {eye.x, eye.y + h * 0.85f, eye.z}, 0.85f, 0.35f, 10,
                           Color{100, 180, 255, (unsigned char)(190 * fade)});
            DrawCylinderEx({eye.x, eye.y + h * 0.55f, eye.z},
                           {eye.x, eye.y + h, eye.z}, 0.45f, 0.12f, 8,
                           Color{200, 235, 255, (unsigned char)(200 * fade)});
            DrawSphere({eye.x, eye.y + h, eye.z}, 1.3f,
                       Color{180, 230, 255, (unsigned char)(160 * fade)});
            DrawSphere({eye.x, eye.y + h * 1.05f, eye.z}, 0.6f,
                       Color{230, 250, 255, (unsigned char)(180 * fade)});
            for (int s = 0; s < 10; s++) {
                float a = z.age * 3.0f + s * (6.28318f / 10.0f);
                float arcT = fmodf(z.age * 1.1f + s * 0.1f, 1.0f);
                float rr = 0.8f + arcT * 2.5f;
                Vector3 p = {eye.x + cosf(a) * rr,
                             eye.y + h * sinf(arcT * 3.14159f),
                             eye.z + sinf(a) * rr};
                DrawSphere(p, 0.18f * (1.0f - arcT * 0.5f),
                           Color{160, 220, 255, (unsigned char)(170 * fade)});
            }
        } else {
            // Expanding blast rings (SuperNova / Inferno / water AoE)
            float alpha = fade;
            bool water = GetSpellDef(id).element == SpellElement::Water;
            bool inferno = (id == SpellId::Inferno);
            bool nova = (id == SpellId::SuperNova);
            Color ringA = water ? Color{80, 170, 255, (unsigned char)(200 * alpha)}
                                : Color{255, 140, 30, (unsigned char)(200 * alpha)};
            Color ringB = water ? Color{40, 100, 220, (unsigned char)(160 * alpha)}
                                : Color{255, 60, 10, (unsigned char)(160 * alpha)};
            Color core  = water ? Color{200, 240, 255, (unsigned char)(180 * alpha)}
                                : Color{255, 220, 80, (unsigned char)(180 * alpha)};

            DrawCircle3D({t.position.x, t.position.y + 0.15f, t.position.z}, z.radius,
                         {1, 0, 0}, 90.0f, ringA);
            DrawCircle3D({t.position.x, t.position.y + 0.35f, t.position.z}, z.radius * 0.78f,
                         {1, 0, 0}, 90.0f, ringB);
            DrawCircle3D({t.position.x, t.position.y + 0.55f, t.position.z}, z.radius * 0.55f,
                         {1, 0, 0}, 90.0f, core);

            int spokes = inferno ? 18 : (nova ? 14 : 10);
            for (int s = 0; s < spokes; s++) {
                float a = s * (6.28318f / spokes) + z.age * (inferno ? 2.0f : 1.2f);
                Vector3 tip = {t.position.x + cosf(a) * z.radius,
                               t.position.y + 0.2f + (inferno ? 0.8f : 0.3f) * sinf(z.age * 8.0f + s),
                               t.position.z + sinf(a) * z.radius};
                if (water) {
                    DrawCylinderEx(t.position, tip, 0.2f, 0.05f, 4,
                                   Color{60, 150, 230, (unsigned char)(120 * alpha)});
                    DrawSphere(tip, 0.2f, Color{180, 230, 255, (unsigned char)(150 * alpha)});
                } else {
                    float fh = inferno ? (1.8f + 1.2f * sinf(z.age * 10.0f + s)) : 0.9f;
                    DrawCylinderEx(tip, {tip.x, tip.y + fh, tip.z}, 0.35f, 0.08f, 5,
                                   Color{255, 90, 20, (unsigned char)(160 * alpha)});
                    DrawSphere({tip.x, tip.y + fh, tip.z}, 0.22f,
                               Color{255, 220, 80, (unsigned char)(170 * alpha)});
                }
            }

            if (nova || inferno || id == SpellId::HellOnEarth) {
                DrawCylinder(t.position, z.radius * 0.9f, z.radius, inferno ? 0.35f : 0.12f, 28,
                             Color{255, 120, 30, (unsigned char)(80 * alpha)});
                float coreR = fmaxf(0.5f, z.radiusMax * (inferno ? 0.18f : 0.12f) * (1.0f - lifeT));
                DrawSphere(t.position, coreR, core);
                DrawSphere(t.position, coreR * 0.45f, Color{255, 255, 200, (unsigned char)(200 * alpha)});
            }
            if (inferno || (id == SpellId::HellOnEarth && !z.damages)) {
                DrawCircle3D({t.position.x, t.position.y + 0.6f, t.position.z}, z.radius * 0.4f,
                             {1, 0, 0}, 90.0f, Color{255, 40, 10, (unsigned char)(150 * alpha)});
                DrawSphere(t.position, 1.5f * (1.0f - lifeT * 0.6f),
                           Color{255, 80, 15, (unsigned char)(140 * alpha)});
                for (int s = 0; s < 8; s++) {
                    float a = z.age * 3.0f + s * 0.785f;
                    Vector3 p = {t.position.x + cosf(a) * z.radius * 0.35f,
                                 t.position.y + 1.0f + sinf(z.age * 6.0f + s),
                                 t.position.z + sinf(a) * z.radius * 0.35f};
                    DrawSphere(p, 0.35f, Color{255, 160, 40, (unsigned char)(150 * alpha)});
                }
            }
        }
    }

    // Ultimate summon intros: hell rift rise / heavenly light beam descent
    for (size_t i = 0; i < reg.summons.data.size(); i++) {
        engine::ecs::Entity e = {reg.summons.indexToEntity[i]};
        if (!reg.transforms.Has(e)) continue;
        const auto& s = reg.summons.data[i];
        if (s.spawnAnimDuration <= 0.0f) continue;

        float introT = s.age / s.spawnAnimDuration;
        float fade = 1.0f;
        if (introT > 1.0f) {
            fade = 1.0f - (introT - 1.0f) / 0.55f; // linger briefly after landing
            if (fade <= 0.0f) continue;
            introT = 1.0f;
        }
        Vector3 ground = {s.spawnHomeX, s.spawnGroundY, s.spawnHomeZ};
        auto& t = reg.transforms.Get(e);

        if (s.kind == game::SummonKind::Reaper) {
            float pulse = 0.85f + 0.15f * sinf(s.age * 10.0f);
            float crackR = 2.2f + 2.8f * Smoothstep01(introT);
            // Hell portal disk
            DrawCylinder(ground, crackR * 0.2f, crackR, 0.12f, 20,
                         Color{40, 5, 10, (unsigned char)(160 * fade)});
            DrawCircle3D({ground.x, ground.y + 0.08f, ground.z}, crackR,
                         {1, 0, 0}, 90.0f, Color{255, 40, 10, (unsigned char)(180 * fade)});
            DrawCircle3D({ground.x, ground.y + 0.18f, ground.z}, crackR * 0.55f,
                         {1, 0, 0}, 90.0f, Color{255, 120, 20, (unsigned char)(140 * fade)});
            // Rising hellfire columns around the fissure
            const int jets = 10;
            for (int j = 0; j < jets; j++) {
                float a = (j / (float)jets) * 6.28318f + s.age * 1.2f;
                float r = crackR * (0.35f + 0.55f * ((j % 3) / 2.0f));
                Vector3 base = {ground.x + cosf(a) * r, ground.y, ground.z + sinf(a) * r};
                float h = (1.5f + 2.5f * (1.0f - introT * 0.4f)) * pulse * (0.7f + 0.3f * sinf(s.age * 9.0f + j));
                DrawCylinderEx(base, {base.x, base.y + h, base.z}, 0.35f, 0.08f, 6,
                               Color{255, 70, 15, (unsigned char)(190 * fade)});
                DrawCylinderEx(base, {base.x, base.y + h * 0.6f, base.z}, 0.16f, 0.04f, 5,
                               Color{255, 180, 40, (unsigned char)(210 * fade)});
            }
            // Dark smoke veil clinging to the rising body
            DrawSphere({t.position.x, t.position.y - s.bodyHeight * 0.15f, t.position.z},
                       2.2f, Color{20, 5, 25, (unsigned char)(90 * fade * (1.0f - introT * 0.5f))});
        } else if (s.kind == game::SummonKind::ArchAngel) {
            float beamTop = s.spawnGroundY + 55.0f;
            float beamBot = s.spawnGroundY;
            float pulse = 0.9f + 0.1f * sinf(s.age * 8.0f);
            // Outer soft beam
            DrawCylinderEx({ground.x, beamBot, ground.z}, {ground.x, beamTop, ground.z},
                           2.8f * pulse, 3.4f * pulse, 16,
                           Color{255, 245, 200, (unsigned char)(55 * fade)});
            // Bright core beam
            DrawCylinderEx({ground.x, beamBot, ground.z}, {ground.x, beamTop, ground.z},
                           0.85f * pulse, 1.1f * pulse, 12,
                           Color{255, 255, 240, (unsigned char)(140 * fade)});
            // Inner white shaft
            DrawCylinderEx({ground.x, beamBot, ground.z}, {ground.x, beamTop, ground.z},
                           0.28f, 0.35f, 8,
                           Color{255, 255, 255, (unsigned char)(200 * fade)});
            // Ground impact halo
            float haloR = 2.0f + 3.5f * Smoothstep01(introT);
            DrawCircle3D({ground.x, ground.y + 0.1f, ground.z}, haloR,
                         {1, 0, 0}, 90.0f, Color{255, 240, 160, (unsigned char)(170 * fade)});
            DrawCircle3D({ground.x, ground.y + 0.25f, ground.z}, haloR * 0.55f,
                         {1, 0, 0}, 90.0f, Color{255, 255, 230, (unsigned char)(140 * fade)});
            // Heavenly glow around descending angel
            DrawSphere(t.position, 3.5f, Color{255, 250, 210, (unsigned char)(50 * fade)});
            DrawSphere(t.position, 1.6f, Color{255, 255, 255, (unsigned char)(80 * fade)});
        }
    }

    DrawParticles();
}

void NotifyProjectileImpact(engine::ecs::Registry& /*reg*/, uint8_t spellId, Vector3 pos) {
    SpawnImpactVfx((SpellId)spellId, pos);
}

} // namespace game::systems
