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

    // Instanced grass (LOD0 chunks). Call after Draw(), before water/landmarks.
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

    // Grass — density/slope/spacing changes need ReloadAround to rebuild instances.
    void SetGrassEnabled(bool enabled);
    bool GetGrassEnabled();
    void SetGrassDensity(float density);       // 0..2 (1 = default)
    float GetGrassDensity();
    void SetGrassMaxSlope(float slope);        // TerrainSlope threshold
    float GetGrassMaxSlope();
    void SetGrassDrawDistance(float meters);
    float GetGrassDrawDistance();
}

}  // namespace engine::terrain
