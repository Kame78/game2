#pragma once
#include "raylib.h"

namespace engine::render::sky {

// Load equirectangular HDRI, sky shader, and CPU irradiance (ambient cube).
void Init();

// Draw camera-centered sky behind the world. Call first inside BeginMode3D.
void Draw(const Camera3D& cam);

void Shutdown();

float GetExposure();
void  SetExposure(float exposure);

// Tonemapped haze / clear-color match.
Color   GetHazeColor();
Vector3 GetHazeColorLinear();

// Editor: override clear/haze tint strength (multiplies linear haze used by terrain).
void  SetHazeTintStrength(float strength);
float GetHazeTintStrength();

// Floating-point equirect HDRI (bind as envMap for reflections).
Texture GetEnvTexture();

// Cosine-weighted irradiance ambient cube: +X,-X,+Y,-Y,+Z,-Z (linear * exposure).
const Vector3* GetAmbientCube(); // 6 entries
Vector3 EvaluateIrradiance(Vector3 normal);

bool IsReady();

}  // namespace engine::render::sky
