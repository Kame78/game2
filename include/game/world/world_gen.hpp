#pragma once
#include "raylib.h"

namespace game::world {

// Register the height modifier that flattens terrain under landmarks.
// Call ONCE at startup before spawning any worker threads (i.e. before terrain::chunks::Init).
void InstallHeightModifier();

// Load/unload water albedo (and related) textures. Call after window init / before close.
void InitWater();
void ShutdownWater();

// Editor: toggle lake/river mesh draw (landmarks still draw).
void SetDrawWaterEnabled(bool enabled);
bool GetDrawWaterEnabled();

// Rebuild cached water geometry next draw (after lake param edits).
void MarkWaterGeometryDirty();

// Draw all landmark blockouts (called inside BeginMode3D / EndMode3D).
// Handles distance culling itself.
void DrawLandmarks(const Camera3D& cam);

// True when the camera is below the water table inside a flooded basin.
bool IsCameraUnderwater(const Camera3D& cam);

// Full-screen blue tint — call after EndMode3D while underwater.
void DrawUnderwaterOverlay();

}  // namespace game::world
