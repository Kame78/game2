#include "engine/math/noise.hpp"
#include "FastNoiseLite.h"

namespace engine::math {

static WorldConfig g_config;
static FastNoiseLite g_baseNoise;
static FastNoiseLite g_mountainNoise;
static FastNoiseLite g_detailNoise;
static bool g_initialized = false;
static uint64_t g_lastSeed = 0;

static void initNoise() {
    if (g_initialized && g_lastSeed == g_config.seed) return;
    g_lastSeed = g_config.seed;

    // Derive separate integer seeds from the world seed so layers are uncorrelated
    const int seedA = static_cast<int>(splitmix64(g_config.seed ^ 0x1111ULL));
    const int seedB = static_cast<int>(splitmix64(g_config.seed ^ 0x2222ULL));
    const int seedC = static_cast<int>(splitmix64(g_config.seed ^ 0x3333ULL));

    // Base rolling terrain — smooth low-frequency simplex, fractal for variety
    g_baseNoise.SetSeed(seedA);
    g_baseNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    g_baseNoise.SetFrequency(0.0035f);
    g_baseNoise.SetFractalType(FastNoiseLite::FractalType_FBm);
    g_baseNoise.SetFractalOctaves(4);
    g_baseNoise.SetFractalLacunarity(2.0f);
    g_baseNoise.SetFractalGain(0.5f);

    // Ridged mountains — sharp peaks, only kick in above threshold
    g_mountainNoise.SetSeed(seedB);
    g_mountainNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    g_mountainNoise.SetFrequency(0.0018f);
    g_mountainNoise.SetFractalType(FastNoiseLite::FractalType_Ridged);
    g_mountainNoise.SetFractalOctaves(5);
    g_mountainNoise.SetFractalLacunarity(2.0f);
    g_mountainNoise.SetFractalGain(0.5f);

    // High-frequency detail for surface texture
    g_detailNoise.SetSeed(seedC);
    g_detailNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    g_detailNoise.SetFrequency(0.05f);
    g_detailNoise.SetFractalType(FastNoiseLite::FractalType_FBm);
    g_detailNoise.SetFractalOctaves(2);

    g_initialized = true;
}

WorldConfig& GetWorldConfig() { return g_config; }

float RawTerrainHeight(float x, float z) {
    initNoise();

    // Base terrain: -1..1 → -baseAmp..baseAmp
    float base = g_baseNoise.GetNoise(x, z) * g_config.baseAmplitude;

    // Mountains: ridged noise returns high values only along ridges. Threshold + smooth ramp.
    float ridge = g_mountainNoise.GetNoise(x, z);  // roughly -1..1
    float mountain = 0.0f;
    if (ridge > g_config.mountainThreshold) {
        float t = (ridge - g_config.mountainThreshold) / (1.0f - g_config.mountainThreshold);
        mountain = t * t * g_config.mountainAmplitude;  // squared for steeper base
    }

    // Fine detail
    float detail = g_detailNoise.GetNoise(x, z) * g_config.detailAmplitude;

    return base + mountain + detail;
}

// Height modifier — game code can register a function that carves landmarks,
// roads, or rivers on top of the raw noise. Called from worker threads, so
// set it ONCE before workers start (typically during Init).
static HeightModifier g_heightModifier = nullptr;

void SetHeightModifier(HeightModifier fn) { g_heightModifier = fn; }

float WorldHeight(float x, float z) {
    float h = RawTerrainHeight(x, z);
    if (g_heightModifier) h = g_heightModifier(x, z, h);
    return h;
}

}  // namespace engine::math
