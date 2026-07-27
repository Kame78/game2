#pragma once
#include "game/world/building_panels.hpp"
#include "raylib.h"

namespace game::world::panel_build {

struct PlacedPiece {
    building_panels::Piece piece = building_panels::Piece::Floor;
    building_panels::Style style = building_panels::Style::Stone;
    Vector3 pos = {0, 0, 0};
    float   yawDeg = 0.0f;
};

void Init();
void Shutdown();

void SetEnabled(bool enabled);
bool IsEnabled();

void SetSelectedPiece(building_panels::Piece piece);
building_panels::Piece GetSelectedPiece();
void SetSelectedStyle(building_panels::Style style);
building_panels::Style GetSelectedStyle();

void RotateYaw(int steps); // ±1 → ±90°
float GetYawDeg();

// Look-ray → snap ghost. Call each frame when enabled.
void Update(const Camera3D& cam);

bool TryPlace();
bool TryRemove();
void ClearAll();

void Draw(); // placed pieces + ghost (when enabled)

bool Load(const char* path = "assets/data/buildings_placed.json");
bool Save(const char* path = "assets/data/buildings_placed.json");

int  PlacedCount();
const PlacedPiece* PlacedData();

// Piece/style name helpers for editor UI / JSON
const char* PieceName(building_panels::Piece p);
const char* StyleName(building_panels::Style s);
building_panels::Piece PieceFromName(const char* name);
building_panels::Style StyleFromName(const char* name);

}  // namespace game::world::panel_build
