#pragma once
#include <cstdint>
#include <vector>

namespace engine::math {

// Splitmix64 — fast, high-quality integer hash used to derive per-location seeds
// from a single world seed. Deterministic across compilers/platforms.
constexpr uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// Combine a world seed with 2D integer coords into a deterministic hash.
inline uint64_t hash2D(uint64_t seed, int32_t x, int32_t z) {
    uint64_t h = seed;
    h = splitmix64(h ^ static_cast<uint64_t>(static_cast<uint32_t>(x)));
    h = splitmix64(h ^ static_cast<uint64_t>(static_cast<uint32_t>(z)));
    return h;
}

// Uniform float in [0, 1) from a hash — for deterministic random placement.
inline float randFloat01(uint64_t hash) {
    // Use top 24 bits for float mantissa precision
    return static_cast<float>(hash >> 40) / static_cast<float>(1u << 24);
}

struct WorldConfig {
    uint64_t seed = 0xC0FFEE1234ULL;

    // World grid in meters — Skyrim-scale target (~6 km per side, ~37 km²)
    static constexpr float CHUNK_SIZE        = 128.0f;
    // 2^n+1 so LOD edge verts nest exactly (33⊃17⊃9⊃5⊃3) and hide cracks
    static constexpr int   CHUNK_RESOLUTION  = 33;   // default/LOD0 resolution
    // Was 32 (≈4225 chunks / ~4 km) — flooded VRAM uploads and hitching while walking.
    static constexpr int   LOAD_RADIUS       = 12;   // 25x25 ≈ 625 chunks (~1.5 km)
    static constexpr int   UNLOAD_RADIUS     = 14;   // hysteresis unload (~1.8 km)
    static constexpr float WORLD_HALF_EXTENT = 3000.0f;  // ±3 km from origin
    // N/S containment mountains use an arced front (see NsAlpineDepth), not a flat |z| wall.

    // Multi-level terrain LOD — near detail, far cheap meshes
    static constexpr int LOD0_RADIUS = 3;   // 0..3  (~384m)  -> 33x33
    static constexpr int LOD1_RADIUS = 7;   // 4..7  (~896m)  -> 17x17
    static constexpr int LOD2_RADIUS = 12;  // 8..12 (~1.5km) -> 9x9
    static constexpr int LOD3_RADIUS = 14;  // 13..14 (unload ring) -> 5x5

    static inline int GetLODForDistance(int dist) {
        if (dist <= LOD0_RADIUS) return 0;
        if (dist <= LOD1_RADIUS) return 1;
        if (dist <= LOD2_RADIUS) return 2;
        if (dist <= LOD3_RADIUS) return 3;
        return 4;
    }

    static inline int GetResolutionForLOD(int lod) {
        switch (lod) {
            case 0:  return 33;
            case 1:  return 17;
            case 2:  return 9;
            case 3:  return 5;
            default: return 5; // keep 5 at far ring (was 3) — nested with LOD3
        }
    }

    // Height layer amplitudes (world units)
    float baseAmplitude     = 8.0f;    // rolling macros (keep low so peaks feel huge)
    float mountainAmplitude = 580.0f;  // tall ranges; seams handled by influence ramp, not amp crush
    float detailAmplitude   = 1.2f;    // plains/hills micro (±~1.2 m); was 2.0
    float mountainThreshold = 0.10f;   // Ridged noise > this becomes mountain

    // Live-editable height-noise knobs (ApplyNoiseSettings + chunk reload to see on mesh)
    float plainsFrequency   = 0.0020f; // base FNL frequency (plains/hills macros)
    float plainsGain        = 0.28f;   // base FNL fractal gain
    float landShelf         = 12.0f;   // absolute plains shelf Y
    float mountainApproach  = 1450.0f; // N/S alpine approach band (from half-extent)
    float waterBodyCoreR    = 200.0f;  // soft lake disc core radius
    float waterBodyShoreW   = 100.0f;  // shore falloff beyond core
};

// Re-apply FastNoiseLite params from GetWorldConfig() (call after editing knobs).
void ApplyNoiseSettings();

inline int GetLODForDistance(int dist) {
    return WorldConfig::GetLODForDistance(dist);
}

inline int GetResolutionForLOD(int lod) {
    return WorldConfig::GetResolutionForLOD(lod);
}

// Global world config — set once at startup, then read from anywhere.
WorldConfig& GetWorldConfig();

// Raw noise-only terrain height (no landmark modifications). Always deterministic.
float RawTerrainHeight(float x, float z);

// Voronoi biome cells — adjacency-constrained layout with soft edge blend.
enum class WorldRegion : uint8_t {
    Plains    = 0,
    Hills     = 1,
    Mountains = 2,
    Wetlands  = 3,
    Water     = 4, // lake site — soft disc bed below water table (not full Voronoi cell)
};

// Soft blend weights at a world position (sum ≈ 1). primary = nearest cell biome.
struct RegionWeights {
    float plains    = 1.0f;
    float hills     = 0.0f;
    float mountains = 0.0f;
    float wetlands  = 0.0f;
    float water     = 0.0f;
    WorldRegion primary = WorldRegion::Plains;
};

RegionWeights SampleRegion(float x, float z);
WorldRegion   PrimaryRegion(float x, float z);

// Voronoi cell sites overlapping [-halfExtent, +halfExtent] (for lakes, etc.).
struct BiomeCellInfo {
    int         cx = 0;
    int         cz = 0;
    float       x  = 0.0f;
    float       z  = 0.0f;
    WorldRegion biome = WorldRegion::Plains;
};
std::vector<BiomeCellInfo> CollectBiomeCells(float halfExtent);

// Moisture in [0,1] — drives biome tint (marsh / plains / dry). Deterministic.
float Moisture(float x, float z);

// Soft lake disc weight [0,1] (0 shore / dry, 1 deep water bed).
float WaterGate(float x, float z);

// Terrain slope factor [0,1] from height samples (0 = flat, 1 = cliff).
float TerrainSlope(float x, float z);

// Local water table Y (nearest lake surface, else fallback WaterLevel).
float LocalWaterLevel(float x, float z);

// Editor / HUD probe under a world XZ.
struct TerrainProbe {
    float         x = 0.0f;
    float         z = 0.0f;
    float         height = 0.0f;
    float         slope = 0.0f;
    float         waterGate = 0.0f;
    float         waterLevel = 1.5f;
    RegionWeights weights;
};
TerrainProbe SampleTerrainProbe(float x, float z);

// Guaranteed interior lake anchors (world XZ). count out-param.
const float (*GetForcedWaterAnchors(size_t* count))[2];

// Fallback / river water Y. Per-biome lakes use LakeSite::surfaceY (above sunken beds).
inline float WaterLevel() { return 1.5f; }

// Land elevation before hydrology carves (includes land shelf). Used by hydrology planning.
float LandSurfaceHeight(float x, float z);

// Local mountain mass in [0,1], gated by mountain biome weight (near-zero outside mountains).
float MountainMask(float x, float z);

// Effective N/S alpine depth: |z| + cosine arc bulge toward center (+ mild edge wobble).
// Compare against WORLD_HALF_EXTENT - band (hills ≈ 700, peaks ≈ 300; approach ≈ 1450).
float NsAlpineDepth(float x, float z);

// Optional modifier applied on top of raw terrain to carve landmarks, roads, rivers.
// Signature: (worldX, worldZ, rawHeight) -> finalHeight.
// Set ONCE at startup before any worker threads run.
using HeightModifier = float(*)(float x, float z, float rawHeight);
void SetHeightModifier(HeightModifier fn);

// Final terrain height — raw noise + registered modifier. This is what all systems call.
float WorldHeight(float x, float z);

}  // namespace engine::math
