#pragma once
#include "raylib.h"
#include <cstdint>
#include <cstddef>
#include <vector>

namespace engine::math {

struct LakeSite {
    float x         = 0.0f;
    float z         = 0.0f;
    float radiusA   = 120.0f; // ellipse major axis (meters) — mesh / cull guide
    float radiusB   = 90.0f;  // ellipse minor axis
    float angle     = 0.0f;   // radians, ellipse rotation
    float depth     = 6.0f;   // expected bed depth below surfaceY (matches biome dig)
    float surfaceY  = 1.5f;   // water table for this Water biome fill
    float warpAmp   = 0.18f;  // shoreline irregularity (visual / coverage helper)
    float warpFreq  = 3.0f;
    float phase     = 0.0f;
    // Conservative AABB half-extent for culling / spatial hash
    float boundR    = 140.0f;
    // Flood / bake scan radius (soft lake disc around Water site)
    float fillRadius = 140.0f;
};

struct RiverPath {
    std::vector<Vector2> points; // XZ samples, upstream → downstream
    float halfWidth = 10.0f;
};

void BuildHydrology(uint64_t seed);
void ClearHydrology();
bool IsHydrologyReady();

// Call before BuildHydrology to keep lakes/rivers out of spawn & authored pads.
void ClearHydrologyExclusions();
void AddHydrologyExclusion(float x, float z, float radius);

struct HydrologyExclusion {
    float x = 0.0f;
    float z = 0.0f;
    float radius = 0.0f;
};
const std::vector<HydrologyExclusion>& GetHydrologyExclusions();

const std::vector<LakeSite>&  GetLakes();
const std::vector<RiverPath>& GetRivers();

// Editor mutation — returns null if index OOB. Caller should reload water mesh / chunks.
LakeSite* GetLakeMutable(size_t index);

// Normalized radial coverage: < 1 = inside lake (ellipse + warped shoreline).
float LakeCoverage(const LakeSite& lake, float x, float z);

// World-space rim radius along angle (for drawing irregular discs).
float LakeRimRadius(const LakeSite& lake, float angleRad);

float ApplyHydrologyCarve(float x, float z, float height);

}  // namespace engine::math
