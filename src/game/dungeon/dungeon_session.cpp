#include "game/dungeon/dungeon.hpp"
#include "game/factories/entity_factory.hpp"
#include "engine/math/noise.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace game::dungeon {

namespace {

    // Instances live far from the ±3 km overworld basin so nothing overlaps.
    constexpr float kRegionZ  = 60000.0f;
    constexpr float kFloorY   = 0.0f;
    constexpr float kWallH    = 10.0f;
    constexpr float kWallT    = 1.5f;
    constexpr float kDoorHalf = 3.75f;   // doorway gap (corridor half width + margin)
    constexpr float kCeilY    = kFloorY + kWallH;

    // Cold stone crypt palette.
    const Color kWallColor    = Color{ 58,  56,  66, 255};
    const Color kPlinthColor  = Color{ 42,  40,  48, 255};
    const Color kCorniceColor = Color{ 72,  70,  82, 255};
    const Color kGateColor    = Color{148, 108,  52, 255};
    const Color kGateDark     = Color{ 88,  62,  28, 255};

    bool     g_active = false;
    Layout   g_layout;
    int      g_currentRoom = -1;
    uint32_t g_nextNetId   = 5000;
    float    g_bannerTimer = 0.0f;
    char     g_banner[96]  = "";

    Vector3  g_overworldPos{};
    bool     g_hasOverworldPos = false;

    std::vector<engine::ecs::Entity> g_walls;
    struct Gate {
        int                 linkIndex = -1;
        engine::ecs::Entity entity{0};
        GateRequirement     req = GateRequirement::None;
    };
    std::vector<Gate> g_gates;

    // --- Campaign / stage state ---
    uint32_t g_baseSeed   = 0;
    int      g_stageIndex = 0;

    // --- Rolled modifiers for the current stage ---
    std::vector<const ModifierDef*> g_activeMods;
    float g_modHealth = 1.0f;
    float g_modDamage = 1.0f;
    float g_modSpeed  = 1.0f;
    float g_modElite  = 0.0f;
    float g_modCount  = 1.0f;
    float g_rewardMul = 1.0f;

    // --- Vault key ---
    engine::ecs::Entity g_keyEntity{0};
    bool                g_keySpawned = false;
    bool                g_hasKey     = false;

    std::vector<Entrance> g_entrances;
    bool                  g_entrancesBuilt = false;

    // The extract seal keys off the boss itself, not off room occupancy — a boss
    // that chases the party into a corridor must not count as cleared.
    engine::ecs::Entity g_boss{0};
    bool                g_bossSpawned = false;

    void setBanner(const char* text) {
        std::snprintf(g_banner, sizeof(g_banner), "%s", text);
        g_bannerTimer = 3.5f;
    }

    engine::ecs::Entity addBox(engine::ecs::Registry& reg, Vector3 center, Vector3 size, Color color) {
        engine::ecs::Entity e = engine::ecs::CreateEntity(reg);

        game::TransformComponent t;
        t.position = center;

        game::RenderComponent r;
        r.color  = color;
        r.width  = size.x;
        r.height = size.y;
        r.depth  = size.z;
        r.visual = game::CharacterVisual::Box;

        reg.transforms.Insert(e, t);
        reg.renderables.Insert(e, r);
        return e;
    }

    const StageDef& currentStage() {
        const CampaignDef& c = GetCampaign();
        const int idx = std::min(std::max(g_stageIndex, 0), (int)c.stages.size() - 1);
        return c.stages[(size_t)idx];
    }

    Color floorColor(RoomType type) {
        switch (type) {
            case RoomType::Secret:    return Color{ 44,  40,  64, 255};
            case RoomType::Vault:     return Color{ 66,  56,  40, 255};
            case RoomType::Entrance:  return Color{ 48,  50,  62, 255};
            case RoomType::SafeHaven: return Color{ 40,  68,  52, 255};
            case RoomType::Treasure:  return Color{ 72,  60,  34, 255};
            case RoomType::Elite:     return Color{ 62,  40,  52, 255};
            case RoomType::Boss:      return Color{ 70,  34,  36, 255};
            case RoomType::Extract:   return Color{ 34,  52,  78, 255};
            default:                  return Color{ 42,  42,  50, 255};
        }
    }

    Color floorAccent(RoomType type) {
        switch (type) {
            case RoomType::Secret:    return Color{ 60,  54,  86, 255};
            case RoomType::Vault:     return Color{ 88,  74,  50, 255};
            case RoomType::Entrance:  return Color{ 62,  64,  78, 255};
            case RoomType::SafeHaven: return Color{ 56,  92,  68, 255};
            case RoomType::Treasure:  return Color{ 96,  80,  42, 255};
            case RoomType::Elite:     return Color{ 84,  48,  64, 255};
            case RoomType::Boss:      return Color{ 96,  42,  44, 255};
            case RoomType::Extract:   return Color{ 48,  72, 108, 255};
            default:                  return Color{ 54,  54,  64, 255};
        }
    }

    Color glowColor(RoomType type) {
        switch (type) {
            case RoomType::Secret:    return Color{170, 130, 255, 170};
            case RoomType::Vault:     return Color{255, 190, 100, 175};
            case RoomType::SafeHaven: return Color{110, 220, 140, 180};
            case RoomType::Treasure:  return Color{255, 200,  80, 170};
            case RoomType::Elite:     return Color{220,  80, 160, 160};
            case RoomType::Boss:      return Color{255,  70,  50, 170};
            case RoomType::Extract:   return Color{100, 180, 255, 190};
            case RoomType::Entrance:  return Color{170, 140, 255, 150};
            default:                  return Color{255, 170,  90, 140};
        }
    }

    const char* roomName(RoomType type) {
        switch (type) {
            case RoomType::Entrance:  return "Entrance";
            case RoomType::SafeHaven: return "Safe Haven";
            case RoomType::Treasure:  return "Treasure Vault";
            case RoomType::Elite:     return "Elite Pack";
            case RoomType::Boss:      return "Boss Arena";
            case RoomType::Extract:   return "Extraction";
            case RoomType::Secret:    return "Secret Chamber";
            case RoomType::Vault:     return "Sealed Vault";
            default:                  return "Combat";
        }
    }

    // 0=+X, 1=-X, 2=+Z, 3=-Z
    int sideForLink(const Room& room, const Transition& link) {
        const float dx = link.center.x - room.center.x;
        const float dz = link.center.z - room.center.z;
        if (fabsf(dx) >= fabsf(dz)) return (dx >= 0.0f) ? 0 : 1;
        return (dz >= 0.0f) ? 2 : 3;
    }

    void buildRoomWalls(engine::ecs::Registry& reg, const Room& room) {
        bool hasDoor[4] = {false, false, false, false};
        for (const auto& link : g_layout.links) {
            if (link.fromRoom != room.id && link.toRoom != room.id) continue;
            hasDoor[sideForLink(room, link)] = true;
        }

        const float wallY   = kFloorY + kWallH * 0.5f;
        const float plinthH = 1.15f;
        const float corniceH = 0.85f;

        auto addWallSeg = [&](Vector3 c, Vector3 size) {
            g_walls.push_back(addBox(reg, c, size, kWallColor));
            // Base plinth
            Vector3 pc = c;
            pc.y = kFloorY + plinthH * 0.5f;
            Vector3 ps = size;
            ps.y = plinthH;
            if (size.x > size.z) { ps.z += 0.35f; } else { ps.x += 0.35f; }
            g_walls.push_back(addBox(reg, pc, ps, kPlinthColor));
            // Top cornice
            Vector3 cc = c;
            cc.y = kCeilY - corniceH * 0.5f;
            Vector3 cs = size;
            cs.y = corniceH;
            if (size.x > size.z) { cs.z += 0.45f; } else { cs.x += 0.45f; }
            g_walls.push_back(addBox(reg, cc, cs, kCorniceColor));
        };

        for (int side = 0; side < 4; ++side) {
            const bool xSide = (side == 0 || side == 1);
            const float span = xSide ? room.halfD : room.halfW;
            const float offs = xSide ? room.halfW : room.halfD;

            float px = room.center.x;
            float pz = room.center.z;
            if (side == 0)      px += offs;
            else if (side == 1) px -= offs;
            else if (side == 2) pz += offs;
            else                pz -= offs;

            const float full = span * 2.0f + kWallT;

            if (!hasDoor[side]) {
                const Vector3 size = xSide ? Vector3{kWallT, kWallH, full}
                                           : Vector3{full, kWallH, kWallT};
                addWallSeg(Vector3{px, wallY, pz}, size);
                continue;
            }

            const float segLen = (span + kWallT * 0.5f) - kDoorHalf;
            if (segLen <= 0.1f) continue;

            const float offset = kDoorHalf + segLen * 0.5f;
            for (int s = 0; s < 2; ++s) {
                const float sign = (s == 0) ? 1.0f : -1.0f;
                Vector3 c{px, wallY, pz};
                Vector3 size;
                if (xSide) {
                    c.z += sign * offset;
                    size = Vector3{kWallT, kWallH, segLen};
                } else {
                    c.x += sign * offset;
                    size = Vector3{segLen, kWallH, kWallT};
                }
                addWallSeg(c, size);
            }

            // Lintel over the doorway
            Vector3 lintel{px, kCeilY - 1.1f, pz};
            Vector3 lsize = xSide
                ? Vector3{kWallT + 0.6f, 1.4f, kDoorHalf * 2.0f + 1.2f}
                : Vector3{kDoorHalf * 2.0f + 1.2f, 1.4f, kWallT + 0.6f};
            g_walls.push_back(addBox(reg, lintel, lsize, kCorniceColor));
        }

        // Corner columns (collision + silhouette).
        const float colW = 1.35f;
        const float inset = room.halfW - 0.15f;
        const float insetZ = room.halfD - 0.15f;
        const float signs[4][2] = {{1,1},{-1,1},{1,-1},{-1,-1}};
        for (int i = 0; i < 4; ++i) {
            Vector3 c{
                room.center.x + signs[i][0] * inset,
                wallY,
                room.center.z + signs[i][1] * insetZ
            };
            g_walls.push_back(addBox(reg, c, Vector3{colW, kWallH, colW}, kPlinthColor));
            g_walls.push_back(addBox(reg,
                Vector3{c.x, kCeilY - 0.35f, c.z},
                Vector3{colW + 0.55f, 0.7f, colW + 0.55f}, kCorniceColor));
        }
    }

    void buildCorridorWalls(engine::ecs::Registry& reg, const Transition& link, int linkIndex) {
        const float wallY  = kFloorY + kWallH * 0.5f;
        const float offset = link.halfWidth + kWallT * 0.5f;
        const float len = link.halfLen * 2.0f + kWallT * 2.0f;

        for (int s = 0; s < 2; ++s) {
            const float sign = (s == 0) ? 1.0f : -1.0f;
            Vector3 c = link.center;
            c.y = wallY;
            Vector3 size;
            if (link.alongX) {
                c.z += sign * offset;
                size = Vector3{len, kWallH, kWallT};
            } else {
                c.x += sign * offset;
                size = Vector3{kWallT, kWallH, len};
            }
            g_walls.push_back(addBox(reg, c, size, kWallColor));
            g_walls.push_back(addBox(reg,
                Vector3{c.x, kFloorY + 0.55f, c.z},
                Vector3{size.x + (link.alongX ? 0.0f : 0.3f), 1.1f, size.z + (link.alongX ? 0.3f : 0.0f)},
                kPlinthColor));
        }

        if (!link.locked) return;

        Vector3 c = link.center;
        c.y = wallY;

        // A hidden passage must look exactly like ordinary masonry.
        const bool hidden = (link.requires_ == GateRequirement::Search);
        const Color color = hidden ? kWallColor : kGateColor;
        const Vector3 size = link.alongX
            ? Vector3{hidden ? kWallT : kWallT * 0.7f, kWallH, link.halfWidth * 2.0f + kWallT}
            : Vector3{link.halfWidth * 2.0f + kWallT, kWallH, hidden ? kWallT : kWallT * 0.7f};

        Gate gate;
        gate.linkIndex = linkIndex;
        gate.req       = link.requires_;
        gate.entity    = addBox(reg, c, size, color);
        g_gates.push_back(gate);

        if (!hidden) {
            Vector3 bar = c;
            bar.y = kFloorY + 4.5f;
            const Vector3 barSize = link.alongX
                ? Vector3{kWallT * 1.2f, 0.55f, link.halfWidth * 2.0f + 0.4f}
                : Vector3{link.halfWidth * 2.0f + 0.4f, 0.55f, kWallT * 1.2f};
            g_walls.push_back(addBox(reg, bar, barSize, kGateDark));
        }
    }

    void rollModifiers(uint32_t stageSeed) {
        g_activeMods.clear();
        g_modHealth = 1.0f;
        g_modDamage = 1.0f;
        g_modSpeed  = 1.0f;
        g_modElite  = 0.0f;
        g_modCount  = 1.0f;
        g_rewardMul = 1.0f;

        const std::vector<ModifierDef>& pool = GetModifierPool();
        if (pool.empty()) return;

        const int rolls = std::min(std::max(currentStage().modifierRolls, 0), (int)pool.size());
        std::vector<bool> taken(pool.size(), false);

        for (int r = 0; r < rolls; ++r) {
            int totalWeight = 0;
            for (size_t i = 0; i < pool.size(); ++i) {
                if (!taken[i]) totalWeight += std::max(1, pool[i].weight);
            }
            if (totalWeight <= 0) break;

            const uint64_t h = engine::math::splitmix64(stageSeed ^ (uint64_t)(r + 1) * 0x9E3779B97F4A7C15ULL);
            int pick = (int)(engine::math::randFloat01(h) * (float)totalWeight);

            for (size_t i = 0; i < pool.size(); ++i) {
                if (taken[i]) continue;
                pick -= std::max(1, pool[i].weight);
                if (pick > 0) continue;

                taken[i] = true;
                const ModifierDef& m = pool[i];
                g_activeMods.push_back(&m);
                g_modHealth *= m.monsterHealthMul;
                g_modDamage *= m.monsterDamageMul;
                g_modSpeed  *= m.monsterSpeedMul;
                g_modElite   = std::max(g_modElite, m.eliteChance);
                g_modCount  *= m.extraEnemyMul;
                g_rewardMul *= m.rewardMul;
                break;
            }
        }
    }

    void applyModsToEnemy(engine::ecs::Registry& reg, engine::ecs::Entity e) {
        const float healthMul = g_modHealth * currentStage().healthMul;
        if (reg.healths.Has(e)) {
            auto& hp = reg.healths.Get(e);
            hp.max     *= healthMul;
            hp.current  = hp.max;
        }
        if (reg.enemyAIs.Has(e)) {
            auto& ai = reg.enemyAIs.Get(e);
            ai.attackDamage *= g_modDamage;
            // Haste plays the locomotion clip faster; root motion follows it, so
            // they cover ground quicker without sliding.
            ai.animRateScale *= g_modSpeed;
        }
    }

    void placeEnemyOnFloor(engine::ecs::Registry& reg, engine::ecs::Entity e) {
        if (!reg.transforms.Has(e) || !reg.renderables.Has(e)) return;
        auto& t = reg.transforms.Get(e);
        t.position.y = kFloorY + reg.renderables.Get(e).height * 0.5f;
    }

    void spawnNormal(engine::ecs::Registry& reg, Vector3 pos) {
        auto e = factories::EntityFactory::CreateEnemy(reg, pos, g_nextNetId++);
        placeEnemyOnFloor(reg, e);
        applyModsToEnemy(reg, e);
    }

    void spawnElite(engine::ecs::Registry& reg, Vector3 pos) {
        auto e = factories::EntityFactory::CreateEliteEnemy(reg, pos, g_nextNetId++);
        placeEnemyOnFloor(reg, e);
        applyModsToEnemy(reg, e);
    }

    engine::ecs::Entity spawnBoss(engine::ecs::Registry& reg, Vector3 pos) {
        auto e = factories::EntityFactory::CreateGiantEnemy(reg, pos, g_nextNetId++);
        placeEnemyOnFloor(reg, e);
        applyModsToEnemy(reg, e);
        return e;
    }

    void spawnVaultKey(engine::ecs::Registry& reg, const Room& room) {
        g_keyEntity = addBox(reg,
                             Vector3{room.center.x, kFloorY + 1.1f, room.center.z},
                             Vector3{0.9f, 0.9f, 0.9f},
                             Color{240, 200, 90, 255});
        g_keySpawned = true;
    }

    bool bossAlive(engine::ecs::Registry& reg) {
        if (!g_bossSpawned) return false;
        return engine::ecs::IsValid(reg, g_boss) && reg.enemyAIs.Has(g_boss);
    }

    void populateRoom(engine::ecs::Registry& reg, Room& room) {
        room.populated = true;

        const uint64_t h = engine::math::splitmix64(g_layout.seed ^ (uint64_t)(room.id * 2654435761u));
        auto rand01 = [h](int salt) {
            return engine::math::randFloat01(engine::math::splitmix64(h ^ (uint64_t)(salt * 0x9E3779B1u)));
        };

        auto ringPos = [&](int i, int count, float radius) {
            const float a = (2.0f * PI * (float)i) / (float)std::max(1, count) + rand01(i + 1) * 1.2f;
            return Vector3{room.center.x + cosf(a) * radius,
                           kFloorY,
                           room.center.z + sinf(a) * radius};
        };

        // Stage escalation and the "swarming" style modifiers both feed pack size.
        const float countMul = currentStage().enemyCountMul * g_modCount;
        // Capped so stacked count modifiers can't build a frame-rate wrecking mob.
        auto scaled = [countMul](int base) {
            return std::min(12, std::max(1, (int)lroundf((float)base * countMul)));
        };

        switch (room.type) {
            case RoomType::Combat: {
                const int count = scaled(3 + (int)(rand01(11) * 2.99f));  // 3-5 before scaling
                // "Champion packs" can promote an ordinary room to an elite pack.
                if (g_modElite > 0.0f && rand01(77) < g_modElite) {
                    spawnElite(reg, Vector3{room.center.x, kFloorY, room.center.z});
                    for (int i = 1; i < count; ++i) spawnNormal(reg, ringPos(i, count, room.halfW * 0.55f));
                } else {
                    for (int i = 0; i < count; ++i) spawnNormal(reg, ringPos(i, count, room.halfW * 0.5f));
                }
                break;
            }
            case RoomType::Elite: {
                spawnElite(reg, Vector3{room.center.x, kFloorY, room.center.z});
                const int count = scaled(2 + (int)(rand01(23) * 1.99f));  // 2-3 before scaling
                for (int i = 0; i < count; ++i) spawnNormal(reg, ringPos(i, count, room.halfW * 0.6f));
                break;
            }
            case RoomType::Boss: {
                g_boss = spawnBoss(reg, Vector3{room.center.x, kFloorY, room.center.z});
                g_bossSpawned = true;
                const int adds = scaled(2);
                for (int i = 0; i < adds; ++i) spawnNormal(reg, ringPos(i, adds, room.halfW * 0.65f));
                setBanner("Boss encounter!");
                break;
            }
            case RoomType::Secret: {
                setBanner("Secret chamber - untouched loot");
                room.cleared = true;
                break;
            }
            default:
                room.cleared = true;  // non-combat rooms are clear on arrival
                break;
        }

        // The vault key is guarded: it drops into its host room's fight.
        if (!g_keySpawned && room.id == g_layout.keyRoom) {
            spawnVaultKey(reg, room);
        }
    }

    bool insideRoom(const Room& room, Vector3 p, float margin) {
        return fabsf(p.x - room.center.x) <= room.halfW + margin &&
               fabsf(p.z - room.center.z) <= room.halfD + margin;
    }

    int countEnemiesInRoom(engine::ecs::Registry& reg, const Room& room) {
        int n = 0;
        for (size_t i = 0; i < reg.enemyAIs.data.size(); ++i) {
            engine::ecs::Entity e = {reg.enemyAIs.indexToEntity[i]};
            if (!reg.transforms.Has(e)) continue;
            if (insideRoom(room, reg.transforms.Get(e).position, 6.0f)) ++n;
        }
        return n;
    }

    void destroyAllEnemies(engine::ecs::Registry& reg) {
        for (int i = (int)reg.enemyAIs.data.size() - 1; i >= 0; --i) {
            engine::ecs::Entity e = {reg.enemyAIs.indexToEntity[i]};
            engine::ecs::DestroyEntity(reg, e);
        }
    }

    void buildEntrances() {
        g_entrancesBuilt = true;
        g_entrances.clear();

        const uint64_t worldSeed = engine::math::GetWorldConfig().seed;
        constexpr int   kWanted   = 6;
        constexpr float kMinApart = 180.0f;

        for (int i = 0; i < kWanted; ++i) {
            for (int attempt = 0; attempt < 48; ++attempt) {
                const uint64_t h  = engine::math::hash2D(worldSeed ^ 0xD0BE17ULL, i, attempt);
                const float angle = engine::math::randFloat01(h) * 2.0f * PI;
                const float dist  = 240.0f + engine::math::randFloat01(engine::math::splitmix64(h)) * 720.0f;

                const float x = cosf(angle) * dist;
                const float z = sinf(angle) * dist;

                if (engine::math::WaterGate(x, z) > 0.10f)   continue;
                if (engine::math::TerrainSlope(x, z) > 0.35f) continue;

                bool tooClose = false;
                for (const auto& e : g_entrances) {
                    const float dx = e.pos.x - x;
                    const float dz = e.pos.z - z;
                    if (dx * dx + dz * dz < kMinApart * kMinApart) { tooClose = true; break; }
                }
                if (tooClose) continue;

                Entrance ent;
                ent.index = i;
                ent.pos   = Vector3{x, engine::math::WorldHeight(x, z), z};
                ent.seed  = (uint32_t)(engine::math::splitmix64(worldSeed ^ (uint64_t)(i + 1) * 0x9E3779B97F4A7C15ULL) >> 16);
                g_entrances.push_back(ent);
                break;
            }
        }
    }

}  // namespace

const std::vector<Entrance>& GetEntrances() {
    if (!g_entrancesBuilt) buildEntrances();
    return g_entrances;
}

int FindNearbyEntrance(Vector3 playerPos, float radius) {
    const auto& list = GetEntrances();
    const float r2 = radius * radius;
    int   best  = -1;
    float bestD = r2;
    for (size_t i = 0; i < list.size(); ++i) {
        const float dx = list[i].pos.x - playerPos.x;
        const float dz = list[i].pos.z - playerPos.z;
        const float d2 = dx * dx + dz * dz;
        if (d2 <= bestD) { bestD = d2; best = (int)i; }
    }
    return best;
}

void DrawEntrances(const Camera3D& cam) {
    const auto& list = GetEntrances();
    const float t = (float)GetTime();
    for (const auto& ent : list) {
        const float dx = ent.pos.x - cam.position.x;
        const float dz = ent.pos.z - cam.position.z;
        if (dx * dx + dz * dz > 700.0f * 700.0f) continue;

        const Color stone  = Color{78, 74, 86, 255};
        const Color dark   = Color{42, 40, 48, 255};
        const Color accent = Color{120, 90, 170, 255};
        const float pillarH = 10.0f;
        const float pulse = 0.5f + 0.5f * sinf(t * 1.8f + (float)ent.index);

        // Stepped platform
        DrawCube(Vector3{ent.pos.x, ent.pos.y + 0.25f, ent.pos.z}, 12.0f, 0.5f, 8.0f, dark);
        DrawCube(Vector3{ent.pos.x, ent.pos.y + 0.65f, ent.pos.z}, 10.0f, 0.4f, 6.5f, stone);

        for (int s = 0; s < 2; ++s) {
            const float sign = (s == 0) ? 1.0f : -1.0f;
            Vector3 c = {ent.pos.x + sign * 3.4f, ent.pos.y + pillarH * 0.5f + 0.7f, ent.pos.z};
            DrawCube(c, 2.0f, pillarH, 2.0f, stone);
            DrawCube(Vector3{c.x, ent.pos.y + 1.1f, c.z}, 2.4f, 0.9f, 2.4f, dark);
            DrawCube(Vector3{c.x, ent.pos.y + pillarH + 0.7f, c.z}, 2.6f, 0.8f, 2.6f, dark);
            // Torch on outer face
            Vector3 torch = {c.x + sign * 1.15f, ent.pos.y + 5.5f, c.z};
            DrawCube(torch, 0.25f, 1.1f, 0.25f, Color{60, 40, 30, 255});
            BeginBlendMode(BLEND_ADDITIVE);
            DrawSphere(Vector3{torch.x, torch.y + 0.7f, torch.z}, 0.45f + 0.08f * pulse,
                       Color{255, 160, 60, (unsigned char)(120 + 80 * pulse)});
            EndBlendMode();
        }

        Vector3 lintel = {ent.pos.x, ent.pos.y + pillarH + 1.1f, ent.pos.z};
        DrawCube(lintel, 10.0f, 1.6f, 2.6f, stone);
        DrawCube(Vector3{lintel.x, lintel.y + 1.1f, lintel.z}, 8.0f, 0.7f, 2.0f, accent);

        // Portal veil
        Vector3 veil = {ent.pos.x, ent.pos.y + pillarH * 0.5f + 0.7f, ent.pos.z};
        BeginBlendMode(BLEND_ALPHA);
        DrawCube(veil, 5.0f, pillarH - 0.4f, 0.35f,
                 Color{(unsigned char)(90 + 50 * pulse), 50,
                       (unsigned char)(160 + 70 * pulse), (unsigned char)(140 + 40 * pulse)});
        EndBlendMode();
        BeginBlendMode(BLEND_ADDITIVE);
        DrawSphere(Vector3{ent.pos.x, ent.pos.y + pillarH + 3.4f, ent.pos.z},
                   1.0f + 0.3f * pulse, Color{180, 120, 255, (unsigned char)(90 + 70 * pulse)});
        DrawSphere(veil, 1.8f, Color{120, 70, 220, 40});
        EndBlendMode();
    }
}

void DrawEntrancePrompt(Vector3 playerPos) {
    if (g_active) return;
    if (FindNearbyEntrance(playerPos, 7.0f) < 0) return;

    const char* msg = "[E]  Enter Dungeon";
    const int   fs   = 26;
    const int   w    = MeasureText(msg, fs);
    const int   x    = GetScreenWidth() / 2 - w / 2;
    const int   y    = GetScreenHeight() - 150;
    DrawRectangle(x - 14, y - 10, w + 28, fs + 20, Color{0, 0, 0, 150});
    DrawText(msg, x, y, fs, Color{235, 220, 255, 255});
}

bool  IsActive() { return g_active; }
float FloorY()   { return kFloorY; }

float GroundY(float x, float z) {
    if (g_active) return kFloorY;
    return engine::math::WorldHeight(x, z);
}

const Layout& GetLayout() { return g_layout; }
int  CurrentRoom()        { return g_currentRoom; }
int  CurrentStage()       { return g_stageIndex; }
int  StageCount()         { return (int)GetCampaign().stages.size(); }
bool HasVaultKey()        { return g_hasKey; }

const std::vector<const ModifierDef*>& ActiveModifiers() { return g_activeMods; }

namespace {

    // Tear down the current stage's geometry and inhabitants (player stays put).
    void clearStageEntities(engine::ecs::Registry& reg) {
        for (auto e : g_walls) engine::ecs::DestroyEntity(reg, e);
        for (auto& g : g_gates) engine::ecs::DestroyEntity(reg, g.entity);
        g_walls.clear();
        g_gates.clear();
        destroyAllEnemies(reg);
        if (g_keySpawned && engine::ecs::IsValid(reg, g_keyEntity)) {
            engine::ecs::DestroyEntity(reg, g_keyEntity);
        }
        g_keySpawned = false;
        g_keyEntity  = engine::ecs::Entity{0};
    }

    // Generate + stamp one stage and drop the player at its entrance.
    void buildStage(engine::ecs::Registry& reg, engine::ecs::Entity player) {
        const uint32_t stageSeed = (uint32_t)(engine::math::splitmix64(
            (uint64_t)g_baseSeed ^ ((uint64_t)(g_stageIndex + 1) * 0x9E3779B97F4A7C15ULL)) >> 16);

        rollModifiers(stageSeed);

        GenProfile profile = GetGenProfile();
        profile.maxBranches += currentStage().extraBranches;

        g_layout = Generate(stageSeed, profile);
        for (auto& r : g_layout.rooms) r.center.z += kRegionZ;
        for (auto& l : g_layout.links) l.center.z += kRegionZ;

        g_boss        = engine::ecs::Entity{0};
        g_bossSpawned = false;
        g_hasKey      = false;

        for (const auto& room : g_layout.rooms) buildRoomWalls(reg, room);
        for (size_t i = 0; i < g_layout.links.size(); ++i) {
            buildCorridorWalls(reg, g_layout.links[i], (int)i);
        }

        const Room& start = g_layout.rooms[g_layout.entranceRoom];
        if (reg.transforms.Has(player)) {
            auto& t = reg.transforms.Get(player);
            t.position = Vector3{start.center.x, kFloorY + 2.0f, start.center.z};
        }
        if (reg.playerInputs.Has(player)) {
            auto& in = reg.playerInputs.Get(player);
            in.velocityY = 0.0f;
            in.grounded  = true;
        }
        g_currentRoom = g_layout.entranceRoom;
    }

}  // namespace

void Enter(engine::ecs::Registry& reg, engine::ecs::Entity player, const Entrance& entrance) {
    if (g_active) return;
    if (!reg.transforms.Has(player)) return;

    LoadData();

    g_overworldPos    = reg.transforms.Get(player).position;
    g_hasOverworldPos = true;

    // Overworld enemies do not follow the party inside.
    destroyAllEnemies(reg);

    g_baseSeed   = entrance.seed;
    g_stageIndex = 0;
    g_walls.clear();
    g_gates.clear();
    buildStage(reg, player);

    g_active = true;

    char msg[128];
    std::snprintf(msg, sizeof(msg), "%s  -  stage 1/%d: %s",
                  GetCampaign().name.c_str(), StageCount(), currentStage().name.c_str());
    setBanner(msg);
}

void Exit(engine::ecs::Registry& reg, engine::ecs::Entity player) {
    if (!g_active) return;

    clearStageEntities(reg);

    if (g_hasOverworldPos && reg.transforms.Has(player)) {
        auto& t = reg.transforms.Get(player);
        t.position = g_overworldPos;
        t.position.y = engine::math::WorldHeight(t.position.x, t.position.z) + 2.0f;
        if (reg.playerInputs.Has(player)) {
            auto& in = reg.playerInputs.Get(player);
            in.velocityY = 0.0f;
            in.grounded  = true;
        }
    }

    g_active      = false;
    g_currentRoom = -1;
    g_stageIndex  = 0;
    g_hasKey      = false;
    g_activeMods.clear();
}

void Update(engine::ecs::Registry& reg, engine::ecs::Entity player) {
    const float dt = GetFrameTime();
    if (g_bannerTimer > 0.0f) g_bannerTimer -= dt;

    if (!g_active) return;
    if (!reg.transforms.Has(player)) return;

    const Vector3 p = reg.transforms.Get(player).position;

    // Wipe: bail back to the overworld instead of stranding the corpse inside.
    if (reg.healths.Has(player)) {
        auto& hp = reg.healths.Get(player);
        if (hp.current <= 0.0f) {
            hp.current = hp.max * 0.25f;
            setBanner("You were driven out of the dungeon");
            Exit(reg, player);
            return;
        }
    }

    for (auto& room : g_layout.rooms) {
        if (!insideRoom(room, p, 0.0f)) continue;

        if (g_currentRoom != room.id) {
            g_currentRoom = room.id;
            if (!room.visited) {
                room.visited = true;
                if (room.type == RoomType::SafeHaven && reg.healths.Has(player)) {
                    auto& hp = reg.healths.Get(player);
                    hp.current = std::min(hp.max, hp.current + hp.max * 0.35f);
                    setBanner("Safe haven - partial recovery");
                } else if (room.type == RoomType::Treasure) {
                    setBanner("Treasure vault");
                }
            }
        }

        if (!room.populated) populateRoom(reg, room);
        break;
    }

    // Room clear tracking (combat rooms only; others clear on arrival).
    for (auto& room : g_layout.rooms) {
        if (!room.populated || room.cleared) continue;
        if (room.type == RoomType::Boss) {
            if (g_bossSpawned && !bossAlive(reg)) {
                room.cleared = true;
                setBanner("Boss slain - extraction unsealed");
            }
            continue;
        }
        if (countEnemiesInRoom(reg, room) == 0) room.cleared = true;
    }

    // Vault key pickup.
    if (g_keySpawned && !g_hasKey && engine::ecs::IsValid(reg, g_keyEntity) &&
        reg.transforms.Has(g_keyEntity)) {
        const Vector3 kp = reg.transforms.Get(g_keyEntity).position;
        const float dx = kp.x - p.x;
        const float dz = kp.z - p.z;
        if (dx * dx + dz * dz < 3.0f * 3.0f) {
            engine::ecs::DestroyEntity(reg, g_keyEntity);
            g_keySpawned = false;
            g_hasKey     = true;
            setBanner("Vault key taken");
        }
    }

    // Sealed transitions open on their own terms.
    const bool bossDown = (g_layout.bossRoom < 0) ||
                          (g_bossSpawned && !bossAlive(reg)) ||
                          g_layout.rooms[g_layout.bossRoom].cleared;

    for (size_t i = 0; i < g_gates.size();) {
        Gate& gate = g_gates[i];
        bool open = false;
        const char* msg = nullptr;

        switch (gate.req) {
            case GateRequirement::BossDead:
                open = bossDown;
                msg  = "Extraction unsealed";
                break;
            case GateRequirement::Key:
                open = g_hasKey;
                msg  = "The vault grinds open";
                break;
            case GateRequirement::Search: {
                // Hidden passages reveal themselves when searched at close range.
                if (gate.linkIndex >= 0 && gate.linkIndex < (int)g_layout.links.size()) {
                    const Vector3 gc = g_layout.links[gate.linkIndex].center;
                    const float dx = gc.x - p.x;
                    const float dz = gc.z - p.z;
                    open = (dx * dx + dz * dz < 7.0f * 7.0f);
                }
                msg = "A hidden passage grinds open";
                break;
            }
            default:
                open = true;
                break;
        }

        if (!open) { ++i; continue; }

        engine::ecs::DestroyEntity(reg, gate.entity);
        if (gate.linkIndex >= 0 && gate.linkIndex < (int)g_layout.links.size()) {
            g_layout.links[gate.linkIndex].locked = false;
        }
        if (msg != nullptr) setBanner(msg);
        g_gates.erase(g_gates.begin() + (long)i);
    }

    // Extraction: descend to the next stage, or finish the campaign run.
    // Copied by value because regenerating the stage reallocates the room list.
    if (g_layout.extractRoom >= 0) {
        const Room ex = g_layout.rooms[g_layout.extractRoom];
        if (insideRoom(ex, p, 0.0f)) {
            if (g_stageIndex + 1 < StageCount()) {
                clearStageEntities(reg);
                ++g_stageIndex;
                buildStage(reg, player);

                char msg[128];
                std::snprintf(msg, sizeof(msg), "Descending  -  stage %d/%d: %s",
                              g_stageIndex + 1, StageCount(), currentStage().name.c_str());
                setBanner(msg);
            } else {
                char msg[128];
                std::snprintf(msg, sizeof(msg), "Campaign complete  -  reward x%.2f", g_rewardMul);
                setBanner(msg);
                Exit(reg, player);
            }
        }
    }
}

void Draw() {
    if (!g_active) return;

    const float t = (float)GetTime();
    const float pulse = 0.5f + 0.5f * sinf(t * 2.2f);

    auto drawTorch = [&](Vector3 base, Color glow) {
        DrawCube(base, 0.22f, 1.0f, 0.22f, Color{55, 38, 28, 255});
        DrawCube(Vector3{base.x, base.y + 0.55f, base.z}, 0.35f, 0.18f, 0.35f, Color{70, 50, 36, 255});
        BeginBlendMode(BLEND_ADDITIVE);
        const float flicker = 0.85f + 0.15f * sinf(t * 11.0f + base.x * 0.3f + base.z * 0.2f);
        DrawSphere(Vector3{base.x, base.y + 0.85f, base.z}, 0.38f * flicker,
                   Color{glow.r, glow.g, glow.b, (unsigned char)(glow.a * flicker)});
        DrawSphere(Vector3{base.x, base.y + 0.85f, base.z}, 0.9f * flicker,
                   Color{glow.r, glow.g, glow.b, (unsigned char)(glow.a / 4)});
        EndBlendMode();
    };

    for (const auto& room : g_layout.rooms) {
        const Color base = floorColor(room.type);
        const Color accent = floorAccent(room.type);
        const Color glow = glowColor(room.type);

        // Thick floor slab
        DrawCube(Vector3{room.center.x, kFloorY - 0.35f, room.center.z},
                 room.halfW * 2.0f + 0.4f, 0.7f, room.halfD * 2.0f + 0.4f, kPlinthColor);

        // Checker / tile pattern
        const float tile = 3.0f;
        const int nx = (int)ceilf((room.halfW * 2.0f) / tile);
        const int nz = (int)ceilf((room.halfD * 2.0f) / tile);
        for (int ix = 0; ix < nx; ++ix) {
            for (int iz = 0; iz < nz; ++iz) {
                const float cx = room.center.x - room.halfW + tile * 0.5f + ix * tile;
                const float cz = room.center.z - room.halfD + tile * 0.5f + iz * tile;
                if (fabsf(cx - room.center.x) > room.halfW - 0.1f) continue;
                if (fabsf(cz - room.center.z) > room.halfD - 0.1f) continue;
                const bool alt = ((ix + iz) & 1) != 0;
                Color c = alt ? accent : base;
                if (room.populated && !room.cleared) {
                    c.r = (unsigned char)std::min(255, c.r + 12);
                }
                DrawCube(Vector3{cx, kFloorY - 0.05f, cz}, tile - 0.12f, 0.18f, tile - 0.12f, c);
            }
        }

        // Ceiling slab + rib beams
        DrawCube(Vector3{room.center.x, kCeilY + 0.35f, room.center.z},
                 room.halfW * 2.0f + 0.6f, 0.7f, room.halfD * 2.0f + 0.6f, Color{32, 30, 38, 255});
        for (float bx = -room.halfW + 4.0f; bx < room.halfW - 2.0f; bx += 6.0f) {
            DrawCube(Vector3{room.center.x + bx, kCeilY - 0.15f, room.center.z},
                     0.55f, 0.45f, room.halfD * 2.0f - 1.0f, Color{48, 46, 56, 255});
        }
        for (float bz = -room.halfD + 4.0f; bz < room.halfD - 2.0f; bz += 6.0f) {
            DrawCube(Vector3{room.center.x, kCeilY - 0.15f, room.center.z + bz},
                     room.halfW * 2.0f - 1.0f, 0.45f, 0.55f, Color{48, 46, 56, 255});
        }

        // Wall-mounted torches (inset from corners)
        const float torchInset = 3.2f;
        drawTorch(Vector3{room.center.x - room.halfW + torchInset, kFloorY + 4.2f, room.center.z - room.halfD + 0.55f}, glow);
        drawTorch(Vector3{room.center.x + room.halfW - torchInset, kFloorY + 4.2f, room.center.z - room.halfD + 0.55f}, glow);
        drawTorch(Vector3{room.center.x - room.halfW + torchInset, kFloorY + 4.2f, room.center.z + room.halfD - 0.55f}, glow);
        drawTorch(Vector3{room.center.x + room.halfW - torchInset, kFloorY + 4.2f, room.center.z + room.halfD - 0.55f}, glow);

        // Central hanging lamp
        BeginBlendMode(BLEND_ADDITIVE);
        DrawSphere(Vector3{room.center.x, kCeilY - 1.8f, room.center.z},
                   0.55f + 0.08f * pulse,
                   Color{glow.r, glow.g, glow.b, (unsigned char)(90 + 50 * pulse)});
        EndBlendMode();
        DrawCube(Vector3{room.center.x, kCeilY - 0.7f, room.center.z}, 0.12f, 1.6f, 0.12f,
                 Color{40, 40, 48, 255});

        // Room-specific props
        if (room.type == RoomType::Treasure) {
            const Vector3 chest{room.center.x, kFloorY + 0.7f, room.center.z};
            DrawCube(chest, 2.4f, 1.3f, 1.5f, Color{168, 122, 48, 255});
            DrawCube(Vector3{chest.x, chest.y + 0.55f, chest.z}, 2.5f, 0.35f, 1.6f, Color{210, 170, 70, 255});
            DrawCube(Vector3{chest.x, chest.y, chest.z + 0.78f}, 0.35f, 0.45f, 0.2f, Color{255, 220, 120, 255});
            BeginBlendMode(BLEND_ADDITIVE);
            DrawSphere(Vector3{chest.x, chest.y + 1.4f, chest.z}, 0.7f + 0.15f * pulse,
                       Color{255, 210, 90, (unsigned char)(70 + 50 * pulse)});
            EndBlendMode();
            // Coin scatter
            for (int i = 0; i < 6; ++i) {
                const float a = i * 1.1f;
                DrawCube(Vector3{chest.x + cosf(a) * 2.2f, kFloorY + 0.12f, chest.z + sinf(a) * 1.6f},
                         0.35f, 0.12f, 0.35f, Color{220, 180, 60, 255});
            }
        } else if (room.type == RoomType::Extract) {
            // Portal ring
            for (int i = 0; i < 12; ++i) {
                const float a = (float)i / 12.0f * 2.0f * PI + t * 0.4f;
                DrawCube(Vector3{room.center.x + cosf(a) * 3.2f, kFloorY + 1.2f + 0.3f * sinf(t * 2.0f + a),
                                 room.center.z + sinf(a) * 3.2f},
                         0.45f, 0.45f, 0.45f, Color{70, 130, 220, 255});
            }
            BeginBlendMode(BLEND_ADDITIVE);
            DrawSphere(Vector3{room.center.x, kFloorY + 2.2f, room.center.z},
                       1.8f + 0.35f * pulse, Color{90, 170, 255, (unsigned char)(90 + 70 * pulse)});
            DrawSphere(Vector3{room.center.x, kFloorY + 2.2f, room.center.z},
                       3.2f, Color{60, 120, 220, 35});
            EndBlendMode();
            DrawCylinder(Vector3{room.center.x, kFloorY, room.center.z}, 3.6f, 3.6f, 0.15f, 24,
                         Color{50, 90, 150, 200});
        } else if (room.type == RoomType::SafeHaven) {
            // Healing dais + runes
            DrawCylinder(Vector3{room.center.x, kFloorY, room.center.z}, 3.5f, 3.5f, 0.35f, 20,
                         Color{70, 120, 88, 255});
            DrawCylinder(Vector3{room.center.x, kFloorY + 0.35f, room.center.z}, 2.4f, 2.4f, 0.25f, 16,
                         Color{100, 170, 120, 255});
            BeginBlendMode(BLEND_ADDITIVE);
            DrawSphere(Vector3{room.center.x, kFloorY + 2.0f, room.center.z},
                       1.2f + 0.2f * pulse, Color{120, 255, 160, (unsigned char)(60 + 50 * pulse)});
            EndBlendMode();
            DrawCube(Vector3{room.center.x, kFloorY + 1.4f, room.center.z}, 0.5f, 2.0f, 0.5f,
                     Color{200, 220, 200, 255});
        } else if (room.type == RoomType::Boss) {
            // Arena ring + braziers
            DrawCylinder(Vector3{room.center.x, kFloorY, room.center.z}, 8.0f, 8.5f, 0.2f, 28,
                         Color{90, 40, 40, 220});
            for (int i = 0; i < 6; ++i) {
                const float a = (float)i / 6.0f * 2.0f * PI;
                Vector3 brazier{room.center.x + cosf(a) * 10.0f, kFloorY + 0.9f,
                                room.center.z + sinf(a) * 10.0f};
                DrawCube(brazier, 1.1f, 1.6f, 1.1f, Color{50, 30, 30, 255});
                BeginBlendMode(BLEND_ADDITIVE);
                DrawSphere(Vector3{brazier.x, brazier.y + 1.2f, brazier.z},
                           0.7f + 0.15f * pulse, Color{255, 90, 40, (unsigned char)(120 + 60 * pulse)});
                EndBlendMode();
            }
        } else if (room.type == RoomType::Secret) {
            // Hidden shrine: floating relic over a rune circle.
            DrawCylinder(Vector3{room.center.x, kFloorY, room.center.z}, 3.2f, 3.2f, 0.2f, 24,
                         Color{60, 48, 92, 220});
            for (int i = 0; i < 8; ++i) {
                const float a = (float)i / 8.0f * 2.0f * PI - t * 0.25f;
                DrawCube(Vector3{room.center.x + cosf(a) * 2.6f, kFloorY + 0.35f,
                                 room.center.z + sinf(a) * 2.6f},
                         0.4f, 0.5f, 0.4f, Color{130, 100, 200, 255});
            }
            const float bob = 0.25f * sinf(t * 1.6f);
            DrawCube(Vector3{room.center.x, kFloorY + 2.4f + bob, room.center.z},
                     0.9f, 0.9f, 0.9f, Color{190, 160, 255, 255});
            BeginBlendMode(BLEND_ADDITIVE);
            DrawSphere(Vector3{room.center.x, kFloorY + 2.4f + bob, room.center.z},
                       1.3f + 0.25f * pulse, Color{170, 120, 255, (unsigned char)(80 + 60 * pulse)});
            EndBlendMode();
        } else if (room.type == RoomType::Vault) {
            // Reward hoard behind the locked gate.
            DrawCylinder(Vector3{room.center.x, kFloorY, room.center.z}, 3.6f, 3.6f, 0.2f, 20,
                         Color{86, 70, 44, 230});
            for (int i = 0; i < 3; ++i) {
                const float a = (float)i / 3.0f * 2.0f * PI + 0.4f;
                Vector3 chest{room.center.x + cosf(a) * 2.4f, kFloorY + 0.7f,
                              room.center.z + sinf(a) * 2.4f};
                DrawCube(chest, 2.0f, 1.2f, 1.3f, Color{168, 122, 48, 255});
                DrawCube(Vector3{chest.x, chest.y + 0.5f, chest.z}, 2.1f, 0.3f, 1.4f,
                         Color{215, 175, 75, 255});
            }
            BeginBlendMode(BLEND_ADDITIVE);
            DrawSphere(Vector3{room.center.x, kFloorY + 2.6f, room.center.z},
                       2.0f + 0.3f * pulse, Color{255, 200, 90, (unsigned char)(60 + 45 * pulse)});
            EndBlendMode();
        } else if (room.type == RoomType::Elite) {
            DrawCylinder(Vector3{room.center.x, kFloorY, room.center.z}, 4.0f, 4.2f, 0.12f, 18,
                         Color{90, 40, 70, 200});
            DrawCube(Vector3{room.center.x, kFloorY + 1.1f, room.center.z}, 1.6f, 2.0f, 1.6f,
                     Color{70, 30, 55, 255});
        } else if (room.type == RoomType::Entrance) {
            DrawCylinder(Vector3{room.center.x, kFloorY, room.center.z}, 2.5f, 2.5f, 0.2f, 16,
                         Color{70, 60, 100, 200});
        } else {
            // Combat clutter — broken crates
            const uint64_t h = engine::math::splitmix64(g_layout.seed ^ (uint64_t)(room.id * 97u));
            for (int i = 0; i < 3; ++i) {
                const float u = engine::math::randFloat01(engine::math::splitmix64(h ^ (uint64_t)(i + 3)));
                const float v = engine::math::randFloat01(engine::math::splitmix64(h ^ (uint64_t)(i + 9)));
                const float ox = (u - 0.5f) * room.halfW * 1.4f;
                const float oz = (v - 0.5f) * room.halfD * 1.4f;
                if (fabsf(ox) < 2.5f && fabsf(oz) < 2.5f) continue;
                DrawCube(Vector3{room.center.x + ox, kFloorY + 0.55f, room.center.z + oz},
                         1.3f, 1.1f, 1.1f, Color{78, 58, 42, 255});
            }
        }
    }

    for (const auto& link : g_layout.links) {
        const float lx = link.alongX ? link.halfLen * 2.0f : link.halfWidth * 2.0f;
        const float lz = link.alongX ? link.halfWidth * 2.0f : link.halfLen * 2.0f;

        DrawCube(Vector3{link.center.x, kFloorY - 0.35f, link.center.z},
                 lx + 0.4f, 0.7f, lz + 0.4f, kPlinthColor);
        DrawCube(Vector3{link.center.x, kFloorY - 0.05f, link.center.z},
                 lx, 0.18f, lz, Color{38, 38, 46, 255});

        // Corridor ceiling
        DrawCube(Vector3{link.center.x, kCeilY + 0.25f, link.center.z},
                 lx + 0.5f, 0.5f, lz + 0.5f, Color{30, 28, 36, 255});

        // Arch rings at each end
        for (int e = 0; e < 2; ++e) {
            Vector3 arch = link.center;
            if (link.alongX) arch.x += (e == 0 ? -1.0f : 1.0f) * (link.halfLen - 0.2f);
            else             arch.z += (e == 0 ? -1.0f : 1.0f) * (link.halfLen - 0.2f);
            arch.y = kFloorY + 5.0f;
            if (link.alongX) {
                DrawCube(arch, 0.5f, 2.2f, link.halfWidth * 2.0f + 1.0f, kCorniceColor);
            } else {
                DrawCube(arch, link.halfWidth * 2.0f + 1.0f, 2.2f, 0.5f, kCorniceColor);
            }
        }

        // Mid-corridor torch pair
        if (link.alongX) {
            drawTorch(Vector3{link.center.x, kFloorY + 4.0f, link.center.z - link.halfWidth + 0.4f},
                      Color{255, 160, 80, 130});
            drawTorch(Vector3{link.center.x, kFloorY + 4.0f, link.center.z + link.halfWidth - 0.4f},
                      Color{255, 160, 80, 130});
        } else {
            drawTorch(Vector3{link.center.x - link.halfWidth + 0.4f, kFloorY + 4.0f, link.center.z},
                      Color{255, 160, 80, 130});
            drawTorch(Vector3{link.center.x + link.halfWidth - 0.4f, kFloorY + 4.0f, link.center.z},
                      Color{255, 160, 80, 130});
        }

        // Hidden passages get no telltale glow — that is the point of them.
        if (link.locked && link.requires_ != GateRequirement::Search) {
            const Color seal = (link.requires_ == GateRequirement::Key)
                                   ? Color{255, 205, 90, 0}
                                   : Color{255, 180, 60, 0};
            BeginBlendMode(BLEND_ADDITIVE);
            DrawSphere(Vector3{link.center.x, kFloorY + 4.5f, link.center.z},
                       1.2f + 0.2f * pulse,
                       Color{seal.r, seal.g, seal.b, (unsigned char)(70 + 40 * pulse)});
            EndBlendMode();

            // Keyhole marker so players know what the seal wants.
            if (link.requires_ == GateRequirement::Key) {
                DrawCube(Vector3{link.center.x, kFloorY + 2.6f, link.center.z},
                         0.6f, 0.9f, 0.6f, g_hasKey ? Color{120, 240, 140, 255}
                                                    : Color{225, 185, 70, 255});
            }
        }
    }

    // Soft dust motes near the player room
    if (g_currentRoom >= 0 && g_currentRoom < (int)g_layout.rooms.size()) {
        const Room& room = g_layout.rooms[g_currentRoom];
        BeginBlendMode(BLEND_ADDITIVE);
        for (int i = 0; i < 18; ++i) {
            const float a = (float)i * 1.7f + t * 0.3f;
            const float r = 3.0f + fmodf((float)i * 1.9f, 6.0f);
            const float y = kFloorY + 1.5f + fmodf((float)i * 0.73f + t * 0.4f, 5.0f);
            DrawSphere(Vector3{room.center.x + cosf(a) * r, y, room.center.z + sinf(a) * r},
                       0.06f, Color{200, 190, 170, 50});
        }
        EndBlendMode();
    }
}

void DrawHUD() {
    if (g_active) {
        int cleared = 0;
        int combat  = 0;
        for (const auto& room : g_layout.rooms) {
            const bool isCombat = (room.type == RoomType::Combat ||
                                   room.type == RoomType::Elite ||
                                   room.type == RoomType::Boss);
            if (!isCombat) continue;
            ++combat;
            if (room.cleared) ++cleared;
        }

        const bool bossDown = (g_layout.bossRoom >= 0) && g_layout.rooms[g_layout.bossRoom].cleared;
        const char* objective = bossDown ? "Objective: reach extraction"
                                         : "Objective: clear the dungeon and kill the boss";

        char stageLine[160];
        std::snprintf(stageLine, sizeof(stageLine), "%s  -  stage %d/%d: %s",
                      GetCampaign().name.c_str(), g_stageIndex + 1, StageCount(),
                      currentStage().name.c_str());

        char line[128];
        std::snprintf(line, sizeof(line), "seed %u   rooms cleared %d/%d   reward x%.2f",
                      g_layout.seed, cleared, combat, g_rewardMul);

        const int panelH = 86 + (int)g_activeMods.size() * 20;
        DrawRectangle(14, 92, 470, panelH, Color{0, 0, 0, 140});
        DrawText(stageLine, 24, 100, 19, Color{235, 225, 200, 255});
        DrawText(line, 24, 124, 17, Color{200, 200, 212, 255});
        DrawText(objective, 24, 146, 17, bossDown ? Color{140, 200, 255, 255}
                                                  : Color{225, 200, 160, 255});

        int y = 170;
        for (const ModifierDef* m : g_activeMods) {
            DrawText(TextFormat("- %s", m->name.c_str()), 28, y, 16, Color{255, 170, 150, 255});
            y += 20;
        }

        if (g_currentRoom >= 0 && g_currentRoom < (int)g_layout.rooms.size()) {
            const Room& room = g_layout.rooms[g_currentRoom];
            DrawText(roomName(room.type), 24, y + 4, 20, Color{200, 200, 210, 255});
            y += 26;
        }

        if (g_hasKey) {
            DrawText("Vault key: held", 24, y + 4, 17, Color{240, 210, 110, 255});
        } else if (g_layout.keyRoom >= 0 && g_keySpawned) {
            DrawText("Vault key: guarded nearby", 24, y + 4, 17, Color{190, 180, 140, 255});
        }
    }

    if (g_bannerTimer > 0.0f && g_banner[0] != 0) {
        const int fs = 28;
        const int w  = MeasureText(g_banner, fs);
        const int x  = GetScreenWidth() / 2 - w / 2;
        const int y  = 120;
        unsigned char a = (unsigned char)std::min(255.0f, g_bannerTimer * 200.0f);
        DrawRectangle(x - 16, y - 10, w + 32, fs + 20, Color{0, 0, 0, (unsigned char)(a / 2)});
        DrawText(g_banner, x, y, fs, Color{240, 230, 210, a});
    }
}

}  // namespace game::dungeon
