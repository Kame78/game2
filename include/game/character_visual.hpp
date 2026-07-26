#pragma once
#include "raylib.h"
#include <cstdint>

namespace game {

    // Procedural character silhouettes (composed primitives — not placeholder cubes).
    enum class CharacterVisual : uint8_t {
        Box = 0,
        Humanoid,
        Elite,
        Gargoyle,
        BattleAngel,
        ArchAngel,
        Reaper,
        Pixie,
        Sprite,
    };

    // `center` is the AABB center (same convention as DrawCube). `yawRadians`: 0 faces +Z.
    void DrawCharacterVisual(CharacterVisual visual, Vector3 center,
                             float width, float height, float depth,
                             Color color, float yawRadians, float animTime);

}
