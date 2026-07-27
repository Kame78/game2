#pragma once
#include "game/dungeon/dungeon.hpp"
#include <cstdint>
#include <vector>

// Theme-driven walkable masks: 2D grid → extruded SolidBox walls + height field.
namespace game::dungeon {

    // Cardinal sockets matching session side indices: 0=+X, 1=-X, 2=+Z, 3=-Z.
    enum class SocketDir : uint8_t {
        PosX = 0,
        NegX = 1,
        PosZ = 2,
        NegZ = 3,
    };

    inline uint8_t SocketBit(SocketDir d) { return (uint8_t)(1u << (uint8_t)d); }

    struct WalkableMask {
        int   nx = 0;
        int   nz = 0;
        float cellSize = 1.5f;
        float halfW = 14.0f;
        float halfD = 14.0f;
        uint8_t sockets = 0; // required door openings used during extrusion
        std::vector<uint8_t> cells;       // row-major iz * nx + ix, 1 = walkable
        std::vector<float>   heights;    // floor offset from dungeon base (meters)
        std::vector<float>   ceilHeights; // ceiling offset from nominal ceil (neg = hangs down)

        bool Valid() const {
            return nx > 0 && nz > 0 && (int)cells.size() == nx * nz &&
                   (heights.empty() || (int)heights.size() == nx * nz) &&
                   (ceilHeights.empty() || (int)ceilHeights.size() == nx * nz);
        }

        bool At(int ix, int iz) const {
            if (ix < 0 || iz < 0 || ix >= nx || iz >= nz) return false;
            return cells[(size_t)iz * (size_t)nx + (size_t)ix] != 0;
        }

        void Set(int ix, int iz, bool on) {
            if (ix < 0 || iz < 0 || ix >= nx || iz >= nz) return;
            cells[(size_t)iz * (size_t)nx + (size_t)ix] = on ? 1 : 0;
        }

        float HeightAt(int ix, int iz) const {
            if (heights.empty() || ix < 0 || iz < 0 || ix >= nx || iz >= nz) return 0.0f;
            return heights[(size_t)iz * (size_t)nx + (size_t)ix];
        }

        void SetHeight(int ix, int iz, float h) {
            if (heights.empty() || ix < 0 || iz < 0 || ix >= nx || iz >= nz) return;
            heights[(size_t)iz * (size_t)nx + (size_t)ix] = h;
        }

        float CeilHeightAt(int ix, int iz) const {
            if (ceilHeights.empty() || ix < 0 || iz < 0 || ix >= nx || iz >= nz) return 0.0f;
            return ceilHeights[(size_t)iz * (size_t)nx + (size_t)ix];
        }

        void SetCeilHeight(int ix, int iz, float h) {
            if (ceilHeights.empty() || ix < 0 || iz < 0 || ix >= nx || iz >= nz) return;
            ceilHeights[(size_t)iz * (size_t)nx + (size_t)ix] = h;
        }

        // True when this outward face must stay open for a corridor socket.
        bool IsDoorMouth(int ix, int iz, int dix, int diz) const;

        // Room-local XZ (origin at room center). Margin expands/shrinks the test.
        bool ContainsLocal(float lx, float lz, float margin = 0.0f) const;

        // World-space convenience.
        bool ContainsWorld(Vector3 roomCenter, Vector3 p, float margin = 0.0f) const;

        // Bilinear height sample in room-local XZ (0 outside / non-walkable → 0).
        float SampleHeightLocal(float lx, float lz) const;
        float SampleHeightWorld(Vector3 roomCenter, float x, float z) const;

        // Bilinear ceiling offset sample (same UV as floor height).
        float SampleCeilHeightLocal(float lx, float lz) const;
        float SampleCeilHeightWorld(Vector3 roomCenter, float x, float z) const;

        // Uniform pick among walkable cell centers (room-local). Returns false if empty.
        bool SampleWalkableLocal(uint64_t seed, int salt, float& outLx, float& outLz) const;

        Vector3 CellCenterLocal(int ix, int iz) const;
    };

    // Build a theme grammar mask that fills the room AABB and opens required sockets.
    WalkableMask BuildMask(Theme theme, SizeTier tier, float halfW, float halfD,
                           uint8_t socketBits, uint32_t seed, float cellSize = 1.5f);

}  // namespace game::dungeon
