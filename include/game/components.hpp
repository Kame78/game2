#pragma once
#include "raylib.h"
#include "game/character_visual.hpp"
#include <cstdint>

namespace game {
    struct TransformComponent {
        Vector3 position = {0.0f, 0.0f, 0.0f};
        Vector3 rotation = {0.0f, 0.0f, 0.0f};
        Vector3 scale    = {1.0f, 1.0f, 1.0f};
    };

    struct CameraComponent {
        Camera3D camera = {};
        float yaw   = 0.0f;
        float pitch = 0.0f;
    };

    struct PlayerInputComponent {
        float velocityY = 0.0f;
        bool grounded   = true;
        bool isFlying   = false;
        bool noClip     = false;
        // Ranger dash burst
        float dashTimer = 0.0f;
        float dashVelX  = 0.0f;
        float dashVelZ  = 0.0f;
        // Ranger double-jump passive
        bool canDoubleJump = false;
    };

    struct RenderComponent {
        Color color  = WHITE;
        float width  = 1.0f;
        float height = 1.0f;
        float depth  = 1.0f;
        CharacterVisual visual = CharacterVisual::Box;
        float facingYaw = 0.0f; // radians, 0 = +Z
    };

    struct HealthComponent {
        float current = 100.0f;
        float max     = 100.0f;
    };

    struct EnemyAIComponent {
        // Optional root-motion multiplier (1.0 = authored/synthetic stride).
        float speed          = 1.0f;
        float attackRange    = 1.8f;
        float attackDamage   = 8.0f;
        float attackCooldown = 1.0f;
        float attackTimer    = 0.0f;
        uint32_t netId       = 0;  // network ID for multiplayer sync

        // Per-enemy animation state (advanced in AI; posed at draw time).
        // Used by all EnemyAI zombie-mesh tiers (normal / elite / giant+).
        int   animClip   = -1;     // game::enemy_model::AnimClip (-1 = unset)
        int   animIndex  = -1;     // resolved ModelAnimation index
        int   animFrame  = 0;
        float animTimer  = 0.0f;
        float animPlaybackRate = 1.0f;
        // Persistent clip-speed scale (haste effects). Root motion advances with
        // playback, so scaling this speeds up travel without foot slip.
        float animRateScale = 1.0f;
        bool  attackAnim = false;  // play Bite through once after a hit
    };

    struct ProjectileComponent {
        Vector3 direction = {0.0f, 0.0f, 0.0f};
        float speed       = 25.0f;
        float damage      = 50.0f;
        float aoeRadius   = 4.0f;
        float lifetime    = 3.0f;
        float radius      = 0.25f;
        uint8_t spellId   = 0;
        bool piercing     = false;
        uint32_t piercedIds[8] = {};
        uint8_t pierceCount    = 0;
    };

    // Player mana + spell cooldowns / cast channel + class hotbar.
    struct SpellCasterComponent {
        float mana         = 160.0f;
        float manaMax      = 160.0f;
        float manaRegen    = 12.0f;
        float cooldowns[32] = {};   // indexed by SpellId (Count <= 32)
        float castTimer    = 0.0f;
        int   castingSpell = -1;
        float castAimX = 0.0f;
        float castAimY = 0.0f;
        float castAimZ = 0.0f;
        float castDirX = 0.0f;
        float castDirY = 0.0f;
        float castDirZ = 0.0f;
        // Class loadout: Fire vs Water; slot indexes spells of that element.
        uint8_t selectedElement = 0; // SpellElement::Fire
        uint8_t selectedSlot    = 0; // 0-based within element list
    };

    // Persistent / expanding / moving spell zones.
    struct SpellZoneComponent {
        uint8_t spellId   = 0;
        float damage      = 0.0f;
        float burnDps     = 0.0f;
        float burnDuration = 0.0f;
        float radius      = 0.0f;
        float radiusMax   = 0.0f;
        float expandSpeed = 0.0f;
        float lifetime    = 1.0f;
        float age         = 0.0f;
        float tickTimer   = 0.0f;
        float tickRate    = 0.25f;
        float height      = 2.5f;
        float force       = 0.0f;   // >0 push out/along, <0 pull in
        float travelSpeed = 0.0f;
        float moveDirX    = 0.0f;
        float moveDirY    = 0.0f;
        float moveDirZ    = 0.0f;
        float waveHalfWidth = 0.0f; // tsunami lateral half-width
        bool  damages     = false;
        bool  expandingVisual = false;
        bool  damageOnce  = false;
        uint32_t hitEntityIds[32] = {};
        uint8_t  hitCount = 0;
    };

    struct StatusEffectComponent {
        float burnDps       = 0.0f;
        float burnRemaining = 0.0f;
        // Priest sprite: heal self over time
        float healPerSec    = 0.0f;
        float healRemaining = 0.0f;
        // Necro pixie: steal HP/s from nearest enemy in range
        float drainPerSec   = 0.0f;
        float drainRemaining = 0.0f;
        float drainRange    = 10.0f;
    };

    // Temporary ally (gargoyle / battle angel) or orbiting familiar (pixie / sprite).
    enum class SummonKind : uint8_t {
        Pixie = 0,
        Gargoyle = 1,
        Sprite = 2,
        BattleAngel = 3,
        Reaper = 4,
        ArchAngel = 5,
    };

    struct SummonComponent {
        SummonKind kind   = SummonKind::Gargoyle;
        uint32_t ownerId  = 0;
        float lifetime    = 10.0f;
        float age         = 0.0f;
        float attackDamage = 25.0f;
        float attackRange  = 12.0f; // horizontal seek / aura radius
        float strikeRange  = 4.5f;  // melee engage distance (XZ)
        float attackCooldown = 0.85f;
        float attackTimer  = 0.0f;
        float hoverHeight  = 1.6f;
        float orbitAngle   = 0.0f;
        bool  combatPet    = true;  // false = orbit-only familiar
        bool  auraAttack   = false; // damage all enemies in attackRange each tick
        // Intro animation (Reaper rises from hell, Arch Angel descends in light)
        float spawnAnimDuration = 0.0f;
        float spawnHomeX = 0.0f;
        float spawnHomeZ = 0.0f;
        float spawnStartY = 0.0f;
        float spawnEndY   = 0.0f;
        float spawnGroundY = 0.0f;
        float bodyHeight   = 2.0f;
    };

    struct SpawnerComponent {
        float radius   = 18.0f;
        float interval = 4.0f;
        float timer    = 0.0f;
        int   maxAlive = 4;
    };

    struct LandmarkProxyComponent {
        int typeIndex = 0;
    };
}
