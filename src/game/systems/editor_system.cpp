#include "game/systems.hpp"
#include "game/factories/entity_factory.hpp"
#include "game/world/world_gen.hpp"
#include "game/world/landmarks.hpp"
#include "engine/input.hpp"
#include "engine/math/noise.hpp"
#include "engine/math/hydrology.hpp"
#include "engine/terrain/chunk_manager.hpp"
#include "engine/render/sky.hpp"
#include "raymath.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace game::systems {

bool g_showEditor = false;
float g_playerMoveSpeed = 10.0f;

namespace {

struct Bookmark {
    char name[48] = {};
    Vector3 pos = {0, 40, 0};
};

std::vector<Bookmark> g_bookmarks;
bool g_bookmarksSeeded = false;
bool g_showExclusions = false;
bool g_showWaterGateViz = false;
bool g_showLoadRadius = false;
int g_selectedLake = -1;
int g_placeMode = 0; // 0 none, 1 enemy, 2 spawner, 3 landmark
int g_landmarkPlaceType = 0;
uint32_t g_editorNetId = 9000;
bool g_probeEnabled = true;

void seedBookmarks() {
    if (g_bookmarksSeeded) return;
    g_bookmarksSeeded = true;

    auto add = [](const char* name, float x, float y, float z) {
        Bookmark b;
        std::snprintf(b.name, sizeof(b.name), "%s", name);
        b.pos = {x, y, z};
        g_bookmarks.push_back(b);
    };

    const float half = engine::math::WorldConfig::WORLD_HALF_EXTENT;
    float spawnY = engine::math::WorldHeight(0.0f, 0.0f) + 8.0f;
    add("Spawn (Church)", 0.0f, spawnY, 0.0f);

    size_t nAnchors = 0;
    const float (*anchors)[2] = engine::math::GetForcedWaterAnchors(&nAnchors);
    for (size_t i = 0; i < nAnchors; ++i) {
        float x = anchors[i][0];
        float z = anchors[i][1];
        float y = engine::math::WorldHeight(x, z) + 25.0f;
        char buf[48];
        std::snprintf(buf, sizeof(buf), "Lake anchor %zu", i + 1);
        add(buf, x, y, z);
    }

    if (engine::math::IsHydrologyReady()) {
        const auto& lakes = engine::math::GetLakes();
        for (size_t i = 0; i < lakes.size() && i < 6; ++i) {
            float x = lakes[i].x;
            float z = lakes[i].z;
            float y = lakes[i].surfaceY + 30.0f;
            char buf[48];
            std::snprintf(buf, sizeof(buf), "Lake %zu", i);
            add(buf, x, y, z);
        }
    }

    float nRimZ = half - 200.0f;
    float sRimZ = -(half - 200.0f);
    add("North rim", 0.0f, engine::math::WorldHeight(0.0f, nRimZ) + 40.0f, nRimZ);
    add("South rim", 0.0f, engine::math::WorldHeight(0.0f, sRimZ) + 40.0f, sRimZ);
}

engine::ecs::Entity playerEntity(engine::ecs::Registry& reg) {
    if (reg.playerInputs.data.empty()) return {0};
    return {reg.playerInputs.indexToEntity[0]};
}

Vector3 cameraLookHit(engine::ecs::Registry& reg, float maxDist = 400.0f) {
    auto pe = playerEntity(reg);
    if (!reg.cameras.Has(pe)) return {0, 0, 0};
    const Camera3D& cam = reg.cameras.Get(pe).camera;
    Vector3 dir = Vector3Normalize(Vector3Subtract(cam.target, cam.position));

    // March along look ray and snap to terrain height when near ground.
    float bestT = maxDist;
    Vector3 best = Vector3Add(cam.position, Vector3Scale(dir, maxDist));
    for (float t = 2.0f; t < maxDist; t += 2.0f) {
        Vector3 p = Vector3Add(cam.position, Vector3Scale(dir, t));
        float ground = engine::math::WorldHeight(p.x, p.z);
        if (p.y <= ground + 1.5f) {
            bestT = t;
            best = {p.x, ground, p.z};
            break;
        }
    }
    (void)bestT;
    return best;
}

Vector3 probeXZ(engine::ecs::Registry& reg) {
    auto pe = playerEntity(reg);
    if (reg.cameras.Has(pe)) {
        const Camera3D& cam = reg.cameras.Get(pe).camera;
        Vector3 dir = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
        // Prefer ground under crosshair; fall back to camera XZ.
        Vector3 hit = cameraLookHit(reg, 250.0f);
        if (Vector3Distance(hit, cam.position) < 240.0f) return hit;
        return {cam.position.x, 0.0f, cam.position.z};
    }
    if (reg.transforms.Has(pe)) {
        auto& t = reg.transforms.Get(pe);
        return t.position;
    }
    return {0, 0, 0};
}

void teleportPlayer(engine::ecs::Registry& reg, Vector3 pos) {
    auto pe = playerEntity(reg);
    if (!reg.transforms.Has(pe)) return;
    auto& t = reg.transforms.Get(pe);
    t.position = pos;
    if (reg.playerInputs.Has(pe)) {
        reg.playerInputs.Get(pe).velocityY = 0.0f;
        reg.playerInputs.Get(pe).isFlying = true;
    }
}

const char* regionName(engine::math::WorldRegion r) {
    using WR = engine::math::WorldRegion;
    switch (r) {
        case WR::Plains: return "Plains";
        case WR::Hills: return "Hills";
        case WR::Mountains: return "Mountains";
        case WR::Wetlands: return "Wetlands";
        case WR::Water: return "Water";
    }
    return "?";
}

void drawCylinderWire(Vector3 center, float radius, float height, Color color, int segments = 32) {
    float y0 = center.y;
    float y1 = center.y + height;
    for (int i = 0; i < segments; ++i) {
        float a0 = (float)i / (float)segments * 2.0f * PI;
        float a1 = (float)(i + 1) / (float)segments * 2.0f * PI;
        Vector3 p0 = {center.x + cosf(a0) * radius, y0, center.z + sinf(a0) * radius};
        Vector3 p1 = {center.x + cosf(a1) * radius, y0, center.z + sinf(a1) * radius};
        Vector3 q0 = {p0.x, y1, p0.z};
        Vector3 q1 = {p1.x, y1, p1.z};
        DrawLine3D(p0, p1, color);
        DrawLine3D(q0, q1, color);
        if ((i % 4) == 0) DrawLine3D(p0, q0, color);
    }
}

}  // namespace

void EditorInputSystem(engine::ecs::Registry& reg) {
    // Toggle editor with ~ (KEY_GRAVE), F2, or P
    if (IsKeyPressed(KEY_GRAVE) || IsKeyPressed(KEY_F2) || IsKeyPressed(KEY_P)) {
        g_showEditor = !g_showEditor;
        if (g_showEditor) {
            engine::input::UnlockCursor();
            seedBookmarks();
        } else {
            engine::input::LockCursor();
        }
    }

    if (g_showEditor) {
        if (IsKeyPressed(KEY_LEFT_ALT)) {
            if (engine::input::IsCursorLocked()) engine::input::UnlockCursor();
            else engine::input::LockCursor();
        }

        // Place / delete while free-look (cursor locked) so mouse aim works.
        if (engine::input::IsCursorLocked() && !ImGui::GetIO().WantCaptureMouse) {
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && g_placeMode != 0) {
                Vector3 hit = cameraLookHit(reg);
                hit.y = engine::math::WorldHeight(hit.x, hit.z) + 1.0f;
                if (g_placeMode == 1) {
                    factories::EntityFactory::CreateEnemy(reg, hit, g_editorNetId++);
                } else if (g_placeMode == 2) {
                    factories::EntityFactory::CreateSpawner(reg, hit);
                } else if (g_placeMode == 3) {
                    factories::EntityFactory::CreateLandmarkProxy(reg, hit, g_landmarkPlaceType);
                }
            }
            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                Vector3 hit = cameraLookHit(reg, 120.0f);
                float bestD = 8.0f;
                engine::ecs::Entity best = {0};
                bool found = false;
                auto consider = [&](engine::ecs::Entity e) {
                    if (!reg.transforms.Has(e)) return;
                    if (reg.playerInputs.Has(e)) return;
                    Vector3 p = reg.transforms.Get(e).position;
                    float d = Vector3Distance(p, hit);
                    if (d < bestD) {
                        bestD = d;
                        best = e;
                        found = true;
                    }
                };
                for (size_t i = 0; i < reg.enemyAIs.data.size(); ++i)
                    consider({reg.enemyAIs.indexToEntity[i]});
                for (size_t i = 0; i < reg.spawners.data.size(); ++i)
                    consider({reg.spawners.indexToEntity[i]});
                for (size_t i = 0; i < reg.landmarkProxies.data.size(); ++i)
                    consider({reg.landmarkProxies.indexToEntity[i]});
                if (found) engine::ecs::DestroyEntity(reg, best);
            }
        }
    }
}

void EditorUISystem(engine::ecs::Registry& reg) {
    if (!g_showEditor) return;
    seedBookmarks();

    ImGui::SetNextWindowPos(ImVec2(20, 40), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 640), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Developer Editor (~ / F2 / P)")) {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped("LEFT ALT: toggle mouse lock. Place modes: LMB place, RMB delete (while locked).");
    ImGui::Separator();

    // --- Flight / speed ---
    if (!reg.playerInputs.data.empty()) {
        auto pe = playerEntity(reg);
        if (reg.playerInputs.Has(pe)) {
            auto& input = reg.playerInputs.Get(pe);
            ImGui::Checkbox("Flight Mode", &input.isFlying);
            ImGui::SameLine();
            ImGui::Checkbox("NoClip", &input.noClip);
        }
    }
    ImGui::SliderFloat("Move Speed", &g_playerMoveSpeed, 5.0f, 500.0f);

    if (ImGui::CollapsingHeader("1. Fly cam + Bookmarks", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto pe = playerEntity(reg);
        if (reg.transforms.Has(pe)) {
            auto& t = reg.transforms.Get(pe);
            ImGui::Text("Pos: %.1f, %.1f, %.1f", t.position.x, t.position.y, t.position.z);
        }
        if (ImGui::Button("Save current cam")) {
            auto pe2 = playerEntity(reg);
            if (reg.transforms.Has(pe2)) {
                Bookmark b;
                std::snprintf(b.name, sizeof(b.name), "User %d", (int)g_bookmarks.size());
                b.pos = reg.transforms.Get(pe2).position;
                g_bookmarks.push_back(b);
            }
        }
        ImGui::BeginChild("bm_list", ImVec2(0, 120), true);
        for (size_t i = 0; i < g_bookmarks.size(); ++i) {
            ImGui::PushID((int)i);
            if (ImGui::Selectable(g_bookmarks[i].name)) {
                teleportPlayer(reg, g_bookmarks[i].pos);
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    if (ImGui::CollapsingHeader("2. Terrain debug overlay")) {
        int mode = engine::terrain::chunks::GetTerrainDebugMode();
        const char* modes[] = {"Off", "Biome", "Slope", "Height bands", "WaterGate"};
        if (ImGui::Combo("Debug mode", &mode, modes, 5)) {
            engine::terrain::chunks::SetTerrainDebugMode(mode);
        }
        ImGui::TextDisabled("Biome/WaterGate need chunk reload after noise edits.");
    }

    if (ImGui::CollapsingHeader("3. Height noise knobs")) {
        auto& cfg = engine::math::GetWorldConfig();
        ImGui::SliderFloat("Plains freq", &cfg.plainsFrequency, 0.0002f, 0.008f, "%.5f");
        ImGui::SliderFloat("Plains gain", &cfg.plainsGain, 0.05f, 0.8f);
        ImGui::SliderFloat("Mountain amp", &cfg.mountainAmplitude, 50.0f, 900.0f);
        ImGui::SliderFloat("Mountain approach", &cfg.mountainApproach, 400.0f, 2000.0f);
        ImGui::SliderFloat("Land shelf", &cfg.landShelf, 0.0f, 40.0f);
        ImGui::SliderFloat("Water core R", &cfg.waterBodyCoreR, 40.0f, 400.0f);
        ImGui::SliderFloat("Water shore W", &cfg.waterBodyShoreW, 10.0f, 250.0f);
        if (ImGui::Button("Apply + reload chunks around cam")) {
            engine::math::ApplyNoiseSettings();
            auto pe = playerEntity(reg);
            Vector3 c = reg.transforms.Has(pe) ? reg.transforms.Get(pe).position : Vector3{0, 0, 0};
            engine::terrain::chunks::ReloadAround(c, 8);
            game::world::MarkWaterGeometryDirty();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload r=16")) {
            engine::math::ApplyNoiseSettings();
            auto pe = playerEntity(reg);
            Vector3 c = reg.transforms.Has(pe) ? reg.transforms.Get(pe).position : Vector3{0, 0, 0};
            engine::terrain::chunks::ReloadAround(c, 16);
        }
        ImGui::TextWrapped("New samples use knobs immediately. Full world: restart or reload while flying.");
    }

    if (ImGui::CollapsingHeader("4. Water tools")) {
        bool drawW = game::world::GetDrawWaterEnabled();
        if (ImGui::Checkbox("Draw water mesh", &drawW)) {
            game::world::SetDrawWaterEnabled(drawW);
        }
        ImGui::Checkbox("Viz waterGate discs", &g_showWaterGateViz);

        const auto& lakes = engine::math::GetLakes();
        ImGui::Text("Lakes: %d", (int)lakes.size());
        if (!lakes.empty()) {
            if (g_selectedLake < 0) g_selectedLake = 0;
            if (g_selectedLake >= (int)lakes.size()) g_selectedLake = (int)lakes.size() - 1;
            ImGui::SliderInt("Selected lake", &g_selectedLake, 0, (int)lakes.size() - 1);
            if (ImGui::Button("Teleport to lake")) {
                const auto& L = lakes[(size_t)g_selectedLake];
                teleportPlayer(reg, {L.x, L.surfaceY + 25.0f, L.z});
            }
            if (engine::math::LakeSite* L = engine::math::GetLakeMutable((size_t)g_selectedLake)) {
                if (ImGui::SliderFloat("fillRadius", &L->fillRadius, 40.0f, 500.0f)) {
                    L->boundR = std::max({L->radiusA, L->radiusB, L->fillRadius}) * 1.08f;
                    game::world::MarkWaterGeometryDirty();
                }
                if (ImGui::SliderFloat("radiusA", &L->radiusA, 20.0f, 500.0f)) {
                    L->boundR = std::max({L->radiusA, L->radiusB, L->fillRadius}) * 1.08f;
                    game::world::MarkWaterGeometryDirty();
                }
                if (ImGui::SliderFloat("radiusB", &L->radiusB, 20.0f, 500.0f)) {
                    L->boundR = std::max({L->radiusA, L->radiusB, L->fillRadius}) * 1.08f;
                    game::world::MarkWaterGeometryDirty();
                }
                if (ImGui::SliderFloat("Depth", &L->depth, 1.0f, 20.0f)) {
                    game::world::MarkWaterGeometryDirty();
                }
                if (ImGui::SliderFloat("Surface Y", &L->surfaceY, -20.0f, 80.0f)) {
                    game::world::MarkWaterGeometryDirty();
                }
                ImGui::Text("boundR=%.0f", L->boundR);
            }
        }
        ImGui::TextDisabled("Shore exposure: Water shore W / core R in noise knobs.");
    }

    if (ImGui::CollapsingHeader("5. Sky / HDRI")) {
        float exp = engine::render::sky::GetExposure();
        if (ImGui::SliderFloat("Exposure", &exp, 0.05f, 4.0f)) {
            engine::render::sky::SetExposure(exp);
        }
        float hs = engine::terrain::chunks::GetHazeStart();
        float he = engine::terrain::chunks::GetHazeEnd();
        float hz = engine::terrain::chunks::GetHazeStrength();
        if (ImGui::SliderFloat("Haze start", &hs, 0.0f, 4000.0f)) {
            engine::terrain::chunks::SetHazeDistance(hs, he);
        }
        if (ImGui::SliderFloat("Haze end", &he, 500.0f, 8000.0f)) {
            engine::terrain::chunks::SetHazeDistance(hs, he);
        }
        if (ImGui::SliderFloat("Haze strength", &hz, 0.0f, 1.0f)) {
            engine::terrain::chunks::SetHazeStrength(hz);
        }
        float tint = engine::render::sky::GetHazeTintStrength();
        if (ImGui::SliderFloat("Haze tint", &tint, 0.0f, 2.0f)) {
            engine::render::sky::SetHazeTintStrength(tint);
        }
    }

    if (ImGui::CollapsingHeader("6. Chunk viz")) {
        bool showB = engine::terrain::chunks::GetShowChunkBounds();
        if (ImGui::Checkbox("Draw chunk bounds", &showB)) {
            engine::terrain::chunks::SetShowChunkBounds(showB);
        }
        ImGui::Checkbox("Show load radius ring", &g_showLoadRadius);
        ImGui::Text("Loaded: %zu  Pending: %zu  LOAD_RADIUS=%d",
                    engine::terrain::chunks::LoadedChunkCount(),
                    engine::terrain::chunks::PendingUploadCount(),
                    engine::math::WorldConfig::LOAD_RADIUS);
        if (ImGui::Button("Force reload around cam (r=6)")) {
            auto pe = playerEntity(reg);
            Vector3 c = reg.transforms.Has(pe) ? reg.transforms.Get(pe).position : Vector3{0, 0, 0};
            engine::terrain::chunks::ReloadAround(c, 6);
        }
    }

    if (ImGui::CollapsingHeader("7. Probe readout", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Live probe", &g_probeEnabled);
        if (g_probeEnabled) {
            Vector3 xz = probeXZ(reg);
            auto probe = engine::math::SampleTerrainProbe(xz.x, xz.z);
            ImGui::Text("XZ: %.1f, %.1f", probe.x, probe.z);
            ImGui::Text("Height: %.2f", probe.height);
            ImGui::Text("Primary: %s", regionName(probe.weights.primary));
            ImGui::Text("Weights P/H/M/W/Wa: %.2f %.2f %.2f %.2f %.2f",
                        probe.weights.plains, probe.weights.hills, probe.weights.mountains,
                        probe.weights.wetlands, probe.weights.water);
            ImGui::Text("Slope: %.3f  waterGate: %.3f  waterLevel: %.2f",
                        probe.slope, probe.waterGate, probe.waterLevel);
        }
    }

    if (ImGui::CollapsingHeader("8. Time-of-day / sun")) {
        Vector3 sun = engine::terrain::chunks::GetSunDirection();
        float yaw = std::atan2(sun.z, sun.x);
        float pitch = std::asin(std::clamp(sun.y, -1.0f, 1.0f));
        float yawDeg = yaw * RAD2DEG;
        float pitchDeg = pitch * RAD2DEG;
        bool changed = false;
        changed |= ImGui::SliderFloat("Sun yaw", &yawDeg, -180.0f, 180.0f);
        changed |= ImGui::SliderFloat("Sun pitch", &pitchDeg, 5.0f, 89.0f);
        if (changed) {
            float yr = yawDeg * DEG2RAD;
            float pr = pitchDeg * DEG2RAD;
            Vector3 d = {
                std::cos(pr) * std::cos(yr),
                std::sin(pr),
                std::cos(pr) * std::sin(yr),
            };
            engine::terrain::chunks::SetSunDirection(d);
        }
        float inten = engine::terrain::chunks::GetSunIntensity();
        if (ImGui::SliderFloat("Sun intensity", &inten, 0.0f, 3.0f)) {
            engine::terrain::chunks::SetSunIntensity(inten);
        }
        ImGui::TextDisabled("Approximate directional sun (terrain + water).");
    }

    if (ImGui::CollapsingHeader("9. Entity place / delete")) {
        const char* modes[] = {"Off", "Enemy", "Spawner", "Landmark proxy"};
        ImGui::Combo("Place mode", &g_placeMode, modes, 4);
        if (g_placeMode == 3) {
            static const char* lmNames[7] = {};
            static bool initNames = false;
            if (!initNames) {
                for (size_t i = 0; i < game::world::LANDMARK_COUNT && i < 7; ++i)
                    lmNames[i] = game::world::LANDMARKS[i].name;
                initNames = true;
            }
            ImGui::Combo("Landmark type", &g_landmarkPlaceType, lmNames,
                         (int)std::min(game::world::LANDMARK_COUNT, size_t{7}));
        }
        ImGui::Text("Enemies:%d Spawners:%d Proxies:%d",
                    (int)reg.enemyAIs.data.size(),
                    (int)reg.spawners.data.size(),
                    (int)reg.landmarkProxies.data.size());
        ImGui::TextWrapped("Lock mouse (ALT), LMB place at ground under crosshair, RMB delete nearest.");
        if (ImGui::Button("Place at camera XZ")) {
            auto pe = playerEntity(reg);
            if (reg.transforms.Has(pe)) {
                Vector3 p = reg.transforms.Get(pe).position;
                p.y = engine::math::WorldHeight(p.x, p.z) + 1.0f;
                if (g_placeMode == 1) factories::EntityFactory::CreateEnemy(reg, p, g_editorNetId++);
                else if (g_placeMode == 2) factories::EntityFactory::CreateSpawner(reg, p);
                else if (g_placeMode == 3)
                    factories::EntityFactory::CreateLandmarkProxy(reg, p, g_landmarkPlaceType);
            }
        }
    }

    if (ImGui::CollapsingHeader("10. Exclusion zones")) {
        ImGui::Checkbox("Visualize exclusions", &g_showExclusions);
        const auto& ex = engine::math::GetHydrologyExclusions();
        ImGui::Text("Zones: %d", (int)ex.size());
        for (size_t i = 0; i < ex.size(); ++i) {
            ImGui::Text("#%zu  xz=(%.0f, %.0f) r=%.0f", i, ex[i].x, ex[i].z, ex[i].radius);
        }
        ImGui::TextDisabled("Hydrology/church pads registered at world init.");
    }

    if (ImGui::CollapsingHeader("11. Grass")) {
        bool en = engine::terrain::chunks::GetGrassEnabled();
        if (ImGui::Checkbox("Enable grass", &en)) {
            engine::terrain::chunks::SetGrassEnabled(en);
        }
        float dens = engine::terrain::chunks::GetGrassDensity();
        float slope = engine::terrain::chunks::GetGrassMaxSlope();
        float dist = engine::terrain::chunks::GetGrassDrawDistance();
        if (ImGui::SliderFloat("Density", &dens, 0.0f, 2.0f, "%.2f")) {
            engine::terrain::chunks::SetGrassDensity(dens);
        }
        if (ImGui::SliderFloat("Max slope", &slope, 0.05f, 0.80f, "%.2f")) {
            engine::terrain::chunks::SetGrassMaxSlope(slope);
        }
        if (ImGui::SliderFloat("Draw distance", &dist, 40.0f, 500.0f, "%.0f m")) {
            engine::terrain::chunks::SetGrassDrawDistance(dist);
        }
        ImGui::Text("Instances (loaded LOD0): %zu", engine::terrain::chunks::GrassInstanceCount());
        ImGui::TextDisabled("Model clumps; Plains heavy, Wetlands/Hills sparse. Cap 1400/chunk.");
        if (ImGui::Button("Rebuild grass (reload r=4)")) {
            auto pe = playerEntity(reg);
            Vector3 c = reg.transforms.Has(pe) ? reg.transforms.Get(pe).position : Vector3{0, 0, 0};
            engine::terrain::chunks::ReloadAround(c, 4);
        }
        ImGui::TextDisabled("After density/slope edits, press Rebuild.");
    }

    ImGui::End();
}

void EditorDebugDrawSystem(engine::ecs::Registry& reg) {
    if (!g_showEditor) return;

    engine::terrain::chunks::DrawDebug();

    auto pe = playerEntity(reg);
    Vector3 camPos = reg.transforms.Has(pe) ? reg.transforms.Get(pe).position : Vector3{0, 0, 0};

    if (g_showLoadRadius) {
        const float S = engine::math::WorldConfig::CHUNK_SIZE;
        const float R = engine::math::WorldConfig::LOAD_RADIUS * S;
        drawCylinderWire({camPos.x, camPos.y - 20.0f, camPos.z}, R, 40.0f, Color{100, 220, 255, 200});
        const float UR = engine::math::WorldConfig::UNLOAD_RADIUS * S;
        drawCylinderWire({camPos.x, camPos.y - 20.0f, camPos.z}, UR, 40.0f, Color{255, 180, 80, 160});
    }

    if (g_showExclusions) {
        for (const auto& ex : engine::math::GetHydrologyExclusions()) {
            float gy = engine::math::WorldHeight(ex.x, ex.z);
            drawCylinderWire({ex.x, gy, ex.z}, ex.radius, 80.0f, Color{255, 60, 60, 200});
            DrawCircle3D({ex.x, gy + 1.0f, ex.z}, ex.radius, {1, 0, 0}, 90.0f, Color{255, 80, 80, 120});
        }
    }

    if (g_showWaterGateViz && engine::math::IsHydrologyReady()) {
        const auto& cfg = engine::math::GetWorldConfig();
        const float outer = cfg.waterBodyCoreR + cfg.waterBodyShoreW;
        for (const auto& lake : engine::math::GetLakes()) {
            DrawCircle3D({lake.x, lake.surfaceY + 0.5f, lake.z}, cfg.waterBodyCoreR,
                         {1, 0, 0}, 90.0f, Color{40, 140, 255, 180});
            DrawCircle3D({lake.x, lake.surfaceY + 0.5f, lake.z}, outer,
                         {1, 0, 0}, 90.0f, Color{80, 200, 255, 120});
            if (g_selectedLake >= 0 &&
                (size_t)g_selectedLake < engine::math::GetLakes().size() &&
                &lake == &engine::math::GetLakes()[(size_t)g_selectedLake]) {
                DrawCircle3D({lake.x, lake.surfaceY + 1.0f, lake.z}, lake.boundR,
                             {1, 0, 0}, 90.0f, YELLOW);
            }
        }
    }

    // Probe marker
    if (g_probeEnabled) {
        Vector3 xz = probeXZ(reg);
        float h = engine::math::WorldHeight(xz.x, xz.z);
        DrawSphere({xz.x, h + 0.5f, xz.z}, 0.6f, Color{255, 255, 0, 200});
    }
}

}  // namespace game::systems
