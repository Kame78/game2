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

    // Instanced grass with distance LODs (baked on LOD0 chunks). Call after Draw().
    void DrawGrass(Vector3 viewPos);

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

    // Live grass counters (updated each DrawGrass). Baked = loaded LOD0 lists; draw = last frame.
    struct GrassDrawStats {
        size_t bakedNear = 0;
        size_t bakedMid  = 0;
        size_t bakedFar  = 0;
        size_t drawNear  = 0;
        size_t drawMid   = 0;
        size_t drawFar   = 0;
        size_t approxTris = 0; // drawNear*clumpTris + (drawMid+drawFar)*impostorTris
    };
    const GrassDrawStats& GetGrassDrawStats();

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

    // Grass — bake knobs need ReloadAround; LOD distances + enable are live.
    void SetGrassEnabled(bool enabled);
    bool GetGrassEnabled();
    void SetGrassDensity(float density);       // master 0..2 (1 = default)
    float GetGrassDensity();
    void SetGrassMaxSlope(float slope);        // TerrainSlope threshold
    float GetGrassMaxSlope();

    // Distance LOD bands (meters). near < mid < far. Live (no rebuild).
    void SetGrassNearDistance(float meters);
    float GetGrassNearDistance();
    void SetGrassMidDistance(float meters);
    float GetGrassMidDistance();
    void SetGrassFarDistance(float meters);
    float GetGrassFarDistance();
    void SetGrassDrawDistance(float meters); // alias for far
    float GetGrassDrawDistance();

    // Per-band density multipliers (relative to master density). Rebuild to apply.
    void SetGrassNearDensity(float mul);
    float GetGrassNearDensity();
    void SetGrassMidDensity(float mul);
    float GetGrassMidDensity();
    void SetGrassFarDensity(float mul);
    float GetGrassFarDensity();

    // Cluster placement (near bake). Rebuild to apply.
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

    // Instance scale multipliers (× base clump scale). Rebuild to apply.
    void SetGrassScaleMin(float mul);
    float GetGrassScaleMin();
    void SetGrassScaleMax(float mul);
    float GetGrassScaleMax();

    // Ground sink in centimeters. Rebuild to apply.
    void SetGrassSinkCm(float cm);
    float GetGrassSinkCm();
}

}  // namespace engine::terrain
