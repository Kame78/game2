#include "game/world/world_gen.hpp"
#include "game/world/landmarks.hpp"
#include "engine/math/noise.hpp"
#include "raylib.h"
#include "raymath.h"
#include <cmath>

namespace game::world {

// Distance budget for per-element detail (trees, gravestones, city buildings).
static constexpr float DETAIL_DRAW_DISTANCE_SQ = 500.0f * 500.0f;
static Vector3 g_camPos = {0, 0, 0};  // set each frame in DrawLandmarks()

// ---------------------------------------------------------------------------
// Landmark table — fixed world coordinates (in meters).
// Coordinate system: origin (0,0) = church spawn. +Z = north. Skyrim scale (~6 km).
// ---------------------------------------------------------------------------
const Landmark LANDMARKS[] = {
    // Spawn: flat elevated churchyard so gravestones sit level.
    {LandmarkType::Church,       "Church of the Vigil",  { 0.0f,   0.0f,    0.0f}, 180.0f, 180.0f,  4.0f, 120.0f},

    // Dwarven mountain range: Mine entrance pad with gentle slope into mountains.
    {LandmarkType::DwarvenMines, "Kharaz-Dûm",           {-2500.0f, 0.0f, -1500.0f}, 700.0f, 60.0f, 30.0f, 180.0f},

    // Elven forest: gently flatten so trees are on rolling but stable ground.
    {LandmarkType::ElvenForest,  "Silverleaf Wood",      { 2500.0f, 0.0f, -1000.0f}, 800.0f, 700.0f, 6.0f, 200.0f},

    // Witch's hut: small clearing hidden inside the forest.
    {LandmarkType::WitchHouse,   "Hut of the Ashwitch",  { 2700.0f, 0.0f, -1200.0f},  40.0f,  30.0f, 6.0f, 60.0f},

    // Lake town: LAKE is dug into terrain (flatHeight negative → lake bed). Town sits on shore.
    {LandmarkType::LakeTown,     "Blackmere",            {-2000.0f, 0.0f,  1500.0f}, 900.0f, 750.0f, -6.0f, 250.0f},

    // Capital city: massive walled plateau.
    {LandmarkType::CapitalCity,  "Aurelia",              { 2000.0f, 0.0f,  2000.0f}, 700.0f, 650.0f, 12.0f, 200.0f},

    // Lich King's castle: raised cursed ground far south.
    {LandmarkType::LichCastle,   "The Black Spire",      { 0.0f,    0.0f, -2800.0f}, 500.0f, 450.0f, 20.0f, 180.0f},
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

// ---------------------------------------------------------------------------
// Blockout drawing — one function per landmark type. All primitives, no textures.
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

// Deterministic per-position hash → float [0,1) for placing scenery.
static float hashUnit(int seedTag, int gx, int gz) {
    uint64_t h = engine::math::hash2D(
        static_cast<uint64_t>(seedTag) ^ engine::math::GetWorldConfig().seed, gx, gz);
    return engine::math::randFloat01(h);
}

// ---------------- Church + graveyard ----------------
static void drawChurch(const Landmark& lm) {
    float baseY = lm.flatHeight;
    Vector3 c   = {lm.center.x, baseY, lm.center.z};

    // Church footprint: 24 x 40 (X x Z). Open "colonnade" style so first-person walks through easily.
    const float lenX = 24.0f, lenZ = 40.0f;

    // Floor slab
    DrawCube({c.x, baseY + 0.25f, c.z}, lenX, 0.5f, lenZ, COLOR_STONE);

    // Four corner pillars
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sz = -1; sz <= 1; sz += 2) {
            Vector3 p = {c.x + sx * (lenX * 0.5f - 1.5f), baseY + 6.0f,
                         c.z + sz * (lenZ * 0.5f - 1.5f)};
            DrawCube(p, 2.0f, 12.0f, 2.0f, COLOR_STONE);
        }
    }

    // Long-side colonnade pillars (thinner, evenly spaced)
    for (int i = -3; i <= 3; ++i) {
        if (i == 0) continue;  // leave center open (main aisle "window")
        float pz = c.z + i * (lenZ * 0.5f - 2.0f) / 3.5f;
        DrawCube({c.x - lenX * 0.5f + 0.5f, baseY + 5.0f, pz}, 1.0f, 10.0f, 1.5f, COLOR_STONE);
        DrawCube({c.x + lenX * 0.5f - 0.5f, baseY + 5.0f, pz}, 1.0f, 10.0f, 1.5f, COLOR_STONE);
    }

    // Peaked roof (single big slab tilted; simple blockout)
    DrawCube({c.x, baseY + 13.0f, c.z}, lenX + 2.0f, 1.5f, lenZ + 2.0f, COLOR_ROOF);
    // Roof ridge
    DrawCube({c.x, baseY + 15.0f, c.z}, 2.0f, 3.0f, lenZ + 2.0f, COLOR_ROOF);

    // Bell tower at north end
    Vector3 tower = {c.x, baseY + 12.0f, c.z + lenZ * 0.5f - 3.0f};
    DrawCube(tower, 8.0f, 24.0f, 8.0f, COLOR_STONE);
    DrawCube({tower.x, tower.y + 14.0f, tower.z}, 6.0f, 4.0f, 6.0f, COLOR_ROOF);

    // Cross on top of tower
    DrawCube({tower.x, tower.y + 18.5f, tower.z}, 0.4f, 4.0f, 0.4f, COLOR_GOLD);
    DrawCube({tower.x, tower.y + 20.0f, tower.z}, 2.5f, 0.4f, 0.4f, COLOR_GOLD);

    // Altar block at south end
    DrawCube({c.x, baseY + 1.0f, c.z - lenZ * 0.5f + 3.0f}, 4.0f, 1.5f, 2.0f, COLOR_STONE_DARK);

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

            // Height at this spot (might be off the flat pad → use terrain height)
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
    // The mountain itself is provided by the noise ridged layer — we DON'T flatten it much.
    // Just draw a mine entrance and a few dwarven pillars/statues at the base.
    Vector3 c = {lm.center.x, lm.flatHeight, lm.center.z};

    // Mine gate — massive stone archway
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
    float lakeY = lm.flatHeight;  // negative → water surface below sea level
    Vector3 c   = {lm.center.x, lakeY, lm.center.z};

    // Water surface — one big translucent plane over the flattened lake bed
    DrawPlane({c.x, lakeY + 0.5f, c.z}, {lm.flatRadius * 1.8f, lm.flatRadius * 1.8f}, COLOR_WATER);

    // Waterfront town on the SOUTH shore (negative Z side of the lake)
    float shoreZ = c.z - lm.flatRadius * 0.85f;
    float townY  = engine::math::WorldHeight(c.x, shoreZ) + 0.5f;

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
        // Stilts
        for (int j = 0; j < 5; ++j) {
            float sz = dz - 8.0f + j * 4.0f;
            DrawCube({dx - 1.0f, lakeY - 1.5f, sz}, 0.4f, 4.0f, 0.4f, COLOR_WOOD);
            DrawCube({dx + 1.0f, lakeY - 1.5f, sz}, 0.4f, 4.0f, 0.4f, COLOR_WOOD);
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

    // Merchant district blockout — grid of small buildings inside the walls
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
// Public entry: draw every landmark within a distance budget.
// ---------------------------------------------------------------------------
static constexpr float LANDMARK_DRAW_DISTANCE = 4000.0f;

void DrawLandmarks(const Camera3D& cam) {
    g_camPos = cam.position;
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
}

}  // namespace game::world
