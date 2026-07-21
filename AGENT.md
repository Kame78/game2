# AGENT.md — Project Guide

## Project Overview

A **first-person apocalyptic action fantasy RPG**. The setting is a fantasy world where a **Lich King** starts a zombie apocalypse by raising the dead and starting a war with humans, elves, and dwarves.

Design pillars:

- **Left 4 Dead-style horde combat** — massive swarms of undead, leader-driven spawning.
- **Diablo / Path of Exile-style ARPG progression** — deep stats, skill trees, loot rarity, build diversity.
- **First-person melee + magic + ranged combat** — class fantasy for holy, arcane, and nature-flavored anti-undead heroes.

**Target platform**: Steam (PC).
**Networking**: Steam API (Steamworks SDK) — for multiplayer, matchmaking, achievements, cloud saves, workshop.

---

## Tech Stack (current)

- **Language**: C++
- **Build**: CMake (see [CMakeLists.txt](CMakeLists.txt))
- **Graphics/engine base**: raylib (fetched via CMake into `build/_deps/raylib-src/`)
- **Architecture**: **Static ECS** (custom, hand-written) — see the Architecture Roadmap below.

> ⚠️ **Engine caveat**: raylib is great for learning and 2D/light-3D, but a first-person RPG with modern lighting, large worlds, animated characters, and Steam multiplayer will likely outgrow it. Consider evaluating **Unreal Engine 5**, **Godot 4**, or **O3DE** early — before too much code is written against raylib. If staying on raylib, plan for integrating additional libraries (physics, animation, networking).

---

## Architecture Roadmap

The codebase is split into three layers: **`core`** (OS/app shell), **`engine`** (reusable tech, no game rules), and **`game`** (rules, content, ECS components & systems). Combined with a **Static ECS**, this structure supports both massive enemy swarms (L4D-style) and data-driven progression (PoE/Diablo-style) without cache misses or virtual-call overhead.

### 1. Predicted Folder Structure

As the game grows, `game/systems/` will be split so no single file becomes 10,000 lines long.

```text
game2/
├── CMakeLists.txt
├── include/
│   ├── core/                  # Application & OS Wrappers
│   │   ├── app.hpp
│   │   └── window.hpp
│   ├── engine/                # Reusable Engine Tech (No game rules here)
│   │   ├── ecs/
│   │   │   └── Registry.hpp   # Static sparse sets & generational IDs
│   │   ├── math/
│   │   │   ├── noise.hpp      # Procedural generation
│   │   │   └── geometry.hpp   # Custom collision math
│   │   ├── physics/           # Raycasting and AABB swept collisions
│   │   └── input.hpp          # Action mapping
│   └── game/                  # The actual game logic
│       ├── components.hpp     # ALL pure data structs live here
│       ├── systems/           # Segmented logic
│       │   ├── ai_system.hpp
│       │   ├── combat_system.hpp
│       │   ├── movement_system.hpp
│       │   ├── render_system.hpp
│       │   └── skill_system.hpp
│       └── game_app.hpp
└── src/
    ├── main.cpp
    ├── core/ ...
    ├── engine/ ...
    └── game/ ...
```

Current files that will fit into this layout:
- [include/core/app.hpp](include/core/app.hpp) / [src/core/app.cpp](src/core/app.cpp) — core application shell (stays in `core/`)
- [include/game/game_app.hpp](include/game/game_app.hpp) / [src/game/game_app.cpp](src/game/game_app.cpp) — game-app layer (stays in `game/`)
- [src/main.cpp](src/main.cpp) — entry point

### 2. Predicted Components (Data)

In a static ECS, all of these are hardcoded as `SparseSet<T>` inside `Registry.hpp`. Components are **pure data** — no methods, no logic.

**Foundation**

- `TransformComponent` — Position, Rotation, Scale.
- `PhysicsComponent` — Velocity, Mass, Bounding Box (for collision).
- `RenderComponent` — 3D Model, Animation state, Color.

**ARPG Elements (Diablo/PoE)**

- `HealthComponent` — Current HP, Max HP.
- `StatsComponent` — STR, DEX, INT, Mana, Crit Chance, Attack Speed. Backbone of ARPG scaling.
- `SkillTreeComponent` — Array/bitmask of unlocked passive nodes, or active skill gems currently equipped.
- `LootComponent` — Attached to dropped items on the ground (Item ID, Rarity).

**Horde Elements (L4D)**

- `HordeAIComponent` — State (Idle, Swarming, Attacking, Stunned), target coordinates.
- `HordeLeaderComponent` — Attached to "Alpha" or elite enemies. They act as mobile spawners, calling in reinforcements or directing the flow of the swarm around them as long as they are alive.

### 3. Predicted Systems (Logic)

Systems live in `src/game/systems/` and iterate over the components above. They are plain free functions taking a `Registry&`.

**The Swarm (L4D mechanics)**

- `LeaderSpawnSystem` — Iterates over enemies with a `HordeLeaderComponent`. If the leader is engaged in combat and their cooldown is ready, spawns waves of lesser enemies near the leader to protect them and overwhelm the player.
- `SwarmMovementSystem` — Instead of expensive A\* pathfinding for 200 enemies, uses **Flow Fields** or **Boids** math. Reads `HordeAI`, calculates swarm pressure (so enemies don't clump inside each other), and updates `PhysicsComponent` velocity.

**The Progression (ARPG mechanics)**

- `SkillExecutionSystem` — Listens to `PlayerInputComponent`. When the player casts (e.g.) "Fireball", checks `SkillTreeComponent` for modifiers like "Multiple Projectiles" or "Explosive" and spawns the appropriate spell entities.
- `StatCalculationSystem` — When the player levels up and clicks a passive node in the UI, recalculates `StatsComponent` (e.g., base damage + 15% fire damage multiplier).
- `CombatSystem` — Handles hit detection. When a sword swings or a fireball explodes, grabs all entities in the radius with a `HealthComponent`. Compares attacker's `Stats` vs. victim's for crits, dodge chances, then applies final damage.

**The Physics**

- `CollisionSystem` — Iterates over all `PhysicsComponent`s, prevents movement through walls/floor, applies gravity.

### 4. Why the Static ECS Dominates Here

Concrete scenario: player casts "Chain Lightning" that bounces between 40 enemies while 100 others pathfind toward the player. `CombatSystem` and `SwarmMovementSystem` process that data instantly.

Because `Health` and `Physics` live in tightly packed `SparseSet`s, the CPU iterates over 100 enemies, applies lightning damage, and computes swarm push-back physics **without a single cache miss**.

**Guardrails for the ECS:**

- Components are POD-ish structs. No constructors that allocate, no virtuals.
- Systems never store state. All state lives in components.
- The `Registry` owns every component array. No `new`/`delete` at runtime for entities.
- Use **generational IDs** for entity handles (index + generation counter) so destroyed entities can't be accidentally referenced.
- One `update(Registry&, float dt)` per system, called in a fixed order from `game_app.cpp`.

---

## Game Design

### Enemy Roster
| Enemy | Role / Notes |
|---|---|
| Lich King | Main boss (endgame) |
| Necromancers | Elite caster, spawns undead — prime candidate for `HordeLeaderComponent` |
| Wraith | Incorporeal, magic-resistant |
| Banshee | Ranged, screams (AoE debuff) |
| Wight | Mid-tier undead warrior — can be a horde leader alpha |
| Mummy | Slow, tanky, curse on hit |
| Ghoul | Fast melee |
| Skeleton | Basic melee/archer trash |
| Zombie (human / dwarf / elf / animal variants) | Basic swarm — primary horde filler |

### Player Classes
| Class | Archetype |
|---|---|
| Templar | Holy heavy-armor melee |
| Crusader | Mounted / two-hand holy warrior |
| Priest / Priestess | Support healer, holy damage |
| Cleric | Hybrid heal + melee |
| Wizard | Ranged elemental caster |
| Ranger | Bow + stealth + nature magic |

> All player classes are **anti-undead flavored** (holy, elemental, nature) — good thematic alignment with the enemy roster. In ECS terms, class is just a preset bundle of `StatsComponent` + `SkillTreeComponent` starting values.

---

## Additional Systems You'll Need

Since this is your first game, here is a checklist of systems that a first-person action RPG typically requires. Not all are needed for a prototype — but plan for them. Items marked **[ECS]** map directly to systems in the roadmap above.

### Core engine systems
- [ ] **First-person camera controller** (mouse-look, head bob, FOV)
- [ ] **Character controller** (walk, run, jump, crouch, stamina) — **[ECS]** `MovementSystem`
- [ ] **Physics / collision** — **[ECS]** `CollisionSystem`. raylib has minimal physics; consider **Jolt** or **Bullet** for real 3D.
- [ ] **Animation system** (skeletal animation, blend trees, IK)
- [ ] **Audio** (3D positional, music layers, dialog) — **miniaudio** or **FMOD**/**Wwise** (Wwise & FMOD free for indies under revenue cap)
- [ ] **Input abstraction** (keyboard + mouse + gamepad, remapping) — `engine/input.hpp`
- [ ] **Asset pipeline** (models, textures, sounds — how do they get from artist tool to game?)
- [ ] **Save/load** (serialization format — JSON, binary, or engine-native). ECS serialization = walk each `SparseSet` and dump.
- [ ] **Scene / level streaming** (open world? hub-based? corridors?)
- [ ] **UI framework** (menus, HUD, inventory) — raylib UI is basic; consider **Dear ImGui** for tools, custom HUD for shipping.

### Gameplay systems
- [ ] **Combat** — **[ECS]** `CombatSystem`: melee hit detection, ranged projectiles, spell system
- [ ] **Stats / attributes** — **[ECS]** `StatsComponent` + `StatCalculationSystem`
- [ ] **Skill trees / class progression / XP / leveling** — **[ECS]** `SkillTreeComponent` + `SkillExecutionSystem`
- [ ] **Inventory & equipment** (weapons, armor, consumables) — new `InventoryComponent`
- [ ] **Loot tables** (weighted drops per enemy tier) — **[ECS]** `LootComponent` + `LootSystem`
- [ ] **Damage types & resistances** (holy vs undead, physical, elemental) — fields on `StatsComponent`
- [ ] **Status effects** (poison, curse, bleed, burn, holy burn on undead) — `StatusEffectComponent` (list of active effects w/ timers)
- [ ] **AI** — **[ECS]** `HordeAIComponent` + `HordeLeaderComponent` + `SwarmMovementSystem` + `LeaderSpawnSystem`; behavior trees or state machines for elites; pathfinding via **recastnavigation**
- [ ] **Enemy spawning** — driven by `HordeLeaderComponent` (elite-triggered waves, not a global director)
- [ ] **Quest system** (main quest, side quests, journal)
- [ ] **Dialog system** (branching, localized)
- [ ] **NPC & faction system** (humans / elves / dwarves relations)
- [ ] **Economy** (merchants, gold, crafting?)
- [ ] **Death & respawn** (checkpoints? soulslike corpse run? saves?)
- [ ] **Difficulty scaling** — tune leader spawn cooldowns, swarm size caps, and leader density per area.

### Content / production
- [ ] **Art style guide** (realistic? stylized? cel-shaded?) — decide EARLY
- [ ] **Concept art** for classes, enemies, bosses, environments
- [ ] **3D models + rigs + animations** (Blender free; MetaHuman for humans if UE)
- [ ] **Textures** (PBR pipeline — albedo/normal/roughness/metallic/AO)
- [ ] **VFX** (particles, spells, gore) — thematic-heavy since undead + magic
- [ ] **Sound effects & music** (dark orchestral? sourcing: freesound, sound designers)
- [ ] **Voice acting** (optional, expensive)
- [ ] **Localization** (min: English; consider zh, ja, ko, de, fr, es, ru, pt-br for Steam reach)
- [ ] **Level design tools** (in-engine editor vs. Blender-as-editor)

### Steam / release
- [ ] **Steamworks SDK integration**:
  - Steam authentication (SteamID)
  - Achievements & stats
  - Cloud saves (Steam Auto-Cloud or ISteamRemoteStorage)
  - Steam Input (controller mapping)
  - Rich Presence & friend invites
  - Workshop (mods — if you want them)
- [ ] **Networking model** (if multiplayer):
  - **Single-player only** ← easiest, recommend for v1
  - **Co-op (2–4)** ← Steam Networking Sockets (P2P, NAT punch, relay via SDR)
  - **MMO/large** ← don't. Not for a first game.
  - Steam's **GameNetworkingSockets** is free & recommended (also open source)
  - For ECS + netcode: only replicate a whitelist of components (`Transform`, `Health`, `HordeAI.state`). Don't blindly serialize everything.
- [ ] **Anti-cheat**: usually not needed for co-op; skip for v1
- [ ] **Store page assets**: capsule art, screenshots, trailer, description, tags
- [ ] **Steam Deck verification** (nice-to-have; controller-friendly UI required)
- [ ] **Age rating** (ESRB / PEGI / CERO / USK — required in some regions)
- [ ] **Steam Direct fee** ($100 USD, refunded after $1000 revenue)

### Legal / business (do NOT skip)
- [ ] **Business entity** (LLC or equivalent — protects personal assets)
- [ ] **Trademark** the game/studio name
- [ ] **License audit** — every library, asset, and font must have a compatible license
- [ ] **Privacy policy & EULA** (required by Steam if collecting any data)
- [ ] **Music/SFX licensing** (many "free" packs are non-commercial only!)
- [ ] **AI-generated content disclosure** (Steam requires it if used)

### Engineering practices (this codebase)
- [ ] **Version control**: git — commit early, commit often; use branches for features
- [ ] **Git LFS** for binary assets (models, textures, audio) — Steam repos get huge fast
- [ ] **CI** (GitHub Actions or similar) — build on push; run unit tests
- [ ] **Automated tests** for pure-logic systems (damage calc, inventory, save/load). ECS makes this easy: build a `Registry`, call one system, assert on components.
- [ ] **Debug tooling**: in-game console, cheat commands, entity inspector (ImGui shines here — write a `Registry` inspector early, it pays back forever)
- [ ] **Profiling**: **Tracy** profiler (free, excellent, C++-native)
- [ ] **Crash reporting**: **Sentry**, **Backtrace**, or Steam's built-in minidump upload
- [ ] **Coding conventions**: pick a style (Google, LLVM, custom) + clang-format + clang-tidy
- [ ] **Third-party deps**: pin versions, prefer CMake FetchContent or vcpkg/Conan

---

## Recommended First Milestones

Given zero prior game-dev experience, aim for **tiny, playable, iterative milestones** — not a giant design doc. Each milestone below also grows the ECS by one or two components/systems.

1. **M1 — Grey-box FPS**: walk around a boxy room, mouse-look, jump. No combat. → `Transform`, `Physics`, `Render`, `MovementSystem`, `CollisionSystem`.
2. **M2 — Punchable dummy**: swing weapon, damage a static enemy, HP bar. → `Health`, `Stats`, `CombatSystem`.
3. **M3 — One enemy AI**: skeleton walks to player, attacks, dies. Loot drops. → `HordeAI`, `Loot`, first `SwarmMovementSystem` (single agent, no swarm yet).
4. **M4 — One class fully playable**: pick Templar, has 3 abilities, HP/stamina, dies & respawns. → `SkillTree`, `SkillExecutionSystem`.
5. **M5 — Leader + swarm**: necromancer spawns skeleton waves while fighting the player. → `HordeLeaderComponent`, `LeaderSpawnSystem`.
6. **M6 — Vertical slice**: 5-minute crypt level, 3 enemy types, 1 mini-boss, save/load. → ECS serialization.
7. **M7 — Steam integration**: SteamID, 1 achievement, cloud save.
8. **M8 — Second class + second biome**.
9. **… iterate outward**.

**Rule of thumb**: if a milestone is bigger than 2 weeks of your calendar time, cut it in half.

---

## Big Risks to Manage

1. **Scope**. This design (9 enemy types × 6 classes × open-world-implied setting) is *AAA-sized*. Ruthlessly cut for v1. A great small game beats an abandoned huge one.
2. **Engine choice on raylib**. Reconsider vs. Unreal/Godot **now**, before more code is written. The Steam networking, animation, and lighting work is drastically less in a mature engine. Note: your ECS layer is engine-agnostic — it survives any engine swap.
3. **ECS over-engineering**. It's tempting to build a fully generic ECS with reflection, scripting, and 12 layers of templates. **Don't.** Keep it static, keep it boring, keep it under 500 lines in `Registry.hpp`.
4. **Art**. Art is usually the #1 cost/time sink. Decide style + pipeline before content production.
5. **First game learning curve**. Consider making a **much smaller game first** (single level, one class, one enemy) to learn the pipeline, then start this project.

---

## Open Questions (decide soon)

1. Stick with raylib, or switch engine? (Recommend deciding before M2.)
2. Single-player only for v1, or co-op from the start?
3. Open world vs. hub-and-spoke vs. linear levels?
4. Combat feel: Skyrim-ish, Dark Souls-ish, Doom-ish, or something else?
5. Perma-death / soulslike / traditional save-anywhere?
6. Art style: realistic, stylized-PBR, low-poly, painterly?
7. Solo dev or team? (Affects everything above.)
8. How generic should the ECS be? (Recommend: hardcode the component list in `Registry.hpp`; no runtime type registration.)
