#pragma once
#include "raylib.h"
#include <cstddef>

namespace game::world {

enum class LandmarkType {
    Church,          // Spawn — gothic church + graveyard
    DwarvenMines,    // Mountain range with a mine entrance (Moria-inspired)
    ElvenForest,     // Dense forest with tree-top elven village
    WitchHouse,      // Small hut hidden inside the elven forest
    LakeTown,        // Waterfront town on a great lake (Blackwater-inspired)
    CapitalCity,     // Walled city (Leyndell-inspired)
    LichCastle,      // Fortress of the Lich King on corrupted ground
};

struct Landmark {
    LandmarkType type;
    const char*  name;
    Vector3      center;        // World coords (Y is ignored — terrain-relative)
    float        radius;        // Visual/interaction radius in meters
    float        flatRadius;    // Terrain flattening radius (0 = no flattening)
    float        flatHeight;    // Absolute Y the ground is pulled toward inside flatRadius
    float        flatFalloff;   // Extra distance outside flatRadius over which we blend
};

// Fixed handcrafted landmarks. Order is stable; indices are not (do not persist).
extern const Landmark LANDMARKS[];
extern const size_t   LANDMARK_COUNT;

}  // namespace game::world
