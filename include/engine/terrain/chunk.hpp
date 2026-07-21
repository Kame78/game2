#pragma once
#include "raylib.h"
#include <cstdint>

namespace engine::terrain {

// Integer chunk coordinate on the world grid.
struct ChunkCoord {
    int32_t x = 0;
    int32_t z = 0;

    bool operator==(const ChunkCoord& o) const { return x == o.x && z == o.z; }
};

// A loaded terrain chunk — GPU mesh + collision data.
// Owns the raylib Model (must be UnloadModel'd on destruction).
struct TerrainChunk {
    ChunkCoord coord = {0, 0};
    Model      model = {};       // GPU mesh
    bool       modelLoaded = false;
    int        lod   = 0;        // 0 = highest detail

    // World-space bounds (recomputed after mesh gen)
    Vector3 aabbMin = {0, 0, 0};
    Vector3 aabbMax = {0, 0, 0};
};

}  // namespace engine::terrain
