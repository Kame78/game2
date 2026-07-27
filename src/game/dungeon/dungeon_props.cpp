#include "game/dungeon/dungeon_props.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <algorithm>
#include <cstdio>
#include <string>

namespace game::dungeon::props {

namespace {

    bool g_loaded = false;

    Texture2D g_caveFloor{};
    Texture2D g_caveWall{};
    Texture2D g_caveMoss{};
    Texture2D g_caveCliff{};
    bool      g_caveTexReady = false;

    constexpr int kMossRockMax = 8;
    Model g_mossRocks[kMossRockMax]{};
    int   g_mossRockCount = 0;
    bool  g_mossRockReady[kMossRockMax]{};

    std::string assetPath(const char* relative) {
        return std::string(GetApplicationDirectory()) + relative;
    }

    Texture2D tryLoadTex(const char* relative) {
        const std::string path = assetPath(relative);
        Texture2D tex = LoadTexture(path.c_str());
        if (tex.id == 0) {
            TraceLog(LOG_WARNING, "DUNGEON: missing texture %s", path.c_str());
            return Texture2D{};
        }
        SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
        SetTextureWrap(tex, TEXTURE_WRAP_REPEAT);
        TraceLog(LOG_INFO, "DUNGEON: loaded %s (%dx%d)", path.c_str(), tex.width, tex.height);
        return tex;
    }

    void unloadTex(Texture2D& t) {
        if (t.id != 0) UnloadTexture(t);
        t = {};
    }

    void drawCubeLocal(Vector3 center, Vector3 size, Color tint) {
        DrawCube(center, size.x, size.y, size.z, tint);
    }

    void loadCaveKit() {
        if (g_caveTexReady) return;

        g_caveFloor = tryLoadTex("assets/textures/dungeon/cave/cave_floor_c.jpg");
        g_caveWall  = tryLoadTex("assets/textures/dungeon/cave/cave_wall_c.jpg");
        g_caveMoss  = tryLoadTex("assets/textures/dungeon/cave/cave_moss_c.jpg");
        g_caveCliff = tryLoadTex("assets/textures/dungeon/cave/cave_cliff_c.jpg");
        g_caveTexReady = (g_caveFloor.id != 0 || g_caveWall.id != 0);

        g_mossRockCount = 0;
        for (int i = 0; i < kMossRockMax; ++i) {
            char rel[96];
            std::snprintf(rel, sizeof(rel), "assets/models/dungeon/cave/moss_rock_%02d.glb", i);
            const std::string path = assetPath(rel);
            if (!FileExists(path.c_str())) break;

            Model m = LoadModel(path.c_str());
            if (m.meshCount <= 0) {
                TraceLog(LOG_WARNING, "DUNGEON: failed to load %s", path.c_str());
                break;
            }
            // Bind shared moss albedo (meshes exported without materials).
            Texture2D albedo = (g_caveMoss.id != 0) ? g_caveMoss
                             : (g_caveWall.id != 0)  ? g_caveWall
                             : g_caveFloor;
            for (int mi = 0; mi < m.materialCount; ++mi) {
                if (albedo.id != 0) {
                    m.materials[mi].maps[MATERIAL_MAP_ALBEDO].texture = albedo;
                }
                m.materials[mi].maps[MATERIAL_MAP_ALBEDO].color = WHITE;
            }
            g_mossRocks[i] = m;
            g_mossRockReady[i] = true;
            g_mossRockCount = i + 1;
            TraceLog(LOG_INFO, "DUNGEON: loaded %s", path.c_str());
        }
    }

    void unloadCaveKit() {
        for (int i = 0; i < kMossRockMax; ++i) {
            if (!g_mossRockReady[i]) continue;
            // Shared textures — detach before UnloadModel so they aren't freed.
            for (int mi = 0; mi < g_mossRocks[i].materialCount; ++mi) {
                g_mossRocks[i].materials[mi].maps[MATERIAL_MAP_ALBEDO].texture = {};
            }
            UnloadModel(g_mossRocks[i]);
            g_mossRocks[i] = {};
            g_mossRockReady[i] = false;
        }
        g_mossRockCount = 0;

        unloadTex(g_caveFloor);
        unloadTex(g_caveWall);
        unloadTex(g_caveMoss);
        unloadTex(g_caveCliff);
        g_caveTexReady = false;
    }

}  // namespace

void Load() {
    // Procedural masonry props + optional cave kit.
    g_loaded = true;
    loadCaveKit();
}

void Unload() {
    unloadCaveKit();
    g_loaded = false;
}

bool Ready() { return g_loaded; }

void Draw(Kind kind, Vector3 pos, float yawDeg, float scale, Color tint) {
    if (!g_loaded) return;

    rlPushMatrix();
    rlTranslatef(pos.x, pos.y, pos.z);
    rlRotatef(yawDeg, 0.0f, 1.0f, 0.0f);
    rlScalef(scale, scale, scale);

    Color dark = tint;
    dark.r = (unsigned char)(tint.r * 0.7f);
    dark.g = (unsigned char)(tint.g * 0.7f);
    dark.b = (unsigned char)(tint.b * 0.7f);

    switch (kind) {
        case Kind::Sarcophagus:
            drawCubeLocal(Vector3{0.0f, 0.12f, 0.0f}, Vector3{2.4f, 0.24f, 1.3f}, dark);
            drawCubeLocal(Vector3{0.0f, 0.55f, 0.0f}, Vector3{2.2f, 0.9f, 1.1f}, tint);
            drawCubeLocal(Vector3{0.0f, 1.15f, 0.08f}, Vector3{2.05f, 0.28f, 1.05f},
                          Color{(unsigned char)std::min(255, tint.r + 25),
                                (unsigned char)std::min(255, tint.g + 20),
                                (unsigned char)std::min(255, tint.b + 15), 255});
            break;

        case Kind::BrokenStatue:
            drawCubeLocal(Vector3{0.0f, 0.35f, 0.0f}, Vector3{1.1f, 0.7f, 1.1f}, dark);
            drawCubeLocal(Vector3{0.0f, 1.25f, 0.0f}, Vector3{0.6f, 1.1f, 0.45f}, tint);
            drawCubeLocal(Vector3{0.35f, 1.55f, 0.05f}, Vector3{0.35f, 0.25f, 0.25f}, tint);
            drawCubeLocal(Vector3{1.05f, 0.22f, 0.85f}, Vector3{0.45f, 0.4f, 0.45f}, dark);
            break;

        case Kind::IronGate: {
            Color iron = Color{55, 58, 66, 255};
            Color bar  = Color{70, 74, 84, 255};
            drawCubeLocal(Vector3{-1.65f, 2.1f, 0.0f}, Vector3{0.28f, 4.2f, 0.28f}, iron);
            drawCubeLocal(Vector3{ 1.65f, 2.1f, 0.0f}, Vector3{0.28f, 4.2f, 0.28f}, iron);
            drawCubeLocal(Vector3{0.0f, 4.05f, 0.0f}, Vector3{3.6f, 0.28f, 0.28f}, iron);
            for (int i = -2; i <= 2; ++i) {
                drawCubeLocal(Vector3{(float)i * 0.55f, 2.0f, 0.0f},
                              Vector3{0.12f, 3.7f, 0.12f}, bar);
            }
            break;
        }
        default:
            break;
    }

    rlPopMatrix();
}

bool CaveKitReady() { return g_caveTexReady; }

Texture2D CaveFloorAlbedo() { return g_caveFloor; }
Texture2D CaveWallAlbedo()  { return g_caveWall; }
Texture2D CaveMossAlbedo()  { return g_caveMoss; }
Texture2D CaveCliffAlbedo() { return g_caveCliff; }

int MossRockCount() { return g_mossRockCount; }

void DrawMossRock(int variant, Vector3 pos, float yawDeg, float scale, Color tint) {
    if (variant < 0 || variant >= g_mossRockCount || !g_mossRockReady[variant]) return;
    DrawModelEx(g_mossRocks[variant], pos, Vector3{0.0f, 1.0f, 0.0f}, yawDeg,
                Vector3{scale, scale, scale}, tint);
}

}  // namespace game::dungeon::props
