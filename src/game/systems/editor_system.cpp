#include "game/systems.hpp"
#include "game/factories/entity_factory.hpp"
#include "game/enemy_model.hpp"
#include "game/spells.hpp"
#include "game/world/world_gen.hpp"
#include "game/world/landmarks.hpp"
#include "game/world/panel_build.hpp"
#include "game/dungeon/dungeon.hpp"
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
#include <cstring>
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

std::string g_defectStatus;
float g_defectStatusUntil = 0.0f;
bool g_defectStatusOk = false;
char g_defectNote[128] = "";

struct DefectMarker {
    Vector3 pos{};
    float until = 0.0f;
};
std::vector<DefectMarker> g_defectMarkers;

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

bool appendTextFile(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return false;
    out.write(contents.data(), (std::streamsize)contents.size());
    return (bool)out;
}

const char* themeId(game::dungeon::Theme theme) {
    return theme == game::dungeon::Theme::Cave ? "Cave" : "Masonry";
}

const char* sizeTierName(game::dungeon::SizeTier tier) {
    using ST = game::dungeon::SizeTier;
    switch (tier) {
        case ST::Small: return "Small";
        case ST::Large: return "Large";
        default:        return "Medium";
    }
}

std::string jsonEscape(const char* s) {
    std::string out;
    if (!s) return out;
    out.reserve(std::strlen(s) + 8);
    for (const char* p = s; *p; ++p) {
        const char c = *p;
        if (c == '\\' || c == '"') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if ((unsigned char)c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)(unsigned char)c);
            out += buf;
        } else {
            out.push_back(c);
        }
    }
    return out;
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
        if (game::dungeon::IsActive()) {
            auto dp = game::dungeon::ProbeDefect(xz.x, xz.z);
            append("dungeon_ground_y: %.3f\n", dp.groundY);
            append("dungeon_room_id: %d\n", dp.roomId);
            append("dungeon_link_index: %d\n", dp.linkIndex);
            append("dungeon_local_xz: %.2f, %.2f\n", dp.localX, dp.localZ);
            append("dungeon_mask_cell: %d, %d\n", dp.maskIx, dp.maskIz);
            append("dungeon_walkable: %s\n", dp.walkable ? "yes" : "no");
            append("dungeon_floor_off: %.3f  ceil_off: %.3f\n", dp.floorOff, dp.ceilOff);
        }
    }
    s += "\n";

    // --- Dungeon session ---
    append("--- Dungeon session ---\n");
    if (!game::dungeon::IsActive()) {
        append("active: no\n");
    } else {
        const auto& layout = game::dungeon::GetLayout();
        const auto& profile = game::dungeon::GetGenProfile();
        append("active: yes\n");
        append("seed: %u (0x%X)\n", game::dungeon::CurrentSeed(), game::dungeon::CurrentSeed());
        append("theme: %s (%s)\n", themeId(game::dungeon::CurrentTheme()),
               game::dungeon::ThemeName(game::dungeon::CurrentTheme()));
        append("stage: %d / %d\n", game::dungeon::CurrentStage(), game::dungeon::StageCount());
        append("current_room: %d\n", game::dungeon::CurrentRoom());
        append("intermission: %s  vault_key: %s\n",
               game::dungeon::InIntermission() ? "yes" : "no",
               game::dungeon::HasVaultKey() ? "yes" : "no");
        append("rooms: %zu  links: %zu\n", layout.rooms.size(), layout.links.size());
        append("entrance_room: %d  boss: %d  extract: %d  key_room: %d\n",
               layout.entranceRoom, layout.bossRoom, layout.extractRoom, layout.keyRoom);
        append("profile cell: %.1f  maskCellSize: %.2f\n", profile.cell, profile.maskCellSize);
        append("path_rooms: %d-%d  max_branches: %d\n",
               profile.minPathRooms, profile.maxPathRooms, profile.maxBranches);
        for (const auto& room : layout.rooms) {
            append("  room %d type=%d size=%s half=%.1fx%.1f center=(%.1f,%.1f,%.1f) crit=%d cleared=%d\n",
                   room.id, (int)room.type, sizeTierName(room.size),
                   room.halfW, room.halfD,
                   room.center.x, room.center.y, room.center.z,
                   room.onCriticalPath ? 1 : 0, room.cleared ? 1 : 0);
        }
        for (size_t i = 0; i < layout.links.size(); ++i) {
            const auto& link = layout.links[i];
            append("  link %zu %d->%d type=%d style=%d alongX=%d halfLen=%.1f halfW=%.1f center=(%.1f,%.1f,%.1f) locked=%d\n",
                   i, link.fromRoom, link.toRoom, (int)link.type, (int)link.style,
                   link.alongX ? 1 : 0, link.halfLen, link.halfWidth,
                   link.center.x, link.center.y, link.center.z, link.locked ? 1 : 0);
        }
        const auto& mods = game::dungeon::ActiveModifiers();
        if (mods.empty()) {
            append("modifiers: (none)\n");
        } else {
            append("modifiers:\n");
            for (const auto* m : mods) {
                if (m) append("  %s (%s)\n", m->id.c_str(), m->name.c_str());
            }
        }
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
        append("coverage_strength: %.3f\n", GetGrassCoverageStrength());
        append("coverage_scale: %.4f\n", GetGrassCoverageScale());
        append("coverage_threshold: %.3f\n", GetGrassCoverageThreshold());
        append("size_noise_scale: %.4f\n", GetGrassSizeNoiseScale());
        append("scale_min/max: %.3f / %.3f\n", GetGrassScaleMin(), GetGrassScaleMax());
        append("ground_sink_cm: %.2f\n", GetGrassSinkCm());
        append("draw_distance_m: %.1f\n", GetGrassDrawDistance());
        append("baked: %zu\n", gs.baked);
        append("drawn: %zu\n", gs.drawn);
        append("approx_tris: %zu\n", gs.approxTris);
        append("total_baked_instances: %zu\n", GrassInstanceCount());
    }
    s += "\n";

    // --- Trees ---
    append("--- Trees ---\n");
    {
        using namespace engine::terrain::chunks;
        const auto& ts = GetTreeDrawStats();
        append("enabled: %s\n", GetTreesEnabled() ? "yes" : "no");
        append("master_density: %.3f\n", GetTreeDensity());
        append("max_slope: %.3f\n", GetTreeMaxSlope());
        append("tree_seed_spacing_m: %.3f\n", GetTreeSeedSpacing());
        append("bush_seed_spacing_m: %.3f\n", GetBushSeedSpacing());
        append("scale_min/max: %.3f / %.3f\n", GetTreeScaleMin(), GetTreeScaleMax());
        append("ground_sink_cm: %.2f\n", GetTreeSinkCm());
        append("draw_distance_m: %.1f\n", GetTreeDrawDistance());
        append("baked_trees: %zu\n", ts.bakedTrees);
        append("baked_bushes: %zu\n", ts.bakedBushes);
        append("drawn: %zu\n", ts.drawn);
        append("approx_tris: %zu\n", ts.approxTris);
        append("total_baked_trees: %zu\n", TreeInstanceCount());
        append("total_baked_bushes: %zu\n", BushInstanceCount());
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

void markDefectAtProbe(engine::ecs::Registry& reg) {
    Vector3 xz = probeXZ(reg);
    game::dungeon::DefectProbe dp{};
    if (game::dungeon::IsActive()) {
        dp = game::dungeon::ProbeDefect(xz.x, xz.z);
    } else {
        dp.world = xz;
        dp.groundY = engine::math::WorldHeight(xz.x, xz.z);
        dp.world.y = dp.groundY;
    }

    const std::string roomTypeEsc = jsonEscape(dp.roomType ? dp.roomType : "");
    const std::string noteEsc = jsonEscape(g_defectNote);
    const char* theme = game::dungeon::IsActive() ? themeId(dp.theme) : "overworld";

    char line[1024];
    std::snprintf(
        line, sizeof(line),
        "{\"seed\":%u,\"theme\":\"%s\",\"stage\":%d,\"roomId\":%d,\"linkIndex\":%d,"
        "\"roomType\":\"%s\",\"world\":[%.3f,%.3f,%.3f],\"local\":[%.3f,%.3f],"
        "\"maskCell\":[%d,%d],\"maskSize\":[%d,%d],\"walkable\":%s,"
        "\"floorOff\":%.3f,\"ceilOff\":%.3f,\"groundY\":%.3f,\"note\":\"%s\",\"timestamp\":\"%s\"}\n",
        dp.seed,
        theme,
        dp.stage,
        dp.roomId,
        dp.linkIndex,
        roomTypeEsc.c_str(),
        dp.world.x, dp.world.y, dp.world.z,
        dp.localX, dp.localZ,
        dp.maskIx, dp.maskIz,
        dp.maskNx, dp.maskNz,
        dp.walkable ? "true" : "false",
        dp.floorOff, dp.ceilOff, dp.groundY,
        noteEsc.c_str(),
        timestampNow().c_str());

    std::vector<std::string> written;
    std::vector<std::string> failed;
    std::string exePath = std::string(GetApplicationDirectory()) + "editor_defects.jsonl";
    if (appendTextFile(exePath, line)) written.push_back(exePath);
    else failed.push_back(exePath);

    std::string root = findRepoRoot();
    if (!root.empty()) {
        namespace fs = std::filesystem;
        std::string repoPath = (fs::path(root) / "editor_defects.jsonl").string();
        if (repoPath != exePath) {
            if (appendTextFile(repoPath, line)) written.push_back(repoPath);
            else failed.push_back(repoPath);
        }
    } else {
        failed.push_back("(repo root not found)");
    }

    g_defectMarkers.push_back({dp.world, (float)GetTime() + 20.0f});

    g_defectStatusOk = !written.empty();
    g_defectStatus.clear();
    if (g_defectStatusOk) {
        g_defectStatus = "Pinned defect:\n";
        for (const auto& p : written) {
            g_defectStatus += "  ";
            g_defectStatus += p;
            g_defectStatus += "\n";
        }
    } else {
        g_defectStatus = "Defect pin FAILED\n";
        for (const auto& p : failed) {
            g_defectStatus += "  ";
            g_defectStatus += p;
            g_defectStatus += "\n";
        }
    }
    g_defectStatusUntil = (float)GetTime() + 12.0f;
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

    // Panel build stays active after enabling in the editor (sticky until unchecked).
    auto runPanelBuildInput = [&]() {
        if (!game::world::panel_build::IsEnabled()) return;
        auto pe = playerEntity(reg);
        if (!reg.cameras.Has(pe)) return;
        game::world::panel_build::Update(reg.cameras.Get(pe).camera);
        if (!engine::input::IsCursorLocked() || ImGui::GetIO().WantCaptureMouse) return;
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) game::world::panel_build::TryPlace();
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) game::world::panel_build::TryRemove();
    };

    if (!g_showEditor) {
        runPanelBuildInput();
        return;
    }

    if (IsKeyPressed(KEY_F7)) {
        markDefectAtProbe(reg);
    }

    if (IsKeyPressed(KEY_LEFT_ALT)) {
        if (engine::input::IsCursorLocked()) engine::input::UnlockCursor();
        else engine::input::LockCursor();
    }

    if (game::world::panel_build::IsEnabled()) {
        runPanelBuildInput();
    } else if (engine::input::IsCursorLocked() && !ImGui::GetIO().WantCaptureMouse) {
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && g_placeMode != 0) {
            Vector3 hit = cameraLookHit(reg);
            hit.y = engine::math::WorldHeight(hit.x, hit.z) +
                    (game::enemy_model::IsReady() ? game::enemy_model::GetTargetHeight() : 2.55f) * 0.5f;
            if (g_placeMode == 1) {
                factories::EntityFactory::CreateEnemy(reg, hit, g_editorNetId++);
            } else if (g_placeMode == 2) {
                factories::EntityFactory::CreateSpawner(reg, hit);
            } else if (g_placeMode == 3) {
                factories::EntityFactory::CreateLandmarkProxy(reg, hit, g_landmarkPlaceType);
            } else if (g_placeMode == 4) {
                hit.y = engine::math::WorldHeight(hit.x, hit.z) + 2.0f;
                factories::EntityFactory::CreateEliteEnemy(reg, hit, g_editorNetId++);
            } else if (g_placeMode == 5) {
                constexpr float kGiantHalfH = 25.0f * 0.3048f * 0.5f;
                hit.y = engine::math::WorldHeight(hit.x, hit.z) + kGiantHalfH;
                factories::EntityFactory::CreateGiantEnemy(reg, hit, g_editorNetId++);
            } else if (g_placeMode == 6) {
                constexpr float kColossalHalfH = 100.0f * 0.3048f * 0.5f;
                hit.y = engine::math::WorldHeight(hit.x, hit.z) + kColossalHalfH;
                factories::EntityFactory::CreateColossalEnemy(reg, hit, g_editorNetId++);
            } else if (g_placeMode == 7) {
                constexpr float kTitanHalfH = 250.0f * 0.3048f * 0.5f;
                hit.y = engine::math::WorldHeight(hit.x, hit.z) + kTitanHalfH;
                factories::EntityFactory::CreateTitanEnemy(reg, hit, g_editorNetId++);
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

    ImGui::InputText("Defect note", g_defectNote, sizeof(g_defectNote));
    if (ImGui::Button("Mark defect at probe")) {
        markDefectAtProbe(reg);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("F7  -> editor_defects.jsonl (F8 exits dungeon)");
    if (game::dungeon::IsActive()) {
        ImGui::Text("Dungeon seed %u  theme %s  room %d",
                    game::dungeon::CurrentSeed(),
                    themeId(game::dungeon::CurrentTheme()),
                    game::dungeon::CurrentRoom());
    } else {
        ImGui::TextDisabled("Enter a dungeon to pin gen defects with seed/room context.");
    }
    if (!g_defectStatus.empty() && (float)GetTime() < g_defectStatusUntil) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              g_defectStatusOk ? ImVec4(0.45f, 0.90f, 0.50f, 1.0f)
                                               : ImVec4(1.0f, 0.40f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", g_defectStatus.c_str());
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
            if (game::dungeon::IsActive()) {
                auto dp = game::dungeon::ProbeDefect(xz.x, xz.z);
                ImGui::Text("Dungeon Y: %.2f  room %d  link %d",
                            dp.groundY, dp.roomId, dp.linkIndex);
                ImGui::Text("Local: %.1f, %.1f  mask [%d,%d] walk=%s",
                            dp.localX, dp.localZ, dp.maskIx, dp.maskIz,
                            dp.walkable ? "yes" : "no");
            }
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
        const char* modes[] = {"Off", "Enemy", "Spawner", "Landmark proxy", "Elite enemy", "Giant (25 ft)", "Colossal (100 ft)", "Titan (250 ft)"};
        ImGui::Combo("Place mode", &g_placeMode, modes, 8);
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
                float halfH = (game::enemy_model::IsReady() ? game::enemy_model::GetTargetHeight() : 2.55f) * 0.5f;
                p.y = engine::math::WorldHeight(p.x, p.z) + halfH;
                if (g_placeMode == 1) factories::EntityFactory::CreateEnemy(reg, p, g_editorNetId++);
                else if (g_placeMode == 2) factories::EntityFactory::CreateSpawner(reg, p);
                else if (g_placeMode == 3)
                    factories::EntityFactory::CreateLandmarkProxy(reg, p, g_landmarkPlaceType);
                else if (g_placeMode == 4) {
                    p.y = engine::math::WorldHeight(p.x, p.z) + 2.0f;
                    factories::EntityFactory::CreateEliteEnemy(reg, p, g_editorNetId++);
                } else if (g_placeMode == 5) {
                    constexpr float kGiantHalfH = 25.0f * 0.3048f * 0.5f;
                    p.y = engine::math::WorldHeight(p.x, p.z) + kGiantHalfH;
                    factories::EntityFactory::CreateGiantEnemy(reg, p, g_editorNetId++);
                } else if (g_placeMode == 6) {
                    constexpr float kColossalHalfH = 100.0f * 0.3048f * 0.5f;
                    p.y = engine::math::WorldHeight(p.x, p.z) + kColossalHalfH;
                    factories::EntityFactory::CreateColossalEnemy(reg, p, g_editorNetId++);
                } else if (g_placeMode == 7) {
                    constexpr float kTitanHalfH = 250.0f * 0.3048f * 0.5f;
                    p.y = engine::math::WorldHeight(p.x, p.z) + kTitanHalfH;
                    factories::EntityFactory::CreateTitanEnemy(reg, p, g_editorNetId++);
                }
            }
        }
    }

    if (ImGui::CollapsingHeader("9b. Panel build (snap)")) {
        bool en = game::world::panel_build::IsEnabled();
        if (ImGui::Checkbox("Panel build mode", &en)) {
            game::world::panel_build::SetEnabled(en);
            if (en) g_placeMode = 0; // avoid fighting entity place
        }
        ImGui::TextWrapped("When on: ALT lock mouse, LMB place, RMB remove, Q/E rotate 90. Snaps to 4m kit grid.");

        int pieceIdx = (int)game::world::panel_build::GetSelectedPiece();
        int styleIdx = (int)game::world::panel_build::GetSelectedStyle();
        const char* pieceNames[] = {
            "Floor", "Wall", "WallDoor", "WallWindow", "Pillar",
            "RoofSlope", "Gable", "GableRamp", "WallRise", "RoofPyramid", "Stairs"};
        const char* styleNames[] = {"Stone", "Wood", "RoofDark"};
        if (ImGui::Combo("Piece", &pieceIdx, pieceNames, IM_ARRAYSIZE(pieceNames))) {
            game::world::panel_build::SetSelectedPiece(
                (game::world::building_panels::Piece)pieceIdx);
        }
        if (ImGui::Combo("Style", &styleIdx, styleNames, IM_ARRAYSIZE(styleNames))) {
            game::world::panel_build::SetSelectedStyle(
                (game::world::building_panels::Style)styleIdx);
        }
        ImGui::Text("Yaw: %.0f deg | Placed: %d",
                    game::world::panel_build::GetYawDeg(),
                    game::world::panel_build::PlacedCount());

        if (ImGui::Button("Save placements")) {
            game::world::panel_build::Save();
        }
        ImGui::SameLine();
        if (ImGui::Button("Load placements")) {
            game::world::panel_build::Load();
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear all")) {
            game::world::panel_build::ClearAll();
        }
        ImGui::TextDisabled("File: assets/data/buildings_placed.json");
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
        if (ImGui::SliderFloat("Seed spacing", &seedSp, 0.70f, 10.0f, "%.2f m")) {
            engine::terrain::chunks::SetGrassSeedSpacing(seedSp);
        }
        ImGui::TextDisabled("World hex lattice; seeds thinned uniformly under budget (no Z-row cutoff).");

        ImGui::SeparatorText("Meadow / coverage / scale (rebuild)");
        float mStr = engine::terrain::chunks::GetGrassMeadowStrength();
        float mScl = engine::terrain::chunks::GetGrassMeadowScale();
        if (ImGui::SliderFloat("Meadow strength", &mStr, 0.0f, 1.0f, "%.2f")) {
            engine::terrain::chunks::SetGrassMeadowStrength(mStr);
        }
        ImGui::TextDisabled("Default 0 = full plains cover. Raise only for intentional clearings.");
        if (ImGui::SliderFloat("Meadow scale", &mScl, 0.01f, 0.08f, "%.3f")) {
            engine::terrain::chunks::SetGrassMeadowScale(mScl);
        }
        float covStr = engine::terrain::chunks::GetGrassCoverageStrength();
        float covScl = engine::terrain::chunks::GetGrassCoverageScale();
        float covThr = engine::terrain::chunks::GetGrassCoverageThreshold();
        if (ImGui::SliderFloat("Coverage strength", &covStr, 0.0f, 1.0f, "%.2f")) {
            engine::terrain::chunks::SetGrassCoverageStrength(covStr);
        }
        if (ImGui::SliderFloat("Coverage scale", &covScl, 0.01f, 0.10f, "%.3f")) {
            engine::terrain::chunks::SetGrassCoverageScale(covScl);
        }
        if (ImGui::SliderFloat("Coverage threshold", &covThr, 0.0f, 0.60f, "%.2f")) {
            engine::terrain::chunks::SetGrassCoverageThreshold(covThr);
        }
        ImGui::TextDisabled("Defaults keep ~90%%+ plains seeds; coverage modulates clumps/radius.");
        float sizeScl = engine::terrain::chunks::GetGrassSizeNoiseScale();
        if (ImGui::SliderFloat("Size noise scale", &sizeScl, 0.01f, 0.12f, "%.3f")) {
            engine::terrain::chunks::SetGrassSizeNoiseScale(sizeScl);
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

        ImGui::SeparatorText("Draw distance (live)");
        float drawD = engine::terrain::chunks::GetGrassDrawDistance();
        if (ImGui::SliderFloat("Draw distance", &drawD, 40.0f, 500.0f, "%.0f m")) {
            engine::terrain::chunks::SetGrassDrawDistance(drawD);
        }
        ImGui::TextDisabled("Default 50 m. Chunk lists are pre-baked (no per-frame gather).");
        ImGui::TextDisabled("Quaternius meshes only; soft fade ~35 m near max. Baked on terrain LOD0–1.");

        ImGui::SeparatorText("Live stats");
        const auto& gs = engine::terrain::chunks::GetGrassDrawStats();
        ImGui::Text("Baked: %zu", gs.baked);
        ImGui::Text("Drawn: %zu", gs.drawn);
        ImGui::Text("Approx tris: %zu", gs.approxTris);
        ImGui::TextDisabled("Total baked: %zu", engine::terrain::chunks::GrassInstanceCount());

        static int rebuildR = 4;
        ImGui::SliderInt("Rebuild radius", &rebuildR, 1, 8);
        if (ImGui::Button("Rebuild grass")) {
            auto pe = playerEntity(reg);
            Vector3 c = reg.transforms.Has(pe) ? reg.transforms.Get(pe).position : Vector3{0, 0, 0};
            engine::terrain::chunks::ReloadAround(c, rebuildR);
        }
        ImGui::TextDisabled("Rebuild after cluster/coverage/scale/sink/density edits.");
        ImGui::TextDisabled("Draw distance + enable apply live.");
    }

    if (ImGui::CollapsingHeader("12. Trees")) {
        bool en = engine::terrain::chunks::GetTreesEnabled();
        if (ImGui::Checkbox("Enable trees", &en)) {
            engine::terrain::chunks::SetTreesEnabled(en);
        }

        float dens = engine::terrain::chunks::GetTreeDensity();
        float slope = engine::terrain::chunks::GetTreeMaxSlope();
        if (ImGui::SliderFloat("Master density", &dens, 0.0f, 2.0f, "%.2f")) {
            engine::terrain::chunks::SetTreeDensity(dens);
        }
        if (ImGui::SliderFloat("Max slope", &slope, 0.05f, 0.80f, "%.2f")) {
            engine::terrain::chunks::SetTreeMaxSlope(slope);
        }

        ImGui::SeparatorText("Placement (rebuild)");
        float treeSp = engine::terrain::chunks::GetTreeSeedSpacing();
        float bushSp = engine::terrain::chunks::GetBushSeedSpacing();
        if (ImGui::SliderFloat("Tree seed spacing", &treeSp, 8.0f, 40.0f, "%.1f m")) {
            engine::terrain::chunks::SetTreeSeedSpacing(treeSp);
        }
        if (ImGui::SliderFloat("Bush seed spacing", &bushSp, 4.0f, 20.0f, "%.1f m")) {
            engine::terrain::chunks::SetBushSeedSpacing(bushSp);
        }
        ImGui::TextDisabled("Plains sparse via spawn chance; hills/foothills denser. Pines prefer hills/foothills.");

        float sMin = engine::terrain::chunks::GetTreeScaleMin();
        float sMax = engine::terrain::chunks::GetTreeScaleMax();
        if (ImGui::SliderFloat("Scale min", &sMin, 0.4f, 2.0f, "%.2f")) {
            engine::terrain::chunks::SetTreeScaleMin(sMin);
        }
        if (ImGui::SliderFloat("Scale max", &sMax, 0.4f, 2.0f, "%.2f")) {
            engine::terrain::chunks::SetTreeScaleMax(sMax);
        }
        float sinkCm = engine::terrain::chunks::GetTreeSinkCm();
        if (ImGui::SliderFloat("Ground sink", &sinkCm, 0.0f, 20.0f, "%.1f cm")) {
            engine::terrain::chunks::SetTreeSinkCm(sinkCm);
        }

        ImGui::SeparatorText("Draw distance (live)");
        float drawD = engine::terrain::chunks::GetTreeDrawDistance();
        if (ImGui::SliderFloat("Draw distance", &drawD, 60.0f, 500.0f, "%.0f m")) {
            engine::terrain::chunks::SetTreeDrawDistance(drawD);
        }
        ImGui::TextDisabled("Default 220 m. Soft fade near max. Baked on terrain LOD0–1.");

        ImGui::SeparatorText("Live stats");
        const auto& ts = engine::terrain::chunks::GetTreeDrawStats();
        ImGui::Text("Baked trees: %zu", ts.bakedTrees);
        ImGui::Text("Baked bushes: %zu", ts.bakedBushes);
        ImGui::Text("Drawn: %zu", ts.drawn);
        ImGui::Text("Approx tris: %zu", ts.approxTris);
        ImGui::TextDisabled("Totals: trees %zu / bushes %zu",
                            engine::terrain::chunks::TreeInstanceCount(),
                            engine::terrain::chunks::BushInstanceCount());

        static int treeRebuildR = 4;
        ImGui::SliderInt("Rebuild radius##trees", &treeRebuildR, 1, 8);
        if (ImGui::Button("Rebuild trees")) {
            auto pe = playerEntity(reg);
            Vector3 c = reg.transforms.Has(pe) ? reg.transforms.Get(pe).position : Vector3{0, 0, 0};
            engine::terrain::chunks::ReloadAround(c, treeRebuildR);
        }
        ImGui::TextDisabled("Rebuild after density/spacing/slope/scale/sink edits.");
        ImGui::TextDisabled("Draw distance + enable apply live.");
    }

    if (ImGui::CollapsingHeader("12. Spells / Combat")) {
        static bool freeCast = true;
        ImGui::Checkbox("Free cast (ignore mana & cooldown)", &freeCast);
        ImGui::TextWrapped("Tab cycles Fire/Water/Necro/Priest/Ranger. Number keys select a spell. RMB casts.");
        ImGui::Separator();

        auto pe = playerEntity(reg);
        if (reg.spellCasters.Has(pe)) {
            auto& caster = reg.spellCasters.Get(pe);
            ImGui::Text("Mana: %.0f / %.0f", caster.mana, caster.manaMax);
            ImGui::ProgressBar(caster.mana / fmaxf(caster.manaMax, 1.0f), ImVec2(-1, 0));
            if (caster.castingSpell >= 0) {
                const auto& def = game::GetSpellDef(caster.castingSpell);
                ImGui::Text("Casting: %s (%.2fs)", def.name, caster.castTimer);
            }
            if (ImGui::Button("Refill mana")) caster.mana = caster.manaMax;
            ImGui::SameLine();
            if (ImGui::Button("Reset cooldowns")) {
                for (int s = 0; s < (int)game::SpellId::Count; s++) caster.cooldowns[s] = 0.0f;
            }
            ImGui::Separator();
        } else {
            ImGui::TextDisabled("No SpellCaster on player.");
        }

        for (int s = 0; s < (int)game::SpellId::Count; s++) {
            const auto& def = game::GetSpellDef(s);
            ImGui::PushID(s);
            float cd = 0.0f;
            if (reg.spellCasters.Has(pe)) cd = reg.spellCasters.Get(pe).cooldowns[s];

            char label[96];
            if (cd > 0.0f && !freeCast) {
                snprintf(label, sizeof(label), "Cast %s  (CD %.1fs)", def.name, cd);
            } else {
                snprintf(label, sizeof(label), "Cast %s  [%.0f dmg | %.0f mana | %.1fs CD]",
                         def.name, def.damage, def.manaCost, def.cooldown);
            }
            if (ImGui::Button(label, ImVec2(-1, 0))) {
                TryCastSpell(reg, s, freeCast);
            }
            ImGui::TextDisabled("  %s | cast %.2fs | %s",
                def.delivery == game::SpellDelivery::Projectile ? "projectile" :
                def.delivery == game::SpellDelivery::InstantAoE ? "instant AoE" :
                def.delivery == game::SpellDelivery::PersistentZone ? "zone" :
                def.delivery == game::SpellDelivery::MovingWave ? "moving wave" :
                def.delivery == game::SpellDelivery::Mobility ? "mobility" :
                def.delivery == game::SpellDelivery::Passive ? "passive" :
                def.delivery == game::SpellDelivery::SelfBuff ? "self buff" :
                def.delivery == game::SpellDelivery::SummonPet ? "summon" : "hitscan",
                def.castTime, def.sfxCast);
            ImGui::PopID();
        }

        ImGui::Separator();
        ImGui::TextDisabled("Tab: cycle class | 1-0: select | RMB: cast");
        ImGui::TextDisabled("Necro: Pixie, Gargoyle, Call of the Dead, Reaper");
        ImGui::TextDisabled("Priest: Sprite, Battle Angel, Arch Angel");
        ImGui::TextDisabled("Ranger: Dash (20 ft), Teleport, Double Jump (passive)");
        if (ImGui::Button("Spawn test enemy ring")) {
            if (reg.transforms.Has(pe)) {
                Vector3 c = reg.transforms.Get(pe).position;
                for (int i = 0; i < 8; i++) {
                    float a = i * (6.28318f / 8.0f);
                    Vector3 p = {c.x + cosf(a) * 8.0f, c.y, c.z + sinf(a) * 8.0f};
                    p.y = engine::math::WorldHeight(p.x, p.z) + 1.0f;
                    factories::EntityFactory::CreateEnemy(reg, p, g_editorNetId++);
                }
            }
        }
        if (ImGui::Button("Spawn elite enemy")) {
            if (reg.transforms.Has(pe)) {
                Vector3 p = reg.transforms.Get(pe).position;
                p.x += 6.0f;
                p.y = engine::math::WorldHeight(p.x, p.z) + 2.0f;
                factories::EntityFactory::CreateEliteEnemy(reg, p, g_editorNetId++);
            }
        }
        if (ImGui::Button("Spawn giant (25 ft)")) {
            if (reg.transforms.Has(pe)) {
                Vector3 p = reg.transforms.Get(pe).position;
                p.x += 10.0f;
                constexpr float kGiantHalfH = 25.0f * 0.3048f * 0.5f;
                p.y = engine::math::WorldHeight(p.x, p.z) + kGiantHalfH;
                factories::EntityFactory::CreateGiantEnemy(reg, p, g_editorNetId++);
            }
        }
        if (ImGui::Button("Spawn colossal (100 ft)")) {
            if (reg.transforms.Has(pe)) {
                Vector3 p = reg.transforms.Get(pe).position;
                p.x += 20.0f;
                constexpr float kColossalHalfH = 100.0f * 0.3048f * 0.5f;
                p.y = engine::math::WorldHeight(p.x, p.z) + kColossalHalfH;
                factories::EntityFactory::CreateColossalEnemy(reg, p, g_editorNetId++);
            }
        }
        if (ImGui::Button("Spawn titan (250 ft)")) {
            if (reg.transforms.Has(pe)) {
                Vector3 p = reg.transforms.Get(pe).position;
                p.x += 40.0f;
                constexpr float kTitanHalfH = 250.0f * 0.3048f * 0.5f;
                p.y = engine::math::WorldHeight(p.x, p.z) + kTitanHalfH;
                factories::EntityFactory::CreateTitanEnemy(reg, p, g_editorNetId++);
            }
        }
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
        float h = game::dungeon::IsActive()
            ? game::dungeon::GroundY(xz.x, xz.z)
            : engine::math::WorldHeight(xz.x, xz.z);
        DrawSphere({xz.x, h + 0.5f, xz.z}, 0.6f, Color{255, 255, 0, 200});
    }

    const float now = (float)GetTime();
    for (size_t i = 0; i < g_defectMarkers.size();) {
        if (g_defectMarkers[i].until < now) {
            g_defectMarkers[i] = g_defectMarkers.back();
            g_defectMarkers.pop_back();
            continue;
        }
        DrawSphere(g_defectMarkers[i].pos, 0.85f, Color{255, 60, 60, 220});
        DrawSphereWires(g_defectMarkers[i].pos, 1.4f, 8, 8, Color{255, 120, 80, 200});
        ++i;
    }
}

}  // namespace game::systems
