#pragma once
#include "raylib.h"

namespace game::dungeon::props {

    enum class Kind : int {
        Sarcophagus = 0,
        BrokenStatue,
        IronGate,
        Count
    };

    void Load();
    void Unload();
    bool Ready();

    // Draw a prop with yaw in degrees. scale multiplies the authored mesh size.
    void Draw(Kind kind, Vector3 pos, float yawDeg, float scale, Color tint);

    // ---- Natural Caves kit (textures + moss rock meshes) ----
    bool CaveKitReady();
    Texture2D CaveFloorAlbedo();
    Texture2D CaveWallAlbedo();   // also used for ceilings
    Texture2D CaveMossAlbedo();
    Texture2D CaveCliffAlbedo();

    int  MossRockCount();
    void DrawMossRock(int variant, Vector3 pos, float yawDeg, float scale, Color tint);

}  // namespace game::dungeon::props
