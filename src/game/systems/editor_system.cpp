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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
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

// Last AI dump export status (shown under the button).
std::string g_dumpStatus;
float g_dumpStatusUntil = 0.0f;
bool g_dumpStatusOk = false;

engine::ecs::Entity playerEntity(engine::ecs::Registry& reg);
Vector3 probeXZ(engine::ecs::Registry& reg);
const char* regionName(engine::math::WorldRegion r);

std::string findRepoRoot() {
    namespace fs = std::filesystem;
    auto tryFrom = [&](fs::path start) -> std::string {
        std::error_code ec;
        fs::path cur = fs::absolute(start, ec);
        if (ec) return {};
        for (int i = 0; i < 12; ++i) {
            if (fs::exists(cur / "CMakeLists.txt", ec)) return cur.string();
            fs::path parent = cur.parent_path();
            if (parent == cur) break;
            cur = parent;
        }
        return {};
    };

    // Prefer walking up from the exe dir (Release -> build_msvc -> repo).
    const char* appDir = GetApplicationDirectory();
    if (appDir && appDir[0]) {
        std::string root = tryFrom(fs::path(appDir));
        if (!root.empty()) return root;
    }
    // Fall back to cwd (useful when launched from the repo).
    std::string root = tryFrom(fs::current_path());
    if (!root.empty()) return root;
    return {};
}

std::string timestampNow() {
    using clock = std::chrono::system_clock;
    auto now = clock::now();
    std::time_t t = clock::to_time_t(now);
    std::tm tmLocal{};
#if defined(_WIN32)
    localtime_s(&tmLocal, &t);
#else
    localtime_r(&t, &tmLocal);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmLocal);
    return buf;
}

bool writeTextFile(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(contents.data(), (std::streamsize)contents.size());
    return (bool)out;
}

std::string buildEditorDump(engine::ecs::Registry& reg) {
    std::string s;
    s.reserve(4096);
    auto append = [&](const char* fmt, auto... args) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), fmt, args...);
        s += buf;
    };

    append("=== Editor dump for AI ===\n");
    append("timestamp: %s\n\n", timestampNow().c_str());

    // --- Camera ---
    append("--- Camera ---\n");
    auto pe = playerEntity(reg);
    if (reg.transforms.Has(pe)) {
        const auto& t = reg.transforms.Get(pe);
        append("position: %.3f, %.3f, %.3f\n", t.position.x, t.position.y, t.position.z);
    } else {
        append("position: (none)\n");
    }
    if (reg.cameras.Has(pe)) {
        const auto& cam = reg.cameras.Get(pe);
        append("yaw_rad: %.4f  pitch_rad: %.4f\n", cam.yaw, cam.pitch);
        append("yaw_deg: %.2f  pitch_deg: %.2f\n", cam.yaw * RAD2DEG, cam.pitch * RAD2DEG);
        append("cam_pos: %.3f, %.3f, %.3f\n",
               cam.camera.position.x, cam.camera.position.y, cam.camera.position.z);
        append("cam_target: %.3f, %.3f, %.3f\n",
               cam.camera.target.x, cam.camera.target.y, cam.camera.target.z);
    } else {
        append("yaw/pitch: (no camera)\n");
    }
    if (reg.playerInputs.Has(pe)) {
        const auto& in = reg.playerInputs.Get(pe);
        append("flight: %s  noclip: %s  move_speed: %.1f\n",
               in.isFlying ? "on" : "off",
               in.noClip ? "on" : "off",
               g_playerMoveSpeed);
    }
    s += "\n";

    // --- Probe ---
    append("--- Probe (under crosshair) ---\n");
    {
        Vector3 xz = probeXZ(reg);
        auto probe = engine::math::SampleTerrainProbe(xz.x, xz.z);
        append("xz: %.2f, %.2f\n", probe.x, probe.z);
        append("height: %.3f\n", probe.height);
        append("primary_biome: %s\n", regionName(probe.weights.primary));
        append("weights P/H/M/W/Wa: %.3f %.3f %.3f %.3f %.3f\n",
               probe.weights.plains, probe.weights.hills, probe.weights.mountains,
               probe.weights.wetlands, probe.weights.water);
        append("slope: %.4f\n", probe.slope);
        append("water_gate: %.4f\n", probe.waterGate);
        append("water_level: %.3f\n", probe.waterLevel);
    }
    s += "\n";

    // --- Grass ---
    append("--- Grass ---\n");
    {
        using namespace engine::terrain::chunks;
        const auto& gs = GetGrassDrawStats();
        append("enabled: %s\n", GetGrassEnabled() ? "yes" : "no");
        append("master_density: %.3f\n", GetGrassDensity());
        append("max_slope: %.3f\n", GetGrassMaxSlope());
        append("cluster_min/max: %d / %d\n", GetGrassClusterMin(), GetGrassClusterMax());
        append("cluster_radius_m: %.3f\n", GetGrassClusterRadius());
        append("seed_spacing_m: %.3f\n", GetGrassSeedSpacing());
        append("meadow_strength: %.3f\n", GetGrassMeadowStrength());
        append("meadow_scale: %.4f\n", GetGrassMeadowScale());
        append("scale_min/max: %.3f / %.3f\n", GetGrassScaleMin(), GetGrassScaleMax());
        append("ground_sink_cm: %.2f\n", GetGrassSinkCm());
        append("lod_near/mid/far_m: %.1f / %.1f / %.1f\n",
               GetGrassNearDistance(), GetGrassMidDistance(), GetGrassFarDistance());
        append("density_mul_near/mid/far: %.3f / %.3f / %.3f\n",
               GetGrassNearDensity(), GetGrassMidDensity(), GetGrassFarDensity());
        append("baked_N/M/F: %zu / %zu / %zu\n", gs.bakedNear, gs.bakedMid, gs.bakedFar);
        append("drawn_N/M/F: %zu / %zu / %zu\n", gs.drawNear, gs.drawMid, gs.drawFar);
        append("approx_tris: %zu\n", gs.approxTris);
        append("total_baked_instances: %zu\n", GrassInstanceCount());
    }
    s += "\n";

    // --- Noise / terrain knobs ---
    append("--- Noise / terrain knobs ---\n");
    {
        const auto& cfg = engine::math::GetWorldConfig();
        append("seed: 0x%llX\n", (unsigned long long)cfg.seed);
        append("plains_frequency: %.6f\n", cfg.plainsFrequency);
        append("plains_gain: %.4f\n", cfg.plainsGain);
        append("mountain_amplitude: %.2f\n", cfg.mountainAmplitude);
        append("mountain_approach: %.2f\n", cfg.mountainApproach);
        append("land_shelf: %.2f\n", cfg.landShelf);
        append("water_body_core_r: %.2f\n", cfg.waterBodyCoreR);
        append("water_body_shore_w: %.2f\n", cfg.waterBodyShoreW);
        append("base_amplitude: %.2f\n", cfg.baseAmplitude);
        append("detail_amplitude: %.2f\n", cfg.detailAmplitude);
        append("LOAD_RADIUS: %d  CHUNK_SIZE: %.0f\n",
               engine::math::WorldConfig::LOAD_RADIUS,
               engine::math::WorldConfig::CHUNK_SIZE);
    }
    s += "\n";

    // --- Sky / haze ---
    append("--- Sky / haze ---\n");
    append("exposure: %.4f\n", engine::render::sky::GetExposure());
    append("haze_start: %.1f\n", engine::terrain::chunks::GetHazeStart());
    append("haze_end: %.1f\n", engine::terrain::chunks::GetHazeEnd());
    append("haze_strength: %.4f\n", engine::terrain::chunks::GetHazeStrength());
    append("haze_tint: %.4f\n", engine::render::sky::GetHazeTintStrength());
    s += "\n";

    // --- Sun ---
    append("--- Sun ---\n");
    {
        Vector3 sun = engine::terrain::chunks::GetSunDirection();
        float yaw = std::atan2(sun.z, sun.x);
        float pitch = std::asin(std::clamp(sun.y, -1.0f, 1.0f));
        append("direction: %.4f, %.4f, %.4f\n", sun.x, sun.y, sun.z);
        append("yaw_deg: %.2f  pitch_deg: %.2f\n", yaw * RAD2DEG, pitch * RAD2DEG);
        append("intensity: %.3f\n", engine::terrain::chunks::GetSunIntensity());
    }
    s += "\n";

    // --- Chunks ---
    append("--- Chunks ---\n");
    append("loaded: %zu\n", engine::terrain::chunks::LoadedChunkCount());
    append("pending_upload: %zu\n", engine::terrain::chunks::PendingUploadCount());
    append("show_chunk_bounds: %s\n",
           engine::terrain::chunks::GetShowChunkBounds() ? "yes" : "no");
    s += "\n";

    // --- Toggles ---
    append("--- Active toggles ---\n");
    {
        int mode = engine::terrain::chunks::GetTerrainDebugMode();
        const char* modes[] = {"Off", "Biome", "Slope", "Height bands", "WaterGate"};
        const char* modeName = (mode >= 0 && mode < 5) ? modes[mode] : "?";
        append("terrain_debug_mode: %d (%s)\n", mode, modeName);
        append("draw_water_mesh: %s\n", game::world::GetDrawWaterEnabled() ? "yes" : "no");
        append("viz_water_gate_discs: %s\n", g_showWaterGateViz ? "yes" : "no");
        append("show_load_radius_ring: %s\n", g_showLoadRadius ? "yes" : "no");
        append("visualize_exclusions: %s\n", g_showExclusions ? "yes" : "no");
        append("live_probe: %s\n", g_probeEnabled ? "yes" : "no");
        append("place_mode: %d\n", g_placeMode);
        append("enemies/spawners/proxies: %d / %d / %d\n",
               (int)reg.enemyAIs.data.size(),
               (int)reg.spawners.data.size(),
               (int)reg.landmarkProxies.data.size());
        append("lakes: %d  selected: %d\n",
               (int)engine::math::GetLakes().size(), g_selectedLake);
    }
    s += "\n=== end dump ===\n";
    return s;
}

void exportEditorDump(engine::ecs::Registry& reg) {
    const std::string contents = buildEditorDump(reg);
    std::vector<std::string> written;
    std::vector<std::string> failed;

    // Always try next to the exe (e.g. build_msvc/Release/editor_dump.txt).
    std::string exePath = std::string(GetApplicationDirectory()) + "editor_dump.txt";
    if (writeTextFile(exePath, contents)) written.push_back(exePath);
    else failed.push_back(exePath);

    // Also write at repo root when CMakeLists.txt is found upward.
    std::string root = findRepoRoot();
    if (!root.empty()) {
        namespace fs = std::filesystem;
        std::string repoPath = (fs::path(root) / "editor_dump.txt").string();
        // Skip duplicate if exe already lives at repo root.
        if (repoPath != exePath) {
            if (writeTextFile(repoPath, contents)) written.push_back(repoPath);
            else failed.push_back(repoPath);
        }
    } else {
        failed.push_back("(repo root not found - no CMakeLists.txt upward)");
    }

    g_dumpStatusOk = !written.empty();
    g_dumpStatus.clear();
    if (g_dumpStatusOk) {
        g_dumpStatus = "Wrote dump:\n";
        for (const auto& p : written) {
            g_dumpStatus += "  ";
            g_dumpStatus += p;
            g_dumpStatus += "\n";
        }
        if (!failed.empty()) {
            g_dumpStatus += "Failed:\n";
            for (const auto& p : failed) {
                g_dumpStatus += "  ";
                g_dumpStatus += p;
                g_dumpStatus += "\n";
            }
        }
    } else {
        g_dumpStatus = "Dump FAILED - could not write any path.\n";
        for (const auto& p : failed) {
            g_dumpStatus += "  ";
            g_dumpStatus += p;
            g_dumpStatus += "\n";
        }
    }
    g_dumpStatusUntil = (float)GetTime() + 12.0f;
}

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

    // --- AI dump export ---
    if (ImGui::Button("Export dump for AI")) {
        exportEditorDump(reg);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("-> editor_dump.txt (repo + exe dir)");
    if (!g_dumpStatus.empty() && (float)GetTime() < g_dumpStatusUntil) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              g_dumpStatusOk ? ImVec4(0.45f, 0.90f, 0.50f, 1.0f)
                                             : ImVec4(1.0f, 0.40f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", g_dumpStatus.c_str());
        ImGui::PopStyleColor();
    }
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
        using engine::terrain::chunks::GetGrassEnabled;
        using engine::terrain::chunks::SetGrassEnabled;

        bool en = GetGrassEnabled();
        if (ImGui::Checkbox("Enable grass", &en)) SetGrassEnabled(en);

        float dens = engine::terrain::chunks::GetGrassDensity();
        float slope = engine::terrain::chunks::GetGrassMaxSlope();
        if (ImGui::SliderFloat("Master density", &dens, 0.0f, 2.0f, "%.2f")) {
            engine::terrain::chunks::SetGrassDensity(dens);
        }
        if (ImGui::SliderFloat("Max slope", &slope, 0.05f, 0.80f, "%.2f")) {
            engine::terrain::chunks::SetGrassMaxSlope(slope);
        }

        ImGui::SeparatorText("Clusters (rebuild)");
        int cMin = engine::terrain::chunks::GetGrassClusterMin();
        int cMax = engine::terrain::chunks::GetGrassClusterMax();
        if (ImGui::SliderInt("Cluster min", &cMin, 1, 20)) {
            engine::terrain::chunks::SetGrassClusterMin(cMin);
        }
        if (ImGui::SliderInt("Cluster max", &cMax, 1, 24)) {
            engine::terrain::chunks::SetGrassClusterMax(cMax);
        }
        float cRad = engine::terrain::chunks::GetGrassClusterRadius();
        if (ImGui::SliderFloat("Cluster radius", &cRad, 0.3f, 3.5f, "%.2f m")) {
            engine::terrain::chunks::SetGrassClusterRadius(cRad);
        }
        float seedSp = engine::terrain::chunks::GetGrassSeedSpacing();
        if (ImGui::SliderFloat("Seed spacing", &seedSp, 1.5f, 10.0f, "%.2f m")) {
            engine::terrain::chunks::SetGrassSeedSpacing(seedSp);
        }
        ImGui::TextDisabled("World hex lattice; seeds thinned uniformly under budget (no Z-row cutoff).");

        ImGui::SeparatorText("Meadow / scale (rebuild)");
        float mStr = engine::terrain::chunks::GetGrassMeadowStrength();
        float mScl = engine::terrain::chunks::GetGrassMeadowScale();
        if (ImGui::SliderFloat("Meadow strength", &mStr, 0.0f, 1.0f, "%.2f")) {
            engine::terrain::chunks::SetGrassMeadowStrength(mStr);
        }
        ImGui::TextDisabled("Default 0 = full plains cover. Raise only for intentional clearings.");
        if (ImGui::SliderFloat("Meadow scale", &mScl, 0.01f, 0.08f, "%.3f")) {
            engine::terrain::chunks::SetGrassMeadowScale(mScl);
        }
        float sMin = engine::terrain::chunks::GetGrassScaleMin();
        float sMax = engine::terrain::chunks::GetGrassScaleMax();
        if (ImGui::SliderFloat("Scale min", &sMin, 0.4f, 2.0f, "%.2f")) {
            engine::terrain::chunks::SetGrassScaleMin(sMin);
        }
        if (ImGui::SliderFloat("Scale max", &sMax, 0.4f, 2.0f, "%.2f")) {
            engine::terrain::chunks::SetGrassScaleMax(sMax);
        }
        float sinkCm = engine::terrain::chunks::GetGrassSinkCm();
        if (ImGui::SliderFloat("Ground sink", &sinkCm, 0.0f, 12.0f, "%.1f cm")) {
            engine::terrain::chunks::SetGrassSinkCm(sinkCm);
        }

        ImGui::SeparatorText("Distance LODs (live)");
        float nearD = engine::terrain::chunks::GetGrassNearDistance();
        float midD = engine::terrain::chunks::GetGrassMidDistance();
        float farD = engine::terrain::chunks::GetGrassFarDistance();
        if (ImGui::SliderFloat("Near (full mesh)", &nearD, 6.0f, 60.0f, "%.0f m")) {
            engine::terrain::chunks::SetGrassNearDistance(nearD);
        }
        if (ImGui::SliderFloat("Mid (impostor)", &midD, 20.0f, 120.0f, "%.0f m")) {
            engine::terrain::chunks::SetGrassMidDistance(midD);
        }
        if (ImGui::SliderFloat("Far (sparse)", &farD, 40.0f, 300.0f, "%.0f m")) {
            engine::terrain::chunks::SetGrassFarDistance(farD);
        }

        float nMul = engine::terrain::chunks::GetGrassNearDensity();
        float mMul = engine::terrain::chunks::GetGrassMidDensity();
        float fMul = engine::terrain::chunks::GetGrassFarDensity();
        if (ImGui::SliderFloat("Near density mul", &nMul, 0.2f, 3.0f, "%.2f")) {
            engine::terrain::chunks::SetGrassNearDensity(nMul);
        }
        if (ImGui::SliderFloat("Mid density mul", &mMul, 0.1f, 2.0f, "%.2f")) {
            engine::terrain::chunks::SetGrassMidDensity(mMul);
        }
        if (ImGui::SliderFloat("Far density mul", &fMul, 0.05f, 1.0f, "%.2f")) {
            engine::terrain::chunks::SetGrassFarDensity(fMul);
        }

        ImGui::SeparatorText("Live stats");
        const auto& gs = engine::terrain::chunks::GetGrassDrawStats();
        ImGui::Text("Baked  N/M/F: %zu / %zu / %zu", gs.bakedNear, gs.bakedMid, gs.bakedFar);
        ImGui::Text("Drawn  N/M/F: %zu / %zu / %zu", gs.drawNear, gs.drawMid, gs.drawFar);
        ImGui::Text("Approx tris:  %zu", gs.approxTris);
        ImGui::TextDisabled("Total baked: %zu", engine::terrain::chunks::GrassInstanceCount());

        static int rebuildR = 4;
        ImGui::SliderInt("Rebuild radius", &rebuildR, 1, 8);
        if (ImGui::Button("Rebuild grass")) {
            auto pe = playerEntity(reg);
            Vector3 c = reg.transforms.Has(pe) ? reg.transforms.Get(pe).position : Vector3{0, 0, 0};
            engine::terrain::chunks::ReloadAround(c, rebuildR);
        }
        ImGui::TextDisabled("Rebuild after cluster/meadow/scale/sink/density edits.");
        ImGui::TextDisabled("LOD distances + enable apply live.");
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
