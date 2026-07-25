#include "engine/math/noise.hpp"
#include "engine/math/hydrology.hpp"
#include "FastNoiseLite.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace engine::math {

static WorldConfig g_config;
static FastNoiseLite g_baseNoise;
static FastNoiseLite g_mountainNoise;
static FastNoiseLite g_mountainMassNoise;
static FastNoiseLite g_detailNoise;
static FastNoiseLite g_moistureNoise;
static bool g_initialized = false;
static uint64_t g_lastSeed = 0;

// Large cells → only a handful of biomes across the ±3 km world (~2–3 cells per axis).
static constexpr float kBiomeCellSize = 3000.0f;
static constexpr float kBiomeJitter = 0.32f;
static constexpr float kBiomeBlendWidth = 900.0f;

static float smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static void initNoise() {
    if (g_initialized && g_lastSeed == g_config.seed) return;
    g_lastSeed = g_config.seed;

    const int seedA = static_cast<int>(splitmix64(g_config.seed ^ 0x1111ULL));
    const int seedB = static_cast<int>(splitmix64(g_config.seed ^ 0x2222ULL));
    const int seedC = static_cast<int>(splitmix64(g_config.seed ^ 0x3333ULL));
    const int seedD = static_cast<int>(splitmix64(g_config.seed ^ 0x4444ULL));
    const int seedF = static_cast<int>(splitmix64(g_config.seed ^ 0x6666ULL));

    // Plains/hills rolling land: wider low-freq macros; muted high octaves
    // (detailNoise supplies ±~1 m micro separately). Mountains use their own FNLs.
    g_baseNoise.SetSeed(seedA);
    g_baseNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    g_baseNoise.SetFrequency(g_config.plainsFrequency);
    g_baseNoise.SetFractalType(FastNoiseLite::FractalType_FBm);
    g_baseNoise.SetFractalOctaves(4);
    g_baseNoise.SetFractalLacunarity(2.0f);
    g_baseNoise.SetFractalGain(g_config.plainsGain);

    // Local mountain mass — broader lateral blobs (lower freq) for a wider footprint.
    g_mountainMassNoise.SetSeed(seedD);
    g_mountainMassNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    g_mountainMassNoise.SetFrequency(0.00022f);
    g_mountainMassNoise.SetFractalType(FastNoiseLite::FractalType_FBm);
    g_mountainMassNoise.SetFractalOctaves(4);
    g_mountainMassNoise.SetFractalLacunarity(2.0f);
    g_mountainMassNoise.SetFractalGain(0.52f);

    // Ridged peaks — fewer octaves / softer gain so high-freq doesn't jut spikes.
    g_mountainNoise.SetSeed(seedB);
    g_mountainNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    g_mountainNoise.SetFrequency(0.00095f);
    g_mountainNoise.SetFractalType(FastNoiseLite::FractalType_Ridged);
    g_mountainNoise.SetFractalOctaves(4);
    g_mountainNoise.SetFractalLacunarity(2.05f);
    g_mountainNoise.SetFractalGain(0.45f);

    g_detailNoise.SetSeed(seedC);
    g_detailNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    g_detailNoise.SetFrequency(0.05f);
    g_detailNoise.SetFractalType(FastNoiseLite::FractalType_FBm);
    g_detailNoise.SetFractalOctaves(2);

    g_moistureNoise.SetSeed(seedF);
    g_moistureNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    g_moistureNoise.SetFrequency(0.00055f);
    g_moistureNoise.SetFractalType(FastNoiseLite::FractalType_FBm);
    g_moistureNoise.SetFractalOctaves(3);
    g_moistureNoise.SetFractalLacunarity(2.0f);
    g_moistureNoise.SetFractalGain(0.5f);

    g_initialized = true;
}

WorldConfig& GetWorldConfig() { return g_config; }

void ApplyNoiseSettings() {
    // Force FNL re-init so frequency/gain/seed-derived params pick up config edits.
    g_initialized = false;
    initNoise();
}

float Moisture(float x, float z) {
    initNoise();
    return g_moistureNoise.GetNoise(x, z) * 0.5f + 0.5f;
}

// ---------------------------------------------------------------------------
// Voronoi biome map — jittered grid + adjacency constraints + edge blend
// ---------------------------------------------------------------------------

static uint64_t biomeSeed() {
    return g_config.seed ^ 0xB10BE5EDULL;
}

static void siteWorldPos(int cx, int cz, float& outX, float& outZ) {
    const uint64_t h = hash2D(biomeSeed() ^ 0x51FEULL, cx, cz);
    const float jx = (randFloat01(h) - 0.5f) * 2.0f * kBiomeJitter;
    const float jz = (randFloat01(splitmix64(h ^ 0xC0FFEEULL)) - 0.5f) * 2.0f * kBiomeJitter;
    outX = (static_cast<float>(cx) + 0.5f + jx) * kBiomeCellSize;
    outZ = (static_cast<float>(cz) + 0.5f + jz) * kBiomeCellSize;
}

static bool biomesCompatible(WorldRegion a, WorldRegion b) {
    if (a == b) return true;
    const auto is = [](WorldRegion r, WorldRegion x) { return r == x; };
    // Mountains <-> Hills only
    if (is(a, WorldRegion::Mountains) || is(b, WorldRegion::Mountains)) {
        return (is(a, WorldRegion::Hills) || is(b, WorldRegion::Hills));
    }
    // Water <-> Plains, Wetlands, Hills (shore)
    if (is(a, WorldRegion::Water) || is(b, WorldRegion::Water)) {
        return is(a, WorldRegion::Plains) || is(b, WorldRegion::Plains)
            || is(a, WorldRegion::Wetlands) || is(b, WorldRegion::Wetlands)
            || is(a, WorldRegion::Hills) || is(b, WorldRegion::Hills);
    }
    // Hills <-> everything remaining
    if (is(a, WorldRegion::Hills) || is(b, WorldRegion::Hills)) {
        return true;
    }
    // Plains <-> Wetlands
    return (is(a, WorldRegion::Plains) || is(a, WorldRegion::Wetlands))
        && (is(b, WorldRegion::Plains) || is(b, WorldRegion::Wetlands));
}

// Water biomes: dig this far below the local dry-land shelf. Hydrology sets waterLevel
// slightly below that shelf so bed < waterLevel < surrounding shore.
static constexpr float kBiomeWaterDepth = 6.0f;

// Soft lake disc around each Water Voronoi *site* (cell type stays Water for
// classification; only this radius sinks / floods). Radii live on WorldConfig.

// N/S alpine bands — thresholds use NsAlpineDepth (arced |z|), not raw |z|.
// Hills sit where Mountains used to start (half-700). Former hills strip (half-1300→half-700)
// is now the wide mountainGate approach (foothill→mass), not a Hills biome strip.
static constexpr float kMountainBand = 300.0f;          // depth >= half-300  → Mountains (~2700)
static constexpr float kHillsBand    = 700.0f;          // depth >= half-700  → Hills (~2300)
// Cosine bulge toward world center (meters at x≈0); 0 at |x|≈half so corners still contain.
static constexpr float kBorderArcAmp   = 420.0f;
static constexpr float kBorderWobbleAmp = 70.0f;

// Inward arc + slow edge wobble. Added to |z| so the mountain front bows toward
// the playable interior at x≈0 instead of reading as a ruler-straight E–W wall.
static float borderArcOffset(float x) {
    const float half = WorldConfig::WORLD_HALF_EXTENT;
    const float nx = std::clamp(x / half, -1.0f, 1.0f);
    // 1 at x=0, 0 at ±half
    const float arc = 0.5f * (1.0f + std::cos(nx * 3.14159265f));
    // Slow organic scallop along the ridgeline (deterministic; tapered by arc so
    // map corners stay sealed).
    const float wobble = g_mountainMassNoise.GetNoise(x * 0.22f, 19.0f) * kBorderWobbleAmp;
    return kBorderArcAmp * arc + wobble * arc;
}

// Effective N/S alpine depth for band tests (arced containment front).
float NsAlpineDepth(float x, float z) {
    initNoise();
    return std::fabs(z) + borderArcOffset(x);
}

// Interior anchors — the Voronoi cell whose site is nearest to each point is Water.
static constexpr float kForcedWaterAnchors[][2] = {
    { 1100.0f,   450.0f },
    {-1100.0f,  -300.0f },
    {  500.0f, -1100.0f },
};

// True when (cx,cz) owns any forced water anchor (nearest site among 3×3 neighbors).
static bool isForcedWaterCell(int cx, int cz) {
    for (const auto& a : kForcedWaterAnchors) {
        const int agx = static_cast<int>(std::floor(a[0] / kBiomeCellSize));
        const int agz = static_cast<int>(std::floor(a[1] / kBiomeCellSize));
        float bestD2 = 1.0e30f;
        int bestCx = agx;
        int bestCz = agz;
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int tx = agx + dx;
                const int tz = agz + dz;
                float sx = 0.0f, sz = 0.0f;
                siteWorldPos(tx, tz, sx, sz);
                const float ddx = sx - a[0];
                const float ddz = sz - a[1];
                const float d2 = ddx * ddx + ddz * ddz;
                if (d2 < bestD2) {
                    bestD2 = d2;
                    bestCx = tx;
                    bestCz = tz;
                }
            }
        }
        if (bestCx == cx && bestCz == cz) return true;
    }
    return false;
}

static WorldRegion climateHint(int cx, int cz) {
    float sx = 0.0f, sz = 0.0f;
    siteWorldPos(cx, cz, sx, sz);

    // Guaranteed lakes: force Water on the cell that contains each interior anchor
    // (before border bands / random climate can steal the cell).
    if (isForcedWaterCell(cx, cz)) {
        return WorldRegion::Water;
    }

    const float half = WorldConfig::WORLD_HALF_EXTENT;
    // Arced front: mountains bulge toward center in X, not a flat |z| wall.
    const float depth = std::fabs(sz) + borderArcOffset(sx);
    if (depth >= half - kMountainBand) {
        return WorldRegion::Mountains;
    }
    if (depth >= half - kHillsBand) {
        return WorldRegion::Hills;
    }

    const float moist = Moisture(sx, sz);
    const uint64_t h = hash2D(biomeSeed() ^ 0xC11A7EULL, cx, cz);
    const float r = randFloat01(h);

    if (r < 0.05f) return WorldRegion::Mountains;
    if (r < 0.20f) return WorldRegion::Hills;
    // Open water biomes (pond/lake sites — flooded disc is much smaller than the cell)
    if (r < 0.34f && moist > 0.38f) return WorldRegion::Water;
    if (r < 0.48f && moist > 0.50f) return WorldRegion::Wetlands;
    return WorldRegion::Plains;
}

// Deterministic biome per Voronoi cell — MUST be order- and thread-independent.
// Chunk meshes build in parallel via WorldHeight; a thread_local window paint made
// the same cell resolve to Mountains on one worker and Plains on another, producing
// 100m+ edge seams that look like floating islands / sky gaps.
// Illegal geometric neighbors are handled in SampleRegion (blend toward Hills).
static WorldRegion biomeAtCell(int cx, int cz) {
    initNoise();
    return climateHint(cx, cz);
}

std::vector<BiomeCellInfo> CollectBiomeCells(float halfExtent) {
    initNoise();
    std::vector<BiomeCellInfo> out;
    const int iMin = static_cast<int>(std::floor((-halfExtent) / kBiomeCellSize)) - 1;
    const int iMax = static_cast<int>(std::floor(( halfExtent) / kBiomeCellSize)) + 1;
    out.reserve(static_cast<size_t>((iMax - iMin + 1) * (iMax - iMin + 1)));
    for (int cz = iMin; cz <= iMax; ++cz) {
        for (int cx = iMin; cx <= iMax; ++cx) {
            BiomeCellInfo info;
            info.cx = cx;
            info.cz = cz;
            siteWorldPos(cx, cz, info.x, info.z);
            if (info.x < -halfExtent || info.x > halfExtent ||
                info.z < -halfExtent || info.z > halfExtent) {
                continue;
            }
            info.biome = biomeAtCell(cx, cz);
            out.push_back(info);
        }
    }
    return out;
}

struct VoronoiHit {
    int cx = 0;
    int cz = 0;
    float d = 0.0f;
    WorldRegion biome = WorldRegion::Plains;
};

static void sampleVoronoi2(float x, float z, VoronoiHit& nearest, VoronoiHit& second) {
    const int gx = static_cast<int>(std::floor(x / kBiomeCellSize));
    const int gz = static_cast<int>(std::floor(z / kBiomeCellSize));

    nearest.d = 1.0e30f;
    second.d = 1.0e30f;

    for (int dz = -2; dz <= 2; ++dz) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int cx = gx + dx;
            const int cz = gz + dz;
            float sx = 0.0f, sz = 0.0f;
            siteWorldPos(cx, cz, sx, sz);
            const float ddx = x - sx;
            const float ddz = z - sz;
            const float d = std::sqrt(ddx * ddx + ddz * ddz);

            if (d < nearest.d) {
                second = nearest;
                nearest.cx = cx;
                nearest.cz = cz;
                nearest.d = d;
            } else if (d < second.d) {
                second.cx = cx;
                second.cz = cz;
                second.d = d;
            }
        }
    }

    nearest.biome = biomeAtCell(nearest.cx, nearest.cz);
    second.biome = biomeAtCell(second.cx, second.cz);
}

static void addBiomeWeight(RegionWeights& w, WorldRegion b, float amount) {
    switch (b) {
        case WorldRegion::Plains:    w.plains += amount; break;
        case WorldRegion::Hills:    w.hills += amount; break;
        case WorldRegion::Mountains: w.mountains += amount; break;
        case WorldRegion::Wetlands: w.wetlands += amount; break;
        case WorldRegion::Water:    w.water += amount; break;
    }
}

// Local mass — used inside mountain extras / mask (softer threshold → broader footprint).
static float localMountainMass(float x, float z) {
    const float massA = g_mountainMassNoise.GetNoise(x, z);
    const float mx = x * 0.75f + z * 0.35f;
    const float mz = z * 0.75f - x * 0.35f;
    const float massB = g_mountainMassNoise.GetNoise(mx, mz);
    const float massVal = std::max(massA, massB * 0.92f);

    if (massVal <= -0.18f) return 0.0f;
    const float t = std::clamp((massVal + 0.18f) / 1.18f, 0.0f, 1.0f);
    return std::pow(t, 0.75f);
}

// ---------------------------------------------------------------------------
// Per-biome height recipes
// ---------------------------------------------------------------------------

static float detailAt(float x, float z) {
    return g_detailNoise.GetNoise(x, z) * g_config.detailAmplitude;
}

static float baseAt(float x, float z, float ampScale) {
    return g_baseNoise.GetNoise(x, z) * (g_config.baseAmplitude * ampScale);
}

static float plainsHeight(float x, float z) {
    return baseAt(x, z, 1.0f) + detailAt(x, z) + g_config.landShelf;
}

static float hillsHeight(float x, float z) {
    return baseAt(x, z, 1.55f) + detailAt(x, z) + g_config.landShelf;
}

static float wetlandsHeight(float x, float z) {
    return baseAt(x, z, 1.15f) + detailAt(x, z) + g_config.landShelf - 2.8f;
}

// Mountain add-on only (no base/shelf) — scaled by continuous mountain influence.
// Bias toward bulk mass over sharp ridges so max height stays similar without spikes.
static float mountainExtras(float x, float z) {
    const float massMask = localMountainMass(x, z);
    const float amp = g_config.mountainAmplitude;
    const float mountainMass = massMask * (amp * 0.80f);

    const float ridge1 = g_mountainNoise.GetNoise(x, z);
    const float rx = x * 0.7071f - z * 0.7071f;
    const float rz = x * 0.7071f + z * 0.7071f;
    const float ridge2 = g_mountainNoise.GetNoise(rx, rz);
    const float ridge = (ridge1 + ridge2 * 0.55f) / 1.55f;
    const float ridgeT = (ridge > 0.04f) ? (ridge - 0.04f) / 0.96f : 0.0f;
    const float peakShape = std::pow(ridgeT, 1.90f);
    const float peakDetail = peakShape * massMask * (amp * 0.38f);

    const float flankMask = massMask * (1.0f - peakShape);

    constexpr float kSlopeEps = 8.0f;
    const float massAmp = amp * 0.80f;
    const float hPx = localMountainMass(x + kSlopeEps, z) * massAmp;
    const float hMx = localMountainMass(x - kSlopeEps, z) * massAmp;
    const float hPz = localMountainMass(x, z + kSlopeEps) * massAmp;
    const float hMz = localMountainMass(x, z - kSlopeEps) * massAmp;
    const float dHdx = (hPx - hMx) / (2.0f * kSlopeEps);
    const float dHdz = (hPz - hMz) / (2.0f * kSlopeEps);
    const float slope = std::sqrt(dHdx * dHdx + dHdz * dHdz);
    const float midSlope =
        smoothstep(0.10f, 0.28f, slope) * (1.0f - smoothstep(0.70f, 1.15f, slope));
    const float slopeCarve = midSlope * (1.0f - peakShape) * massMask * (amp * 0.030f);

    constexpr float kGullyScale = 2.70f;
    const float warp = g_detailNoise.GetNoise(x * 0.018f, z * 0.018f) * 22.0f;
    const float gRaw = g_mountainNoise.GetNoise(
        x * kGullyScale + warp + 1700.0f,
        z * kGullyScale - 910.0f);
    const float gValley = std::pow(
        1.0f - std::clamp((gRaw + 0.15f) / 1.15f, 0.0f, 1.0f), 1.45f);
    // Milder random gully, slightly stronger valley settle (v1 light pass).
    const float gullyCarve = gValley * flankMask * (amp * 0.018f); // was 0.024

    const float valleySettle =
        massMask * std::pow(1.0f - peakShape, 1.75f) * (amp * 0.020f); // was 0.016

    return mountainMass + peakDetail - slopeCarve - gullyCarve - valleySettle;
}

// Continuous site field: distance to mountain vs lowland (no discrete label flips).
struct BiomeField {
    float mountainGate = 0.0f;
    float waterGate    = 0.0f; // 0 on shore, 1 deep in water biome
    float plains = 0.0f;
    float hills = 0.0f;
    float mountains = 0.0f;
    float wetlands = 0.0f;
    float water = 0.0f;
    WorldRegion primary = WorldRegion::Plains;
};

static BiomeField sampleBiomeField(float x, float z) {
    const int gx = static_cast<int>(std::floor(x / kBiomeCellSize));
    const int gz = static_cast<int>(std::floor(z / kBiomeCellSize));

    float dMountain = 1.0e30f;
    float dLowland = 1.0e30f;
    float dWater = 1.0e30f;

    float wPlains = 0.0f, wHills = 0.0f, wMountains = 0.0f, wWetlands = 0.0f, wWater = 0.0f;
    float wSum = 0.0f;
    constexpr float kSigma = 700.0f;
    const float invTwoSigma2 = 1.0f / (2.0f * kSigma * kSigma);

    WorldRegion bestBiome = WorldRegion::Plains;
    float bestW = -1.0f;

    for (int dz = -2; dz <= 2; ++dz) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int cx = gx + dx;
            const int cz = gz + dz;
            float sx = 0.0f, sz = 0.0f;
            siteWorldPos(cx, cz, sx, sz);
            const float ddx = x - sx;
            const float ddz = z - sz;
            const float d = std::sqrt(ddx * ddx + ddz * ddz);
            const float d2 = ddx * ddx + ddz * ddz;

            const WorldRegion biome = biomeAtCell(cx, cz);

            if (biome == WorldRegion::Mountains) {
                dMountain = std::min(dMountain, d);
            } else {
                dLowland = std::min(dLowland, d);
            }
            if (biome == WorldRegion::Water) {
                dWater = std::min(dWater, d);
            }

            const float w = std::exp(-d2 * invTwoSigma2);
            wSum += w;
            switch (biome) {
                case WorldRegion::Plains:    wPlains += w; break;
                case WorldRegion::Hills:     wHills += w; break;
                case WorldRegion::Mountains: wMountains += w; break;
                case WorldRegion::Wetlands:  wWetlands += w; break;
                case WorldRegion::Water:     wWater += w; break;
            }
            if (w > bestW) {
                bestW = w;
                bestBiome = biome;
            }
        }
    }

    BiomeField f;
    if (wSum > 1.0e-8f) {
        f.plains = wPlains / wSum;
        f.hills = wHills / wSum;
        f.mountains = wMountains / wSum;
        f.wetlands = wWetlands / wSum;
        f.water = wWater / wSum;
    } else {
        f.plains = 1.0f;
    }
    f.primary = bestBiome;

    if (dMountain > 1.0e20f) {
        f.mountainGate = 0.0f;
    } else if (dLowland > 1.0e20f) {
        f.mountainGate = 1.0f;
    } else {
        const float t = dLowland / (dMountain + dLowland);
        // Wider, softer Voronoi mountain influence (less cliffy edge).
        float g = smoothstep(0.42f, 0.84f, t);
        f.mountainGate = std::pow(g, 1.25f);
    }

    // Soft disc around the nearest Water site — not the full Voronoi cell.
    // Outside the disc, fold leftover water weight into wetlands/plains shore.
    float lakeDisc = 0.0f;
    if (dWater < 1.0e20f) {
        const float coreR = g_config.waterBodyCoreR;
        const float shoreW = g_config.waterBodyShoreW;
        lakeDisc = 1.0f - smoothstep(coreR * 0.85f,
                                     coreR + shoreW * 1.15f,
                                     dWater);
        const float g = 1.0f - smoothstep(coreR,
                                         coreR + shoreW,
                                         dWater);
        f.waterGate = g * g;
    } else {
        f.waterGate = 0.0f;
    }
    {
        const float spill = f.water * (1.0f - lakeDisc);
        f.water *= lakeDisc;
        f.wetlands += spill * 0.65f;
        f.plains += spill * 0.35f;
        if (f.primary == WorldRegion::Water && lakeDisc < 0.35f) {
            f.primary = (f.wetlands >= f.plains) ? WorldRegion::Wetlands
                                                 : WorldRegion::Plains;
        }
    }

    {
        const float half = WorldConfig::WORLD_HALF_EXTENT;
        // Long foothill→peak ramp (approach band → mountain band) for gentler slopes /
        // larger lateral mountain mass. Hills biome classification stays at kHillsBand.
        const float depth = std::fabs(z) + borderArcOffset(x);
        float border = smoothstep(half - g_config.mountainApproach, half - kMountainBand, depth);
        // Soft ease — no sharpening; foothills read as mass, not a cliff wall.
        border = std::pow(border, 0.88f);
        if (border > f.mountainGate) {
            f.mountainGate = border;
            f.waterGate = 0.0f; // no open water on alpine borders
            if (border > 0.55f) {
                f.primary = WorldRegion::Mountains;
                f.mountains = std::max(f.mountains, border);
                f.hills = std::max(f.hills, (1.0f - border) * 0.65f);
            } else if (border > 0.12f) {
                f.primary = WorldRegion::Hills;
                f.hills = std::max(f.hills, border);
            }
        }
    }
    return f;
}

RegionWeights SampleRegion(float x, float z) {
    initNoise();
    const BiomeField f = sampleBiomeField(x, z);
    RegionWeights w;
    w.plains = f.plains;
    w.hills = f.hills;
    w.mountains = f.mountains;
    w.wetlands = f.wetlands;
    w.water = f.water;
    w.primary = f.primary;
    return w;
}

WorldRegion PrimaryRegion(float x, float z) {
    return SampleRegion(x, z).primary;
}

float MountainMask(float x, float z) {
    initNoise();
    const BiomeField f = sampleBiomeField(x, z);
    return localMountainMass(x, z) * f.mountainGate;
}

float LandSurfaceHeight(float x, float z) {
    initNoise();

    const BiomeField f = sampleBiomeField(x, z);

    const float lp = f.plains + f.water * 0.15f; // thin beach footing
    const float lh = f.hills + f.mountains;
    const float lw = f.wetlands;
    const float lsum = lp + lh + lw;
    float baseH;
    if (lsum < 1.0e-5f) {
        baseH = hillsHeight(x, z);
    } else {
        baseH = (lp * plainsHeight(x, z) + lh * hillsHeight(x, z) + lw * wetlandsHeight(x, z))
              / lsum;
    }

    // Kill mountain extras inside the lake disc — peaks must not ride the waterGate lerp.
    float h = baseH + mountainExtras(x, z) * f.mountainGate * (1.0f - f.waterGate);

    // Water biomes: sink only inside the soft lake disc (waterGate), not the whole cell.
    // Dig toward a *calmed* basin floor (damped amplitude). Using full local baseH as the
    // bed target leaves ±8–12 m undulation, so interior peaks poke above the flat water table.
    const float sink = f.waterGate;
    if (sink > 1.0e-4f) {
        const float calm = g_config.landShelf
            + g_baseNoise.GetNoise(x, z) * (g_config.baseAmplitude * 0.28f);
        const float bed = calm - kBiomeWaterDepth;
        h = h * (1.0f - sink) + bed * sink;
    }
    return h;
}

float RawTerrainHeight(float x, float z) {
    float h = LandSurfaceHeight(x, z);
    h = ApplyHydrologyCarve(x, z, h);
    return h;
}

static HeightModifier g_heightModifier = nullptr;

void SetHeightModifier(HeightModifier fn) { g_heightModifier = fn; }

float WorldHeight(float x, float z) {
    float h = RawTerrainHeight(x, z);
    if (g_heightModifier) h = g_heightModifier(x, z, h);
    return h;
}

float WaterGate(float x, float z) {
    initNoise();
    return sampleBiomeField(x, z).waterGate;
}

float TerrainSlope(float x, float z) {
    constexpr float e = 1.5f;
    const float hL = WorldHeight(x - e, z);
    const float hR = WorldHeight(x + e, z);
    const float hD = WorldHeight(x, z - e);
    const float hU = WorldHeight(x, z + e);
    float nx = (hL - hR);
    float ny = 2.0f * e;
    float nz = (hD - hU);
    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len < 1.0e-6f) return 0.0f;
    ny /= len;
    return std::clamp(1.0f - ny, 0.0f, 1.0f);
}

float LocalWaterLevel(float x, float z) {
    if (!IsHydrologyReady()) return WaterLevel();
    const auto& lakes = GetLakes();
    float bestD2 = 1.0e30f;
    float y = WaterLevel();
    for (const LakeSite& lake : lakes) {
        const float dx = x - lake.x;
        const float dz = z - lake.z;
        const float d2 = dx * dx + dz * dz;
        if (d2 < bestD2) {
            bestD2 = d2;
            y = lake.surfaceY;
        }
    }
    return y;
}

TerrainProbe SampleTerrainProbe(float x, float z) {
    TerrainProbe p;
    p.x = x;
    p.z = z;
    p.height = WorldHeight(x, z);
    p.slope = TerrainSlope(x, z);
    p.waterGate = WaterGate(x, z);
    p.waterLevel = LocalWaterLevel(x, z);
    p.weights = SampleRegion(x, z);
    return p;
}

const float (*GetForcedWaterAnchors(size_t* count))[2] {
    static constexpr float kAnchors[][2] = {
        { 1100.0f,   450.0f },
        {-1100.0f,  -300.0f },
        {  500.0f, -1100.0f },
    };
    if (count) *count = 3;
    return kAnchors;
}

}  // namespace engine::math
