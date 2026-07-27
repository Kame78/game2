#pragma once
#include "engine/terrain/chunk.hpp"
#include "raylib.h"
#include <cstddef>

namespace engine::terrain {

// Chunk manager — Phase 2 streaming grid.
// - Loads a radius of chunks around the player on background workers.
// - Uploads finished meshes to the GPU on the main thread (budgeted per frame).
// - Unloads chunks that fall outside the load radius.
namespace chunks {

    // Call once after raylib window init. Blocks briefly to prewarm chunks around the origin
    // so the first frame has no holes.
    void Init();

    // Call every frame from Update() — enqueues loads/unloads based on player position.
    void Update(Vector3 playerPos);

    // Call inside BeginMode3D / EndMode3D.
    void Draw();

    // Instanced Quaternius grass (baked within draw distance). Call after Draw().
    void DrawGrass(Vector3 viewPos);

    // Instanced Quaternius trees / undergrowth (baked within draw distance). Call after DrawGrass().
    void DrawTrees(Vector3 viewPos);

    // Optional debug overlays (chunk AABB wires). Call inside BeginMode3D after Draw().
    void DrawDebug();

    // Call before CloseWindow(); frees all GPU resources and joins pending work.
    void Shutdown();

    // Drop loaded chunks in a Chebyshev radius so Update() regenerates with current noise.
    void ReloadAround(Vector3 center, int radiusChunks);

    // Read-only accessors for HUD / debugging.
    size_t LoadedChunkCount();
    size_t PendingUploadCount();
    size_t GrassInstanceCount();
    size_t TreeInstanceCount();
    size_t BushInstanceCount();

    // Live grass counters (updated each DrawGrass). Baked = loaded LOD0–1 lists; draw = last frame.
    struct GrassDrawStats {
        size_t baked = 0;
        size_t drawn = 0;
        size_t approxTris = 0; // sum of drawn instance triangle counts
    };
    const GrassDrawStats& GetGrassDrawStats();

    // Live tree/undergrowth counters (updated each DrawTrees).
    struct TreeDrawStats {
        size_t bakedTrees = 0;
        size_t bakedBushes = 0;
        size_t drawn = 0;
        size_t approxTris = 0;
    };
    const TreeDrawStats& GetTreeDrawStats();

    void SetShowChunkBounds(bool show);
    bool GetShowChunkBounds();

    // 0=off, 1=biome, 2=slope, 3=height bands, 4=waterGate
    void SetTerrainDebugMode(int mode);
    int  GetTerrainDebugMode();

    void SetSunDirection(Vector3 dir);
    Vector3 GetSunDirection();
    void SetSunIntensity(float intensity);
    float GetSunIntensity();

    void SetHazeDistance(float start, float end);
    void SetHazeStrength(float strength);
    float GetHazeStart();
    float GetHazeEnd();
    float GetHazeStrength();

    // Grass — bake knobs need ReloadAround; draw distance + enable are live.
    void SetGrassEnabled(bool enabled);
    bool GetGrassEnabled();
    void SetGrassDensity(float density);       // master 0..2 (1 = default)
    float GetGrassDensity();
    void SetGrassMaxSlope(float slope);        // TerrainSlope threshold
    float GetGrassMaxSlope();

    // Single draw distance (meters). Soft fade near the end. Live (no rebuild).
    void SetGrassDrawDistance(float meters);
    float GetGrassDrawDistance();

    // Deprecated stubs — all set/get the single draw distance.
    void SetGrassNearDistance(float meters);
    float GetGrassNearDistance();
    void SetGrassMidDistance(float meters);
    float GetGrassMidDistance();
    void SetGrassFarDistance(float meters);
    float GetGrassFarDistance();

    // Cluster placement. Rebuild to apply.
    void SetGrassClusterMin(int n);
    int  GetGrassClusterMin();
    void SetGrassClusterMax(int n);
    int  GetGrassClusterMax();
    void SetGrassClusterRadius(float meters); // max patch disk radius
    float GetGrassClusterRadius();
    void SetGrassSeedSpacing(float meters);   // patch center spacing (independent of master density)
    float GetGrassSeedSpacing();

    // Meadow clearings. Rebuild to apply.
    void SetGrassMeadowStrength(float strength); // 0=carpet, higher=more clearings
    float GetGrassMeadowStrength();
    void SetGrassMeadowScale(float scale);       // noise frequency (~0.01–0.08)
    float GetGrassMeadowScale();

    // Coverage noise (soft density variation). Rebuild to apply.
    void SetGrassCoverageStrength(float strength);   // 0=ignore noise, 1=full
    float GetGrassCoverageStrength();
    void SetGrassCoverageScale(float scale);         // noise frequency
    float GetGrassCoverageScale();
    void SetGrassCoverageThreshold(float threshold); // drop seeds below this (after strength)
    float GetGrassCoverageThreshold();

    // Size noise frequency (remaps into scale min/max). Rebuild to apply.
    void SetGrassSizeNoiseScale(float scale);
    float GetGrassSizeNoiseScale();

    // Instance scale multipliers (× base clump scale). Rebuild to apply.
    void SetGrassScaleMin(float mul);
    float GetGrassScaleMin();
    void SetGrassScaleMax(float mul);
    float GetGrassScaleMax();

    // Ground sink in centimeters. Rebuild to apply.
    void SetGrassSinkCm(float cm);
    float GetGrassSinkCm();

    // Axis-aligned XZ rects where grass will not bake (building foundations, etc.).
    // Register before chunks::Init or call ReloadAround after changing.
    void ClearGrassExclusions();
    void AddGrassExclusionRect(float minX, float minZ, float maxX, float maxZ);

    // Trees / undergrowth — bake knobs need ReloadAround; draw distance + enable are live.
    void SetTreesEnabled(bool enabled);
    bool GetTreesEnabled();
    void SetTreeDensity(float density);       // master 0..2 (1 = default)
    float GetTreeDensity();
    void SetTreeMaxSlope(float slope);
    float GetTreeMaxSlope();
    void SetTreeDrawDistance(float meters);
    float GetTreeDrawDistance();
    void SetTreeSeedSpacing(float meters);    // hex lattice spacing for trees
    float GetTreeSeedSpacing();
    void SetBushSeedSpacing(float meters);
    float GetBushSeedSpacing();
    void SetTreeScaleMin(float mul);
    float GetTreeScaleMin();
    void SetTreeScaleMax(float mul);
    float GetTreeScaleMax();
    void SetTreeSinkCm(float cm);
    float GetTreeSinkCm();
}

}  // namespace engine::terrain
