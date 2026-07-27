#pragma once
#include "raylib.h"

namespace game::world::building_panels {

enum class Piece {
    Floor,
    Wall,
    WallDoor,
    WallWindow,
    Pillar,
    RoofSlope,
    Gable,       // one-bay right triangle (Grid × RoofRise); high side = local +X
    GableRamp,   // one-bay trapezoid (left Rise, right 2*Rise) for cascade after Gable
    WallRise,    // short wall: Grid wide × RoofRise tall (matches gable bay / roof end)
    RoofPyramid, // square-base pyramid cap (tower / hip roof); base = 2*Grid
    Stairs,
    Count
};

enum class Style {
    Stone,
    Wood,
    RoofDark
};

// Generate and upload shared piece Models. Call once on the main thread after window init.
void Init();
void Shutdown();

// Draw a shared piece. Origins:
//   Floor / Stairs: center of the 4x4 module on the ground plane (bottom of slab).
//   Wall / WallDoor / WallWindow: centerline of the wall bottom (panel sits on +Z side of wall line).
//   Pillar: center of pillar footprint on ground.
//   RoofSlope: center of the 4x4 bay (like Floor); eave on local -Z, ridge on local +Z.
//   Gable: wall-center bottom at mid-base of one Grid bay; right triangle, high at local +X,
//          height RoofRise (same pitch as RoofSlope). Use mirrorX for the opposite rake.
//   GableRamp: same origin; trapezoid continuing the rake (place at +RoofRise Y after a Gable).
//   WallRise: wall-center bottom; Grid × RoofRise panel (closes gable-high openings / roof ends).
//   RoofPyramid: center of square base on the lower plane; base side = 2*Grid.
// yawDeg is typically a multiple of 90 for landmarks.
// mirrorX flips local X (opposite side of a peaked gable); backface cull is disabled while drawing.
void Draw(Piece piece, Vector3 pos, float yawDeg, Style style, Color tint = WHITE,
          bool mirrorX = false);

float Grid();              // 4.f — module width / floor span
float WallHeight();        // 3.f — wall panel height
float DoorHeight();        // ~2.6f opening clearance
float RoofRise();          // rise from eave to ridge over one grid bay
float RoofPyramidHeight(); // peak height of RoofPyramid above its base

}  // namespace game::world::building_panels
