#pragma once
#include "engine/ecs/registry.hpp"
#include "raylib.h"
#include <cstdint>
#include <string>
#include <vector>

// Open-world-discovered dungeon instances.
//
// Layouts are Diablo-style procedural room graphs: a deterministic critical path
// of typed rooms (entrance -> combat/elite/haven -> boss -> extract) plus optional
// side branches, connected by explicit transitions (corridor / locked gate).
//
// Rooms live in a dedicated far-off region of world space. While a session is
// active the overworld is not streamed or drawn, and ground height comes from
// GroundY() instead of the terrain noise.
namespace game::dungeon {

    enum class RoomType : uint8_t {
        Entrance = 0,
        Combat,
        Elite,
        SafeHaven,
        Treasure,
        Boss,
        Extract,
        Secret,   // reached through a hidden passage
        Vault,    // key-locked reward room
    };

    enum class TransitionType : uint8_t {
        Corridor = 0,
        Door,
        LockedGate,
        HiddenPassage,
    };

    // What has to happen before a sealed transition opens.
    enum class GateRequirement : uint8_t {
        None = 0,
        BossDead,
        Key,
        Search,   // hidden passage revealed by walking near it
    };

    // Floor-plan silhouette. Prefer WalkableMask extrusion; RoomShape remains as a
    // legacy fallback tag (gen now leaves Rect when masks are used).
    enum class RoomShape : uint8_t {
        Rect = 0,
        LongHall,  // elongated rectangle
        LWing,     // missing one corner quadrant
        Cross,     // plus-shaped walkable area
        Circle,    // radial arena (octagon walls)
    };

    // Footprint tier — drives mask scale and light enemy pack scaling.
    enum class SizeTier : uint8_t {
        Small = 0,   // ~35–55m span
        Medium,      // ~50–65m span
        Large,       // ~70–110m span
    };

    enum class FloorStyle : uint8_t {
        Checker = 0,
        Mosaic,    // diamond / inset pattern
        Cracked,   // irregular broken slabs
        BloodRing, // dark ring around the center
        Flooded,   // shallow water plane over stone
    };

    enum class CorridorStyle : uint8_t {
        Standard = 0,
        Ruined,    // rubble, broken wall gaps
        Choke,     // narrow torch-lit passage
        Flooded,   // shallow water trench
    };

    // Fixed for a given overworld entrance — known before you step inside.
    // Later this also selects the space grammar (masonry chambers vs carved caves).
    enum class Theme : uint8_t {
        Masonry = 0,  // brick / flagstone crypt halls
        Cave,         // natural carved rock
    };

    inline const char* ThemeName(Theme theme) {
        switch (theme) {
            case Theme::Cave: return "Natural Caves";
            default:          return "Brick Crypt";
        }
    }

    struct Room {
        int        id    = 0;
        RoomType   type  = RoomType::Combat;
        RoomShape  shape = RoomShape::Rect;
        SizeTier   size  = SizeTier::Medium;
        FloorStyle floor = FloorStyle::Checker;
        int        cellX = 0;
        int        cellZ = 0;
        Vector3    center{};          // floor-plane center
        float      halfW = 14.0f;
        float      halfD = 14.0f;
        // Legacy shape variant (unused when masks drive silhouette).
        int        shapeVariant = 0;
        bool       onCriticalPath = true;
        bool       populated = false; // encounter already spawned
        bool       cleared   = false;
        bool       visited   = false;
    };

    struct Transition {
        int             fromRoom = 0;
        int             toRoom   = 0;
        TransitionType  type     = TransitionType::Corridor;
        CorridorStyle   style    = CorridorStyle::Standard;
        Vector3         center{};     // corridor floor-plane center
        bool            alongX    = true;
        float           halfLen   = 8.0f;   // along travel axis
        float           halfWidth = 3.0f;   // across travel axis
        bool            locked    = false;
        GateRequirement requires_ = GateRequirement::None;
    };

    struct GenProfile {
        int   minPathRooms  = 6;
        int   maxPathRooms  = 9;
        int   maxBranches   = 3;
        float cell          = 120.0f; // grid spacing between room centers
        float secretChance  = 0.55f;  // chance a branch becomes a hidden room
        float vaultChance   = 0.5f;   // chance a branch becomes a key-locked vault
        float maskCellSize  = 1.5f;   // walkable mask resolution (meters)
        float largeCombatChance = 0.72f; // combat/elite roll Large vs Medium
        float smallSpanMin  = 35.0f;
        float smallSpanMax  = 55.0f;
        float mediumSpanMin = 50.0f;
        float mediumSpanMax = 65.0f;
        float largeSpanMin  = 70.0f;
        float largeSpanMax  = 100.0f;
        float bossSpanMin   = 90.0f;
        float bossSpanMax   = 110.0f;
    };

    struct Layout {
        uint32_t                seed = 0;
        std::vector<Room>       rooms;
        std::vector<Transition> links;
        int                     entranceRoom = 0;
        int                     bossRoom     = -1;
        int                     extractRoom  = -1;
        int                     keyRoom      = -1; // room holding the vault key
    };

    // Deterministic: same seed + profile always yields the same graph.
    Layout Generate(uint32_t seed, const GenProfile& profile = GenProfile{});

    // ---- Data assets (assets/data/dungeons) ----

    struct ModifierDef {
        std::string id;
        std::string name;
        int   weight            = 10;
        float monsterHealthMul  = 1.0f;
        float monsterDamageMul  = 1.0f;
        float monsterSpeedMul   = 1.0f;
        float eliteChance       = 0.0f;  // chance a combat room upgrades to an elite pack
        float extraEnemyMul     = 1.0f;
        float rewardMul         = 1.0f;
    };

    struct StageDef {
        std::string name;
        float enemyCountMul = 1.0f;
        float healthMul     = 1.0f;
        int   modifierRolls = 1;
        int   extraBranches = 0;
    };

    struct CampaignDef {
        std::string           id;
        std::string           name;
        std::vector<StageDef> stages;
    };

    // Loads generation_profile.json / modifiers.json / campaign_proto.json.
    // Safe to call once at startup; falls back to built-in defaults if absent.
    void LoadData();
    const GenProfile&               GetGenProfile();
    const CampaignDef&              GetCampaign();
    const std::vector<ModifierDef>& GetModifierPool();

    // ---- Overworld entrances (discovery) ----

    struct Entrance {
        Vector3  pos{};      // ground position in the overworld
        uint32_t seed = 0;   // dungeon seed for this entrance
        int      index = 0;
        Theme    theme = Theme::Masonry; // fixed at discovery; shown on the portal
    };

    const std::vector<Entrance>& GetEntrances();
    void DrawEntrances(const Camera3D& cam);            // inside BeginMode3D
    void DrawEntrancePrompt(Vector3 playerPos);         // 2D overlay
    int  FindNearbyEntrance(Vector3 playerPos, float radius);

    // ---- Session ----

    bool  IsActive();
    float FloorY();
    // Dungeon floor while a session is active, terrain height otherwise.
    float GroundY(float x, float z);

    void Enter(engine::ecs::Registry& reg, engine::ecs::Entity player, const Entrance& entrance);
    void Exit(engine::ecs::Registry& reg, engine::ecs::Entity player);
    void Update(engine::ecs::Registry& reg, engine::ecs::Entity player);

    // Host-authoritative co-op: apply a sync message received from the host.
    void ApplyNetworkSync(engine::ecs::Registry& reg, engine::ecs::Entity player,
                          uint8_t op, uint32_t seed, uint8_t theme, uint8_t stage);
    // Host broadcasts current session lifecycle to lobby peers (no-op if solo/client).
    void BroadcastLifecycle(uint8_t op);

    void Draw();      // inside BeginMode3D
    void DrawHUD();   // 2D overlay

    // Axis-aligned solid collision for the local player while a session is active.
    // Walls are intentionally NOT ECS renderables (that path was too heavy).
    void ResolvePlayerCollision(Vector3& position, float radius, float height);

    const Layout& GetLayout();
    int  CurrentRoom();
    int  CurrentStage();       // 0-based index into the campaign stage list
    int  StageCount();
    bool HasVaultKey();
    bool InIntermission();
    Theme CurrentTheme();
    uint32_t CurrentSeed();
    const std::vector<const ModifierDef*>& ActiveModifiers();

    // Editor / AI bug pins: locate a world XZ in the active dungeon.
    struct DefectProbe {
        bool     inDungeon = false;
        uint32_t seed      = 0;
        Theme    theme     = Theme::Masonry;
        int      stage     = 0;
        int      roomId    = -1;
        int      linkIndex = -1;
        const char* roomType = "";
        Vector3  world{};
        float    localX = 0.0f;
        float    localZ = 0.0f;
        int      maskIx = -1;
        int      maskIz = -1;
        int      maskNx = 0;
        int      maskNz = 0;
        bool     walkable = false;
        float    groundY  = 0.0f;
        float    floorOff = 0.0f;
        float    ceilOff  = 0.0f;
    };
    DefectProbe ProbeDefect(float x, float z);

}  // namespace game::dungeon
