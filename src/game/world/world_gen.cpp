#include "game/world/world_gen.hpp"
#include "game/world/building_panels.hpp"
#include "game/world/panel_build.hpp"
#include "game/world/landmarks.hpp"
#include "engine/math/noise.hpp"
#include "engine/math/hydrology.hpp"
#include "engine/render/sky.hpp"
#include "engine/terrain/chunk_manager.hpp"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <string>
#include <vector>

namespace game::world {

static float smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Distance budget for per-element detail (trees, gravestones, city buildings).
static constexpr float DETAIL_DRAW_DISTANCE_SQ = 500.0f * 500.0f;
static Vector3 g_camPos = {0, 0, 0};  // set each frame in DrawLandmarks()

// ---------------------------------------------------------------------------
// Landmark table  Efixed world coordinates (in meters).
// Coordinate system: origin (0,0) = church spawn. +Z = north. Skyrim scale (~6 km).
// ---------------------------------------------------------------------------
const Landmark LANDMARKS[] = {
    // Spawn: flat elevated churchyard so gravestones sit level.
    {LandmarkType::Church,       "Church of the Vigil",  { 0.0f,   0.0f,    0.0f}, 180.0f, 180.0f, 16.0f, 120.0f},

    // Dwarven mountain range: Mine entrance pad with gentle slope into mountains.
    {LandmarkType::DwarvenMines, "Kharaz-Dûm",           {-2500.0f, 0.0f, -1500.0f}, 700.0f, 60.0f, 42.0f, 180.0f},

    // Elven forest: gently flatten so trees are on rolling but stable ground.
    {LandmarkType::ElvenForest,  "Silverleaf Wood",      { 2500.0f, 0.0f, -1000.0f}, 800.0f, 700.0f, 18.0f, 200.0f},

    // Witch's hut: small clearing hidden inside the forest.
    {LandmarkType::WitchHouse,   "Hut of the Ashwitch",  { 2700.0f, 0.0f, -1200.0f},  40.0f,  30.0f, 18.0f, 60.0f},

    // Capital city: massive walled plateau.
    {LandmarkType::CapitalCity,  "Aurelia",              { 2000.0f, 0.0f,  2000.0f}, 700.0f, 650.0f, 24.0f, 200.0f},

    // Lich King's castle: raised cursed ground far south.
    {LandmarkType::LichCastle,   "The Black Spire",      { 0.0f,    0.0f, -2800.0f}, 500.0f, 450.0f, 32.0f, 180.0f},
};

const size_t LANDMARK_COUNT = sizeof(LANDMARKS) / sizeof(LANDMARKS[0]);

// ---------------------------------------------------------------------------
// Height modifier: smoothly pull terrain toward each landmark's target height
// within its flatRadius, feathering out over flatFalloff. Additive blending
// so overlapping landmarks average out (unlikely but safe).
// ---------------------------------------------------------------------------
static float applyLandmarks(float x, float z, float rawHeight) {
    float result = rawHeight;

    for (size_t i = 0; i < LANDMARK_COUNT; ++i) {
        const Landmark& lm = LANDMARKS[i];
        if (lm.flatRadius <= 0.0f) continue;

        float dx = x - lm.center.x;
        float dz = z - lm.center.z;
        float d  = std::sqrt(dx * dx + dz * dz);

        float innerR = lm.flatRadius;
        float outerR = lm.flatRadius + lm.flatFalloff;

        if (d >= outerR) continue;

        // Blend factor: 1 fully flat inside innerR, smoothly falls to 0 at outerR.
        float blend;
        if (d <= innerR) {
            blend = 1.0f;
        } else {
            float t = (d - innerR) / (outerR - innerR);
            blend = 1.0f - (t * t * (3.0f - 2.0f * t));  // smoothstep
        }
        result = result * (1.0f - blend) + lm.flatHeight * blend;
    }

    return result;
}

void InstallHeightModifier() {
    engine::math::SetHeightModifier(applyLandmarks);
}

void InitBuildingPanels() {
    building_panels::Init();

    // Keep grass from baking through foundation Floor tiles (and a small edge pad).
    engine::terrain::chunks::ClearGrassExclusions();
    const float G = building_panels::Grid();
    const float pad = 0.4f;
    for (size_t i = 0; i < LANDMARK_COUNT; ++i) {
        const Landmark& lm = LANDMARKS[i];
        if (lm.type != LandmarkType::Church) continue;
        // Matches drawChurch: 6 x 10 bays = 24 x 40 m
        const float halfX = 6.0f * G * 0.5f;
        const float halfZ = 10.0f * G * 0.5f;
        engine::terrain::chunks::AddGrassExclusionRect(
            lm.center.x - halfX - pad,
            lm.center.z - halfZ - pad,
            lm.center.x + halfX + pad,
            lm.center.z + halfZ + pad);
    }

    panel_build::Init(); // loads placements + adds floor grass exclusions
}

void ShutdownBuildingPanels() {
    panel_build::Shutdown();
    building_panels::Shutdown();
}

// ---------------------------------------------------------------------------
// Blockout drawing  Eone function per landmark type. All primitives, no textures.
// Everything is drawn on the main thread inside BeginMode3D/EndMode3D.
// ---------------------------------------------------------------------------

static Color COLOR_STONE      = {150, 145, 140, 255};
static Color COLOR_STONE_DARK = { 90,  85,  80, 255};
static Color COLOR_WOOD       = {110,  70,  40, 255};
static Color COLOR_ROOF       = { 70,  35,  25, 255};
static Color COLOR_GOLD       = {200, 170,  60, 255};
static Color COLOR_TREE_TRUNK = { 60,  40,  20, 255};
static Color COLOR_TREE_LEAF  = { 40,  70,  30, 255};
static Color COLOR_WATER      = { 30,  60, 100, 220};
static Color COLOR_CURSED     = { 40,  20,  30, 255};
static Color COLOR_LAVA       = {220,  80,  20, 255};

// Deterministic per-position hash ↁEfloat [0,1) for placing scenery.
static float hashUnit(int seedTag, int gx, int gz) {
    uint64_t h = engine::math::hash2D(
        static_cast<uint64_t>(seedTag) ^ engine::math::GetWorldConfig().seed, gx, gz);
    return engine::math::randFloat01(h);
}

// ---------------- Church + graveyard ----------------
// Footprint on the 4 m panel grid: 6 bays wide (X) x 10 bays long (Z) = 24 x 40 m.
static void drawChurch(const Landmark& lm) {
    using namespace building_panels;
    const float G  = Grid();
    const float WH = WallHeight();

    float baseY = lm.flatHeight;
    Vector3 c   = {lm.center.x, baseY, lm.center.z};

    const int baysX = 6;
    const int baysZ = 10;
    const float lenX = baysX * G;
    const float lenZ = baysZ * G;
    const float halfX = lenX * 0.5f;
    const float halfZ = lenZ * 0.5f;

    // Floors — tiled Floor panels (floor-center origin)
    for (int ix = 0; ix < baysX; ++ix) {
        for (int iz = 0; iz < baysZ; ++iz) {
            float px = c.x - halfX + (ix + 0.5f) * G;
            float pz = c.z - halfZ + (iz + 0.5f) * G;
            Draw(Piece::Floor, {px, baseY, pz}, 0.0f, Style::Stone);
        }
    }

    // Long walls (east/west) — wall-center bottom on footprint edges; openings for colonnade feel
    for (int iz = 0; iz < baysZ; ++iz) {
        float pz = c.z - halfZ + (iz + 0.5f) * G;
        Piece west = (iz == baysZ / 2) ? Piece::WallDoor
                     : ((iz % 2 == 0) ? Piece::WallWindow : Piece::Wall);
        Piece east = west;
        // West wall faces inward (+X): yaw 90 → local +Z of panel points world +X
        Draw(west, {c.x - halfX, baseY, pz}, 90.0f, Style::Stone);
        // East wall faces inward (-X): yaw -90 / 270
        Draw(east, {c.x + halfX, baseY, pz}, -90.0f, Style::Stone);
        // Second story wall run for nave height
        Draw(Piece::Wall, {c.x - halfX, baseY + WH, pz}, 90.0f, Style::Stone);
        Draw(Piece::Wall, {c.x + halfX, baseY + WH, pz}, -90.0f, Style::Stone);
    }

    // South end (altar) — solid with windows; north end has main door
    for (int ix = 0; ix < baysX; ++ix) {
        float px = c.x - halfX + (ix + 0.5f) * G;
        Piece south = (ix == 2 || ix == 3) ? Piece::WallWindow : Piece::Wall;
        Piece north = (ix == 2 || ix == 3) ? Piece::WallDoor : Piece::WallWindow;
        // South wall faces +Z (into nave): yaw 0
        Draw(south, {px, baseY, c.z - halfZ}, 0.0f, Style::Stone);
        Draw(Piece::Wall, {px, baseY + WH, c.z - halfZ}, 0.0f, Style::Stone);
        // North wall faces -Z (into nave): yaw 180
        Draw(north, {px, baseY, c.z + halfZ}, 180.0f, Style::Stone);
        Draw(Piece::Wall, {px, baseY + WH, c.z + halfZ}, 180.0f, Style::Stone);
    }

    // Corner pillars (two stories)
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sz = -1; sz <= 1; sz += 2) {
            Vector3 p = {c.x + sx * (halfX - 0.3f), baseY, c.z + sz * (halfZ - 0.3f)};
            Draw(Piece::Pillar, p, 0.0f, Style::Stone);
            Draw(Piece::Pillar, {p.x, baseY + WH, p.z}, 0.0f, Style::Stone);
        }
    }

    // Long-side colonnade pillars along nave
    for (int i = 1; i < baysZ; ++i) {
        if (i == baysZ / 2) continue;
        float pz = c.z - halfZ + i * G;
        Draw(Piece::Pillar, {c.x - halfX + 0.3f, baseY, pz}, 0.0f, Style::Stone);
        Draw(Piece::Pillar, {c.x + halfX - 0.3f, baseY, pz}, 0.0f, Style::Stone);
        Draw(Piece::Pillar, {c.x - halfX + 0.3f, baseY + WH, pz}, 0.0f, Style::Stone);
        Draw(Piece::Pillar, {c.x + halfX - 0.3f, baseY + WH, pz}, 0.0f, Style::Stone);
    }

    // Peaked roof: cascade RoofSlope bays so each continues the previous rise to the ridge.
    // RoofSlope origin = bay center; eave on local -Z, ridge on local +Z.
    const float eaveY = baseY + WH * 2.0f;
    const float rise  = RoofRise();
    const int halfBaysX = baysX / 2;
    for (int iz = 0; iz < baysZ; ++iz) {
        float pz = c.z - halfZ + (iz + 0.5f) * G;
        // West half: eave at west wall, slopes toward +X (yaw +90 → local +Z → world +X)
        for (int ix = 0; ix < halfBaysX; ++ix) {
            float px = c.x - halfX + (ix + 0.5f) * G;
            float py = eaveY + (float)ix * rise;
            Draw(Piece::RoofSlope, {px, py, pz}, 90.0f, Style::RoofDark);
        }
        // East half: eave at east wall, slopes toward -X (yaw -90 → local +Z → world -X)
        for (int ix = 0; ix < halfBaysX; ++ix) {
            float px = c.x + halfX - (ix + 0.5f) * G;
            float py = eaveY + (float)ix * rise;
            Draw(Piece::RoofSlope, {px, py, pz}, -90.0f, Style::RoofDark);
        }
    }
    // Ridge beam where the two slopes meet
    const float ridgeY = eaveY + (float)halfBaysX * rise;
    DrawCube({c.x, ridgeY + 0.15f, c.z}, 0.45f, 0.35f, lenZ + 0.5f, COLOR_ROOF);

    // End-wall gables: modular cascade (Gable + GableRamp) matching RoofSlope bays.
    auto drawGableRun = [&](float wallZ, float yaw, bool mirror) {
        for (int ix = 0; ix < halfBaysX; ++ix) {
            float px = mirror ? (c.x + halfX - (ix + 0.5f) * G)
                              : (c.x - halfX + (ix + 0.5f) * G);
            if (ix == 0) {
                Draw(Piece::Gable, {px, eaveY, wallZ}, yaw, Style::Stone, WHITE, mirror);
            } else {
                float py = eaveY + (float)(ix - 1) * rise;
                Draw(Piece::GableRamp, {px, py, wallZ}, yaw, Style::Stone, WHITE, mirror);
            }
        }
    };
    // South (yaw 0): west rake high=+X, east rake mirrored
    drawGableRun(c.z - halfZ, 0.0f, false);
    drawGableRun(c.z - halfZ, 0.0f, true);
    // North (yaw 180): facing flips X, so west uses mirror and east does not
    drawGableRun(c.z + halfZ, 180.0f, true);
    drawGableRun(c.z + halfZ, 180.0f, false);

    // Bell tower at north end — panel box + pyramid cap + cross
    const float towerHalf = G;
    Vector3 towerBase = {c.x, baseY, c.z + halfZ - G};
    for (int story = 0; story < 4; ++story) {
        float y = baseY + story * WH;
        Draw(Piece::Wall, {towerBase.x - towerHalf, y, towerBase.z}, 90.0f, Style::Stone);
        Draw(Piece::Wall, {towerBase.x + towerHalf, y, towerBase.z}, -90.0f, Style::Stone);
        Draw(Piece::Wall, {towerBase.x, y, towerBase.z - towerHalf}, 0.0f, Style::Stone);
        Draw(Piece::WallWindow, {towerBase.x, y, towerBase.z + towerHalf}, 180.0f, Style::Stone);
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sz = -1; sz <= 1; sz += 2)
                Draw(Piece::Pillar,
                     {towerBase.x + sx * (towerHalf - 0.2f), y, towerBase.z + sz * (towerHalf - 0.2f)},
                     0.0f, Style::Stone);
    }
    const float towerTop = baseY + 4.0f * WH;
    Draw(Piece::RoofPyramid, {towerBase.x, towerTop, towerBase.z}, 0.0f, Style::RoofDark);
    const float peakY = towerTop + RoofPyramidHeight();
    DrawCube({towerBase.x, peakY + 1.6f, towerBase.z}, 0.35f, 3.2f, 0.35f, COLOR_GOLD);
    DrawCube({towerBase.x, peakY + 2.6f, towerBase.z}, 2.2f, 0.35f, 0.35f, COLOR_GOLD);

    // Altar block at south end
    DrawCube({c.x, baseY + 1.0f, c.z - halfZ + 3.0f}, 4.0f, 1.5f, 2.0f, COLOR_STONE_DARK);

    // ------- Graveyard: deterministic scatter of gravestones around the church -------
    // Grid of cells around the church footprint, one gravestone per cell (with jitter).
    const int   gridR = 8;
    const float cell  = 8.0f;
    for (int gz = -gridR; gz <= gridR; ++gz) {
        for (int gx = -gridR; gx <= gridR; ++gx) {
            float gwx = c.x + gx * cell;
            float gwz = c.z + gz * cell;

            // Per-gravestone distance cull from camera
            float gdx = gwx - g_camPos.x, gdz = gwz - g_camPos.z;
            if (gdx * gdx + gdz * gdz > DETAIL_DRAW_DISTANCE_SQ) continue;

            if (std::abs(gwx - c.x) < lenX * 0.5f + 3.0f &&
                std::abs(gwz - c.z) < lenZ * 0.5f + 3.0f) continue;

            // Skip cells outside the graveyard radius
            float rr = (gwx - c.x) * (gwx - c.x) + (gwz - c.z) * (gwz - c.z);
            if (rr > lm.radius * lm.radius) continue;

            float r1 = hashUnit(101, gx, gz);
            float r2 = hashUnit(102, gx, gz);
            float r3 = hashUnit(103, gx, gz);
            if (r1 > 0.85f) continue;  // some empty plots

            float jx = (r2 - 0.5f) * cell * 0.6f;
            float jz = (r3 - 0.5f) * cell * 0.6f;
            float px = gwx + jx;
            float pz = gwz + jz;

            // Height at this spot (might be off the flat pad ↁEuse terrain height)
            float py = engine::math::WorldHeight(px, pz);

            // Alternate gravestone shapes
            if (r1 < 0.5f) {
                // Simple tombstone
                DrawCube({px, py + 0.6f, pz}, 0.8f, 1.2f, 0.2f, COLOR_STONE_DARK);
            } else if (r1 < 0.75f) {
                // Cross grave
                DrawCube({px, py + 0.8f, pz}, 0.3f, 1.6f, 0.3f, COLOR_STONE_DARK);
                DrawCube({px, py + 1.2f, pz}, 1.0f, 0.3f, 0.3f, COLOR_STONE_DARK);
            } else {
                // Sarcophagus
                DrawCube({px, py + 0.4f, pz}, 1.6f, 0.8f, 0.8f, COLOR_STONE);
            }
        }
    }
}

// ---------------- Dwarven mountain range + mine entrance ----------------
static void drawDwarvenMines(const Landmark& lm) {
    // The mountain itself is provided by the noise ridged layer  Ewe DON'T flatten it much.
    // Just draw a mine entrance and a few dwarven pillars/statues at the base.
    Vector3 c = {lm.center.x, lm.flatHeight, lm.center.z};

    // Mine gate  Emassive stone archway
    DrawCube({c.x - 6.0f, c.y + 6.0f, c.z}, 3.0f, 12.0f, 4.0f, COLOR_STONE_DARK);
    DrawCube({c.x + 6.0f, c.y + 6.0f, c.z}, 3.0f, 12.0f, 4.0f, COLOR_STONE_DARK);
    DrawCube({c.x, c.y + 12.0f, c.z}, 15.0f, 2.0f, 4.0f, COLOR_STONE_DARK);
    // Dark opening (the "mine")
    DrawCube({c.x, c.y + 4.0f, c.z + 0.5f}, 8.0f, 8.0f, 1.0f, {10, 10, 10, 255});

    // Two "torches" flanking the entrance (glow)
    DrawSphere({c.x - 10.0f, c.y + 8.0f, c.z + 2.0f}, 0.8f, COLOR_LAVA);
    DrawSphere({c.x + 10.0f, c.y + 8.0f, c.z + 2.0f}, 0.8f, COLOR_LAVA);

    // Runestone pillars scattered in front of the gate
    for (int i = -2; i <= 2; ++i) {
        float px = c.x + i * 8.0f;
        float pz = c.z + 15.0f;
        float py = engine::math::WorldHeight(px, pz);
        DrawCube({px, py + 3.0f, pz}, 1.2f, 6.0f, 1.2f, COLOR_STONE);
        DrawCube({px, py + 6.5f, pz}, 2.0f, 1.0f, 2.0f, COLOR_GOLD);
    }
}

// ---------------- Elven forest ----------------
static void drawElvenForest(const Landmark& lm) {
    Vector3 c = {lm.center.x, lm.flatHeight, lm.center.z};

    // Only draw individual trees if player is within range of the forest
    float fDx = g_camPos.x - c.x, fDz = g_camPos.z - c.z;
    if (fDx * fDx + fDz * fDz > DETAIL_DRAW_DISTANCE_SQ * 4.0f) return;  // skip entire forest if very far

    // Deterministic tree grid within the forest radius
    const float cell = 18.0f;
    int gridR = static_cast<int>(lm.radius / cell) + 1;

    for (int gz = -gridR; gz <= gridR; ++gz) {
        for (int gx = -gridR; gx <= gridR; ++gx) {
            float px = c.x + gx * cell;
            float pz = c.z + gz * cell;
            float dx = px - c.x, dz = pz - c.z;
            float dd = dx * dx + dz * dz;
            if (dd > lm.radius * lm.radius) continue;

            // Per-tree distance cull from camera
            float tdx = px - g_camPos.x, tdz = pz - g_camPos.z;
            if (tdx * tdx + tdz * tdz > DETAIL_DRAW_DISTANCE_SQ) continue;

            float r1 = hashUnit(201, gx, gz);
            float r2 = hashUnit(202, gx, gz);
            float r3 = hashUnit(203, gx, gz);
            if (r1 > 0.9f) continue;  // some clearings

            float jx = (r2 - 0.5f) * cell * 0.7f;
            float jz = (r3 - 0.5f) * cell * 0.7f;
            float wx = px + jx;
            float wz = pz + jz;
            float wy = engine::math::WorldHeight(wx, wz);

            float trunkH = 10.0f + r1 * 8.0f;
            float trunkR = 0.6f  + r2 * 0.4f;
            float leafR  = 3.5f  + r3 * 1.5f;

            // Trunk (tall cylinder)
            DrawCylinder({wx, wy, wz}, trunkR, trunkR * 0.7f, trunkH, 6, COLOR_TREE_TRUNK);
            // Canopy (big sphere)
            DrawSphere({wx, wy + trunkH + 1.0f, wz}, leafR, COLOR_TREE_LEAF);
        }
    }

    // A few tree-top elven platforms clustered near center
    for (int i = 0; i < 6; ++i) {
        float ang = i * (2.0f * PI / 6.0f);
        float dist = 40.0f + i * 8.0f;
        float px = c.x + std::cos(ang) * dist;
        float pz = c.z + std::sin(ang) * dist;
        float py = engine::math::WorldHeight(px, pz);
        // Platform floor
        DrawCube({px, py + 14.0f, pz}, 6.0f, 0.4f, 6.0f, COLOR_WOOD);
        // Little hut
        DrawCube({px, py + 16.0f, pz}, 4.0f, 3.5f, 4.0f, COLOR_WOOD);
        // Roof
        DrawCube({px, py + 18.5f, pz}, 5.0f, 1.0f, 5.0f, COLOR_ROOF);
    }
}

// ---------------- Witch's house ----------------
static void drawWitchHouse(const Landmark& lm) {
    float y = lm.flatHeight;
    Vector3 c = {lm.center.x, y, lm.center.z};

    // Crooked hut
    DrawCube({c.x, y + 2.0f, c.z}, 5.0f, 4.0f, 5.0f, COLOR_WOOD);
    // Thatched roof
    DrawCube({c.x, y + 4.5f, c.z}, 6.0f, 1.5f, 6.0f, COLOR_ROOF);
    // Crooked chimney
    DrawCube({c.x + 1.8f, y + 6.5f, c.z}, 0.8f, 3.0f, 0.8f, COLOR_STONE_DARK);
    // Cauldron out front
    DrawCylinder({c.x, y + 0.5f, c.z + 4.0f}, 1.0f, 1.2f, 1.0f, 8, COLOR_STONE_DARK);
    DrawSphere({c.x, y + 1.4f, c.z + 4.0f}, 0.6f, {100, 200, 100, 200});  // green glow
}

// ---------------- Lake town ----------------
static void drawLakeTown(const Landmark& lm) {
    // Basin is a hydrology heightmap carve; water surface is drawn in drawWorldWater().
    const float waterY = engine::math::WaterLevel();
    Vector3 c = {lm.center.x, waterY, lm.center.z};

    // Prefer Blackmere's irregular rim for shore placement; fall back to landmark radius.
    float shoreR = lm.radius * 0.55f;
    for (const engine::math::LakeSite& lake : engine::math::GetLakes()) {
        const float dx = lake.x - lm.center.x;
        const float dz = lake.z - lm.center.z;
        if (dx * dx + dz * dz < 40.0f * 40.0f) {
            shoreR = engine::math::LakeRimRadius(lake, -1.5707963f) * 0.92f; // south rim
            break;
        }
    }

    float shoreZ = c.z - shoreR;
    float townY  = engine::math::WorldHeight(c.x, shoreZ) + 0.5f;
    const float bedY = waterY - 8.0f;

    // Row of buildings along the shore
    for (int i = -4; i <= 4; ++i) {
        float bx = c.x + i * 10.0f;
        float bz = shoreZ - 8.0f;
        float by = engine::math::WorldHeight(bx, bz);
        float h  = 6.0f + hashUnit(301, i, 0) * 3.0f;
        DrawCube({bx, by + h * 0.5f, bz}, 7.0f, h, 7.0f, COLOR_WOOD);
        DrawCube({bx, by + h + 0.5f, bz}, 8.0f, 1.0f, 8.0f, COLOR_ROOF);
    }

    // Docks jutting into the lake (planks + stilts)
    for (int i = -1; i <= 1; ++i) {
        float dx = c.x + i * 15.0f;
        float dz = shoreZ + 12.0f;
        DrawCube({dx, townY, dz}, 3.0f, 0.3f, 20.0f, COLOR_WOOD);
        for (int j = 0; j < 5; ++j) {
            float sz = dz - 8.0f + j * 4.0f;
            float stiltH = townY - bedY;
            DrawCube({dx - 1.0f, bedY + stiltH * 0.5f, sz}, 0.4f, stiltH, 0.4f, COLOR_WOOD);
            DrawCube({dx + 1.0f, bedY + stiltH * 0.5f, sz}, 0.4f, stiltH, 0.4f, COLOR_WOOD);
        }
    }
}

// ---------------- Capital city ----------------
static void drawCapitalCity(const Landmark& lm) {
    float y = lm.flatHeight;
    Vector3 c = {lm.center.x, y, lm.center.z};

    // Ring of outer walls (four long segments with towers at corners)
    const float wallLen = lm.flatRadius * 0.9f;   // half-length
    const float wallH   = 25.0f;
    const float wallT   = 6.0f;

    // North + south walls
    DrawCube({c.x, y + wallH * 0.5f, c.z - wallLen}, wallLen * 2.0f, wallH, wallT, COLOR_STONE);
    DrawCube({c.x, y + wallH * 0.5f, c.z + wallLen}, wallLen * 2.0f, wallH, wallT, COLOR_STONE);
    // East + west walls
    DrawCube({c.x - wallLen, y + wallH * 0.5f, c.z}, wallT, wallH, wallLen * 2.0f, COLOR_STONE);
    DrawCube({c.x + wallLen, y + wallH * 0.5f, c.z}, wallT, wallH, wallLen * 2.0f, COLOR_STONE);

    // Corner towers
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sz = -1; sz <= 1; sz += 2) {
            Vector3 t = {c.x + sx * wallLen, y + 20.0f, c.z + sz * wallLen};
            DrawCube(t, 12.0f, 40.0f, 12.0f, COLOR_STONE);
            // Cone-ish roof (approximate with tapered cylinder)
            DrawCylinder({t.x, t.y + 20.0f, t.z}, 7.0f, 1.0f, 8.0f, 6, COLOR_ROOF);
        }
    }

    // Central castle keep
    DrawCube({c.x, y + 30.0f, c.z}, 40.0f, 60.0f, 40.0f, COLOR_STONE);
    // Cathedral spire behind the keep
    DrawCube({c.x, y + 40.0f, c.z + 40.0f}, 16.0f, 80.0f, 16.0f, COLOR_STONE);
    DrawCylinder({c.x, y + 80.0f, c.z + 40.0f}, 10.0f, 0.5f, 30.0f, 8, COLOR_GOLD);

    // Merchant district blockout  Egrid of small buildings inside the walls
    float cdx = g_camPos.x - c.x, cdz = g_camPos.z - c.z;
    bool nearCity = (cdx * cdx + cdz * cdz) < DETAIL_DRAW_DISTANCE_SQ;
    if (nearCity) {
        for (int gx = -3; gx <= 3; ++gx) {
            for (int gz = -3; gz <= 3; ++gz) {
                if (std::abs(gx) < 2 && std::abs(gz) < 2) continue;
                float bx = c.x + gx * 40.0f;
                float bz = c.z + gz * 40.0f;
                float bh = 8.0f + hashUnit(401, gx, gz) * 8.0f;
                DrawCube({bx, y + bh * 0.5f, bz}, 15.0f, bh, 15.0f, COLOR_STONE);
                DrawCube({bx, y + bh + 0.5f, bz}, 16.0f, 1.0f, 16.0f, COLOR_ROOF);
            }
        }
    }
}

// ---------------- Lich King's castle ----------------
static void drawLichCastle(const Landmark& lm) {
    float y = lm.flatHeight;
    Vector3 c = {lm.center.x, y, lm.center.z};

    // Dark ground disc (cursed plateau)
    DrawCylinder({c.x, y + 0.1f, c.z}, lm.flatRadius, lm.flatRadius, 0.2f, 24, COLOR_CURSED);

    // Central black keep
    DrawCube({c.x, y + 40.0f, c.z}, 30.0f, 80.0f, 30.0f, COLOR_CURSED);

    // Four surrounding spires
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sz = -1; sz <= 1; sz += 2) {
            Vector3 t = {c.x + sx * 40.0f, y + 60.0f, c.z + sz * 40.0f};
            DrawCube(t, 10.0f, 120.0f, 10.0f, COLOR_CURSED);
            DrawCylinder({t.x, t.y + 60.0f, t.z}, 6.0f, 0.5f, 20.0f, 6, COLOR_CURSED);
        }
    }

    // Sickly green flames atop the central keep
    DrawSphere({c.x, y + 82.0f, c.z}, 5.0f, {80, 200, 80, 180});

    // Dead trees ringing the plateau
    for (int i = 0; i < 12; ++i) {
        float ang = i * (2.0f * PI / 12.0f);
        float dist = lm.flatRadius * 0.85f;
        float px = c.x + std::cos(ang) * dist;
        float pz = c.z + std::sin(ang) * dist;
        float py = engine::math::WorldHeight(px, pz);
        DrawCylinder({px, py, pz}, 0.4f, 0.2f, 6.0f, 5, COLOR_STONE_DARK);
    }
}

// ---------------------------------------------------------------------------
// World water  Etranslucent fresnel surface (assets/shaders/water.*).
// Drawn as a Model so raylib binds albedo + normal maps correctly.
// ---------------------------------------------------------------------------
static Texture g_waterColor  = {};
static Texture g_waterNormal = {};
static Shader  g_waterShader = {};
static Model   g_waterModel  = {};
static int     g_locCamPos     = -1;
static int     g_locSunDir     = -1;
static int     g_locTime       = -1;
static int     g_locUvScale    = -1;
static int     g_locOpacity    = -1;
static int     g_locBrightness = -1;
static int     g_locLakeMode   = -1;
static int     g_locLakeCenter = -1;
static int     g_locLakeRadii  = -1;
static int     g_locLakeAngle  = -1;
static int     g_locLakeDepth  = -1;
static int     g_locLakeWarpAmp  = -1;
static int     g_locLakeWarpFreq = -1;
static int     g_locLakePhase    = -1;
static int     g_locEnvMap       = -1;
static int     g_locExposure     = -1;
static int     g_locAmbientCube  = -1;
static int     g_locIblStrength  = -1;
static bool    g_waterReady    = false;
static bool    g_waterModelOk  = false;
static bool    g_waterGeoDirty = true;
static bool    g_drawWaterEnabled = true;

struct WaterVert {
    float x = 0, y = 0, z = 0;
    float nx = 0, ny = 1, nz = 0;
    float u = 0, v = 0;
    unsigned char r = 255, g = 255, b = 255, a = 255;
};

struct CachedLakeMesh {
    engine::math::LakeSite site{};
    std::vector<WaterVert> verts;
};

static std::vector<CachedLakeMesh> g_cachedLakes;
static std::vector<WaterVert>      g_cachedRiverVerts;

static void InvalidateWaterGeometry() {
    g_cachedLakes.clear();
    g_cachedRiverVerts.clear();
    g_waterGeoDirty = true;
}

static void BakeWaterGeometry(); // defined with lake/river emitters below
static void EnsureWaterGeometry() {
    if (!g_waterGeoDirty) return;
    if (!engine::math::IsHydrologyReady()) return;
    BakeWaterGeometry();
}

static Texture loadWaterTexture(const std::string& path, Color fallback) {
    Image img = LoadImage(path.c_str());
    if (img.data == nullptr) {
        TraceLog(LOG_WARNING, "WATER: missing %s", path.c_str());
        img = GenImageColor(4, 4, fallback);
    }
    Texture t = LoadTextureFromImage(img);
    UnloadImage(img);
    GenTextureMipmaps(&t);
    SetTextureFilter(t, TEXTURE_FILTER_ANISOTROPIC_16X);
    SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
    return t;
}

void InitWater() {
    if (g_waterReady) return;
    const std::string texDir = std::string(GetApplicationDirectory()) + "assets/textures/water/";
    const std::string shDir  = std::string(GetApplicationDirectory()) + "assets/shaders/";

    g_waterColor  = loadWaterTexture(texDir + "water_c.jpg", {34, 88, 138, 255});
    g_waterNormal = loadWaterTexture(texDir + "water_n.jpg", {128, 128, 255, 255});

    g_waterShader = LoadShader((shDir + "water.vs").c_str(), (shDir + "water.fs").c_str());
    if (g_waterShader.id > 0) {
        g_locCamPos     = GetShaderLocation(g_waterShader, "camPos");
        g_locSunDir     = GetShaderLocation(g_waterShader, "sunDir");
        g_locTime       = GetShaderLocation(g_waterShader, "uTime");
        g_locUvScale    = GetShaderLocation(g_waterShader, "uvScale");
        g_locOpacity    = GetShaderLocation(g_waterShader, "opacity");
        g_locBrightness = GetShaderLocation(g_waterShader, "brightness");
        g_locLakeMode   = GetShaderLocation(g_waterShader, "lakeMode");
        g_locLakeCenter = GetShaderLocation(g_waterShader, "lakeCenter");
        g_locLakeRadii  = GetShaderLocation(g_waterShader, "lakeRadii");
        g_locLakeAngle  = GetShaderLocation(g_waterShader, "lakeAngle");
        g_locLakeDepth  = GetShaderLocation(g_waterShader, "lakeMaxDepth");
        g_locLakeWarpAmp  = GetShaderLocation(g_waterShader, "lakeWarpAmp");
        g_locLakeWarpFreq = GetShaderLocation(g_waterShader, "lakeWarpFreq");
        g_locLakePhase    = GetShaderLocation(g_waterShader, "lakePhase");
        g_locEnvMap       = GetShaderLocation(g_waterShader, "envMap");
        g_locExposure     = GetShaderLocation(g_waterShader, "exposure");
        g_locAmbientCube  = GetShaderLocation(g_waterShader, "ambientCube");
        g_locIblStrength  = GetShaderLocation(g_waterShader, "iblStrength");
        TraceLog(LOG_INFO, "WATER: shader ready (id=%u)", g_waterShader.id);
    } else {
        TraceLog(LOG_WARNING, "WATER: shader failed to load");
    }

    // Unit disc in XZ (diameter 2). Scaled per-lake by radius at draw time.
    Mesh plane = GenMeshPlane(2.0f, 2.0f, 1, 1);
    g_waterModel = LoadModelFromMesh(plane);
    g_waterModelOk = (g_waterModel.meshCount > 0 && g_waterModel.materialCount > 0);
    if (g_waterModelOk) {
        if (g_waterShader.id > 0) {
            g_waterModel.materials[0].shader = g_waterShader;
        }
        g_waterModel.materials[0].maps[MATERIAL_MAP_ALBEDO].texture = g_waterColor;
        g_waterModel.materials[0].maps[MATERIAL_MAP_NORMAL].texture = g_waterNormal;
        g_waterModel.materials[0].maps[MATERIAL_MAP_ALBEDO].color = WHITE;
    }

    g_waterReady = true;
    // Bake lake/river meshes once  Eper-frame WorldHeight grids were a large FPS hit
    InvalidateWaterGeometry();
    EnsureWaterGeometry();
}

void ShutdownWater() {
    if (!g_waterReady) return;
    InvalidateWaterGeometry();
    if (g_waterModelOk) {
        // Textures are owned separately  Eclear material refs before unload.
        if (g_waterModel.materialCount > 0) {
            g_waterModel.materials[0].maps[MATERIAL_MAP_ALBEDO].texture = {};
            g_waterModel.materials[0].maps[MATERIAL_MAP_NORMAL].texture = {};
            g_waterModel.materials[0].shader = {};
        }
        UnloadModel(g_waterModel);
        g_waterModel = {};
        g_waterModelOk = false;
    }
    if (g_waterColor.id > 0)  UnloadTexture(g_waterColor);
    if (g_waterNormal.id > 0) UnloadTexture(g_waterNormal);
    if (g_waterShader.id > 0) UnloadShader(g_waterShader);
    g_waterColor  = {};
    g_waterNormal = {};
    g_waterShader = {};
    g_waterReady  = false;
}

void SetDrawWaterEnabled(bool enabled) { g_drawWaterEnabled = enabled; }
bool GetDrawWaterEnabled() { return g_drawWaterEnabled; }

void MarkWaterGeometryDirty() { InvalidateWaterGeometry(); }

bool IsCameraUnderwater(const Camera3D& cam) {
    // Global fallback (rivers / low floods)
    if (cam.position.y < engine::math::WaterLevel()) {
        float ground = engine::math::WorldHeight(cam.position.x, cam.position.z);
        if (ground < engine::math::WaterLevel() - 0.25f) return true;
    }
    // Per-lake: must be inside the disc AND standing in a flooded basin
    // (ellipse alone is too wide  Ehills near lakes were false-positive blue overlays).
    if (engine::math::IsHydrologyReady()) {
        for (const engine::math::LakeSite& lake : engine::math::GetLakes()) {
            if (cam.position.y >= lake.surfaceY) continue;
            if (engine::math::LakeCoverage(lake, cam.position.x, cam.position.z) >= 1.0f) continue;
            const float ground = engine::math::WorldHeight(cam.position.x, cam.position.z);
            if (ground < lake.surfaceY - 0.35f) return true;
        }
    }
    return false;
}

void DrawUnderwaterOverlay() {
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), {20, 70, 120, 110});
}

// Albedo stays on unit 0 via rlSetTexture (survives batch flushes).
// Do NOT SetShaderValueTexture for texture0  Ethat remaps the sampler off unit 0
// onto raylib's extra units, which clear on every batch split (diagonal seam).
// Normals are procedural in the shader; texture2 is optional detail only.
static void bindWaterShaderFrame(const Camera3D& cam, float opacity, float brightness) {
    if (g_waterShader.id == 0) return;

    const float t       = static_cast<float>(GetTime());
    const float uvScale = 0.085f;
    Vector3 sun = engine::terrain::chunks::GetSunDirection();
    const float lakeOff = 0.0f;

    SetShaderValue(g_waterShader, g_locCamPos, &cam.position, SHADER_UNIFORM_VEC3);
    SetShaderValue(g_waterShader, g_locSunDir, &sun, SHADER_UNIFORM_VEC3);
    SetShaderValue(g_waterShader, g_locTime, &t, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_waterShader, g_locUvScale, &uvScale, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_waterShader, g_locOpacity, &opacity, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_waterShader, g_locBrightness, &brightness, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_waterShader, g_locLakeMode, &lakeOff, SHADER_UNIFORM_FLOAT);

    // HDRI env reflection + diffuse irradiance (re-bind each frame; extra units can flush)
    if (engine::render::sky::IsReady()) {
        float exposure = engine::render::sky::GetExposure();
        float iblStrength = 0.65f;
        if (g_locExposure >= 0) {
            SetShaderValue(g_waterShader, g_locExposure, &exposure, SHADER_UNIFORM_FLOAT);
        }
        if (g_locIblStrength >= 0) {
            SetShaderValue(g_waterShader, g_locIblStrength, &iblStrength, SHADER_UNIFORM_FLOAT);
        }
        if (g_locAmbientCube >= 0) {
            SetShaderValueV(g_waterShader, g_locAmbientCube,
                            engine::render::sky::GetAmbientCube(),
                            SHADER_UNIFORM_VEC3, 6);
        }
        if (g_locEnvMap >= 0) {
            // Dedicated high unit ? never SetShaderValueTexture (stomps batch slots 1-4).
            const int envUnit = 10;
            rlActiveTextureSlot(envUnit);
            rlEnableTexture(engine::render::sky::GetEnvTexture().id);
            SetShaderValue(g_waterShader, g_locEnvMap, &envUnit, SHADER_UNIFORM_INT);
        }
    }
}

static void bindLakeShaderParams(const engine::math::LakeSite& lake) {
    if (g_waterShader.id == 0) return;
    const float mode = 1.0f;
    float center[2] = {lake.x, lake.z};
    float radii[2]  = {lake.radiusA, lake.radiusB};
    SetShaderValue(g_waterShader, g_locLakeMode, &mode, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_waterShader, g_locLakeCenter, center, SHADER_UNIFORM_VEC2);
    SetShaderValue(g_waterShader, g_locLakeRadii, radii, SHADER_UNIFORM_VEC2);
    SetShaderValue(g_waterShader, g_locLakeAngle, &lake.angle, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_waterShader, g_locLakeDepth, &lake.depth, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_waterShader, g_locLakeWarpAmp, &lake.warpAmp, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_waterShader, g_locLakeWarpFreq, &lake.warpFreq, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_waterShader, g_locLakePhase, &lake.phase, SHADER_UNIFORM_FLOAT);
    if (g_locEnvMap >= 0 && engine::render::sky::IsReady()) {
        const int envUnit = 10;
        rlActiveTextureSlot(envUnit);
        rlEnableTexture(engine::render::sky::GetEnvTexture().id);
        SetShaderValue(g_waterShader, g_locEnvMap, &envUnit, SHADER_UNIFORM_INT);
    }
}

// Continuous sloping river strip (avoids stacked flat segment planes on grades).
struct RiverSample {
    float x = 0.0f, z = 0.0f;
    float y = 0.0f;   // water surface
    float dx = 1.0f, dz = 0.0f; // unit tangent in XZ
    float s = 0.0f;   // arc length along path
    float depth01 = 0.45f;
    float mouth = 0.0f; // 0..1 blend into a lake disc
};

static thread_local std::vector<RiverSample> g_riverSamples;
static thread_local std::vector<float>       g_riverYScratch;

static void pushRiverSample(float x, float z, float freeboard, float& arcS) {
    RiverSample s;
    s.x = x;
    s.z = z;
    const float bed = engine::math::WorldHeight(x, z);
    s.y = bed + freeboard;
    s.s = arcS;
    s.depth01 = std::clamp((s.y - bed) / 5.5f, 0.15f, 1.0f);
    g_riverSamples.push_back(s);
}

static void blendRiverMouths() {
    if (!engine::math::IsHydrologyReady()) return;
    const auto& lakes = engine::math::GetLakes();
    if (lakes.empty()) return;

    for (RiverSample& s : g_riverSamples) {
        float bestT = 0.0f;
        float targetY = s.y;
        for (const engine::math::LakeSite& lake : lakes) {
            const float dx = s.x - lake.x;
            const float dz = s.z - lake.z;
            const float dist = std::sqrt(dx * dx + dz * dz);
            const float r = lake.boundR * 1.05f;
            if (dist >= r) continue;
            // Smoothstep into the basin so the ribbon meets the lake surfaceY
            const float t = 1.0f - dist / r;
            const float w = t * t * (3.0f - 2.0f * t);
            if (w > bestT) {
                bestT = w;
                targetY = lake.surfaceY + 0.08f;
            }
        }
        if (bestT > 0.0f) {
            s.y = s.y * (1.0f - bestT) + targetY * bestT;
            s.mouth = bestT;
            // Hide the ribbon under the lake disc near the mouth
            if (bestT > 0.55f) {
                s.depth01 *= 1.0f - (bestT - 0.55f) * 1.5f;
            }
        }
    }
}

static void buildRiverSamples(const engine::math::RiverPath& river, float freeboard) {
    g_riverSamples.clear();
    if (river.points.size() < 2) return;

    constexpr float kStep = 5.5f; // dense enough that downhill reads as one ribbon
    float arcS = 0.0f;
    pushRiverSample(river.points[0].x, river.points[0].y, freeboard, arcS);

    for (size_t i = 1; i < river.points.size(); ++i) {
        const Vector2& a = river.points[i - 1];
        const Vector2& b = river.points[i];
        float dx = b.x - a.x;
        float dz = b.y - a.y;
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len < 0.25f) continue;
        dx /= len;
        dz /= len;

        float traveled = 0.0f;
        while (traveled + kStep < len - 0.5f) {
            traveled += kStep;
            arcS += kStep;
            pushRiverSample(a.x + dx * traveled, a.y + dz * traveled, freeboard, arcS);
        }
        arcS += (len - traveled);
        pushRiverSample(b.x, b.y, freeboard, arcS);
    }

    if (g_riverSamples.size() < 2) return;

    // Smooth water Y along the path so grades aren't faceted.
    g_riverYScratch.resize(g_riverSamples.size());
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < g_riverSamples.size(); ++i) {
            g_riverYScratch[i] = g_riverSamples[i].y;
        }
        for (size_t i = 1; i + 1 < g_riverSamples.size(); ++i) {
            g_riverSamples[i].y =
                g_riverYScratch[i - 1] * 0.25f +
                g_riverYScratch[i]     * 0.50f +
                g_riverYScratch[i + 1] * 0.25f;
        }
    }

    // Soft downhill bias (upstream ↁEdownstream)
    for (size_t i = 1; i < g_riverSamples.size(); ++i) {
        const float maxRise = 0.12f;
        if (g_riverSamples[i].y > g_riverSamples[i - 1].y + maxRise) {
            g_riverSamples[i].y = g_riverSamples[i - 1].y + maxRise;
        }
    }

    for (RiverSample& s : g_riverSamples) {
        const float bed = engine::math::WorldHeight(s.x, s.z);
        s.y = std::max(s.y, bed + 0.20f);
        s.depth01 = std::clamp((s.y - bed) / 6.0f, 0.12f, 1.0f);
    }

    blendRiverMouths();

    // Tangents from neighbors
    for (size_t i = 0; i < g_riverSamples.size(); ++i) {
        const RiverSample& a = g_riverSamples[i > 0 ? i - 1 : i];
        const RiverSample& b = g_riverSamples[i + 1 < g_riverSamples.size() ? i + 1 : i];
        float dx = b.x - a.x;
        float dz = b.z - a.z;
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len > 1.0e-4f) {
            g_riverSamples[i].dx = dx / len;
            g_riverSamples[i].dz = dz / len;
        } else if (i > 0) {
            g_riverSamples[i].dx = g_riverSamples[i - 1].dx;
            g_riverSamples[i].dz = g_riverSamples[i - 1].dz;
        }
    }
}

static unsigned char depthToU8(float d01) {
    return static_cast<unsigned char>(std::clamp(d01, 0.0f, 1.0f) * 255.0f + 0.5f);
}

static bool pointInLakeBasin(float x, float z) {
    if (!engine::math::IsHydrologyReady()) return false;
    for (const engine::math::LakeSite& lake : engine::math::GetLakes()) {
        const float dx = x - lake.x;
        const float dz = z - lake.z;
        const float r = lake.boundR * 1.05f;
        if (dx * dx + dz * dz > r * r) continue;
        // Inside hydrology influence and bed below the water table ↁElake owns this
        if (engine::math::LakeCoverage(lake, x, z) < 1.15f) {
            const float bed = engine::math::WorldHeight(x, z);
            if (bed < lake.surfaceY + 0.75f) return true;
        }
        // Also catch carve that extends past the ellipse
        const float bed = engine::math::WorldHeight(x, z);
        if (bed < lake.surfaceY - 0.15f) return true;
    }
    return false;
}

static void emitRiverRibbon(float halfW, std::vector<WaterVert>& out) {
    const size_t n = g_riverSamples.size();
    if (n < 2) return;

    constexpr unsigned char kEdgeA = 55;
    constexpr unsigned char kMidA  = 255;
    constexpr float kUvAlong = 0.045f;

    auto push = [&](float x, float y, float z, float nx, float ny, float nz,
                    float u, float v, unsigned char dr, unsigned char a) {
        WaterVert w;
        w.x = x; w.y = y; w.z = z;
        w.nx = nx; w.ny = ny; w.nz = nz;
        w.u = u; w.v = v;
        w.r = dr; w.g = 255; w.b = 255; w.a = a;
        out.push_back(w);
    };

    for (size_t i = 0; i + 1 < n; ++i) {
        const RiverSample& a = g_riverSamples[i];
        const RiverSample& b = g_riverSamples[i + 1];

        if (a.mouth > 0.72f && b.mouth > 0.72f) continue;
        if (pointInLakeBasin(a.x, a.z) && pointInLakeBasin(b.x, b.z)) continue;

        float tdx = a.dx + b.dx;
        float tdz = a.dz + b.dz;
        float tlen = std::sqrt(tdx * tdx + tdz * tdz);
        if (tlen < 1.0e-4f) {
            tdx = a.dx;
            tdz = a.dz;
            tlen = 1.0f;
        }
        tdx /= tlen;
        tdz /= tlen;

        const float px = -tdz * halfW;
        const float pz =  tdx * halfW;

        const float t3x = b.x - a.x;
        const float t3y = b.y - a.y;
        const float t3z = b.z - a.z;
        float nx = t3y * pz;
        float ny = t3z * px - t3x * pz;
        float nz = -t3y * px;
        float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (nlen < 1.0e-5f) {
            nx = 0.0f; ny = 1.0f; nz = 0.0f;
        } else {
            nx /= nlen; ny /= nlen; nz /= nlen;
            if (ny < 0.0f) { nx = -nx; ny = -ny; nz = -nz; }
        }

        const float v0 = a.s * kUvAlong;
        const float v1 = b.s * kUvAlong;
        const unsigned char dA = depthToU8(a.depth01);
        const unsigned char dB = depthToU8(b.depth01);
        const float mouthFadeA = 1.0f - std::clamp((a.mouth - 0.45f) * 2.2f, 0.0f, 1.0f);
        const float mouthFadeB = 1.0f - std::clamp((b.mouth - 0.45f) * 2.2f, 0.0f, 1.0f);
        const unsigned char edgeA = static_cast<unsigned char>(kEdgeA * mouthFadeA);
        const unsigned char midA  = static_cast<unsigned char>(kMidA * mouthFadeA);
        const unsigned char edgeB = static_cast<unsigned char>(kEdgeA * mouthFadeB);
        const unsigned char midB  = static_cast<unsigned char>(kMidA * mouthFadeB);

        push(a.x + px, a.y, a.z + pz, nx, ny, nz, 0.0f, v0, dA, edgeA);
        push(a.x,      a.y, a.z,      nx, ny, nz, 0.5f, v0, dA, midA);
        push(b.x,      b.y, b.z,      nx, ny, nz, 0.5f, v1, dB, midB);
        push(b.x + px, b.y, b.z + pz, nx, ny, nz, 0.0f, v1, dB, edgeB);

        push(a.x,      a.y, a.z,      nx, ny, nz, 0.5f, v0, dA, midA);
        push(a.x - px, a.y, a.z - pz, nx, ny, nz, 1.0f, v0, dA, edgeA);
        push(b.x - px, b.y, b.z - pz, nx, ny, nz, 1.0f, v1, dB, edgeB);
        push(b.x,      b.y, b.z,      nx, ny, nz, 0.5f, v1, dB, midB);
    }
}

static void drawWaterVertList(const std::vector<WaterVert>& verts) {
    // One continuous immediate-mode stream. Albedo is unit 0 via rlSetTexture;
    // normals are procedural  Eno mid-begin SetShaderValueTexture (breaks batches).
    for (const WaterVert& v : verts) {
        rlNormal3f(v.nx, v.ny, v.nz);
        rlColor4ub(v.r, v.g, v.b, v.a);
        rlTexCoord2f(v.u, v.v);
        rlVertex3f(v.x, v.y, v.z);
    }
}

static void drawRiverRibbons(const Camera3D& cam) {
    if (!engine::math::IsHydrologyReady() || !g_waterReady) return;
    EnsureWaterGeometry();
    if (g_cachedRiverVerts.empty()) return;

    constexpr float kCullDist = 2000.0f;
    constexpr float kCullDist2 = kCullDist * kCullDist;

    // Cheap reject: any river vert near camera? (cache is one blob  Ecull by first/last buckets)
    bool near = false;
    for (size_t i = 0; i < g_cachedRiverVerts.size(); i += 32) {
        const WaterVert& v = g_cachedRiverVerts[i];
        const float dx = v.x - cam.position.x;
        const float dz = v.z - cam.position.z;
        if (dx * dx + dz * dz < kCullDist2) { near = true; break; }
    }
    if (!near) return;

    const bool useShader = (g_waterShader.id > 0);
    if (useShader) {
        if (g_waterShader.locs != nullptr && g_waterShader.locs[SHADER_LOC_MATRIX_MODEL] >= 0) {
            Matrix id = MatrixIdentity();
            SetShaderValueMatrix(g_waterShader, g_waterShader.locs[SHADER_LOC_MATRIX_MODEL], id);
        }
        BeginShaderMode(g_waterShader);
    }

    rlSetTexture(g_waterColor.id);
    rlBegin(RL_QUADS);
    drawWaterVertList(g_cachedRiverVerts);
    rlEnd();
    rlDrawRenderBatchActive();
    rlSetTexture(0);
    if (useShader) EndShaderMode();
}

// Bake water surface over the sunken lake disc: quads where bed < waterLevel,
// flood-connected from the site center (soft shore, not full Voronoi cell).
static void bakeLakeMesh(const engine::math::LakeSite& lake, CachedLakeMesh& out) {
    out.site = lake;
    out.verts.clear();

    // Slight lift above bed clamp so translucent surface doesn't z-fight terrain.
    const float waterY = lake.surfaceY + 0.18f;
    const float invDepth = 1.0f / std::max(lake.depth, 1.0f);
    constexpr float kWetEps = 0.06f;
    // Soft disc radii  Ematch noise.cpp waterGate / hydrology clamp.
    constexpr float kCoreR  = 200.0f;
    constexpr float kShoreW = 100.0f;
    constexpr float kUvWorld = 0.045f;

    auto softDiscGate = [&](float x, float z) {
        const float dx = x - lake.x;
        const float dz = z - lake.z;
        const float dist = std::sqrt(dx * dx + dz * dz);
        const float t = std::clamp((dist - kCoreR) / kShoreW, 0.0f, 1.0f);
        const float g = 1.0f - (t * t * (3.0f - 2.0f * t));
        return g * g;
    };

    // --- Pass A: scan over the simulated fill extent ---
    const float scanR = std::max({lake.fillRadius * 1.20f, lake.boundR * 1.15f,
                                  std::max(lake.radiusA, lake.radiusB) * 1.30f,
                                  kCoreR + kShoreW * 1.05f});
    constexpr int kScan = 80;
    const float scanStep = (2.0f * scanR) / static_cast<float>(kScan);
    const float sx0 = lake.x - scanR;
    const float sz0 = lake.z - scanR;
    // Disc gate is the terrain-bowl truth; ellipse cover only trims far outliers.
    const float coverLim = 1.40f;

    float minX = lake.x, maxX = lake.x, minZ = lake.z, maxZ = lake.z;
    bool anyWet = false;
    for (int j = 0; j <= kScan; ++j) {
        for (int i = 0; i <= kScan; ++i) {
            const float x = sx0 + static_cast<float>(i) * scanStep;
            const float z = sz0 + static_cast<float>(j) * scanStep;
            const float gate = softDiscGate(x, z);
            const float cover = engine::math::LakeCoverage(lake, x, z);
            // Keep mesh extent aligned with waterGate disc (not the tighter ellipse).
            if (gate < 0.015f && cover > coverLim) continue;
            const float bed = engine::math::WorldHeight(x, z);
            if (bed >= waterY - kWetEps && gate < 0.05f) continue;
            if (bed >= waterY + 0.85f) continue;
            anyWet = true;
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minZ = std::min(minZ, z);
            maxZ = std::max(maxZ, z);
        }
    }
    if (!anyWet) return;

    // Pad so shore cells aren't flush against the grid wall
    const float pad = std::max(scanStep * 1.25f, 6.0f);
    minX -= pad; maxX += pad;
    minZ -= pad; maxZ += pad;

    // --- Pass B: fine grid  Estair-step silhouettes come from coarse binary cells ---
    const float spanX = std::max(maxX - minX, 16.0f);
    const float spanZ = std::max(maxZ - minZ, 16.0f);
    const float cellTarget = std::clamp(std::max(spanX, spanZ) * 0.009f, 1.20f, 2.25f);
    int nx = std::clamp(static_cast<int>(std::ceil(spanX / cellTarget)), 28, 192);
    int nz = std::clamp(static_cast<int>(std::ceil(spanZ / cellTarget)), 28, 192);
    const float stepX = spanX / static_cast<float>(nx);
    const float stepZ = spanZ / static_cast<float>(nz);
    const int stride = nx + 1;

    std::vector<float> beds(static_cast<size_t>(stride * (nz + 1)));
    for (int j = 0; j <= nz; ++j) {
        for (int i = 0; i <= nx; ++i) {
            const float x = minX + static_cast<float>(i) * stepX;
            const float z = minZ + static_cast<float>(j) * stepZ;
            beds[static_cast<size_t>(j * stride + i)] = engine::math::WorldHeight(x, z);
        }
    }

    std::vector<uint8_t> wet(static_cast<size_t>(nx * nz), 0);
    for (int j = 0; j < nz; ++j) {
        for (int i = 0; i < nx; ++i) {
            const float h00 = beds[static_cast<size_t>(j * stride + i)];
            const float h10 = beds[static_cast<size_t>(j * stride + i + 1)];
            const float h01 = beds[static_cast<size_t>((j + 1) * stride + i)];
            const float h11 = beds[static_cast<size_t>((j + 1) * stride + i + 1)];
            const float minH = std::min(std::min(h00, h10), std::min(h01, h11));
            const float avgH = 0.25f * (h00 + h10 + h01 + h11);

            const float cx = minX + (static_cast<float>(i) + 0.5f) * stepX;
            const float cz = minZ + (static_cast<float>(j) + 0.5f) * stepZ;
            const float gate = softDiscGate(cx, cz);
            const float cover = engine::math::LakeCoverage(lake, cx, cz);
            if (gate < 0.015f && cover > coverLim) continue;

            // Soft inclusion: flooded bed, or shore inside the waterGate disc.
            // Do NOT require cover < 1.15  Eellipse is often tighter than the circular bowl
            // (especially on the minor axis), which left a dry ring of sunken dirt.
            const bool deepWet = (minH < waterY - kWetEps);
            const bool softEdge = (gate > 0.05f && avgH < waterY + 0.85f);
            if (!deepWet && !softEdge) continue;

            wet[static_cast<size_t>(j * nx + i)] = 1;
        }
    }

    std::vector<uint8_t> reach(static_cast<size_t>(nx * nz), 0);
    const int ci = std::clamp(static_cast<int>((lake.x - minX) / stepX), 0, nx - 1);
    const int cj = std::clamp(static_cast<int>((lake.z - minZ) / stepZ), 0, nz - 1);
    std::queue<int> q;
    auto trySeed = [&](int i, int j) {
        if (i < 0 || j < 0 || i >= nx || j >= nz) return;
        const int idx = j * nx + i;
        if (!wet[static_cast<size_t>(idx)] || reach[static_cast<size_t>(idx)]) return;
        reach[static_cast<size_t>(idx)] = 1;
        q.push(idx);
    };
    for (int dj = -3; dj <= 3; ++dj)
        for (int di = -3; di <= 3; ++di)
            trySeed(ci + di, cj + dj);

    if (q.empty()) {
        int best = -1;
        float bestD = 1.0e12f;
        for (int j = 0; j < nz; ++j) {
            for (int i = 0; i < nx; ++i) {
                const int idx = j * nx + i;
                if (!wet[static_cast<size_t>(idx)]) continue;
                const float cx = minX + (static_cast<float>(i) + 0.5f) * stepX;
                const float cz = minZ + (static_cast<float>(j) + 0.5f) * stepZ;
                const float d2 = (cx - lake.x) * (cx - lake.x) + (cz - lake.z) * (cz - lake.z);
                if (d2 < bestD) { bestD = d2; best = idx; }
            }
        }
        if (best >= 0) {
            reach[static_cast<size_t>(best)] = 1;
            q.push(best);
        }
    }

    while (!q.empty()) {
        const int idx = q.front();
        q.pop();
        const int i = idx % nx;
        const int j = idx / nx;
        // 8-connected so thin shallows don't split the basin
        trySeed(i + 1, j);
        trySeed(i - 1, j);
        trySeed(i, j + 1);
        trySeed(i, j - 1);
        trySeed(i + 1, j + 1);
        trySeed(i - 1, j + 1);
        trySeed(i + 1, j - 1);
        trySeed(i - 1, j - 1);
    }

    auto pushV = [&](float x, float z, float bed, float alpha) {
        WaterVert w;
        w.x = x; w.y = waterY; w.z = z;
        w.nx = 0; w.ny = 1; w.nz = 0;
        // World-space UVs (continuous across cells  Eper-cell 0..1 caused river-mode seams)
        w.u = x * kUvWorld;
        w.v = z * kUvWorld;
        w.r = depthToU8(std::clamp((waterY - bed) * invDepth, 0.0f, 1.0f));
        w.g = 255; w.b = 255;
        w.a = static_cast<unsigned char>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
        out.verts.push_back(w);
    };

    for (int j = 0; j < nz; ++j) {
        for (int i = 0; i < nx; ++i) {
            if (!reach[static_cast<size_t>(j * nx + i)]) continue;

            const float h00 = beds[static_cast<size_t>(j * stride + i)];
            const float h10 = beds[static_cast<size_t>(j * stride + i + 1)];
            const float h01 = beds[static_cast<size_t>((j + 1) * stride + i)];
            const float h11 = beds[static_cast<size_t>((j + 1) * stride + i + 1)];

            int wetN = 0;
            const float d00 = waterY - h00;
            const float d10 = waterY - h10;
            const float d01 = waterY - h01;
            const float d11 = waterY - h11;
            if (d00 > kWetEps) ++wetN;
            if (d10 > kWetEps) ++wetN;
            if (d01 > kWetEps) ++wetN;
            if (d11 > kWetEps) ++wetN;
            // Skip one-corner nibbles  Ethey read as blocky stairsteps
            if (wetN < 2) continue;

            const float xA = minX + static_cast<float>(i) * stepX;
            const float zA = minZ + static_cast<float>(j) * stepZ;
            const float xB = xA + stepX;
            const float zB = zA + stepZ;

            // Continuous shore alpha from depth + waterGate disc (ellipse is secondary).
            auto cornerA = [&](float depth, float px, float pz) {
                const float gate = softDiscGate(px, pz);
                const float cover = engine::math::LakeCoverage(lake, px, pz);
                // Gate carries the circular basin; cover only trims far ellipse outliers.
                const float gateFade = smoothstep(0.02f, 0.10f, gate);
                const float coverFade = 1.0f - smoothstep(1.10f, 1.40f, cover);
                const float edgeFade = std::max(gateFade, coverFade * 0.35f);
                if (depth <= 0.0f) {
                    // Faint fringe inside the disc  Egate > ~0.05 matches softEdge
                    if (gate < 0.04f) return 0.0f;
                    return std::clamp(0.22f + gate * 0.40f, 0.16f, 0.55f) * edgeFade;
                }
                const float depthA = smoothstep(0.0f, 0.55f, depth);
                if (wetN >= 4) {
                    return std::clamp(0.62f + depthA * 0.38f, 0.62f, 1.0f)
                         * std::max(edgeFade, 0.80f);
                }
                return std::clamp(0.28f + depthA * 0.45f + gate * 0.25f, 0.22f, 0.92f)
                     * std::max(edgeFade, 0.55f);
            };

            pushV(xA, zA, h00, cornerA(d00, xA, zA));
            pushV(xB, zA, h10, cornerA(d10, xB, zA));
            pushV(xB, zB, h11, cornerA(d11, xB, zB));
            pushV(xA, zB, h01, cornerA(d01, xA, zB));
        }
    }
}

static void BakeWaterGeometry() {
    g_cachedLakes.clear();
    g_cachedRiverVerts.clear();
    if (!engine::math::IsHydrologyReady()) {
        g_waterGeoDirty = false;
        return;
    }

    // Skip only fills seeded on spawn itself; small lake discs may sit near origin.
    constexpr float kSpawnCenterMin = 400.0f;
    for (const engine::math::LakeSite& lake : engine::math::GetLakes()) {
        if (lake.x * lake.x + lake.z * lake.z < kSpawnCenterMin * kSpawnCenterMin) continue;
        CachedLakeMesh mesh;
        bakeLakeMesh(lake, mesh);
        const int vertCount = static_cast<int>(mesh.verts.size());
        if (!mesh.verts.empty()) g_cachedLakes.push_back(std::move(mesh));

        // Verify core bed stays under the water table (no mid-lake islands).
        int fail = 0;
        float worstClear = 99.0f;
        static const float kOffsets[][2] = {
            {0.f, 0.f}, {40.f, 0.f}, {-40.f, 0.f}, {0.f, 40.f}, {0.f, -40.f},
            {90.f, 90.f}, {-90.f, 90.f}, {90.f, -90.f}, {-90.f, -90.f},
            {150.f, 0.f}, {-150.f, 0.f}, {0.f, 150.f}, {0.f, -150.f},
        };
        for (const auto& o : kOffsets) {
            const float dx = o[0], dz = o[1];
            const float dist = std::sqrt(dx * dx + dz * dz);
            if (dist > 195.0f) continue; // interior core only
            const float bed = engine::math::WorldHeight(lake.x + dx, lake.z + dz);
            const float clearance = lake.surfaceY - bed;
            if (clearance < worstClear) worstClear = clearance;
            if (clearance < 0.2f) ++fail;
        }
        TraceLog(LOG_INFO,
                 "WATER: fill (%.0f, %.0f) waterLevel=%.1f verts=%d coreFail=%d minClear=%.2f",
                 lake.x, lake.z, lake.surfaceY, vertCount, fail, worstClear);
    }

    constexpr float kFreeboard = 0.42f;
    for (const engine::math::RiverPath& river : engine::math::GetRivers()) {
        if (river.points.size() < 2) continue;
        buildRiverSamples(river, kFreeboard);
        emitRiverRibbon(river.halfWidth * 1.35f, g_cachedRiverVerts);
    }

    g_waterGeoDirty = false;
    TraceLog(LOG_INFO, "WATER: baked %d biome fills (%d river verts)",
             static_cast<int>(g_cachedLakes.size()),
             static_cast<int>(g_cachedRiverVerts.size()));
}

static void drawLakeDiscs(const Camera3D& cam) {
    if (!engine::math::IsHydrologyReady() || !g_waterReady) return;
    EnsureWaterGeometry();

    constexpr float kCull = 2800.0f;

    const bool useShader = (g_waterShader.id > 0);
    if (useShader) {
        if (g_waterShader.locs != nullptr && g_waterShader.locs[SHADER_LOC_MATRIX_MODEL] >= 0) {
            Matrix id = MatrixIdentity();
            SetShaderValueMatrix(g_waterShader, g_waterShader.locs[SHADER_LOC_MATRIX_MODEL], id);
        }
        BeginShaderMode(g_waterShader);
    }

    rlSetTexture(g_waterColor.id);

    for (const CachedLakeMesh& mesh : g_cachedLakes) {
        const float dx = mesh.site.x - cam.position.x;
        const float dz = mesh.site.z - cam.position.z;
        const float cullR = kCull + mesh.site.boundR;
        if (dx * dx + dz * dz > cullR * cullR) continue;
        if (mesh.verts.empty()) continue;

        if (useShader) bindLakeShaderParams(mesh.site);
        rlBegin(RL_QUADS);
        drawWaterVertList(mesh.verts);
        rlEnd();
        // Flush per lake so the next lake's uniforms apply cleanly
        rlDrawRenderBatchActive();
    }

    if (useShader) {
        const float off = 0.0f;
        SetShaderValue(g_waterShader, g_locLakeMode, &off, SHADER_UNIFORM_FLOAT);
    }

    rlSetTexture(0);
    if (useShader) EndShaderMode();
}

static void drawWorldWater(const Camera3D& cam) {
    if (!g_drawWaterEnabled) return;
    if (!g_waterReady) InitWater();

    const bool under = IsCameraUnderwater(cam);
    const float opacity    = under ? 0.82f : 0.72f;
    const float brightness = under ? 0.70f : 1.0f;
    bindWaterShaderFrame(cam, opacity, brightness);

    // Depth test ON so terrain hills occlude water; mask OFF so translucent water
    // does not punch holes in later transparent passes.
    rlEnableDepthTest();
    rlDisableBackfaceCulling();
    rlDisableDepthMask();
    BeginBlendMode(BLEND_ALPHA);

    // Rivers first (generic mode), then per-lake depth-tinted discs
    drawRiverRibbons(cam);
    drawLakeDiscs(cam);

    EndBlendMode();
    rlEnableDepthMask();
    rlEnableBackfaceCulling();
}

// ---------------------------------------------------------------------------
// Public entry: draw every landmark within a distance budget.
// ---------------------------------------------------------------------------
static constexpr float LANDMARK_DRAW_DISTANCE = 7500.0f;

void DrawLandmarks(const Camera3D& cam) {
    g_camPos = cam.position;

    drawWorldWater(cam);

    for (size_t i = 0; i < LANDMARK_COUNT; ++i) {
        const Landmark& lm = LANDMARKS[i];
        float dx = cam.position.x - lm.center.x;
        float dz = cam.position.z - lm.center.z;
        float d2 = dx * dx + dz * dz;
        if (d2 > LANDMARK_DRAW_DISTANCE * LANDMARK_DRAW_DISTANCE) continue;

        switch (lm.type) {
            case LandmarkType::Church:       drawChurch(lm);       break;
            case LandmarkType::DwarvenMines: drawDwarvenMines(lm); break;
            case LandmarkType::ElvenForest:  drawElvenForest(lm);  break;
            case LandmarkType::WitchHouse:   drawWitchHouse(lm);   break;
            case LandmarkType::LakeTown:     drawLakeTown(lm);     break;
            case LandmarkType::CapitalCity:  drawCapitalCity(lm);  break;
            case LandmarkType::LichCastle:   drawLichCastle(lm);   break;
        }
    }

    panel_build::Draw();
}

}  // namespace game::world
