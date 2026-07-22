#include "engine/math/noise.hpp"
#include "FastNoiseLite.h"

namespace engine::math {

static WorldConfig g_config;
static FastNoiseLite g_baseNoise;
static FastNoiseLite g_mountainNoise;
static FastNoiseLite g_mountainMassNoise;
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
    const int seedD = static_cast<int>(splitmix64(g_config.seed ^ 0x4444ULL));

    // Base rolling terrain — smooth low-frequency simplex, fractal for variety
    g_baseNoise.SetSeed(seedA);
    g_baseNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    g_baseNoise.SetFrequency(0.0035f);
    g_baseNoise.SetFractalType(FastNoiseLite::FractalType_FBm);
    g_baseNoise.SetFractalOctaves(4);
    g_baseNoise.SetFractalLacunarity(2.0f);
    g_baseNoise.SetFractalGain(0.5f);

    // --- NEW: Broad 3D Mountain Mass (FBm Simplex gives 3D round mountain bodies with broad bases) ---
    g_mountainMassNoise.SetSeed(seedD);
    g_mountainMassNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    g_mountainMassNoise.SetFrequency(0.0006f);
    g_mountainMassNoise.SetFractalType(FastNoiseLite::FractalType_FBm);
    g_mountainMassNoise.SetFractalOctaves(4);
    g_mountainMassNoise.SetFractalLacunarity(2.0f);
    g_mountainMassNoise.SetFractalGain(0.5f);

    // Ridged mountains — sharp peak details applied on top of mountain mass
    g_mountainNoise.SetSeed(seedB);
    g_mountainNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    g_mountainNoise.SetFrequency(0.0012f); 
    g_mountainNoise.SetFractalType(FastNoiseLite::FractalType_Ridged);
    g_mountainNoise.SetFractalOctaves(4);
    g_mountainNoise.SetFractalLacunarity(2.0f);
    g_mountainNoise.SetFractalGain(0.4f);

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

    // --- MODIFIED: Two-Tier Volumetric Mountain System with C1 Continuity ---
    // Tier 1: Broad 3D Mountain Mass (FBm Simplex creates volumetric 3D bases with equal width in all directions)
    float massVal = g_mountainMassNoise.GetNoise(x, z); // roughly -1..1
    float massMask = (massVal > 0.0f) ? std::pow(massVal, 1.3f) : 0.0f;
    float mountainMass = massMask * (g_config.mountainAmplitude * 0.75f);

    // Tier 2: Pointy Peak Details (Ridged noise scaled smoothly by massMask to guarantee C1 mathematical continuity)
    float ridge1 = g_mountainNoise.GetNoise(x, z);
    float rx = x * 0.7071f - z * 0.7071f;
    float rz = x * 0.7071f + z * 0.7071f;
    float ridge2 = g_mountainNoise.GetNoise(rx, rz);
    float ridge = (ridge1 + ridge2 * 0.5f) / 1.5f;

    float ridgeT = (ridge > 0.1f) ? (ridge - 0.1f) / 0.9f : 0.0f;
    float peakDetail = (ridgeT * ridgeT) * massMask * (g_config.mountainAmplitude * 0.25f);

    float mountain = mountainMass + peakDetail;

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
