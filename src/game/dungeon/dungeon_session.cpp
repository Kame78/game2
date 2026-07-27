#include "game/dungeon/dungeon.hpp"
#include "game/dungeon/dungeon_events.hpp"
#include "game/dungeon/dungeon_mask.hpp"
#include "game/dungeon/dungeon_props.hpp"
#include "game/factories/entity_factory.hpp"
#include "engine/math/noise.hpp"
#include "engine/networking.hpp"
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
    // Corridor doorway half-width — keep in sync with Transition::halfWidth defaults
    // so extruded room walls don't leave open flanks beside the passage.
    constexpr float kDoorHalf = 3.75f;   // doorway gap (corridor half width + margin)
    constexpr float kCeilY    = kFloorY + kWallH;
    // How far corridor collision walls / floor-ceil meshes poke into rooms.
    constexpr float kCorridorOverlap = 3.0f;
    // Visual mesh uses the same reach; Y-bias keeps overlap from z-fighting.
    constexpr float kCorridorMeshOverlap = 3.0f;
    constexpr float kCorridorFloorBias = 0.04f;
    constexpr float kCorridorCeilBias  = -0.04f;

    Theme g_theme = Theme::Masonry;

    // Cold stone crypt palette (masonry). Cave theme shifts cooler / dirtier.
    Color wallColor() {
        return (g_theme == Theme::Cave) ? Color{ 52,  48,  44, 255} : Color{ 58,  56,  66, 255};
    }
    Color plinthColor() {
        return (g_theme == Theme::Cave) ? Color{ 40,  36,  32, 255} : Color{ 42,  40,  48, 255};
    }
    Color corniceColor() {
        return (g_theme == Theme::Cave) ? Color{ 68,  62,  54, 255} : Color{ 72,  70,  82, 255};
    }

    const Color kGateColor    = Color{148, 108,  52, 255};
    const Color kGateDark     = Color{ 88,  62,  28, 255};

    bool     g_active = false;
    Layout   g_layout;
    std::vector<WalkableMask> g_masks;

    // One GPU floor + ceiling mesh per room (heightfields).
    struct RoomSurfaceGpu {
        Model floor{};
        Model ceil{};
        Model cove{};   // wall–ceiling bevel (caves)
        bool  floorReady = false;
        bool  ceilReady  = false;
        bool  coveReady  = false;
    };
    std::vector<RoomSurfaceGpu> g_surfaces;

    // Corridor height profiles (along travel axis) + baked meshes.
    struct CorridorHeights {
        int n = 0;
        std::vector<float> floor; // offset from kFloorY
        std::vector<float> ceil;  // offset from kCeilY (neg = hangs)
    };
    std::vector<CorridorHeights> g_linkHeights;
    std::vector<RoomSurfaceGpu>  g_linkSurfaces;

    int      g_currentRoom = -1;
    uint32_t g_nextNetId   = 5000;
    float    g_bannerTimer = 0.0f;
    char     g_banner[96]  = "";

    Vector3  g_overworldPos{};
    bool     g_hasOverworldPos = false;
    Vector3  g_playerDrawPos{};

    std::vector<engine::ecs::Entity> g_walls; // legacy unused — kept empty
    struct SolidBox {
        Vector3 center{};
        float   w = 1.0f;
        float   h = 1.0f;
        float   d = 1.0f;
        Color   color{255, 255, 255, 255};
        bool    active = true;
        bool    collide = true;
        bool    caveTex = false; // draw with cave wall albedo + world UVs
    };
    std::vector<SolidBox> g_solids;

    struct Gate {
        int             linkIndex     = -1;
        int             solidIndex    = -1;
        int             barSolidIndex = -1;
        GateRequirement req = GateRequirement::None;
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
    Vector3 g_keyPos{};
    bool    g_keySpawned = false;
    bool    g_hasKey     = false;

    // --- Inter-stage SafeHaven rest ---
    bool     g_intermission     = false;
    int      g_intermissionNext = 0;
    bool     g_lootPending      = false;
    Vector3  g_lootPos{};
    float    g_lootReward       = 0.0f;
    bool     g_lootTaken        = false;

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

    int addSolid(Vector3 center, Vector3 size, Color color, bool collide = true, bool caveTex = false) {
        SolidBox b;
        b.center  = center;
        b.w       = size.x;
        b.h       = size.y;
        b.d       = size.z;
        b.color   = color;
        b.active  = true;
        b.collide = collide;
        b.caveTex = caveTex;
        g_solids.push_back(b);
        return (int)g_solids.size() - 1;
    }

    // Visual-only trim (plinth / cornice) — no collision cost.
    void addDecor(Vector3 center, Vector3 size, Color color, bool caveTex = false) {
        addSolid(center, size, color, false, caveTex);
    }

    // Textured AABB with world-space UVs so long wall runs tile instead of stretch.
    void drawTexturedBox(Texture2D tex, Vector3 c, float w, float h, float d, Color tint) {
        if (tex.id == 0) {
            DrawCube(c, w, h, d, tint);
            return;
        }
        constexpr float kUv = 5.0f;
        const float x0 = c.x - w * 0.5f, x1 = c.x + w * 0.5f;
        const float y0 = c.y - h * 0.5f, y1 = c.y + h * 0.5f;
        const float z0 = c.z - d * 0.5f, z1 = c.z + d * 0.5f;

        rlSetTexture(tex.id);
        rlBegin(RL_QUADS);
        rlColor4ub(tint.r, tint.g, tint.b, tint.a);

        // +Z
        rlNormal3f(0, 0, 1);
        rlTexCoord2f(x0 / kUv, y0 / kUv); rlVertex3f(x0, y0, z1);
        rlTexCoord2f(x1 / kUv, y0 / kUv); rlVertex3f(x1, y0, z1);
        rlTexCoord2f(x1 / kUv, y1 / kUv); rlVertex3f(x1, y1, z1);
        rlTexCoord2f(x0 / kUv, y1 / kUv); rlVertex3f(x0, y1, z1);
        // -Z
        rlNormal3f(0, 0, -1);
        rlTexCoord2f(x1 / kUv, y0 / kUv); rlVertex3f(x1, y0, z0);
        rlTexCoord2f(x0 / kUv, y0 / kUv); rlVertex3f(x0, y0, z0);
        rlTexCoord2f(x0 / kUv, y1 / kUv); rlVertex3f(x0, y1, z0);
        rlTexCoord2f(x1 / kUv, y1 / kUv); rlVertex3f(x1, y1, z0);
        // +X
        rlNormal3f(1, 0, 0);
        rlTexCoord2f(z1 / kUv, y0 / kUv); rlVertex3f(x1, y0, z1);
        rlTexCoord2f(z0 / kUv, y0 / kUv); rlVertex3f(x1, y0, z0);
        rlTexCoord2f(z0 / kUv, y1 / kUv); rlVertex3f(x1, y1, z0);
        rlTexCoord2f(z1 / kUv, y1 / kUv); rlVertex3f(x1, y1, z1);
        // -X
        rlNormal3f(-1, 0, 0);
        rlTexCoord2f(z0 / kUv, y0 / kUv); rlVertex3f(x0, y0, z0);
        rlTexCoord2f(z1 / kUv, y0 / kUv); rlVertex3f(x0, y0, z1);
        rlTexCoord2f(z1 / kUv, y1 / kUv); rlVertex3f(x0, y1, z1);
        rlTexCoord2f(z0 / kUv, y1 / kUv); rlVertex3f(x0, y1, z0);
        // +Y
        rlNormal3f(0, 1, 0);
        rlTexCoord2f(x0 / kUv, z1 / kUv); rlVertex3f(x0, y1, z1);
        rlTexCoord2f(x1 / kUv, z1 / kUv); rlVertex3f(x1, y1, z1);
        rlTexCoord2f(x1 / kUv, z0 / kUv); rlVertex3f(x1, y1, z0);
        rlTexCoord2f(x0 / kUv, z0 / kUv); rlVertex3f(x0, y1, z0);
        // -Y
        rlNormal3f(0, -1, 0);
        rlTexCoord2f(x0 / kUv, z0 / kUv); rlVertex3f(x0, y0, z0);
        rlTexCoord2f(x1 / kUv, z0 / kUv); rlVertex3f(x1, y0, z0);
        rlTexCoord2f(x1 / kUv, z1 / kUv); rlVertex3f(x1, y0, z1);
        rlTexCoord2f(x0 / kUv, z1 / kUv); rlVertex3f(x0, y0, z1);

        rlEnd();
        rlSetTexture(0);
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

    uint8_t socketsForRoom(const Room& room) {
        uint8_t bits = 0;
        for (const auto& link : g_layout.links) {
            if (link.fromRoom != room.id && link.toRoom != room.id) continue;
            bits |= (uint8_t)(1u << sideForLink(room, link));
        }
        return bits;
    }

    const WalkableMask* maskFor(const Room& room) {
        if (room.id < 0 || room.id >= (int)g_masks.size()) return nullptr;
        if (!g_masks[(size_t)room.id].Valid()) return nullptr;
        return &g_masks[(size_t)room.id];
    }

    void detachModelTextures(Model& m) {
        for (int i = 0; i < m.materialCount; ++i) {
            m.materials[i].maps[MATERIAL_MAP_ALBEDO].texture = {};
            m.materials[i].maps[MATERIAL_MAP_NORMAL].texture = {};
        }
    }

    void unloadSurfaceSlot(RoomSurfaceGpu& s) {
        if (s.floorReady) {
            detachModelTextures(s.floor);
            UnloadModel(s.floor);
            s.floor = {};
            s.floorReady = false;
        }
        if (s.ceilReady) {
            detachModelTextures(s.ceil);
            UnloadModel(s.ceil);
            s.ceil = {};
            s.ceilReady = false;
        }
        if (s.coveReady) {
            detachModelTextures(s.cove);
            UnloadModel(s.cove);
            s.cove = {};
            s.coveReady = false;
        }
    }

    void bindSharedAlbedo(Model& m, Texture2D tex, Color tint) {
        if (tex.id == 0 || m.materialCount <= 0) return;
        for (int i = 0; i < m.materialCount; ++i) {
            m.materials[i].maps[MATERIAL_MAP_ALBEDO].texture = tex;
            m.materials[i].maps[MATERIAL_MAP_ALBEDO].color = tint;
        }
    }

    Texture2D caveCeilTex() {
        // Prefer cliff / ground rock over dark_rock for shell surfaces.
        Texture2D t = props::CaveCliffAlbedo();
        if (t.id != 0) return t;
        t = props::CaveFloorAlbedo();
        if (t.id != 0) return t;
        t = props::CaveMossAlbedo();
        if (t.id != 0) return t;
        return props::CaveWallAlbedo();
    }

    Texture2D caveFloorTexFor(const Room& room) {
        if (room.floor == FloorStyle::Flooded) {
            Texture2D moss = props::CaveMossAlbedo();
            if (moss.id != 0) return moss;
        }
        Texture2D floor = props::CaveFloorAlbedo();
        if (floor.id != 0) return floor;
        return caveCeilTex();
    }

    struct CaveRockInst {
        int     variant = 0;
        Vector3 pos{};
        float   yaw = 0.0f;
        float   scale = 1.0f;
    };
    std::vector<CaveRockInst> g_caveRocks;

    void unloadFloorMeshes() {
        for (auto& s : g_surfaces) unloadSurfaceSlot(s);
        g_surfaces.clear();
        for (auto& s : g_linkSurfaces) unloadSurfaceSlot(s);
        g_linkSurfaces.clear();
        g_linkHeights.clear();
        g_caveRocks.clear();
    }

    float hash01(uint64_t seed) {
        return engine::math::randFloat01(engine::math::splitmix64(seed));
    }

    bool pointInLink(const Transition& link, float x, float z, float margin = 0.25f) {
        const float halfL = link.halfLen + kCorridorOverlap;
        if (link.alongX) {
            return fabsf(x - link.center.x) <= halfL + margin &&
                   fabsf(z - link.center.z) <= link.halfWidth + margin;
        }
        return fabsf(z - link.center.z) <= halfL + margin &&
               fabsf(x - link.center.x) <= link.halfWidth + margin;
    }

    float sampleLinkHeight(const CorridorHeights& h, const Transition& link,
                           float x, float z, bool ceiling) {
        if (h.n < 2) return 0.0f;
        const float along = link.alongX ? (x - link.center.x) : (z - link.center.z);
        // Profile is authored over the geometric gap; overlap zones clamp to door heights.
        float t = (along + link.halfLen) / std::max(0.01f, link.halfLen * 2.0f);
        t = std::clamp(t, 0.0f, 1.0f);
        const float f = t * (float)(h.n - 1);
        const int i0 = (int)floorf(f);
        const int i1 = std::min(i0 + 1, h.n - 1);
        const float u = f - (float)i0;
        const auto& arr = ceiling ? h.ceil : h.floor;
        return arr[(size_t)i0] + (arr[(size_t)i1] - arr[(size_t)i0]) * u;
    }

    Vector3 doorSamplePos(const Room& room, const Transition& link) {
        Vector3 p = room.center;
        const int side = sideForLink(room, link);
        // Sample on the AABB face so corridor ends match the room floor edge.
        if (side == 0 || side == 1) {
            p.x += (side == 0 ? 1.0f : -1.0f) * room.halfW;
            p.z = link.center.z;
        } else {
            p.z += (side == 2 ? 1.0f : -1.0f) * room.halfD;
            p.x = link.center.x;
        }
        return p;
    }

    void sampleRoomDoorHeights(const Room& room, const Transition& link,
                               float& outFloor, float& outCeil) {
        outFloor = 0.0f;
        outCeil  = 0.0f;
        const WalkableMask* m = maskFor(room);
        if (!m) return;
        const Vector3 p = doorSamplePos(room, link);
        outFloor = m->SampleHeightWorld(room.center, p.x, p.z);
        outCeil  = m->SampleCeilHeightWorld(room.center, p.x, p.z);
    }

    Color mixFloorVertColor(const Room& room, int ix, int iz, float hOff) {
        Color base = floorColor(room.type);
        Color accent = floorAccent(room.type);
        Color c = ((ix + iz) & 1) ? accent : base;

        switch (room.floor) {
            case FloorStyle::Mosaic:
                c = ((ix + iz) & 1) ? accent : base;
                break;
            case FloorStyle::Cracked: {
                const uint64_t h = engine::math::splitmix64(
                    g_layout.seed ^ (uint64_t)(room.id * 2654435761u) ^
                    (uint64_t)(ix * 97 + iz * 13));
                c = (engine::math::randFloat01(h) > 0.55f) ? accent : base;
                break;
            }
            case FloorStyle::BloodRing: {
                const float nx = ((float)ix + 0.5f) / std::max(1.0f, (float)(maskFor(room) ? maskFor(room)->nx : 1)) - 0.5f;
                const float nz = ((float)iz + 0.5f) / std::max(1.0f, (float)(maskFor(room) ? maskFor(room)->nz : 1)) - 0.5f;
                const float dist = sqrtf(nx * nx + nz * nz) * 2.0f;
                if (fabsf(dist - 0.55f) < 0.12f) {
                    c = Color{(unsigned char)std::min(255, c.r + 50),
                              (unsigned char)(c.g * 0.45f),
                              (unsigned char)(c.b * 0.45f), 255};
                }
                break;
            }
            case FloorStyle::Flooded:
                c.g = (unsigned char)std::min(255, c.g + 18);
                c.b = (unsigned char)std::min(255, c.b + 12);
                break;
            default:
                break;
        }

        if (g_theme == Theme::Cave) {
            c.r = (unsigned char)(c.r * 0.85f);
            c.g = (unsigned char)(c.g * 0.9f);
            c.b = (unsigned char)std::min(255, (int)c.b + 12);
        }
        if (hOff > 0.25f) {
            c.r = (unsigned char)std::min(255, c.r + 16);
            c.g = (unsigned char)std::min(255, c.g + 12);
        } else if (hOff < -0.25f) {
            c.r = (unsigned char)(c.r * 0.82f);
            c.g = (unsigned char)(c.g * 0.88f);
        }
        return c;
    }

    void buildFloorMesh(const Room& room) {
        if (room.id < 0) return;
        if (room.id >= (int)g_surfaces.size()) g_surfaces.resize((size_t)room.id + 1);

        RoomSurfaceGpu& slot = g_surfaces[(size_t)room.id];
        if (slot.floorReady) {
            UnloadModel(slot.floor);
            slot.floor = {};
            slot.floorReady = false;
        }

        const WalkableMask* mask = maskFor(room);
        if (!mask || !mask->Valid()) return;

        const WalkableMask& m = *mask;
        const float cellW = (room.halfW * 2.0f) / (float)m.nx;
        const float cellD = (room.halfD * 2.0f) / (float)m.nz;

        auto cornerH = [&](int cx, int cz) {
            float sum = 0.0f;
            int n = 0;
            for (int dz = -1; dz <= 0; ++dz) {
                for (int dx = -1; dx <= 0; ++dx) {
                    if (m.At(cx + dx, cz + dz)) {
                        sum += m.HeightAt(cx + dx, cz + dz);
                        ++n;
                    }
                }
            }
            return (n > 0) ? (sum / (float)n) : 0.0f;
        };

        auto cornerPos = [&](int cx, int cz) {
            return Vector3{
                room.center.x - room.halfW + (float)cx * cellW,
                kFloorY + cornerH(cx, cz),
                room.center.z - room.halfD + (float)cz * cellD
            };
        };

        int walkable = 0;
        for (int iz = 0; iz < m.nz; ++iz)
            for (int ix = 0; ix < m.nx; ++ix)
                if (m.At(ix, iz)) ++walkable;
        if (walkable <= 0) return;

        const int vertsPerCell = 12;
        const int vertexCount = walkable * vertsPerCell;
        const int triangleCount = walkable * 4;

        Mesh mesh = {0};
        mesh.vertexCount = vertexCount;
        mesh.triangleCount = triangleCount;
        mesh.vertices  = (float*)MemAlloc((unsigned int)(vertexCount * 3 * (int)sizeof(float)));
        mesh.normals   = (float*)MemAlloc((unsigned int)(vertexCount * 3 * (int)sizeof(float)));
        mesh.texcoords = (float*)MemAlloc((unsigned int)(vertexCount * 2 * (int)sizeof(float)));
        mesh.colors    = (unsigned char*)MemAlloc((unsigned int)(vertexCount * 4 * (int)sizeof(unsigned char)));
        if (!mesh.vertices || !mesh.normals || !mesh.texcoords || !mesh.colors) {
            if (mesh.vertices) MemFree(mesh.vertices);
            if (mesh.normals) MemFree(mesh.normals);
            if (mesh.texcoords) MemFree(mesh.texcoords);
            if (mesh.colors) MemFree(mesh.colors);
            return;
        }

        const bool textured = (g_theme == Theme::Cave && props::CaveKitReady());
        constexpr float kUvScale = 6.0f;

        int v = 0;
        auto emitTri = [&](Vector3 a, Vector3 b, Vector3 c, Color col, bool flip) {
            Vector3 e1 = Vector3Subtract(b, a);
            Vector3 e2 = Vector3Subtract(c, a);
            Vector3 n = Vector3Normalize(Vector3CrossProduct(e1, e2));
            if (flip) n = Vector3Negate(n);
            Vector3 verts[3] = {a, flip ? c : b, flip ? b : c};
            for (int i = 0; i < 3; ++i) {
                mesh.vertices[v * 3 + 0] = verts[i].x;
                mesh.vertices[v * 3 + 1] = verts[i].y;
                mesh.vertices[v * 3 + 2] = verts[i].z;
                mesh.normals[v * 3 + 0] = n.x;
                mesh.normals[v * 3 + 1] = n.y;
                mesh.normals[v * 3 + 2] = n.z;
                mesh.texcoords[v * 2 + 0] = verts[i].x / kUvScale;
                mesh.texcoords[v * 2 + 1] = verts[i].z / kUvScale;
                mesh.colors[v * 4 + 0] = col.r;
                mesh.colors[v * 4 + 1] = col.g;
                mesh.colors[v * 4 + 2] = col.b;
                mesh.colors[v * 4 + 3] = col.a;
                ++v;
            }
        };

        constexpr float kSlab = 0.55f;
        for (int iz = 0; iz < m.nz; ++iz) {
            for (int ix = 0; ix < m.nx; ++ix) {
                if (!m.At(ix, iz)) continue;

                Vector3 p00 = cornerPos(ix, iz);
                Vector3 p10 = cornerPos(ix + 1, iz);
                Vector3 p01 = cornerPos(ix, iz + 1);
                Vector3 p11 = cornerPos(ix + 1, iz + 1);

                const float hAvg = 0.25f * (p00.y + p10.y + p01.y + p11.y - 4.0f * kFloorY);
                Color col = mixFloorVertColor(room, ix, iz, hAvg);
                // Textured caves: keep vertex color near-white so albedo reads.
                if (textured) {
                    col = Color{
                        (unsigned char)std::min(255, 200 + (int)(hAvg * 12.0f)),
                        (unsigned char)std::min(255, 195 + (int)(hAvg * 10.0f)),
                        (unsigned char)std::min(255, 185 + (int)(hAvg * 8.0f)), 255};
                }

                // CCW when viewed from +Y so backface cull keeps the walk surface.
                emitTri(p00, p11, p10, col, false);
                emitTri(p00, p01, p11, col, false);

                Color under = Color{
                    (unsigned char)(col.r * 0.55f),
                    (unsigned char)(col.g * 0.55f),
                    (unsigned char)(col.b * 0.55f), 255};
                Vector3 b00 = {p00.x, p00.y - kSlab, p00.z};
                Vector3 b10 = {p10.x, p10.y - kSlab, p10.z};
                Vector3 b01 = {p01.x, p01.y - kSlab, p01.z};
                Vector3 b11 = {p11.x, p11.y - kSlab, p11.z};
                // Underside faces -Y (visible if you look up from a pit).
                emitTri(b00, b10, b11, under, false);
                emitTri(b00, b11, b01, under, false);
            }
        }

        if (v != vertexCount) {
            MemFree(mesh.vertices);
            MemFree(mesh.normals);
            MemFree(mesh.texcoords);
            MemFree(mesh.colors);
            return;
        }

        UploadMesh(&mesh, false);
        slot.floor = LoadModelFromMesh(mesh);
        slot.floorReady = true;
        if (textured) {
            bindSharedAlbedo(slot.floor, caveFloorTexFor(room), WHITE);
        }
    }

    void buildCeilMesh(const Room& room) {
        if (room.id < 0) return;
        if (room.id >= (int)g_surfaces.size()) g_surfaces.resize((size_t)room.id + 1);

        RoomSurfaceGpu& slot = g_surfaces[(size_t)room.id];
        if (slot.ceilReady) {
            UnloadModel(slot.ceil);
            slot.ceil = {};
            slot.ceilReady = false;
        }

        const WalkableMask* mask = maskFor(room);
        if (!mask || !mask->Valid()) return;

        const WalkableMask& m = *mask;
        const float cellW = (room.halfW * 2.0f) / (float)m.nx;
        const float cellD = (room.halfD * 2.0f) / (float)m.nz;

        auto cornerC = [&](int cx, int cz) {
            float sum = 0.0f;
            int n = 0;
            for (int dz = -1; dz <= 0; ++dz) {
                for (int dx = -1; dx <= 0; ++dx) {
                    if (m.At(cx + dx, cz + dz)) {
                        sum += m.CeilHeightAt(cx + dx, cz + dz);
                        ++n;
                    }
                }
            }
            return (n > 0) ? (sum / (float)n) : 0.0f;
        };

        auto cornerPos = [&](int cx, int cz) {
            return Vector3{
                room.center.x - room.halfW + (float)cx * cellW,
                kCeilY + cornerC(cx, cz),
                room.center.z - room.halfD + (float)cz * cellD
            };
        };

        int walkable = 0;
        for (int iz = 0; iz < m.nz; ++iz)
            for (int ix = 0; ix < m.nx; ++ix)
                if (m.At(ix, iz)) ++walkable;
        if (walkable <= 0) return;

        const int vertsPerCell = 12; // underside (visible) + topside
        const int vertexCount = walkable * vertsPerCell;

        Mesh mesh = {0};
        mesh.vertexCount = vertexCount;
        mesh.triangleCount = walkable * 4;
        mesh.vertices  = (float*)MemAlloc((unsigned int)(vertexCount * 3 * (int)sizeof(float)));
        mesh.normals   = (float*)MemAlloc((unsigned int)(vertexCount * 3 * (int)sizeof(float)));
        mesh.texcoords = (float*)MemAlloc((unsigned int)(vertexCount * 2 * (int)sizeof(float)));
        mesh.colors    = (unsigned char*)MemAlloc((unsigned int)(vertexCount * 4 * (int)sizeof(unsigned char)));
        if (!mesh.vertices || !mesh.normals || !mesh.texcoords || !mesh.colors) {
            if (mesh.vertices) MemFree(mesh.vertices);
            if (mesh.normals) MemFree(mesh.normals);
            if (mesh.texcoords) MemFree(mesh.texcoords);
            if (mesh.colors) MemFree(mesh.colors);
            return;
        }

        const bool textured = (g_theme == Theme::Cave && props::CaveKitReady());
        constexpr float kUvScale = 5.0f;

        int v = 0;
        auto emitTri = [&](Vector3 a, Vector3 b, Vector3 c, Color col) {
            Vector3 e1 = Vector3Subtract(b, a);
            Vector3 e2 = Vector3Subtract(c, a);
            Vector3 n = Vector3Normalize(Vector3CrossProduct(e1, e2));
            Vector3 verts[3] = {a, b, c};
            for (int i = 0; i < 3; ++i) {
                mesh.vertices[v * 3 + 0] = verts[i].x;
                mesh.vertices[v * 3 + 1] = verts[i].y;
                mesh.vertices[v * 3 + 2] = verts[i].z;
                mesh.normals[v * 3 + 0] = n.x;
                mesh.normals[v * 3 + 1] = n.y;
                mesh.normals[v * 3 + 2] = n.z;
                mesh.texcoords[v * 2 + 0] = verts[i].x / kUvScale;
                mesh.texcoords[v * 2 + 1] = verts[i].z / kUvScale;
                mesh.colors[v * 4 + 0] = col.r;
                mesh.colors[v * 4 + 1] = col.g;
                mesh.colors[v * 4 + 2] = col.b;
                mesh.colors[v * 4 + 3] = col.a;
                ++v;
            }
        };

        Color baseCeil = textured ? Color{210, 200, 190, 255}
            : ((g_theme == Theme::Cave) ? Color{42, 38, 34, 255} : Color{36, 34, 42, 255});
        constexpr float kSlab = 0.65f;

        for (int iz = 0; iz < m.nz; ++iz) {
            for (int ix = 0; ix < m.nx; ++ix) {
                if (!m.At(ix, iz)) continue;

                Vector3 p00 = cornerPos(ix, iz);
                Vector3 p10 = cornerPos(ix + 1, iz);
                Vector3 p01 = cornerPos(ix, iz + 1);
                Vector3 p11 = cornerPos(ix + 1, iz + 1);

                const float cAvg = 0.25f * (p00.y + p10.y + p01.y + p11.y) - kCeilY;
                Color col = baseCeil;
                if (!textured) {
                    if (cAvg < -0.4f) {
                        col.r = (unsigned char)(col.r * 0.85f);
                        col.g = (unsigned char)(col.g * 0.85f);
                    } else if (cAvg > 0.5f) {
                        col.r = (unsigned char)std::min(255, col.r + 18);
                        col.g = (unsigned char)std::min(255, col.g + 14);
                        col.b = (unsigned char)std::min(255, col.b + 20);
                    }
                }

                // Underside faces the room (-Y). Topside slab faces outward (+Y).
                emitTri(p00, p10, p11, col);
                emitTri(p00, p11, p01, col);

                Color top = Color{
                    (unsigned char)(col.r * 0.7f),
                    (unsigned char)(col.g * 0.7f),
                    (unsigned char)(col.b * 0.7f), 255};
                Vector3 t00 = {p00.x, p00.y + kSlab, p00.z};
                Vector3 t10 = {p10.x, p10.y + kSlab, p10.z};
                Vector3 t01 = {p01.x, p01.y + kSlab, p01.z};
                Vector3 t11 = {p11.x, p11.y + kSlab, p11.z};
                emitTri(t00, t11, t10, top);
                emitTri(t00, t01, t11, top);
            }
        }

        if (v != vertexCount) {
            MemFree(mesh.vertices);
            MemFree(mesh.normals);
            MemFree(mesh.texcoords);
            MemFree(mesh.colors);
            return;
        }

        UploadMesh(&mesh, false);
        slot.ceil = LoadModelFromMesh(mesh);
        slot.ceilReady = true;
        if (textured) bindSharedAlbedo(slot.ceil, caveCeilTex(), WHITE);
    }

    // Soften the wall/ceiling right angle with a rock cove (two-step bevel).
    void buildCoveMesh(const Room& room) {
        if (g_theme != Theme::Cave) return;
        if (room.id < 0) return;
        if (room.id >= (int)g_surfaces.size()) g_surfaces.resize((size_t)room.id + 1);

        RoomSurfaceGpu& slot = g_surfaces[(size_t)room.id];
        if (slot.coveReady) {
            detachModelTextures(slot.cove);
            UnloadModel(slot.cove);
            slot.cove = {};
            slot.coveReady = false;
        }

        const WalkableMask* mask = maskFor(room);
        if (!mask || !mask->Valid()) return;
        const WalkableMask& m = *mask;

        const float cellW = (room.halfW * 2.0f) / (float)m.nx;
        const float cellD = (room.halfD * 2.0f) / (float)m.nz;
        constexpr float kCoveIn   = 1.35f; // how far the bevel reaches into the room
        constexpr float kCoveDrop = 1.55f; // how far it runs down the wall
        constexpr float kUvScale  = 5.0f;

        struct Vtx { Vector3 p; Vector2 uv; };
        std::vector<Vtx> verts;
        verts.reserve(512);

        auto emitTri = [&](Vector3 a, Vector3 b, Vector3 c) {
            verts.push_back({a, {a.x / kUvScale, a.z / kUvScale}});
            verts.push_back({b, {b.x / kUvScale, b.z / kUvScale}});
            verts.push_back({c, {c.x / kUvScale, c.z / kUvScale}});
        };
        auto emitQuad = [&](Vector3 a, Vector3 b, Vector3 c, Vector3 d) {
            emitTri(a, b, c);
            emitTri(a, c, d);
        };

        auto ceilAt = [&](float wx, float wz) {
            return kCeilY + m.SampleCeilHeightWorld(room.center, wx, wz);
        };

        // Horizontal edges (constant Z)
        for (int iz = 0; iz < m.nz; ++iz) {
            for (int face = 0; face < 2; ++face) {
                const int diz = (face == 0) ? 1 : -1;
                const float inward = (diz > 0) ? -1.0f : 1.0f; // into the room
                int runStart = -1;
                for (int ix = 0; ix <= m.nx; ++ix) {
                    const bool raw = (ix < m.nx) && m.At(ix, iz) && !m.At(ix, iz + diz);
                    const bool edge = raw && !m.IsDoorMouth(ix, iz, 0, diz);
                    if (edge && runStart < 0) runStart = ix;
                    if ((!edge || ix == m.nx) && runStart >= 0) {
                        const int runEnd = ix - 1;
                        const float zWall = room.center.z - room.halfD
                            + ((float)iz + (face == 0 ? 1.0f : 0.0f)) * cellD;
                        // Subdivide run so cove follows ceiling height variation.
                        for (int sx = runStart; sx <= runEnd; ++sx) {
                            const float x0 = room.center.x - room.halfW + (float)sx * cellW;
                            const float x1 = room.center.x - room.halfW + (float)(sx + 1) * cellW;
                            const float zA = zWall + inward * kCoveIn;
                            const float zM = zWall + inward * (kCoveIn * 0.45f);
                            const float zB = zWall + inward * 0.08f;

                            Vector3 a0{x0, ceilAt(x0, zA), zA};
                            Vector3 a1{x1, ceilAt(x1, zA), zA};
                            Vector3 m0{x0, ceilAt(x0, zM) - kCoveDrop * 0.45f, zM};
                            Vector3 m1{x1, ceilAt(x1, zM) - kCoveDrop * 0.45f, zM};
                            Vector3 b0{x0, ceilAt(x0, zB) - kCoveDrop, zB};
                            Vector3 b1{x1, ceilAt(x1, zB) - kCoveDrop, zB};

                            // Two bands: ceiling→mid, mid→wall (soft fillet).
                            if (inward < 0) {
                                emitQuad(a0, a1, m1, m0);
                                emitQuad(m0, m1, b1, b0);
                            } else {
                                emitQuad(a1, a0, m0, m1);
                                emitQuad(m1, m0, b0, b1);
                            }
                        }
                        runStart = -1;
                    }
                }
            }
        }

        // Vertical edges (constant X)
        for (int ix = 0; ix < m.nx; ++ix) {
            for (int face = 0; face < 2; ++face) {
                const int dix = (face == 0) ? 1 : -1;
                const float inward = (dix > 0) ? -1.0f : 1.0f;
                int runStart = -1;
                for (int iz = 0; iz <= m.nz; ++iz) {
                    const bool raw = (iz < m.nz) && m.At(ix, iz) && !m.At(ix + dix, iz);
                    const bool edge = raw && !m.IsDoorMouth(ix, iz, dix, 0);
                    if (edge && runStart < 0) runStart = iz;
                    if ((!edge || iz == m.nz) && runStart >= 0) {
                        const int runEnd = iz - 1;
                        const float xWall = room.center.x - room.halfW
                            + ((float)ix + (face == 0 ? 1.0f : 0.0f)) * cellW;
                        for (int sz = runStart; sz <= runEnd; ++sz) {
                            const float z0 = room.center.z - room.halfD + (float)sz * cellD;
                            const float z1 = room.center.z - room.halfD + (float)(sz + 1) * cellD;
                            const float xA = xWall + inward * kCoveIn;
                            const float xM = xWall + inward * (kCoveIn * 0.45f);
                            const float xB = xWall + inward * 0.08f;

                            Vector3 a0{xA, ceilAt(xA, z0), z0};
                            Vector3 a1{xA, ceilAt(xA, z1), z1};
                            Vector3 m0{xM, ceilAt(xM, z0) - kCoveDrop * 0.45f, z0};
                            Vector3 m1{xM, ceilAt(xM, z1) - kCoveDrop * 0.45f, z1};
                            Vector3 b0{xB, ceilAt(xB, z0) - kCoveDrop, z0};
                            Vector3 b1{xB, ceilAt(xB, z1) - kCoveDrop, z1};

                            if (inward < 0) {
                                emitQuad(a0, a1, m1, m0);
                                emitQuad(m0, m1, b1, b0);
                            } else {
                                emitQuad(a1, a0, m0, m1);
                                emitQuad(m1, m0, b0, b1);
                            }
                        }
                        runStart = -1;
                    }
                }
            }
        }

        if (verts.size() < 3) return;

        Mesh mesh = {0};
        mesh.vertexCount = (int)verts.size();
        mesh.triangleCount = mesh.vertexCount / 3;
        mesh.vertices  = (float*)MemAlloc((unsigned int)(mesh.vertexCount * 3 * (int)sizeof(float)));
        mesh.normals   = (float*)MemAlloc((unsigned int)(mesh.vertexCount * 3 * (int)sizeof(float)));
        mesh.texcoords = (float*)MemAlloc((unsigned int)(mesh.vertexCount * 2 * (int)sizeof(float)));
        mesh.colors    = (unsigned char*)MemAlloc((unsigned int)(mesh.vertexCount * 4 * (int)sizeof(unsigned char)));
        if (!mesh.vertices || !mesh.normals || !mesh.texcoords || !mesh.colors) {
            if (mesh.vertices) MemFree(mesh.vertices);
            if (mesh.normals) MemFree(mesh.normals);
            if (mesh.texcoords) MemFree(mesh.texcoords);
            if (mesh.colors) MemFree(mesh.colors);
            return;
        }

        for (int i = 0; i < mesh.vertexCount; i += 3) {
            Vector3 a = verts[(size_t)i].p;
            Vector3 b = verts[(size_t)i + 1].p;
            Vector3 c = verts[(size_t)i + 2].p;
            Vector3 n = Vector3Normalize(Vector3CrossProduct(
                Vector3Subtract(b, a), Vector3Subtract(c, a)));
            for (int k = 0; k < 3; ++k) {
                const Vtx& vt = verts[(size_t)(i + k)];
                mesh.vertices[(i + k) * 3 + 0] = vt.p.x;
                mesh.vertices[(i + k) * 3 + 1] = vt.p.y;
                mesh.vertices[(i + k) * 3 + 2] = vt.p.z;
                mesh.normals[(i + k) * 3 + 0] = n.x;
                mesh.normals[(i + k) * 3 + 1] = n.y;
                mesh.normals[(i + k) * 3 + 2] = n.z;
                mesh.texcoords[(i + k) * 2 + 0] = vt.uv.x;
                mesh.texcoords[(i + k) * 2 + 1] = vt.uv.y;
                mesh.colors[(i + k) * 4 + 0] = 210;
                mesh.colors[(i + k) * 4 + 1] = 200;
                mesh.colors[(i + k) * 4 + 2] = 190;
                mesh.colors[(i + k) * 4 + 3] = 255;
            }
        }

        UploadMesh(&mesh, false);
        slot.cove = LoadModelFromMesh(mesh);
        slot.coveReady = true;
        bindSharedAlbedo(slot.cove, caveCeilTex(), WHITE);
    }

    void buildCorridorCove(int linkIndex) {
        if (g_theme != Theme::Cave) return;
        if (linkIndex < 0 || linkIndex >= (int)g_layout.links.size()) return;
        if (linkIndex >= (int)g_linkSurfaces.size()) g_linkSurfaces.resize((size_t)linkIndex + 1);
        if (linkIndex >= (int)g_linkHeights.size() || g_linkHeights[(size_t)linkIndex].n < 2) return;

        RoomSurfaceGpu& slot = g_linkSurfaces[(size_t)linkIndex];
        if (slot.coveReady) {
            detachModelTextures(slot.cove);
            UnloadModel(slot.cove);
            slot.cove = {};
            slot.coveReady = false;
        }

        const Transition& link = g_layout.links[(size_t)linkIndex];
        const CorridorHeights& h = g_linkHeights[(size_t)linkIndex];
        constexpr float kCoveIn = 0.95f;
        constexpr float kCoveDrop = 1.25f;
        constexpr float kUvScale = 5.0f;
        const float halfSpan = link.halfLen + kCorridorMeshOverlap;
        const int segs = std::max(4, h.n);

        struct Vtx { Vector3 p; Vector2 uv; };
        std::vector<Vtx> verts;
        verts.reserve((size_t)segs * 24);

        auto emitTri = [&](Vector3 a, Vector3 b, Vector3 c) {
            verts.push_back({a, {a.x / kUvScale, a.z / kUvScale}});
            verts.push_back({b, {b.x / kUvScale, b.z / kUvScale}});
            verts.push_back({c, {c.x / kUvScale, c.z / kUvScale}});
        };
        auto emitQuad = [&](Vector3 a, Vector3 b, Vector3 c, Vector3 d) {
            emitTri(a, b, c);
            emitTri(a, c, d);
        };

        auto ceilAtAlong = [&](float along) {
            float t = (along + link.halfLen) / std::max(0.01f, link.halfLen * 2.0f);
            t = std::clamp(t, 0.0f, 1.0f);
            const float f = t * (float)(h.n - 1);
            const int i0 = (int)floorf(f);
            const int i1 = std::min(i0 + 1, h.n - 1);
            const float u = f - (float)i0;
            return kCeilY + h.ceil[(size_t)i0] + (h.ceil[(size_t)i1] - h.ceil[(size_t)i0]) * u
                 + kCorridorCeilBias;
        };

        for (int side = 0; side < 2; ++side) {
            const float inward = (side == 0) ? 1.0f : -1.0f; // toward corridor center
            for (int i = 0; i < segs - 1; ++i) {
                const float t0 = (float)i / (float)(segs - 1);
                const float t1 = (float)(i + 1) / (float)(segs - 1);
                const float along0 = -halfSpan + t0 * halfSpan * 2.0f;
                const float along1 = -halfSpan + t1 * halfSpan * 2.0f;
                if (g_theme != Theme::Cave && link.style == CorridorStyle::Ruined) {
                    const float tm = 0.5f * (t0 + t1);
                    if (fabsf(tm - 0.5f) < 0.18f) continue;
                }

                auto pos = [&](float along, float across, float y) {
                    if (link.alongX) return Vector3{link.center.x + along, y, link.center.z + across};
                    return Vector3{link.center.x + across, y, link.center.z + along};
                };

                // side 0 = -halfWidth wall (inward +), side 1 = +halfWidth wall (inward -)
                const float wallA = (side == 0) ? -link.halfWidth : link.halfWidth;
                const float aA = wallA + inward * kCoveIn;
                const float aM = wallA + inward * (kCoveIn * 0.45f);
                const float aB = wallA + inward * 0.08f;

                Vector3 pA0 = pos(along0, aA, ceilAtAlong(along0));
                Vector3 pA1 = pos(along1, aA, ceilAtAlong(along1));
                Vector3 pM0 = pos(along0, aM, ceilAtAlong(along0) - kCoveDrop * 0.45f);
                Vector3 pM1 = pos(along1, aM, ceilAtAlong(along1) - kCoveDrop * 0.45f);
                Vector3 pB0 = pos(along0, aB, ceilAtAlong(along0) - kCoveDrop);
                Vector3 pB1 = pos(along1, aB, ceilAtAlong(along1) - kCoveDrop);

                if (inward > 0) {
                    emitQuad(pA0, pA1, pM1, pM0);
                    emitQuad(pM0, pM1, pB1, pB0);
                } else {
                    emitQuad(pA1, pA0, pM0, pM1);
                    emitQuad(pM1, pM0, pB0, pB1);
                }
            }
        }

        if (verts.size() < 3) return;

        Mesh mesh = {0};
        mesh.vertexCount = (int)verts.size();
        mesh.triangleCount = mesh.vertexCount / 3;
        mesh.vertices  = (float*)MemAlloc((unsigned int)(mesh.vertexCount * 3 * (int)sizeof(float)));
        mesh.normals   = (float*)MemAlloc((unsigned int)(mesh.vertexCount * 3 * (int)sizeof(float)));
        mesh.texcoords = (float*)MemAlloc((unsigned int)(mesh.vertexCount * 2 * (int)sizeof(float)));
        mesh.colors    = (unsigned char*)MemAlloc((unsigned int)(mesh.vertexCount * 4 * (int)sizeof(unsigned char)));
        if (!mesh.vertices || !mesh.normals || !mesh.texcoords || !mesh.colors) {
            if (mesh.vertices) MemFree(mesh.vertices);
            if (mesh.normals) MemFree(mesh.normals);
            if (mesh.texcoords) MemFree(mesh.texcoords);
            if (mesh.colors) MemFree(mesh.colors);
            return;
        }

        for (int i = 0; i < mesh.vertexCount; i += 3) {
            Vector3 a = verts[(size_t)i].p;
            Vector3 b = verts[(size_t)i + 1].p;
            Vector3 c = verts[(size_t)i + 2].p;
            Vector3 n = Vector3Normalize(Vector3CrossProduct(
                Vector3Subtract(b, a), Vector3Subtract(c, a)));
            for (int k = 0; k < 3; ++k) {
                const Vtx& vt = verts[(size_t)(i + k)];
                mesh.vertices[(i + k) * 3 + 0] = vt.p.x;
                mesh.vertices[(i + k) * 3 + 1] = vt.p.y;
                mesh.vertices[(i + k) * 3 + 2] = vt.p.z;
                mesh.normals[(i + k) * 3 + 0] = n.x;
                mesh.normals[(i + k) * 3 + 1] = n.y;
                mesh.normals[(i + k) * 3 + 2] = n.z;
                mesh.texcoords[(i + k) * 2 + 0] = vt.uv.x;
                mesh.texcoords[(i + k) * 2 + 1] = vt.uv.y;
                mesh.colors[(i + k) * 4 + 0] = 210;
                mesh.colors[(i + k) * 4 + 1] = 200;
                mesh.colors[(i + k) * 4 + 2] = 190;
                mesh.colors[(i + k) * 4 + 3] = 255;
            }
        }

        UploadMesh(&mesh, false);
        slot.cove = LoadModelFromMesh(mesh);
        slot.coveReady = true;
        bindSharedAlbedo(slot.cove, caveCeilTex(), WHITE);
    }

    void buildCorridorHeights(int linkIndex) {
        if (linkIndex < 0 || linkIndex >= (int)g_layout.links.size()) return;
        if (linkIndex >= (int)g_linkHeights.size()) g_linkHeights.resize((size_t)linkIndex + 1);

        const Transition& link = g_layout.links[(size_t)linkIndex];
        CorridorHeights& h = g_linkHeights[(size_t)linkIndex];

        const int n = std::max(5, (int)ceilf(link.halfLen * 2.0f / 1.6f) + 1);
        h.n = n;
        h.floor.assign((size_t)n, 0.0f);
        h.ceil.assign((size_t)n, 0.0f);

        float f0 = 0.0f, f1 = 0.0f, c0 = 0.0f, c1 = 0.0f;
        if (link.fromRoom >= 0 && link.fromRoom < (int)g_layout.rooms.size())
            sampleRoomDoorHeights(g_layout.rooms[(size_t)link.fromRoom], link, f0, c0);
        if (link.toRoom >= 0 && link.toRoom < (int)g_layout.rooms.size())
            sampleRoomDoorHeights(g_layout.rooms[(size_t)link.toRoom], link, f1, c1);

        const bool cave = (g_theme == Theme::Cave);
        const uint64_t linkHash = engine::math::splitmix64(
            g_layout.seed ^ (uint64_t)((linkIndex + 3) * 2246822519u));

        float floorMid = 0.0f;
        float ceilMid  = 0.0f;
        if (cave) {
            floorMid = -0.15f - hash01(linkHash) * 0.35f;
            ceilMid  = -0.7f - hash01(linkHash ^ 7u) * 1.1f; // sagging tunnel
        } else {
            floorMid = 0.0f;
            ceilMid  = 0.55f + hash01(linkHash) * 0.7f; // barrel vault rise
        }
        if (link.style == CorridorStyle::Flooded) floorMid -= 0.28f;
        if (link.style == CorridorStyle::Ruined)  floorMid += 0.12f;
        if (link.style == CorridorStyle::Choke)   ceilMid *= 0.55f;

        constexpr float kNominalClear = 10.0f;
        constexpr float kMinClear = 3.4f;

        for (int i = 0; i < n; ++i) {
            const float t = (n == 1) ? 0.0f : (float)i / (float)(n - 1);
            const float bell = sinf(t * 3.14159265f); // 0 at ends, 1 mid
            float fl = f0 + (f1 - f0) * t + floorMid * bell;
            float cl = c0 + (c1 - c0) * t + ceilMid * bell;

            if (link.style == CorridorStyle::Ruined) {
                // Masonry only: open mid-span roof. Caves keep a continuous rock ceiling
                // (open void reads as missing geometry against the clear color).
                if (!cave && fabsf(t - 0.5f) < 0.18f) cl = 4.0f;
            }

            // Keep headroom.
            const float clear = kNominalClear + cl - fl;
            if (clear < kMinClear) cl = fl + kMinClear - kNominalClear;

            // Soft noise so corridors aren't perfectly smooth.
            const float n0 = (hash01(linkHash ^ (uint64_t)(i * 13 + 1)) - 0.5f) * (cave ? 0.35f : 0.12f);
            const float n1 = (hash01(linkHash ^ (uint64_t)(i * 17 + 5)) - 0.5f) * (cave ? 0.45f : 0.1f);
            fl += n0;
            cl += n1;

            h.floor[(size_t)i] = fl;
            h.ceil[(size_t)i]  = cl;
        }
        // Force ends to match room sockets exactly (smooth transition).
        h.floor[0] = f0; h.ceil[0] = c0;
        h.floor[(size_t)n - 1] = f1; h.ceil[(size_t)n - 1] = c1;
    }

    void buildCorridorMeshes(int linkIndex) {
        if (linkIndex < 0 || linkIndex >= (int)g_layout.links.size()) return;
        if (linkIndex >= (int)g_linkSurfaces.size()) g_linkSurfaces.resize((size_t)linkIndex + 1);
        if (linkIndex >= (int)g_linkHeights.size() || g_linkHeights[(size_t)linkIndex].n < 2) return;

        RoomSurfaceGpu& slot = g_linkSurfaces[(size_t)linkIndex];
        unloadSurfaceSlot(slot);

        const Transition& link = g_layout.links[(size_t)linkIndex];
        const CorridorHeights& h = g_linkHeights[(size_t)linkIndex];
        const int nAlong = h.n;
        const int nAcross = 5; // denser across width so edges don't drop out
        const int cellsAlong = nAlong - 1;
        const int cellsAcross = nAcross - 1;
        const int cellCount = cellsAlong * cellsAcross;
        if (cellCount <= 0) return;

        // Visual span slightly enters rooms to seal the door threshold.
        // Y-bias below avoids z-fight with room floor/ceil in that strip.
        const float halfSpan = link.halfLen + kCorridorMeshOverlap;

        auto heightAtAlong = [&](float along, bool ceiling) {
            float t = (along + link.halfLen) / std::max(0.01f, link.halfLen * 2.0f);
            t = std::clamp(t, 0.0f, 1.0f);
            const float f = t * (float)(h.n - 1);
            const int i0 = (int)floorf(f);
            const int i1 = std::min(i0 + 1, h.n - 1);
            const float u = f - (float)i0;
            const auto& arr = ceiling ? h.ceil : h.floor;
            return arr[(size_t)i0] + (arr[(size_t)i1] - arr[(size_t)i0]) * u;
        };

        auto cornerXZ = [&](int ia, int ic, float& x, float& z, float& along) {
            const float t = (float)ia / (float)(nAlong - 1);
            along = -halfSpan + t * halfSpan * 2.0f;
            const float acrossT = (float)ic / (float)(nAcross - 1);
            const float across = -link.halfWidth + acrossT * link.halfWidth * 2.0f;
            if (link.alongX) { x = link.center.x + along; z = link.center.z + across; }
            else             { x = link.center.x + across; z = link.center.z + along; }
        };

        auto emitStrip = [&](bool ceiling) -> Model {
            Model empty{};
            // Ruined mid-gap: masonry can skip ceil cells; caves always keep a roof.
            int emitCells = 0;
            for (int ia = 0; ia < cellsAlong; ++ia) {
                const float tMid = ((float)ia + 0.5f) / (float)cellsAlong;
                if (ceiling && g_theme != Theme::Cave &&
                    link.style == CorridorStyle::Ruined && fabsf(tMid - 0.5f) < 0.18f)
                    continue;
                emitCells += cellsAcross;
            }
            if (emitCells <= 0) return empty;

            const bool textured = (g_theme == Theme::Cave && props::CaveKitReady());
            constexpr float kUvScale = 5.5f;
            constexpr float kSlab = 0.45f;
            Color base = textured ? Color{210, 200, 190, 255}
                : (ceiling
                    ? ((g_theme == Theme::Cave) ? Color{42, 38, 34, 255} : Color{36, 34, 42, 255})
                    : Color{38, 38, 46, 255});
            if (!ceiling && !textured) {
                if (link.style == CorridorStyle::Flooded) base = Color{34, 52, 58, 255};
                else if (link.style == CorridorStyle::Ruined) base = Color{48, 42, 40, 255};
                else if (link.style == CorridorStyle::Choke) base = Color{42, 40, 48, 255};
                if (g_theme == Theme::Cave) {
                    base.r = (unsigned char)(base.r * 0.9f + 8);
                    base.g = (unsigned char)(base.g * 0.88f + 6);
                    base.b = (unsigned char)(base.b * 0.82f);
                }
            }

            // Floors get an under-slab; ceilings get a topside slab so cull can't erase them.
            const int vertsPerCell = 12;
            const int vertexCount = emitCells * vertsPerCell;
            Mesh mesh = {0};
            mesh.vertexCount = vertexCount;
            mesh.triangleCount = emitCells * 4;
            mesh.vertices  = (float*)MemAlloc((unsigned int)(vertexCount * 3 * (int)sizeof(float)));
            mesh.normals   = (float*)MemAlloc((unsigned int)(vertexCount * 3 * (int)sizeof(float)));
            mesh.texcoords = (float*)MemAlloc((unsigned int)(vertexCount * 2 * (int)sizeof(float)));
            mesh.colors    = (unsigned char*)MemAlloc((unsigned int)(vertexCount * 4 * (int)sizeof(unsigned char)));
            if (!mesh.vertices || !mesh.normals || !mesh.texcoords || !mesh.colors) {
                if (mesh.vertices) MemFree(mesh.vertices);
                if (mesh.normals) MemFree(mesh.normals);
                if (mesh.texcoords) MemFree(mesh.texcoords);
                if (mesh.colors) MemFree(mesh.colors);
                return empty;
            }

            int v = 0;
            auto emitTri = [&](Vector3 a, Vector3 b, Vector3 c, Color col) {
                Vector3 e1 = Vector3Subtract(b, a);
                Vector3 e2 = Vector3Subtract(c, a);
                Vector3 nrm = Vector3Normalize(Vector3CrossProduct(e1, e2));
                Vector3 verts[3] = {a, b, c};
                for (int i = 0; i < 3; ++i) {
                    mesh.vertices[v * 3 + 0] = verts[i].x;
                    mesh.vertices[v * 3 + 1] = verts[i].y;
                    mesh.vertices[v * 3 + 2] = verts[i].z;
                    mesh.normals[v * 3 + 0] = nrm.x;
                    mesh.normals[v * 3 + 1] = nrm.y;
                    mesh.normals[v * 3 + 2] = nrm.z;
                    mesh.texcoords[v * 2 + 0] = verts[i].x / kUvScale;
                    mesh.texcoords[v * 2 + 1] = verts[i].z / kUvScale;
                    mesh.colors[v * 4 + 0] = col.r;
                    mesh.colors[v * 4 + 1] = col.g;
                    mesh.colors[v * 4 + 2] = col.b;
                    mesh.colors[v * 4 + 3] = col.a;
                    ++v;
                }
            };

            for (int ia = 0; ia < cellsAlong; ++ia) {
                const float tMid = ((float)ia + 0.5f) / (float)cellsAlong;
                if (ceiling && g_theme != Theme::Cave &&
                    link.style == CorridorStyle::Ruined && fabsf(tMid - 0.5f) < 0.18f)
                    continue;

                for (int ic = 0; ic < cellsAcross; ++ic) {
                    float x00, z00, x10, z10, x01, z01, x11, z11;
                    float a00, a10, a01, a11;
                    cornerXZ(ia, ic, x00, z00, a00);
                    cornerXZ(ia + 1, ic, x10, z10, a10);
                    cornerXZ(ia, ic + 1, x01, z01, a01);
                    cornerXZ(ia + 1, ic + 1, x11, z11, a11);

                    const float y00 = ceiling ? kCeilY + heightAtAlong(a00, true) + kCorridorCeilBias
                                             : kFloorY + heightAtAlong(a00, false) + kCorridorFloorBias;
                    const float y10 = ceiling ? kCeilY + heightAtAlong(a10, true) + kCorridorCeilBias
                                             : kFloorY + heightAtAlong(a10, false) + kCorridorFloorBias;
                    const float y01 = ceiling ? kCeilY + heightAtAlong(a01, true) + kCorridorCeilBias
                                             : kFloorY + heightAtAlong(a01, false) + kCorridorFloorBias;
                    const float y11 = ceiling ? kCeilY + heightAtAlong(a11, true) + kCorridorCeilBias
                                             : kFloorY + heightAtAlong(a11, false) + kCorridorFloorBias;

                    Vector3 p00{x00, y00, z00};
                    Vector3 p10{x10, y10, z10};
                    Vector3 p01{x01, y01, z01};
                    Vector3 p11{x11, y11, z11};

                    if (ceiling) {
                        // Underside facing into the corridor (-Y) + topside slab.
                        emitTri(p00, p10, p11, base);
                        emitTri(p00, p11, p01, base);
                        Color top = Color{
                            (unsigned char)(base.r * 0.7f),
                            (unsigned char)(base.g * 0.7f),
                            (unsigned char)(base.b * 0.7f), 255};
                        constexpr float kCeilSlab = 0.55f;
                        Vector3 t00{x00, y00 + kCeilSlab, z00};
                        Vector3 t10{x10, y10 + kCeilSlab, z10};
                        Vector3 t01{x01, y01 + kCeilSlab, z01};
                        Vector3 t11{x11, y11 + kCeilSlab, z11};
                        emitTri(t00, t11, t10, top);
                        emitTri(t00, t01, t11, top);
                    } else {
                        // Walk surface facing +Y (CCW from above) + under-slab.
                        emitTri(p00, p11, p10, base);
                        emitTri(p00, p01, p11, base);
                        Color under = Color{
                            (unsigned char)(base.r * 0.55f),
                            (unsigned char)(base.g * 0.55f),
                            (unsigned char)(base.b * 0.55f), 255};
                        Vector3 b00{x00, y00 - kSlab, z00};
                        Vector3 b10{x10, y10 - kSlab, z10};
                        Vector3 b01{x01, y01 - kSlab, z01};
                        Vector3 b11{x11, y11 - kSlab, z11};
                        emitTri(b00, b10, b11, under);
                        emitTri(b00, b11, b01, under);
                    }
                }
            }

            if (v != vertexCount) {
                MemFree(mesh.vertices);
                MemFree(mesh.normals);
                MemFree(mesh.texcoords);
                MemFree(mesh.colors);
                return empty;
            }
            UploadMesh(&mesh, false);
            Model model = LoadModelFromMesh(mesh);
            if (textured) {
                Texture2D tex = ceiling ? caveCeilTex() : (
                    link.style == CorridorStyle::Flooded ? props::CaveMossAlbedo()
                                                        : props::CaveFloorAlbedo());
                if (tex.id == 0) tex = caveCeilTex();
                bindSharedAlbedo(model, tex, WHITE);
            }
            return model;
        };

        slot.floor = emitStrip(false);
        slot.floorReady = (slot.floor.meshCount > 0);
        slot.ceil = emitStrip(true);
        slot.ceilReady = (slot.ceil.meshCount > 0);
    }

    void scatterCaveRocks() {
        g_caveRocks.clear();
        if (g_theme != Theme::Cave || props::MossRockCount() <= 0) return;

        for (const auto& room : g_layout.rooms) {
            const WalkableMask* mask = maskFor(room);
            if (!mask || !mask->Valid()) continue;

            int count = 3;
            if (room.size == SizeTier::Large) count = 7;
            else if (room.size == SizeTier::Small) count = 2;
            if (room.type == RoomType::Boss) count += 3;
            if (room.type == RoomType::SafeHaven || room.type == RoomType::Entrance)
                count = std::max(2, count - 2);

            const uint64_t roomHash = engine::math::splitmix64(
                g_layout.seed ^ (uint64_t)(room.id * 2654435761u) ^ 0x524F434Bu);
            for (int i = 0; i < count; ++i) {
                float lx = 0.0f, lz = 0.0f;
                if (!mask->SampleWalkableLocal(roomHash, i * 17 + 3, lx, lz)) continue;
                // Keep clear of room center and door mouths.
                if (fabsf(lx) < room.halfW * 0.18f && fabsf(lz) < room.halfD * 0.18f) continue;
                if (fabsf(fabsf(lx) - room.halfW) < 3.5f && fabsf(lz) < kDoorHalf + 1.0f) continue;
                if (fabsf(fabsf(lz) - room.halfD) < 3.5f && fabsf(lx) < kDoorHalf + 1.0f) continue;

                const float wx = room.center.x + lx;
                const float wz = room.center.z + lz;
                const float gy = kFloorY + mask->SampleHeightWorld(room.center, wx, wz);

                CaveRockInst rock;
                rock.variant = (int)(engine::math::splitmix64(roomHash ^ (uint64_t)(i * 91 + 11)) %
                                     (uint64_t)props::MossRockCount());
                rock.pos = Vector3{wx, gy, wz};
                rock.yaw = engine::math::randFloat01(
                    engine::math::splitmix64(roomHash ^ (uint64_t)(i * 33 + 7))) * 360.0f;
                rock.scale = 0.85f + engine::math::randFloat01(
                    engine::math::splitmix64(roomHash ^ (uint64_t)(i * 19 + 5))) * 0.7f;
                g_caveRocks.push_back(rock);
            }
        }
    }

    void buildAllFloorMeshes() {
        // Room surfaces only — corridor meshes baked separately after heights exist.
        for (auto& s : g_surfaces) unloadSurfaceSlot(s);
        g_surfaces.clear();
        g_surfaces.resize(g_layout.rooms.size());
        for (const auto& room : g_layout.rooms) {
            buildFloorMesh(room);
            buildCeilMesh(room);
            buildCoveMesh(room);
        }
    }

    void buildAllCorridorMeshes() {
        for (auto& s : g_linkSurfaces) unloadSurfaceSlot(s);
        g_linkSurfaces.clear();
        g_linkSurfaces.resize(g_layout.links.size());
        for (size_t i = 0; i < g_layout.links.size(); ++i) {
            buildCorridorMeshes((int)i);
            buildCorridorCove((int)i);
        }
    }

    // Solid floor/ceil plugs at every door mouth so cave mask inset can't open a
    // black void between room heightfield and corridor deck.
    void buildDoorThresholdPlugs() {
        const bool cave = (g_theme == Theme::Cave);
        for (size_t li = 0; li < g_layout.links.size(); ++li) {
            const Transition& link = g_layout.links[li];
            float floorOff = 0.0f;
            float ceilOff  = 0.0f;
            if (li < g_linkHeights.size() && g_linkHeights[li].n >= 2) {
                floorOff = sampleLinkHeight(g_linkHeights[li], link,
                                            link.center.x, link.center.z, false);
                ceilOff  = sampleLinkHeight(g_linkHeights[li], link,
                                            link.center.x, link.center.z, true);
            }
            const float floorY = kFloorY + floorOff - 0.12f;
            const float ceilY  = kCeilY + ceilOff + 0.12f;
            const float width  = link.halfWidth * 2.0f + 1.0f;
            // Reach from well inside each room, across the gap, into the other room.
            const float span = link.halfLen * 2.0f + kCorridorOverlap * 2.0f + 2.5f;

            Vector3 floorC = link.center;
            floorC.y = floorY;
            Vector3 ceilC = link.center;
            ceilC.y = ceilY;
            Vector3 floorSize = link.alongX ? Vector3{span, 0.55f, width}
                                           : Vector3{width, 0.55f, span};
            Vector3 ceilSize  = link.alongX ? Vector3{span + 0.4f, 0.45f, width + 0.4f}
                                           : Vector3{width + 0.4f, 0.45f, span + 0.4f};
            addSolid(floorC, floorSize, cave ? WHITE : Color{42, 42, 50, 255}, false, cave);
            addSolid(ceilC,  ceilSize,  cave ? WHITE : Color{36, 34, 42, 255}, false, cave);
        }
    }

    void extrudeMaskWalls(const Room& room, const WalkableMask& mask) {
        // Extend walls below base floor so pits don't open under the shell,
        // and above nominal ceiling so vault rises stay enclosed.
        float maxCeil = 0.0f;
        for (float c : mask.ceilHeights) if (c > maxCeil) maxCeil = c;
        const float wallBottom = kFloorY - 2.5f;
        const float wallTop    = kCeilY + maxCeil + 0.8f;
        const float wallH      = wallTop - wallBottom;
        const float wallY      = (wallBottom + wallTop) * 0.5f;
        const bool cave = (g_theme == Theme::Cave);
        const float plinthH  = cave ? 0.85f : 1.15f;
        const float corniceH = 0.85f;
        const uint64_t roomHash = engine::math::splitmix64(
            g_layout.seed ^ (uint64_t)(room.id * 2654435761u));

        auto addWallSeg = [&](Vector3 c, Vector3 size, int runCells) {
            float thick = kWallT;
            if (cave) {
                const float n = hash01(roomHash ^ (uint64_t)((int)(c.x * 10) * 73856093 ^
                                                             (int)(c.z * 10) * 19349663));
                thick = kWallT + 0.25f + n * 0.55f;
            }
            if (size.x > size.z) size.z = thick;
            else                 size.x = thick;
            size.y = wallH;
            c.y = wallY;
            addSolid(c, size, wallColor(), true, cave);

            // Base trim: masonry plinth vs rough cave footing.
            Vector3 pc = c;
            pc.y = wallBottom + plinthH * 0.5f;
            Vector3 ps = size;
            ps.y = plinthH;
            if (size.x > size.z) { ps.z += cave ? 0.55f : 0.35f; }
            else                 { ps.x += cave ? 0.55f : 0.35f; }
            addDecor(pc, ps, plinthColor(), cave);

            if (!cave) {
                Vector3 cc = c;
                cc.y = wallTop - corniceH * 0.5f;
                Vector3 cs = size;
                cs.y = corniceH;
                if (size.x > size.z) { cs.z += 0.45f; } else { cs.x += 0.45f; }
                addDecor(cc, cs, corniceColor());

                // Pilasters on long dressed runs.
                if (runCells >= 4) {
                    const float span = (size.x > size.z) ? size.x : size.z;
                    for (float d = -span * 0.35f; d <= span * 0.35f + 0.01f; d += 5.5f) {
                        Vector3 col = c;
                        Vector3 cs2 = size;
                        if (size.x > size.z) {
                            col.x += d;
                            cs2.x = 0.7f;
                            cs2.z += 0.25f;
                        } else {
                            col.z += d;
                            cs2.z = 0.7f;
                            cs2.x += 0.25f;
                        }
                        cs2.y = wallH * 0.92f;
                        col.y = wallY;
                        addDecor(col, cs2, corniceColor());
                    }
                }
            } else {
                // Rock bulges along the face — breaks the extruded-box read.
                const int bulges = 1 + runCells / 3;
                for (int b = 0; b < bulges; ++b) {
                    const float u = hash01(roomHash ^ (uint64_t)(b * 911 + (int)(c.x * 3) + (int)(c.z * 7)));
                    const float v = hash01(roomHash ^ (uint64_t)(b * 417 + 99));
                    Vector3 rock = c;
                    const float along = ((u - 0.5f) * ((size.x > size.z) ? size.x : size.z)) * 0.7f;
                    if (size.x > size.z) rock.x += along;
                    else                 rock.z += along;
                    rock.y = wallBottom + 1.5f + v * (wallH - 3.0f);
                    const float rw = 0.9f + u * 1.1f;
                    const float rh = 0.8f + v * 1.4f;
                    const float rd = thick * 0.55f + 0.35f;
                    Vector3 rs = (size.x > size.z) ? Vector3{rw, rh, rd} : Vector3{rd, rh, rw};
                    addDecor(rock, rs, WHITE, true);
                }
            }
        };

        const float cellW = (room.halfW * 2.0f) / (float)mask.nx;
        const float cellD = (room.halfD * 2.0f) / (float)mask.nz;

        // Merge consecutive exterior edges into longer SolidBoxes (fewer draws).
        for (int iz = 0; iz < mask.nz; ++iz) {
            for (int face = 0; face < 2; ++face) {
                const int diz = (face == 0) ? 1 : -1;
                int runStart = -1;
                for (int ix = 0; ix <= mask.nx; ++ix) {
                    const bool rawEdge = (ix < mask.nx) && mask.At(ix, iz) && !mask.At(ix, iz + diz);
                    const bool edge = rawEdge && !mask.IsDoorMouth(ix, iz, 0, diz);
                    if (edge && runStart < 0) runStart = ix;
                    if ((!edge || ix == mask.nx) && runStart >= 0) {
                        const int runEnd = ix - 1;
                        const float x0 = room.center.x - room.halfW + ((float)runStart) * cellW;
                        const float x1 = room.center.x - room.halfW + ((float)(runEnd + 1)) * cellW;
                        const float z = room.center.z - room.halfD
                                      + ((float)iz + (face == 0 ? 1.0f : 0.0f)) * cellD;
                        addWallSeg(Vector3{(x0 + x1) * 0.5f, wallY, z},
                                   Vector3{x1 - x0 + kWallT * 0.15f, wallH, kWallT},
                                   runEnd - runStart + 1);
                        runStart = -1;
                    }
                }
            }
        }

        for (int ix = 0; ix < mask.nx; ++ix) {
            for (int face = 0; face < 2; ++face) {
                const int dix = (face == 0) ? 1 : -1;
                int runStart = -1;
                for (int iz = 0; iz <= mask.nz; ++iz) {
                    const bool rawEdge = (iz < mask.nz) && mask.At(ix, iz) && !mask.At(ix + dix, iz);
                    const bool edge = rawEdge && !mask.IsDoorMouth(ix, iz, dix, 0);
                    if (edge && runStart < 0) runStart = iz;
                    if ((!edge || iz == mask.nz) && runStart >= 0) {
                        const int runEnd = iz - 1;
                        const float z0 = room.center.z - room.halfD + ((float)runStart) * cellD;
                        const float z1 = room.center.z - room.halfD + ((float)(runEnd + 1)) * cellD;
                        const float x = room.center.x - room.halfW
                                      + ((float)ix + (face == 0 ? 1.0f : 0.0f)) * cellW;
                        addWallSeg(Vector3{x, wallY, (z0 + z1) * 0.5f},
                                   Vector3{kWallT, wallH, z1 - z0 + kWallT * 0.15f},
                                   runEnd - runStart + 1);
                        runStart = -1;
                    }
                }
            }
        }

        // Door frames: dressed lintel (masonry) vs rock overhang teeth (cave).
        for (const auto& link : g_layout.links) {
            if (link.fromRoom != room.id && link.toRoom != room.id) continue;
            const int side = sideForLink(room, link);
            Vector3 mouth = room.center;
            if (side == 0 || side == 1) {
                mouth.x += (side == 0 ? room.halfW : -room.halfW);
                mouth.z = link.center.z;
            } else {
                mouth.z += (side == 2 ? room.halfD : -room.halfD);
                mouth.x = link.center.x;
            }

            if (!cave) {
                Vector3 lintel = mouth;
                lintel.y = kCeilY - 1.1f;
                Vector3 lsize = (side == 0 || side == 1)
                    ? Vector3{kWallT + 0.6f, 1.4f, kDoorHalf * 2.0f + 1.2f}
                    : Vector3{kDoorHalf * 2.0f + 1.2f, 1.4f, kWallT + 0.6f};
                addSolid(lintel, lsize, corniceColor(), false);
            } else {
                for (int t = 0; t < 3; ++t) {
                    const float u = hash01(roomHash ^ (uint64_t)(link.fromRoom * 31 + link.toRoom * 17 + t * 13));
                    const float across = ((float)t - 1.0f) * (kDoorHalf * 0.7f);
                    Vector3 tooth = mouth;
                    tooth.y = kCeilY - 0.4f - u * 1.1f;
                    if (side == 0 || side == 1) tooth.z += across;
                    else                       tooth.x += across;
                    const float inward = (side == 0 || side == 2) ? -0.35f : 0.35f;
                    if (side == 0 || side == 1) tooth.x += inward;
                    else                       tooth.z += inward;
                    Vector3 ts = (side == 0 || side == 1)
                        ? Vector3{1.1f + u * 0.6f, 0.7f + u * 0.5f, 1.0f + u * 0.4f}
                        : Vector3{1.0f + u * 0.4f, 0.7f + u * 0.5f, 1.1f + u * 0.6f};
                    addDecor(tooth, ts, WHITE, true);
                }
            }
        }
    }

    void buildRoomWalls(engine::ecs::Registry& /*reg*/, const Room& room) {
        if (room.id >= 0 && room.id < (int)g_masks.size() && g_masks[(size_t)room.id].Valid()) {
            extrudeMaskWalls(room, g_masks[(size_t)room.id]);
            return;
        }

        // Fallback: simple AABB box room (intermission / missing mask).
        bool hasDoor[4] = {false, false, false, false};
        for (const auto& link : g_layout.links) {
            if (link.fromRoom != room.id && link.toRoom != room.id) continue;
            hasDoor[sideForLink(room, link)] = true;
        }

        const float wallY = kFloorY + kWallH * 0.5f;
        const bool cave = (g_theme == Theme::Cave);
        auto addWallSeg = [&](Vector3 c, Vector3 size) {
            addSolid(c, size, wallColor(), true, cave);
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
                if (xSide) { c.z += sign * offset; size = Vector3{kWallT, kWallH, segLen}; }
                else       { c.x += sign * offset; size = Vector3{segLen, kWallH, kWallT}; }
                addWallSeg(c, size);
            }
        }
    }

    void buildCorridorWalls(engine::ecs::Registry& /*reg*/, const Transition& link, int linkIndex) {
        const bool cave = (g_theme == Theme::Cave);
        float maxCeilOff = 0.0f;
        if (linkIndex >= 0 && linkIndex < (int)g_linkHeights.size()) {
            for (float c : g_linkHeights[(size_t)linkIndex].ceil)
                if (c > maxCeilOff) maxCeilOff = c;
        }
        const float wallBottom = kFloorY - 1.5f;
        const float wallTop = kCeilY + maxCeilOff + (cave ? 0.6f : 0.5f);
        const float wallH = wallTop - wallBottom;
        const float wallY  = (wallBottom + wallTop) * 0.5f;
        const float offset = link.halfWidth + (cave ? kWallT * 0.65f : kWallT * 0.5f);
        // Reach into both rooms so corridor walls meet the door jambs.
        const float len = link.halfLen * 2.0f + kCorridorOverlap * 2.0f + kWallT * 2.0f;
        // Cave ruined corridors keep continuous walls (mid gaps read as missing geometry).
        const bool ruinedGap = (link.style == CorridorStyle::Ruined) && !cave;
        const uint64_t linkHash = engine::math::splitmix64(
            g_layout.seed ^ (uint64_t)((linkIndex + 1) * 2654435761u));

        auto addCorridorSeg = [&](Vector3 c, Vector3 size) {
            if (cave) {
                const float n = hash01(linkHash ^ (uint64_t)((int)(c.x * 8) * 91 + (int)(c.z * 8) * 57));
                if (link.alongX) size.z = kWallT + 0.2f + n * 0.5f;
                else             size.x = kWallT + 0.2f + n * 0.5f;
            }
            size.y = wallH;
            c.y = wallY;
            addSolid(c, size, wallColor(), true, cave);

            Vector3 pc = c;
            pc.y = wallBottom + (cave ? 0.55f : 0.55f);
            Vector3 ps = size;
            ps.y = cave ? 1.0f : 1.1f;
            if (link.alongX) ps.z += cave ? 0.45f : 0.3f;
            else             ps.x += cave ? 0.45f : 0.3f;
            addDecor(pc, ps, plinthColor(), cave);

            if (!cave) {
                Vector3 cc = c;
                cc.y = wallTop - 0.4f;
                Vector3 cs = size;
                cs.y = 0.75f;
                if (link.alongX) cs.z += 0.35f;
                else             cs.x += 0.35f;
                addDecor(cc, cs, corniceColor());
            } else {
                const int bulges = 2;
                for (int b = 0; b < bulges; ++b) {
                    const float u = hash01(linkHash ^ (uint64_t)(b * 33 + (int)(c.x + c.z)));
                    Vector3 rock = c;
                    const float along = (u - 0.5f) * ((link.alongX ? size.x : size.z) * 0.6f);
                    if (link.alongX) rock.x += along;
                    else             rock.z += along;
                    rock.y = wallBottom + 2.0f + u * (wallH - 4.0f);
                    Vector3 rs = link.alongX
                        ? Vector3{1.0f + u, 1.0f + u * 0.8f, size.z * 0.55f}
                        : Vector3{size.x * 0.55f, 1.0f + u * 0.8f, 1.0f + u};
                    addDecor(rock, rs, WHITE, true);
                }
            }
        };

        for (int s = 0; s < 2; ++s) {
            const float sign = (s == 0) ? 1.0f : -1.0f;
            Vector3 c = link.center;
            Vector3 size;
            if (link.alongX) {
                c.z += sign * offset;
                size = Vector3{len, wallH, kWallT};
            } else {
                c.x += sign * offset;
                size = Vector3{kWallT, wallH, len};
            }

            if (ruinedGap) {
                const float halfGap = std::min(2.2f, link.halfLen * 0.35f);
                const float seg = (len * 0.5f) - halfGap;
                if (seg > 0.5f) {
                    for (int e = 0; e < 2; ++e) {
                        Vector3 sc = c;
                        const float along = (e == 0 ? -1.0f : 1.0f) * (halfGap + seg * 0.5f);
                        if (link.alongX) sc.x += along;
                        else             sc.z += along;
                        Vector3 ss = size;
                        if (link.alongX) ss.x = seg;
                        else             ss.z = seg;
                        addCorridorSeg(sc, ss);
                    }
                }
                for (int r = 0; r < 2; ++r) {
                    Vector3 rubble = link.center;
                    if (link.alongX) {
                        rubble.x += (r == 0 ? -0.8f : 0.9f);
                        rubble.z += sign * (link.halfWidth * 0.35f);
                    } else {
                        rubble.z += (r == 0 ? -0.8f : 0.9f);
                        rubble.x += sign * (link.halfWidth * 0.35f);
                    }
                    rubble.y = kFloorY + 0.55f;
                    addSolid(rubble, Vector3{1.1f, 1.1f, 1.1f}, cave ? WHITE : plinthColor(), false, cave);
                }
            } else {
                addCorridorSeg(c, size);
            }
        }

        // Door jambs at both corridor ends — seal flanks where room mouth meets passage.
        for (int e = 0; e < 2; ++e) {
            const float along = (e == 0 ? -1.0f : 1.0f) * (link.halfLen + kCorridorOverlap * 0.35f);
            for (int s = 0; s < 2; ++s) {
                const float side = (s == 0) ? -1.0f : 1.0f;
                Vector3 jamb = link.center;
                if (link.alongX) {
                    jamb.x += along;
                    jamb.z += side * (link.halfWidth + kWallT * 0.35f);
                } else {
                    jamb.z += along;
                    jamb.x += side * (link.halfWidth + kWallT * 0.35f);
                }
                jamb.y = wallY;
                Vector3 js = link.alongX
                    ? Vector3{kWallT * 1.4f + kCorridorOverlap * 0.5f, wallH, kWallT * 1.1f}
                    : Vector3{kWallT * 1.1f, wallH, kWallT * 1.4f + kCorridorOverlap * 0.5f};
                addSolid(jamb, js, cave ? WHITE : wallColor(), true, cave);
            }
        }

        // Choke: masonry door posts vs cave squeeze pinch.
        if (link.style == CorridorStyle::Choke && !link.locked) {
            for (int e = 0; e < 2; ++e) {
                Vector3 post = link.center;
                post.y = wallY;
                if (link.alongX) post.x += (e == 0 ? -1.0f : 1.0f) * (link.halfLen - 0.4f);
                else             post.z += (e == 0 ? -1.0f : 1.0f) * (link.halfLen - 0.4f);
                if (cave) {
                    const float pinch = link.halfWidth * 0.35f;
                    if (link.alongX) {
                        addDecor(Vector3{post.x, wallY, post.z - link.halfWidth + pinch * 0.5f},
                                 Vector3{1.2f, wallH * 0.7f, pinch + 0.4f}, WHITE, true);
                        addDecor(Vector3{post.x, wallY, post.z + link.halfWidth - pinch * 0.5f},
                                 Vector3{1.2f, wallH * 0.7f, pinch + 0.4f}, WHITE, true);
                    } else {
                        addDecor(Vector3{post.x - link.halfWidth + pinch * 0.5f, wallY, post.z},
                                 Vector3{pinch + 0.4f, wallH * 0.7f, 1.2f}, WHITE, true);
                        addDecor(Vector3{post.x + link.halfWidth - pinch * 0.5f, wallY, post.z},
                                 Vector3{pinch + 0.4f, wallH * 0.7f, 1.2f}, WHITE, true);
                    }
                } else if (link.alongX) {
                    addDecor(Vector3{post.x, wallY, post.z - link.halfWidth},
                             Vector3{0.7f, wallH, 0.7f}, corniceColor());
                    addDecor(Vector3{post.x, wallY, post.z + link.halfWidth},
                             Vector3{0.7f, wallH, 0.7f}, corniceColor());
                } else {
                    addDecor(Vector3{post.x - link.halfWidth, wallY, post.z},
                             Vector3{0.7f, wallH, 0.7f}, corniceColor());
                    addDecor(Vector3{post.x + link.halfWidth, wallY, post.z},
                             Vector3{0.7f, wallH, 0.7f}, corniceColor());
                }
            }
        }

        if (!link.locked) return;

        Vector3 c = link.center;
        c.y = wallY;

        const bool hidden = (link.requires_ == GateRequirement::Search);
        const Color color = hidden ? (cave ? WHITE : wallColor()) : kGateColor;
        const Vector3 size = link.alongX
            ? Vector3{hidden ? kWallT : kWallT * 0.7f, wallH, link.halfWidth * 2.0f + kWallT}
            : Vector3{link.halfWidth * 2.0f + kWallT, wallH, hidden ? kWallT : kWallT * 0.7f};

        Gate gate;
        gate.linkIndex = linkIndex;
        gate.req       = link.requires_;
        gate.solidIndex = addSolid(c, size, color, true, hidden && cave);

        if (!hidden) {
            Vector3 bar = c;
            bar.y = kFloorY + 4.5f;
            const Vector3 barSize = link.alongX
                ? Vector3{kWallT * 1.2f, 0.55f, link.halfWidth * 2.0f + 0.4f}
                : Vector3{link.halfWidth * 2.0f + 0.4f, 0.55f, kWallT * 1.2f};
            gate.barSolidIndex = addSolid(bar, barSize, kGateDark, false);
        }
        g_gates.push_back(gate);
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
        const float gy = GroundY(t.position.x, t.position.z);
        t.position.y = gy + reg.renderables.Get(e).height * 0.5f;
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

    void spawnVaultKey(engine::ecs::Registry& /*reg*/, const Room& room) {
        float hx = 0.0f, hz = 0.0f;
        float gy = kFloorY;
        if (const WalkableMask* m = maskFor(room);
            m && m->SampleWalkableLocal(g_layout.seed ^ (uint64_t)room.id, 77, hx, hz)) {
            gy = kFloorY + m->SampleHeightLocal(hx, hz);
            g_keyPos = Vector3{room.center.x + hx, gy + 1.1f, room.center.z + hz};
        } else {
            g_keyPos = Vector3{room.center.x, kFloorY + 1.1f, room.center.z};
        }
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
        // SizeTier scales packs lightly so Large rooms stay explorative.
        float sizePack = 1.0f;
        switch (room.size) {
            case SizeTier::Small:  sizePack = 0.85f; break;
            case SizeTier::Large:  sizePack = 1.25f; break;
            default:               sizePack = 1.0f;  break;
        }
        const float countMul = currentStage().enemyCountMul * g_modCount * sizePack;
        // Capped so stacked count modifiers can't build a frame-rate wrecking mob.
        auto scaled = [countMul](int base) {
            return std::min(12, std::max(1, (int)lroundf((float)base * countMul)));
        };

        auto spawnAt = [&](int salt, float radiusHint) {
            float lx = 0.0f, lz = 0.0f;
            if (const WalkableMask* m = maskFor(room);
                m && m->SampleWalkableLocal(g_layout.seed ^ (uint64_t)room.id, salt, lx, lz)) {
                // Pull slightly toward center so packs don't hug doorways.
                lx *= 0.75f;
                lz *= 0.75f;
                return Vector3{room.center.x + lx, kFloorY, room.center.z + lz};
            }
            return ringPos(salt, 8, radiusHint);
        };

        switch (room.type) {
            case RoomType::Combat: {
                const int count = scaled(3 + (int)(rand01(11) * 2.99f));  // 3-5 before scaling
                // "Champion packs" can promote an ordinary room to an elite pack.
                if (g_modElite > 0.0f && rand01(77) < g_modElite) {
                    spawnElite(reg, spawnAt(0, room.halfW * 0.25f));
                    for (int i = 1; i < count; ++i) spawnNormal(reg, spawnAt(i + 10, room.halfW * 0.55f));
                } else {
                    for (int i = 0; i < count; ++i) spawnNormal(reg, spawnAt(i + 20, room.halfW * 0.5f));
                }
                break;
            }
            case RoomType::Elite: {
                spawnElite(reg, spawnAt(0, room.halfW * 0.2f));
                const int count = scaled(2 + (int)(rand01(23) * 1.99f));  // 2-3 before scaling
                for (int i = 0; i < count; ++i) spawnNormal(reg, spawnAt(i + 30, room.halfW * 0.6f));
                break;
            }
            case RoomType::Boss: {
                g_boss = spawnBoss(reg, spawnAt(0, room.halfW * 0.15f));
                g_bossSpawned = true;
                const int adds = scaled(2);
                for (int i = 0; i < adds; ++i) spawnNormal(reg, spawnAt(i + 40, room.halfW * 0.65f));
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
        if (const WalkableMask* m = maskFor(room)) {
            return m->ContainsWorld(room.center, p, margin);
        }
        const float lx = p.x - room.center.x;
        const float lz = p.z - room.center.z;
        return fabsf(lx) <= room.halfW + margin &&
               fabsf(lz) <= room.halfD + margin;
    }

    // Same mask used for floor tiles so visuals match walkability.
    bool tileInShape(const Room& room, float wx, float wz) {
        return insideRoom(room, Vector3{wx, kFloorY, wz}, -0.15f);
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
                // Theme is fixed to the entrance so discovery / portal / run all agree.
                const float themeRoll = engine::math::randFloat01(
                    engine::math::splitmix64((uint64_t)ent.seed ^ 0x71E11E01ULL));
                ent.theme = (themeRoll < 0.45f) ? Theme::Cave : Theme::Masonry;
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

        const bool cave = (ent.theme == Theme::Cave);
        const Color stone  = cave ? Color{ 72,  64,  54, 255} : Color{ 78,  74,  86, 255};
        const Color dark   = cave ? Color{ 36,  32,  28, 255} : Color{ 42,  40,  48, 255};
        const Color accent = cave ? Color{ 90, 140, 120, 255} : Color{120,  90, 170, 255};
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

        // Portal veil — purple for masonry, teal for caves.
        Vector3 veil = {ent.pos.x, ent.pos.y + pillarH * 0.5f + 0.7f, ent.pos.z};
        BeginBlendMode(BLEND_ALPHA);
        if (cave) {
            DrawCube(veil, 5.0f, pillarH - 0.4f, 0.35f,
                     Color{40, (unsigned char)(110 + 50 * pulse),
                           (unsigned char)(100 + 40 * pulse), (unsigned char)(140 + 40 * pulse)});
        } else {
            DrawCube(veil, 5.0f, pillarH - 0.4f, 0.35f,
                     Color{(unsigned char)(90 + 50 * pulse), 50,
                           (unsigned char)(160 + 70 * pulse), (unsigned char)(140 + 40 * pulse)});
        }
        EndBlendMode();
        BeginBlendMode(BLEND_ADDITIVE);
        const Color glow = cave ? Color{100, 220, 180, (unsigned char)(90 + 70 * pulse)}
                                : Color{180, 120, 255, (unsigned char)(90 + 70 * pulse)};
        DrawSphere(Vector3{ent.pos.x, ent.pos.y + pillarH + 3.4f, ent.pos.z},
                   1.0f + 0.3f * pulse, glow);
        DrawSphere(veil, 1.8f, cave ? Color{60, 140, 110, 40} : Color{120, 70, 220, 40});
        EndBlendMode();
    }
}

void DrawEntrancePrompt(Vector3 playerPos) {
    if (g_active) return;
    const int idx = FindNearbyEntrance(playerPos, 7.0f);
    if (idx < 0) return;

    const Entrance& ent = GetEntrances()[(size_t)idx];
    char msg[96];
    std::snprintf(msg, sizeof(msg), "[E]  Enter %s", ThemeName(ent.theme));
    const int   fs   = 26;
    const int   w    = MeasureText(msg, fs);
    const int   x    = GetScreenWidth() / 2 - w / 2;
    const int   y    = GetScreenHeight() - 150;
    DrawRectangle(x - 14, y - 10, w + 28, fs + 20, Color{0, 0, 0, 150});
    DrawText(msg, x, y, fs, ent.theme == Theme::Cave ? Color{190, 235, 220, 255}
                                                      : Color{235, 220, 255, 255});
}

bool  IsActive() { return g_active; }
float FloorY()   { return kFloorY; }

float GroundY(float x, float z) {
    if (!g_active) return engine::math::WorldHeight(x, z);

    const Vector3 p{x, kFloorY, z};
    for (const auto& room : g_layout.rooms) {
        if (const WalkableMask* m = maskFor(room)) {
            if (m->ContainsWorld(room.center, p, 0.15f)) {
                return kFloorY + m->SampleHeightWorld(room.center, x, z);
            }
        } else if (insideRoom(room, p, 0.15f)) {
            return kFloorY;
        }
    }
    for (size_t i = 0; i < g_layout.links.size(); ++i) {
        const Transition& link = g_layout.links[i];
        if (!pointInLink(link, x, z, 0.2f)) continue;
        if (i < g_linkHeights.size() && g_linkHeights[i].n >= 2) {
            return kFloorY + sampleLinkHeight(g_linkHeights[i], link, x, z, false);
        }
        return kFloorY;
    }
    return kFloorY;
}

void ResolvePlayerCollision(Vector3& position, float radius, float height) {
    if (!g_active) return;

    const float pMinX = position.x - radius;
    const float pMaxX = position.x + radius;
    const float pMinY = position.y;
    const float pMaxY = position.y + height;
    const float pMinZ = position.z - radius;
    const float pMaxZ = position.z + radius;

    for (const auto& s : g_solids) {
        if (!s.active || !s.collide) continue;

        const float halfW = s.w * 0.5f;
        const float halfH = s.h * 0.5f;
        const float halfD = s.d * 0.5f;
        const float oMinX = s.center.x - halfW;
        const float oMaxX = s.center.x + halfW;
        const float oMinY = s.center.y - halfH;
        const float oMaxY = s.center.y + halfH;
        const float oMinZ = s.center.z - halfD;
        const float oMaxZ = s.center.z + halfD;

        if (pMaxX <= oMinX || pMinX >= oMaxX ||
            pMaxY <= oMinY || pMinY >= oMaxY ||
            pMaxZ <= oMinZ || pMinZ >= oMaxZ) {
            continue;
        }

        const float pushL = pMaxX - oMinX;
        const float pushR = oMaxX - pMinX;
        const float pushB = pMaxZ - oMinZ;
        const float pushF = oMaxZ - pMinZ;

        float minPush = pushL;
        int axis = 0;
        if (pushR < minPush) { minPush = pushR; axis = 1; }
        if (pushB < minPush) { minPush = pushB; axis = 2; }
        if (pushF < minPush) { minPush = pushF; axis = 3; }

        if      (axis == 0) position.x -= minPush;
        else if (axis == 1) position.x += minPush;
        else if (axis == 2) position.z -= minPush;
        else                position.z += minPush;
    }
}

const Layout& GetLayout() { return g_layout; }
int  CurrentRoom()        { return g_currentRoom; }
int  CurrentStage()       { return g_stageIndex; }
int  StageCount()         { return (int)GetCampaign().stages.size(); }
bool HasVaultKey()        { return g_hasKey; }
bool InIntermission()     { return g_intermission; }
Theme CurrentTheme()      { return g_theme; }
uint32_t CurrentSeed()    { return g_baseSeed; }

const std::vector<const ModifierDef*>& ActiveModifiers() { return g_activeMods; }

DefectProbe ProbeDefect(float x, float z) {
    DefectProbe out;
    out.world = {x, 0.0f, z};
    if (!g_active) return out;

    out.inDungeon = true;
    out.seed  = g_baseSeed;
    out.theme = g_theme;
    out.stage = g_stageIndex;
    out.groundY = GroundY(x, z);
    out.world.y = out.groundY;

    const Vector3 p{x, kFloorY, z};
    for (const auto& room : g_layout.rooms) {
        if (!insideRoom(room, p, 0.5f)) continue;
        out.roomId = room.id;
        out.roomType = roomName(room.type);
        out.localX = x - room.center.x;
        out.localZ = z - room.center.z;
        if (const WalkableMask* m = maskFor(room)) {
            out.maskNx = m->nx;
            out.maskNz = m->nz;
            out.walkable = m->ContainsLocal(out.localX, out.localZ, 0.0f);
            out.floorOff = m->SampleHeightLocal(out.localX, out.localZ);
            out.ceilOff  = m->SampleCeilHeightLocal(out.localX, out.localZ);
            if (m->Valid() && m->halfW > 0.01f && m->halfD > 0.01f) {
                const float u = (out.localX + m->halfW) / (m->halfW * 2.0f);
                const float v = (out.localZ + m->halfD) / (m->halfD * 2.0f);
                out.maskIx = (int)floorf(u * (float)m->nx);
                out.maskIz = (int)floorf(v * (float)m->nz);
            }
        } else {
            out.walkable = fabsf(out.localX) <= room.halfW && fabsf(out.localZ) <= room.halfD;
        }
        break;
    }

    for (size_t i = 0; i < g_layout.links.size(); ++i) {
        if (!pointInLink(g_layout.links[i], x, z, 0.35f)) continue;
        out.linkIndex = (int)i;
        if (i < g_linkHeights.size() && g_linkHeights[i].n >= 2) {
            out.floorOff = sampleLinkHeight(g_linkHeights[i], g_layout.links[i], x, z, false);
            out.ceilOff  = sampleLinkHeight(g_linkHeights[i], g_layout.links[i], x, z, true);
        }
        break;
    }

    if (out.roomId < 0 && g_currentRoom >= 0 && g_currentRoom < (int)g_layout.rooms.size()) {
        const Room& room = g_layout.rooms[(size_t)g_currentRoom];
        out.roomId = room.id;
        out.roomType = roomName(room.type);
        out.localX = x - room.center.x;
        out.localZ = z - room.center.z;
    }
    return out;
}

namespace {

    bool isSessionAuthority() {
        return !engine::networking::HasRemotePeer() || engine::networking::IsHost();
    }

    void hostBroadcast(engine::networking::DungeonOp op) {
        if (!engine::networking::IsHost() || !engine::networking::HasRemotePeer()) return;
        engine::networking::DungeonSyncMsg msg;
        msg.op    = op;
        msg.seed  = g_baseSeed;
        msg.theme = static_cast<uint8_t>(g_theme);
        msg.stage = static_cast<uint8_t>(
            (op == engine::networking::DungeonOp::Intermission) ? g_intermissionNext : g_stageIndex);
        engine::networking::BroadcastDungeonSync(msg);
    }

    // Tear down the current stage's geometry and inhabitants (player stays put).
    void clearStageEntities(engine::ecs::Registry& reg) {
        g_solids.clear();
        g_gates.clear();
        g_walls.clear();
        g_masks.clear();
        unloadFloorMeshes();
        destroyAllEnemies(reg);
        g_keySpawned = false;
        g_keyPos     = Vector3{};
    }

    // Generate + stamp one stage and drop the player at its entrance.
    void buildStage(engine::ecs::Registry& reg, engine::ecs::Entity player) {
        g_solids.clear();
        g_gates.clear();
        g_keySpawned = false;
        g_keyPos     = Vector3{};
        g_intermission = false;
        g_lootPending  = false;
        g_lootTaken    = false;
        g_lootReward   = 0.0f;

        const uint32_t stageSeed = (uint32_t)(engine::math::splitmix64(
            (uint64_t)g_baseSeed ^ ((uint64_t)(g_stageIndex + 1) * 0x9E3779B97F4A7C15ULL)) >> 16);

        rollModifiers(stageSeed);

        GenProfile profile = GetGenProfile();
        profile.maxBranches += currentStage().extraBranches;

        g_layout = Generate(stageSeed, profile);
        for (auto& r : g_layout.rooms) r.center.z += kRegionZ;
        for (auto& l : g_layout.links) l.center.z += kRegionZ;

        // Stage 2 (Flooded Ossuary) biases wet floors / flooded corridors.
        // Cave themes also prefer wet/cracked floors on combat rooms.
        if (g_stageIndex == 1 || g_theme == Theme::Cave) {
            for (auto& r : g_layout.rooms) {
                if (r.type == RoomType::Combat || r.type == RoomType::Entrance) {
                    if (g_theme == Theme::Cave && g_stageIndex != 1) {
                        r.floor = (r.id & 1) ? FloorStyle::Cracked : FloorStyle::Flooded;
                    } else {
                        r.floor = FloorStyle::Flooded;
                    }
                }
            }
            if (g_stageIndex == 1) {
                for (auto& l : g_layout.links) {
                    if (l.style == CorridorStyle::Standard && !l.locked) {
                        l.style = CorridorStyle::Flooded;
                    }
                }
            }
        }

        g_boss        = engine::ecs::Entity{0};
        g_bossSpawned = false;
        g_hasKey      = false;

        // Theme masks from required graph sockets, then extrude to SolidBoxes.
        g_masks.clear();
        g_masks.resize(g_layout.rooms.size());
        const float maskCell = GetGenProfile().maskCellSize;
        for (const auto& room : g_layout.rooms) {
            const uint32_t roomSeed = g_layout.seed ^ (uint32_t)(room.id * 2654435761u) ^
                                     ((uint32_t)g_theme * 0x9E37u);
            g_masks[(size_t)room.id] = BuildMask(
                g_theme, room.size, room.halfW, room.halfD,
                socketsForRoom(room), roomSeed, maskCell);
        }

        unloadFloorMeshes();
        g_linkHeights.resize(g_layout.links.size());
        for (size_t i = 0; i < g_layout.links.size(); ++i) {
            buildCorridorHeights((int)i);
        }

        for (const auto& room : g_layout.rooms) buildRoomWalls(reg, room);
        for (size_t i = 0; i < g_layout.links.size(); ++i) {
            buildCorridorWalls(reg, g_layout.links[i], (int)i);
        }
        buildDoorThresholdPlugs();
        buildAllFloorMeshes();
        buildAllCorridorMeshes();
        scatterCaveRocks();

        const Room& start = g_layout.rooms[g_layout.entranceRoom];
        if (reg.transforms.Has(player)) {
            auto& t = reg.transforms.Get(player);
            float gy = kFloorY;
            if (const WalkableMask* m = maskFor(start)) {
                gy = kFloorY + m->SampleHeightWorld(start.center, start.center.x, start.center.z);
            }
            t.position = Vector3{start.center.x, gy + 2.0f, start.center.z};
        }
        if (reg.playerInputs.Has(player)) {
            auto& in = reg.playerInputs.Get(player);
            in.velocityY = 0.0f;
            in.grounded  = true;
        }
        g_currentRoom = g_layout.entranceRoom;
    }

    void buildIntermissionHaven(engine::ecs::Registry& reg, engine::ecs::Entity player) {
        g_solids.clear();
        g_gates.clear();
        g_walls.clear();
        unloadFloorMeshes();
        destroyAllEnemies(reg);
        g_keySpawned = false;
        g_hasKey     = false;
        g_boss       = engine::ecs::Entity{0};
        g_bossSpawned = false;
        g_activeMods.clear();
        g_lootPending = false;

        g_layout = Layout{};
        g_layout.seed = g_baseSeed ^ (0xA11CEu + (uint32_t)g_stageIndex * 97u);
        Room haven;
        haven.id = 0;
        haven.type = RoomType::SafeHaven;
        haven.shape = RoomShape::Rect;
        haven.floor = FloorStyle::Mosaic;
        haven.center = Vector3{0.0f, kFloorY, kRegionZ};
        haven.halfW = 18.0f;
        haven.halfD = 18.0f;
        haven.size = SizeTier::Small;
        haven.populated = true;
        haven.cleared = true;
        haven.visited = true;
        g_layout.rooms.push_back(haven);
        g_layout.entranceRoom = 0;
        g_layout.bossRoom = -1;
        g_layout.extractRoom = -1;
        g_layout.keyRoom = -1;

        g_masks.clear();
        g_masks.resize(1);
        g_masks[0] = BuildMask(g_theme, haven.size, haven.halfW, haven.halfD,
                               0, g_layout.seed, GetGenProfile().maskCellSize);
        buildRoomWalls(reg, haven);
        buildAllFloorMeshes();

        if (reg.transforms.Has(player)) {
            auto& t = reg.transforms.Get(player);
            float gy = kFloorY;
            if (const WalkableMask* m = maskFor(haven)) {
                gy = kFloorY + m->SampleHeightWorld(haven.center, haven.center.x, haven.center.z);
            }
            t.position = Vector3{haven.center.x, gy + 2.0f, haven.center.z};
        }
        if (reg.playerInputs.Has(player)) {
            auto& in = reg.playerInputs.Get(player);
            in.velocityY = 0.0f;
            in.grounded  = true;
        }
        if (reg.healths.Has(player)) {
            auto& hp = reg.healths.Get(player);
            hp.current = std::min(hp.max, hp.current + hp.max * 0.5f);
        }
        g_currentRoom = 0;
        g_intermission = true;
    }

    void beginIntermission(engine::ecs::Registry& reg, engine::ecs::Entity player) {
        events::Publish(events::Event{
            events::Type::StageComplete, -1, g_stageIndex, 0.0f});
        g_intermissionNext = g_stageIndex + 1;
        clearStageEntities(reg);
        buildIntermissionHaven(reg, player);
        events::Publish(events::Event{
            events::Type::IntermissionStarted, 0, g_intermissionNext, 0.0f});
        char msg[128];
        std::snprintf(msg, sizeof(msg), "Checkpoint  -  rest before stage %d/%d",
                      g_intermissionNext + 1, StageCount());
        setBanner(msg);
        hostBroadcast(engine::networking::DungeonOp::Intermission);
    }

    void advanceFromIntermission(engine::ecs::Registry& reg, engine::ecs::Entity player) {
        if (!g_intermission) return;
        g_stageIndex = g_intermissionNext;
        g_intermission = false;
        buildStage(reg, player);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "Descending  -  stage %d/%d: %s",
                      g_stageIndex + 1, StageCount(), currentStage().name.c_str());
        setBanner(msg);
        hostBroadcast(engine::networking::DungeonOp::Stage);
    }

    void spawnEndLoot(const Room& extract) {
        g_lootPending = true;
        g_lootTaken   = false;
        float gy = kFloorY;
        if (const WalkableMask* m = maskFor(extract)) {
            gy = kFloorY + m->SampleHeightWorld(extract.center, extract.center.x, extract.center.z);
        }
        g_lootPos     = Vector3{extract.center.x, gy + 0.9f, extract.center.z};
        g_lootReward  = 100.0f * g_rewardMul;
        events::Publish(events::Event{
            events::Type::RunComplete, extract.id, g_stageIndex, g_lootReward});
        setBanner("Spoils chest appeared - press E to claim");
    }

    void claimLootAndExit(engine::ecs::Registry& reg, engine::ecs::Entity player) {
        if (!g_lootPending || g_lootTaken) return;
        g_lootTaken   = true;
        g_lootPending = false;
        events::Publish(events::Event{
            events::Type::LootClaimed, -1, g_stageIndex, g_lootReward});
        char msg[128];
        std::snprintf(msg, sizeof(msg), "Claimed spoils  -  +%.0f reward (x%.2f)",
                      g_lootReward, g_rewardMul);
        setBanner(msg);
        hostBroadcast(engine::networking::DungeonOp::Exit);
        Exit(reg, player);
    }

}  // namespace

void Enter(engine::ecs::Registry& reg, engine::ecs::Entity player, const Entrance& entrance) {
    if (g_active) return;
    if (!reg.transforms.Has(player)) return;

    LoadData();
    props::Load();
    events::Reset();

    g_overworldPos    = reg.transforms.Get(player).position;
    g_hasOverworldPos = true;

    // Overworld enemies do not follow the party inside.
    destroyAllEnemies(reg);

    g_baseSeed   = entrance.seed;
    g_theme      = entrance.theme;
    g_stageIndex = 0;
    g_intermission = false;
    g_lootPending  = false;
    g_lootTaken    = false;
    g_walls.clear();
    g_gates.clear();
    buildStage(reg, player);

    g_active = true;

    char msg[128];
    std::snprintf(msg, sizeof(msg), "%s  -  %s  -  stage 1/%d: %s",
                  ThemeName(g_theme), GetCampaign().name.c_str(),
                  StageCount(), currentStage().name.c_str());
    setBanner(msg);

    if (isSessionAuthority()) {
        hostBroadcast(engine::networking::DungeonOp::Enter);
    }
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
    g_theme       = Theme::Masonry;
    g_hasKey      = false;
    g_intermission = false;
    g_lootPending  = false;
    g_lootTaken    = false;
    g_activeMods.clear();
    props::Unload();
}

void BroadcastLifecycle(uint8_t op) {
    hostBroadcast(static_cast<engine::networking::DungeonOp>(op));
}

void ApplyNetworkSync(engine::ecs::Registry& reg, engine::ecs::Entity player,
                      uint8_t op, uint32_t seed, uint8_t theme, uint8_t stage) {
    using Op = engine::networking::DungeonOp;
    const auto dop = static_cast<Op>(op);

    if (dop == Op::Enter) {
        if (g_active) return;
        Entrance e;
        e.pos   = reg.transforms.Has(player) ? reg.transforms.Get(player).position : Vector3{};
        e.seed  = seed;
        e.theme = static_cast<Theme>(theme);
        e.index = -1;
        Enter(reg, player, e);
        return;
    }

    if (!g_active) return;

    if (dop == Op::Exit) {
        Exit(reg, player);
        return;
    }

    if (dop == Op::Intermission) {
        g_intermissionNext = (int)stage;
        clearStageEntities(reg);
        buildIntermissionHaven(reg, player);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "Checkpoint  -  rest before stage %d/%d",
                      g_intermissionNext + 1, StageCount());
        setBanner(msg);
        return;
    }

    if (dop == Op::Stage) {
        g_stageIndex   = (int)stage;
        g_intermission = false;
        buildStage(reg, player);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "Descending  -  stage %d/%d: %s",
                      g_stageIndex + 1, StageCount(), currentStage().name.c_str());
        setBanner(msg);
    }
}

void Update(engine::ecs::Registry& reg, engine::ecs::Entity player) {
    const float dt = GetFrameTime();
    if (g_bannerTimer > 0.0f) g_bannerTimer -= dt;

    // Clients apply host-authoritative dungeon lifecycle first.
    if (!isSessionAuthority()) {
        engine::networking::DungeonSyncMsg sync;
        while (engine::networking::GetPendingDungeonSync(sync)) {
            ApplyNetworkSync(reg, player,
                             static_cast<uint8_t>(sync.op), sync.seed, sync.theme, sync.stage);
        }
    }

    if (!g_active) return;
    if (!reg.transforms.Has(player)) return;

    const Vector3 p = reg.transforms.Get(player).position;
    g_playerDrawPos = p;

    // Wipe: bail back to the overworld instead of stranding the corpse inside.
    if (reg.healths.Has(player)) {
        auto& hp = reg.healths.Get(player);
        if (hp.current <= 0.0f) {
            hp.current = hp.max * 0.25f;
            setBanner("You were driven out of the dungeon");
            if (isSessionAuthority()) hostBroadcast(engine::networking::DungeonOp::Exit);
            Exit(reg, player);
            return;
        }
    }

    // Inter-stage SafeHaven: rest, then descend on E (host drives co-op).
    if (g_intermission) {
        if (IsKeyPressed(KEY_E) && isSessionAuthority()) {
            advanceFromIntermission(reg, player);
        }
        return;
    }

    // End-of-run loot chest claim.
    if (g_lootPending && !g_lootTaken) {
        const float dx = g_lootPos.x - p.x;
        const float dz = g_lootPos.z - p.z;
        if (dx * dx + dz * dz < 4.0f * 4.0f && IsKeyPressed(KEY_E) && isSessionAuthority()) {
            claimLootAndExit(reg, player);
            return;
        }
    }

    for (auto& room : g_layout.rooms) {
        if (!insideRoom(room, p, 0.0f)) continue;

        if (g_currentRoom != room.id) {
            g_currentRoom = room.id;
            if (!room.visited) {
                room.visited = true;
                events::Publish(events::Event{
                    events::Type::RoomEntered, room.id, g_stageIndex, 0.0f});

                if (room.type == RoomType::SafeHaven && reg.healths.Has(player)) {
                    auto& hp = reg.healths.Get(player);
                    hp.current = std::min(hp.max, hp.current + hp.max * 0.35f);
                    setBanner("Safe haven - partial recovery");
                } else if (room.type == RoomType::Treasure) {
                    setBanner("Treasure vault");
                } else if (room.type == RoomType::Combat || room.type == RoomType::Elite) {
                    // Ambush hook: chance of an extra pack when first entering a fight room.
                    const uint64_t h = engine::math::splitmix64(
                        g_layout.seed ^ (uint64_t)(room.id * 0xC2B2AE3Du) ^ (uint64_t)0xA11B05u);
                    if (engine::math::randFloat01(h) < 0.28f) {
                        const int extras = 2;
                        for (int i = 0; i < extras; ++i) {
                            float lx = 0.0f, lz = 0.0f;
                            Vector3 pos{room.center.x, kFloorY, room.center.z};
                            if (const WalkableMask* m = maskFor(room);
                                m && m->SampleWalkableLocal(g_layout.seed, 900 + i, lx, lz)) {
                                pos = Vector3{room.center.x + lx * 0.7f, kFloorY,
                                              room.center.z + lz * 0.7f};
                            } else {
                                const float a = (float)i * 2.1f;
                                pos = Vector3{
                                    room.center.x + cosf(a) * room.halfW * 0.35f,
                                    kFloorY,
                                    room.center.z + sinf(a) * room.halfW * 0.35f};
                            }
                            spawnNormal(reg, pos);
                        }
                        events::Publish(events::Event{
                            events::Type::AmbushTriggered, room.id, g_stageIndex, (float)extras});
                        setBanner("Ambush!");
                    }
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
                events::Publish(events::Event{
                    events::Type::RoomCleared, room.id, g_stageIndex, 0.0f});
                setBanner("Boss slain - extraction unsealed");
            }
            continue;
        }
        if (countEnemiesInRoom(reg, room) == 0) {
            room.cleared = true;
            if (room.type == RoomType::Combat || room.type == RoomType::Elite) {
                events::Publish(events::Event{
                    events::Type::RoomCleared, room.id, g_stageIndex, 0.0f});
            }
        }
    }

    // Director telemetry stubs.
    {
        int cleared = 0, visited = 0;
        for (const auto& room : g_layout.rooms) {
            if (room.visited) ++visited;
            if (room.cleared && (room.type == RoomType::Combat ||
                                 room.type == RoomType::Elite ||
                                 room.type == RoomType::Boss)) {
                ++cleared;
            }
        }
        float hpFrac = 1.0f;
        if (reg.healths.Has(player)) {
            const auto& hp = reg.healths.Get(player);
            hpFrac = (hp.max > 0.0f) ? (hp.current / hp.max) : 0.0f;
        }
        events::TickTelemetry(dt, hpFrac, cleared, visited);
    }

    // Vault key pickup.
    if (g_keySpawned && !g_hasKey) {
        const float dx = g_keyPos.x - p.x;
        const float dz = g_keyPos.z - p.z;
        if (dx * dx + dz * dz < 3.0f * 3.0f) {
            g_keySpawned = false;
            g_hasKey     = true;
            setBanner("Vault key taken");
        }
    }

    // Sealed transitions open on their own terms.
    bool bossDown = (g_layout.bossRoom < 0);
    if (!bossDown) {
        const Room& bossRoom = g_layout.rooms[g_layout.bossRoom];
        bossDown = bossRoom.cleared || (g_bossSpawned && !bossAlive(reg));
        // Also treat a dead-but-not-yet-destroyed boss as down.
        if (!bossDown && g_bossSpawned && engine::ecs::IsValid(reg, g_boss) &&
            reg.healths.Has(g_boss) && reg.healths.Get(g_boss).current <= 0.0f) {
            bossDown = true;
        }
    }

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
                // Reveal when the player is near the corridor (center OR either end).
                if (gate.linkIndex >= 0 && gate.linkIndex < (int)g_layout.links.size()) {
                    const Transition& link = g_layout.links[gate.linkIndex];
                    auto near = [&](Vector3 c) {
                        const float dx = c.x - p.x;
                        const float dz = c.z - p.z;
                        return dx * dx + dz * dz < 9.0f * 9.0f;
                    };
                    Vector3 end0 = link.center;
                    Vector3 end1 = link.center;
                    if (link.alongX) {
                        end0.x -= link.halfLen;
                        end1.x += link.halfLen;
                    } else {
                        end0.z -= link.halfLen;
                        end1.z += link.halfLen;
                    }
                    open = near(link.center) || near(end0) || near(end1);
                }
                msg = "A hidden passage grinds open";
                break;
            }
            default:
                open = true;
                break;
        }

        if (!open) { ++i; continue; }

        if (gate.solidIndex >= 0 && gate.solidIndex < (int)g_solids.size()) {
            g_solids[gate.solidIndex].active = false;
        }
        if (gate.barSolidIndex >= 0 && gate.barSolidIndex < (int)g_solids.size()) {
            g_solids[gate.barSolidIndex].active = false;
        }
        if (gate.linkIndex >= 0 && gate.linkIndex < (int)g_layout.links.size()) {
            g_layout.links[gate.linkIndex].locked = false;
        }
        if (msg != nullptr) setBanner(msg);
        g_gates.erase(g_gates.begin() + (long)i);
    }

    // Extraction: checkpoint rest between stages, or end-of-run loot then exit.
    // Host-authoritative when lobbied — clients wait for sync.
    if (!isSessionAuthority()) return;
    if (g_lootPending) return;

    if (g_layout.extractRoom >= 0) {
        const Room ex = g_layout.rooms[g_layout.extractRoom];
        if (insideRoom(ex, p, 0.0f)) {
            if (g_stageIndex + 1 < StageCount()) {
                beginIntermission(reg, player);
            } else {
                spawnEndLoot(ex);
            }
        }
    }
}

void Draw() {
    if (!g_active) return;

    const float t = (float)GetTime();
    const float pulse = 0.5f + 0.5f * sinf(t * 2.2f);

    // Walls / gates / columns — single lightweight pass (not ECS renderables).
    // Cave walls share the ceiling albedo (cliff/ground rock), not dark_rock.
    Texture2D caveShellTex = (g_theme == Theme::Cave) ? caveCeilTex() : Texture2D{};
    for (const auto& s : g_solids) {
        if (!s.active) continue;
        const float dx = s.center.x - g_playerDrawPos.x;
        const float dz = s.center.z - g_playerDrawPos.z;
        if (dx * dx + dz * dz > 160.0f * 160.0f) continue;
        if (s.caveTex && caveShellTex.id != 0) {
            drawTexturedBox(caveShellTex, s.center, s.w, s.h, s.d, WHITE);
        } else {
            DrawCube(s.center, s.w, s.h, s.d, s.color);
        }
    }

    if (g_keySpawned) {
        DrawCube(g_keyPos, 0.9f, 0.9f, 0.9f, Color{240, 200, 90, 255});
        BeginBlendMode(BLEND_ADDITIVE);
        DrawSphere(g_keyPos, 0.8f + 0.15f * pulse, Color{255, 210, 90, (unsigned char)(80 + 50 * pulse)});
        EndBlendMode();
    }

    if (g_lootPending && !g_lootTaken) {
        DrawCube(g_lootPos, 1.4f, 1.1f, 1.0f, Color{160, 110, 40, 255});
        DrawCube(Vector3{g_lootPos.x, g_lootPos.y + 0.65f, g_lootPos.z}, 1.5f, 0.25f, 1.1f,
                 Color{200, 150, 60, 255});
        BeginBlendMode(BLEND_ADDITIVE);
        DrawSphere(g_lootPos, 1.2f + 0.2f * pulse, Color{255, 200, 80, (unsigned char)(70 + 40 * pulse)});
        EndBlendMode();
    }

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
        const Color glow = glowColor(room.type);

        const float dxp = room.center.x - g_playerDrawPos.x;
        const float dzp = room.center.z - g_playerDrawPos.z;
        const float dist2 = dxp * dxp + dzp * dzp;
        const bool nearDetail = dist2 < (110.0f * 110.0f);
        const bool midDetail  = dist2 < (200.0f * 200.0f);
        if (!midDetail) continue;  // skip far rooms entirely

        // Continuous heightfield floor + ceiling (one Model each per room).
        if (room.id >= 0 && room.id < (int)g_surfaces.size()) {
            const RoomSurfaceGpu& surf = g_surfaces[(size_t)room.id];
            if (surf.floorReady) DrawModel(surf.floor, Vector3Zero(), 1.0f, WHITE);
            if (surf.ceilReady)  DrawModel(surf.ceil,  Vector3Zero(), 1.0f, WHITE);
            if (surf.coveReady)  DrawModel(surf.cove,  Vector3Zero(), 1.0f, WHITE);
        }

        // Light flooded sheen — small lift so it doesn't z-fight the floor mesh.
        if (nearDetail && room.floor == FloorStyle::Flooded) {
            float gy = kFloorY + 0.22f;
            if (const WalkableMask* m = maskFor(room)) {
                gy = kFloorY + m->SampleHeightWorld(room.center, room.center.x, room.center.z) + 0.22f;
            }
            BeginBlendMode(BLEND_ALPHA);
            DrawCube(Vector3{room.center.x, gy, room.center.z},
                     room.halfW * 1.05f, 0.06f, room.halfD * 1.05f,
                     Color{40, 90, 110, 90});
            EndBlendMode();
        }

        // Rib beams hang just under the ceiling heightfield (masonry only — caves read as rock).
        if (nearDetail && g_theme != Theme::Cave) {
            const WalkableMask* cm = maskFor(room);
            auto ceilAt = [&](float wx, float wz) {
                float off = cm ? cm->SampleCeilHeightWorld(room.center, wx, wz) : 0.0f;
                return kCeilY + off - 0.25f;
            };
            for (float bx = -room.halfW + 4.0f; bx < room.halfW - 2.0f; bx += 6.0f) {
                const float wx = room.center.x + bx;
                if (!tileInShape(room, wx, room.center.z)) continue;
                DrawCube(Vector3{wx, ceilAt(wx, room.center.z), room.center.z},
                         0.55f, 0.45f, room.halfD * 2.0f - 1.0f, Color{48, 46, 56, 255});
            }
            for (float bz = -room.halfD + 4.0f; bz < room.halfD - 2.0f; bz += 6.0f) {
                const float wz = room.center.z + bz;
                if (!tileInShape(room, room.center.x, wz)) continue;
                DrawCube(Vector3{room.center.x, ceilAt(room.center.x, wz), wz},
                         room.halfW * 2.0f - 1.0f, 0.45f, 0.55f, Color{48, 46, 56, 255});
            }
        }

        // Wall-mounted torches (inset from corners) — skip cut L quadrant.
        const float torchInset = 3.2f;
        auto maybeTorch = [&](float ox, float oz) {
            if (!tileInShape(room, room.center.x + ox, room.center.z + oz)) return;
            drawTorch(Vector3{room.center.x + ox, kFloorY + 4.2f, room.center.z + oz}, glow);
        };
        maybeTorch(-room.halfW + torchInset, -room.halfD + 0.55f);
        maybeTorch( room.halfW - torchInset, -room.halfD + 0.55f);
        maybeTorch(-room.halfW + torchInset,  room.halfD - 0.55f);
        maybeTorch( room.halfW - torchInset,  room.halfD - 0.55f);

        // Central hanging lamp — follows ceiling height at room center.
        float ceilOff = 0.0f;
        if (const WalkableMask* m = maskFor(room)) {
            ceilOff = m->SampleCeilHeightWorld(room.center, room.center.x, room.center.z);
        }
        const float lampY = kCeilY + ceilOff;
        BeginBlendMode(BLEND_ADDITIVE);
        DrawSphere(Vector3{room.center.x, lampY - 1.8f, room.center.z},
                   0.55f + 0.08f * pulse,
                   Color{glow.r, glow.g, glow.b, (unsigned char)(90 + 50 * pulse)});
        EndBlendMode();
        DrawCube(Vector3{room.center.x, lampY - 0.7f, room.center.z}, 0.12f, 1.6f, 0.12f,
                 Color{40, 40, 48, 255});

        // Mesh set-dressing — sarcophagi / statues placed off-center by room seed.
        if (nearDetail) {
            const uint64_t roomHash = engine::math::splitmix64(g_layout.seed ^ (uint64_t)(room.id * 2654435761u));
            const float u = engine::math::randFloat01(roomHash);
            const float v = engine::math::randFloat01(engine::math::splitmix64(roomHash ^ 0xABCDu));
            const float yaw = u * 360.0f;
            if (room.type == RoomType::Combat || room.type == RoomType::Elite) {
                const float ox = (u - 0.5f) * room.halfW * 1.1f;
                const float oz = (v - 0.5f) * room.halfD * 1.1f;
                if (tileInShape(room, room.center.x + ox, room.center.z + oz) &&
                    (fabsf(ox) > 3.0f || fabsf(oz) > 3.0f)) {
                    props::Draw(props::Kind::Sarcophagus,
                                Vector3{room.center.x + ox, kFloorY, room.center.z + oz},
                                yaw, 1.0f, Color{120, 118, 128, 255});
                }
                if (v > 0.4f) {
                    const float ox2 = (v - 0.5f) * room.halfW * 1.2f;
                    const float oz2 = (u - 0.5f) * room.halfD * 1.2f;
                    if (tileInShape(room, room.center.x + ox2, room.center.z + oz2)) {
                        props::Draw(props::Kind::BrokenStatue,
                                    Vector3{room.center.x + ox2, kFloorY, room.center.z + oz2},
                                    yaw + 40.0f, 1.15f, Color{140, 136, 148, 255});
                    }
                }
            } else if (room.type == RoomType::Secret || room.type == RoomType::SafeHaven) {
                props::Draw(props::Kind::BrokenStatue,
                            Vector3{room.center.x + room.halfW * 0.45f, kFloorY,
                                    room.center.z - room.halfD * 0.35f},
                            200.0f, 1.2f, Color{150, 145, 160, 255});
            } else if (room.type == RoomType::Boss) {
                for (int i = 0; i < 4; ++i) {
                    const float a = (float)i / 4.0f * 2.0f * PI + 0.4f;
                    const float rr = std::min(room.halfW, room.halfD) * 0.72f;
                    props::Draw(props::Kind::BrokenStatue,
                                Vector3{room.center.x + cosf(a) * rr, kFloorY,
                                        room.center.z + sinf(a) * rr},
                                a * RAD2DEG + 90.0f, 1.35f, Color{110, 100, 108, 255});
                }
            }
        }

        if (!nearDetail) continue;

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
            const float arenaR = std::min(room.halfW, room.halfD) * 0.45f;
            DrawCylinder(Vector3{room.center.x, kFloorY, room.center.z}, arenaR, arenaR + 0.5f, 0.2f, 28,
                         Color{90, 40, 40, 220});
            for (int i = 0; i < 6; ++i) {
                const float a = (float)i / 6.0f * 2.0f * PI;
                const float br = std::min(room.halfW, room.halfD) * 0.62f;
                Vector3 brazier{room.center.x + cosf(a) * br, kFloorY + 0.9f,
                                room.center.z + sinf(a) * br};
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
                if (!tileInShape(room, room.center.x + ox, room.center.z + oz)) continue;
                DrawCube(Vector3{room.center.x + ox, kFloorY + 0.55f, room.center.z + oz},
                         1.3f, 1.1f, 1.1f, Color{78, 58, 42, 255});
            }
        }
    }

    // Moss rock dressing for Natural Caves.
    if (g_theme == Theme::Cave) {
        for (const auto& rock : g_caveRocks) {
            const float dx = rock.pos.x - g_playerDrawPos.x;
            const float dz = rock.pos.z - g_playerDrawPos.z;
            if (dx * dx + dz * dz > 130.0f * 130.0f) continue;
            props::DrawMossRock(rock.variant, rock.pos, rock.yaw, rock.scale,
                                Color{220, 215, 205, 255});
        }
    }

    for (size_t li = 0; li < g_layout.links.size(); ++li) {
        const auto& link = g_layout.links[li];
        const bool cave = (g_theme == Theme::Cave);

        float midFloor = kFloorY;
        float midCeil  = kCeilY;
        if (li < g_linkHeights.size() && g_linkHeights[li].n >= 2) {
            midFloor = kFloorY + sampleLinkHeight(g_linkHeights[li], link,
                                                 link.center.x, link.center.z, false);
            midCeil  = kCeilY  + sampleLinkHeight(g_linkHeights[li], link,
                                                 link.center.x, link.center.z, true);
        }

        // Heightfield floor + ceiling.
        bool ceilReady = false;
        if (li < g_linkSurfaces.size()) {
            const RoomSurfaceGpu& surf = g_linkSurfaces[li];
            ceilReady = surf.ceilReady;
            if (surf.floorReady) DrawModel(surf.floor, Vector3Zero(), 1.0f, WHITE);
            if (surf.ceilReady)  DrawModel(surf.ceil,  Vector3Zero(), 1.0f, WHITE);
            if (surf.coveReady)  DrawModel(surf.cove,  Vector3Zero(), 1.0f, WHITE);
        }

        // Deep under-deck / over-shell for nearby corridors only — seals threshold
        // gaps without paying drawTexturedBox for every link in the graph.
        {
            const float ldx = link.center.x - g_playerDrawPos.x;
            const float ldz = link.center.z - g_playerDrawPos.z;
            if (ldx * ldx + ldz * ldz < 140.0f * 140.0f) {
                const float spanL = (link.halfLen + kCorridorOverlap) * 2.0f;
                const float spanW = link.halfWidth * 2.0f + 0.5f;
                const float bx = link.alongX ? spanL : spanW;
                const float bz = link.alongX ? spanW : spanL;
                Texture2D floorTex = (cave && props::CaveKitReady()) ? props::CaveFloorAlbedo() : Texture2D{};
                Texture2D ceilTex  = (cave && props::CaveKitReady()) ? caveCeilTex() : Texture2D{};
                const Color floorTint = cave ? Color{90, 82, 72, 255} : Color{48, 46, 52, 255};

                if (floorTex.id != 0) {
                    drawTexturedBox(floorTex,
                        Vector3{link.center.x, midFloor - 0.28f, link.center.z},
                        bx, 0.45f, bz, WHITE);
                } else {
                    DrawCube(Vector3{link.center.x, midFloor - 0.28f, link.center.z},
                             bx, 0.45f, bz, floorTint);
                }
                if (ceilTex.id != 0) {
                    drawTexturedBox(ceilTex,
                        Vector3{link.center.x, midCeil + 0.35f, link.center.z},
                        bx + 0.35f, 0.4f, bz + 0.35f, WHITE);
                } else if (!ceilReady) {
                    DrawCube(Vector3{link.center.x, midCeil + 0.3f, link.center.z},
                             bx + 0.4f, 0.4f, bz + 0.4f, Color{30, 28, 36, 255});
                }
            }
        }

        const float lx = link.alongX ? link.halfLen * 2.0f : link.halfWidth * 2.0f;
        const float lz = link.alongX ? link.halfWidth * 2.0f : link.halfLen * 2.0f;

        if (link.style == CorridorStyle::Flooded) {
            BeginBlendMode(BLEND_ALPHA);
            DrawCube(Vector3{link.center.x, midFloor + 0.2f, link.center.z},
                     lx * 0.9f, 0.08f, lz * 0.9f, Color{40, 95, 115, 115});
            EndBlendMode();
        }

        if (link.style == CorridorStyle::Ruined) {
            for (int i = 0; i < 4; ++i) {
                const float a = (float)i * 1.7f;
                Vector3 r = link.center;
                if (link.alongX) { r.x += cosf(a) * 1.4f; r.z += sinf(a) * link.halfWidth * 0.4f; }
                else             { r.z += cosf(a) * 1.4f; r.x += sinf(a) * link.halfWidth * 0.4f; }
                r.y = midFloor + 0.4f;
                DrawCube(r, 0.9f + 0.2f * sinf(a), 0.7f, 0.8f, Color{70, 64, 60, 255});
            }
        }

        // End frames: masonry arch rings vs cave rock mouth.
        for (int e = 0; e < 2; ++e) {
            Vector3 arch = link.center;
            if (link.alongX) arch.x += (e == 0 ? -1.0f : 1.0f) * (link.halfLen - 0.2f);
            else             arch.z += (e == 0 ? -1.0f : 1.0f) * (link.halfLen - 0.2f);
            float endCeil = midCeil;
            float endFloor = midFloor;
            if (li < g_linkHeights.size() && g_linkHeights[li].n >= 2) {
                endCeil = kCeilY + g_linkHeights[li].ceil[(size_t)(e == 0 ? 0 : g_linkHeights[li].n - 1)];
                endFloor = kFloorY + g_linkHeights[li].floor[(size_t)(e == 0 ? 0 : g_linkHeights[li].n - 1)];
            }
            arch.y = (endFloor + endCeil) * 0.5f;
            const float frameH = std::max(2.0f, endCeil - endFloor - 1.2f);

            if (cave) {
                // Irregular rock collar instead of a cut arch.
                for (int t = 0; t < 3; ++t) {
                    const float u = hash01(engine::math::splitmix64(
                        g_layout.seed ^ (uint64_t)(li * 97 + e * 11 + t * 3)));
                    Vector3 tooth = arch;
                    const float across = ((float)t - 1.0f) * link.halfWidth * 0.55f;
                    if (link.alongX) tooth.z += across;
                    else             tooth.x += across;
                    tooth.y = endCeil - 0.5f - u * 0.8f;
                    Vector3 ts = link.alongX
                        ? Vector3{0.9f + u * 0.5f, 0.7f + u * 0.5f, 1.0f + u * 0.4f}
                        : Vector3{1.0f + u * 0.4f, 0.7f + u * 0.5f, 0.9f + u * 0.5f};
                    DrawCube(tooth, ts.x, ts.y, ts.z, plinthColor());
                }
            } else {
                if (link.alongX) {
                    DrawCube(arch, 0.5f, frameH, link.halfWidth * 2.0f + 1.0f, corniceColor());
                } else {
                    DrawCube(arch, link.halfWidth * 2.0f + 1.0f, frameH, 0.5f, corniceColor());
                }
            }
        }

        // Locked seal: one iron gate at the corridor center (removed when unlocked).
        if (link.locked && link.requires_ != GateRequirement::Search) {
            Vector3 gpos = link.center;
            gpos.y = midFloor;
            const float yaw = link.alongX ? 90.0f : 0.0f;
            const float scale = (link.halfWidth * 2.0f) / 3.6f;
            props::Draw(props::Kind::IronGate, gpos, yaw, std::max(0.55f, scale),
                        Color{90, 78, 48, 255});

            const Color seal = (link.requires_ == GateRequirement::Key)
                                   ? Color{255, 205, 90, 0}
                                   : Color{255, 180, 60, 0};
            BeginBlendMode(BLEND_ADDITIVE);
            DrawSphere(Vector3{link.center.x, midFloor + 4.5f, link.center.z},
                       1.2f + 0.2f * pulse,
                       Color{seal.r, seal.g, seal.b, (unsigned char)(70 + 40 * pulse)});
            EndBlendMode();

            if (link.requires_ == GateRequirement::Key) {
                DrawCube(Vector3{link.center.x, midFloor + 2.6f, link.center.z},
                         0.6f, 0.9f, 0.6f, g_hasKey ? Color{120, 240, 140, 255}
                                                    : Color{225, 185, 70, 255});
            }
        }

        if (link.style == CorridorStyle::Choke) {
            if (link.alongX) {
                drawTorch(Vector3{link.center.x - link.halfLen * 0.4f, midFloor + 4.0f,
                                  link.center.z - link.halfWidth + 0.35f}, Color{255, 160, 80, 160});
                drawTorch(Vector3{link.center.x + link.halfLen * 0.4f, midFloor + 4.0f,
                                  link.center.z + link.halfWidth - 0.35f}, Color{255, 160, 80, 160});
            } else {
                drawTorch(Vector3{link.center.x - link.halfWidth + 0.35f, midFloor + 4.0f,
                                  link.center.z - link.halfLen * 0.4f}, Color{255, 160, 80, 160});
                drawTorch(Vector3{link.center.x + link.halfWidth - 0.35f, midFloor + 4.0f,
                                  link.center.z + link.halfLen * 0.4f}, Color{255, 160, 80, 160});
            }
        }

        // Mid-corridor torch pair — hang under local ceiling a bit.
        const float torchY = std::min(midFloor + 4.0f, midCeil - 1.2f);
        if (link.alongX) {
            drawTorch(Vector3{link.center.x, torchY, link.center.z - link.halfWidth + 0.4f},
                      Color{255, 160, 80, 130});
            drawTorch(Vector3{link.center.x, torchY, link.center.z + link.halfWidth - 0.4f},
                      Color{255, 160, 80, 130});
        } else {
            drawTorch(Vector3{link.center.x - link.halfWidth + 0.4f, torchY, link.center.z},
                      Color{255, 160, 80, 130});
            drawTorch(Vector3{link.center.x + link.halfWidth - 0.4f, torchY, link.center.z},
                      Color{255, 160, 80, 130});
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
        std::snprintf(stageLine, sizeof(stageLine), "%s  -  %s  -  stage %d/%d: %s",
                      ThemeName(g_theme), GetCampaign().name.c_str(),
                      g_stageIndex + 1, StageCount(),
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
            y += 22;
        } else if (g_layout.keyRoom >= 0 && g_keySpawned) {
            DrawText("Vault key: guarded nearby", 24, y + 4, 17, Color{190, 180, 140, 255});
            y += 22;
        }

        if (g_intermission) {
            DrawText("[E] Descend to next stage", 24, y + 8, 22, Color{140, 220, 255, 255});
        } else if (g_lootPending && !g_lootTaken) {
            DrawText("[E] Claim spoils chest", 24, y + 8, 22, Color{255, 210, 120, 255});
        }

        const auto& tel = events::GetTelemetry();
        DrawText(TextFormat("tension %.0f%%", tel.tension * 100.0f),
                 GetScreenWidth() - 160, 100, 16, Color{180, 180, 190, 200});
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
