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

    // Call before CloseWindow(); frees all GPU resources and joins pending work.
    void Shutdown();

    // Read-only accessors for HUD / debugging.
    size_t LoadedChunkCount();
    size_t PendingUploadCount();
}

}  // namespace engine::terrain
