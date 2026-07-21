#pragma once
#include <cstdint>

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
    static constexpr int   CHUNK_RESOLUTION  = 32;   // vertices per chunk edge
    static constexpr int   LOAD_RADIUS       = 3;    // 7x7 = 49 chunks loaded (~450 m visible)
    static constexpr float WORLD_HALF_EXTENT = 3000.0f;  // ±3 km from origin

    // Height layer amplitudes (world units)
    float baseAmplitude     = 8.0f;    // rolling hills
    float mountainAmplitude = 250.0f;  // massive scaled up mountains
    float detailAmplitude   = 1.5f;    // small variation
    float mountainThreshold = 0.10f;   // Ridged noise > this becomes mountain
};

// Global world config — set once at startup, then read from anywhere.
WorldConfig& GetWorldConfig();

// Raw noise-only terrain height (no landmark modifications). Always deterministic.
float RawTerrainHeight(float x, float z);

// Optional modifier applied on top of raw terrain to carve landmarks, roads, rivers.
// Signature: (worldX, worldZ, rawHeight) -> finalHeight.
// Set ONCE at startup before any worker threads run.
using HeightModifier = float(*)(float x, float z, float rawHeight);
void SetHeightModifier(HeightModifier fn);

// Final terrain height — raw noise + registered modifier. This is what all systems call.
float WorldHeight(float x, float z);

}  // namespace engine::math
