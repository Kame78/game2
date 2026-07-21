#pragma once
#include "raylib.h"

namespace game::world {

// Register the height modifier that flattens terrain under landmarks.
// Call ONCE at startup before spawning any worker threads (i.e. before terrain::chunks::Init).
void InstallHeightModifier();

// Draw all landmark blockouts (called inside BeginMode3D / EndMode3D).
// Handles distance culling itself.
void DrawLandmarks(const Camera3D& cam);

}  // namespace game::world
