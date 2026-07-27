#include "game/game_app.hpp"
#include "game/factories/entity_factory.hpp"
#include "game/enemy_model.hpp"
#include "game/ui/menu_screens.hpp"
#include "game/ui/hud.hpp"
#include "game/systems.hpp"
#include "game/spells.hpp"
#include "game/world/world_gen.hpp"
#include "game/world/landmarks.hpp"
#include "game/dungeon/dungeon.hpp"
#include "engine/ecs/registry.hpp"
#include "engine/input.hpp"
#include "engine/networking.hpp"
#include "engine/math/noise.hpp"
#include "engine/math/hydrology.hpp"
#include "engine/terrain/chunk_manager.hpp"
#include "engine/render/sky.hpp"
#include "raylib.h"
#include "rlgl.h"
#include "imgui.h"
#include "rlImGui.h"
#include <algorithm>
#include <cstring>

namespace Game::GameApp {

    enum class State {
        EnterUsername,
        MainMenu,
        LobbyBrowser,
        Lobby,
        InGame,
    };
    static State g_state = State::EnterUsername;

    static engine::ecs::Registry registry;
    static engine::ecs::Entity   playerEntity;
    static bool                  g_gameplayStarted = false;

    static char  g_usernameBuf[32] = "";
    static int   g_usernameLen     = 0;
    static float g_refreshCooldown = 0.0f;
    static bool  g_localReady      = false;
    static bool  g_remoteReady     = false;
    static bool  g_isSinglePlayer  = false;
    static uint8_t g_selectedSpellClass = 0; // 0 Fire, 1 Water

    static void applySpellClassToPlayer() {
        if (!g_gameplayStarted || !registry.spellCasters.Has(playerEntity)) return;
        auto& caster = registry.spellCasters.Get(playerEntity);
        caster.selectedElement = g_selectedSpellClass;
        caster.selectedSlot = 0;
    }

    static void spawnPlayerEntity() {
        float spawnGround = engine::math::WorldHeight(0.0f, 0.0f);
        Vector3 spawnPos = {0.0f, spawnGround + 2.0f, 0.0f};
        auto spellClass = (game::SpellElement)(g_selectedSpellClass % (uint8_t)game::SpellElement::Count);
        playerEntity = game::factories::EntityFactory::CreatePlayer(registry, spawnPos, spellClass);
        g_gameplayStarted = true;
    }

    static void enterGame() {
        if (!g_gameplayStarted) spawnPlayerEntity();
        else {
            auto& t  = registry.transforms.Get(playerEntity);
            auto& hp = registry.healths.Get(playerEntity);
            float spawnGround = engine::math::WorldHeight(0.0f, 0.0f);
            t.position = {0.0f, spawnGround + 2.0f, 0.0f};
            hp.current = hp.max;
            applySpellClassToPlayer();
        }
        engine::input::LockCursor();
        g_state = State::InGame;
    }

    static void leaveGame() {
        engine::input::UnlockCursor();
        engine::networking::LeaveLobby();
        g_state = State::MainMenu;
    }

    void Init() {
        rlImGuiSetup(true);

        engine::input::BindKey("MoveForward",  KEY_W);
        engine::input::BindKey("MoveBackward", KEY_S);
        engine::input::BindKey("MoveRight",    KEY_D);
        engine::input::BindKey("MoveLeft",     KEY_A);
        engine::input::BindKey("Jump",         KEY_SPACE);

        game::world::InstallHeightModifier();

        // Keep procedural lakes/rivers off spawn & authored flatten pads.
        engine::math::ClearHydrologyExclusions();
        for (size_t i = 0; i < game::world::LANDMARK_COUNT; ++i) {
            const game::world::Landmark& lm = game::world::LANDMARKS[i];
            if (lm.flatRadius <= 0.0f) continue;

            float protectR = lm.flatRadius + lm.flatFalloff + 48.0f;
            if (lm.type == game::world::LandmarkType::Church) {
                // Cover churchyard flatten + falloff with margin so discs can't flood spawn
                protectR = std::max(protectR, 420.0f);
            }
            engine::math::AddHydrologyExclusion(lm.center.x, lm.center.z, protectR);
        }

        engine::math::BuildHydrology(engine::math::GetWorldConfig().seed);
        engine::render::sky::Init();
        game::world::InitWater();
        game::world::InitBuildingPanels();
        engine::terrain::chunks::Init();
        game::enemy_model::Init();
        game::systems::WeaponViewmodelInit();

        std::string steamName = engine::networking::GetSteamPersonaName();
        if (!steamName.empty()) {
            std::strncpy(g_usernameBuf, steamName.c_str(), sizeof(g_usernameBuf) - 1);
            g_usernameBuf[sizeof(g_usernameBuf) - 1] = 0;
            g_usernameLen = (int)std::strlen(g_usernameBuf);
        }

        engine::input::UnlockCursor();
        g_state = State::EnterUsername;
    }

    void Shutdown() {
        game::systems::WeaponViewmodelShutdown();
        game::enemy_model::Shutdown();
        engine::terrain::chunks::Shutdown();
        game::world::ShutdownWater();
        game::world::ShutdownBuildingPanels();
        engine::render::sky::Shutdown();
        engine::math::ClearHydrology();
        rlImGuiShutdown();
    }

    static void updateInGame() {
        if (IsKeyPressed(KEY_ESCAPE)) engine::input::UnlockCursor();
        
        if (!engine::input::IsCursorLocked() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (!ImGui::GetIO().WantCaptureMouse) {
                engine::input::LockCursor();
            }
        }
        
        if (IsKeyPressed(KEY_F10)) { leaveGame(); return; }
        if (IsKeyPressed(KEY_F3))  engine::networking::OpenInviteOverlay();

        game::systems::EditorInputSystem(registry);
        game::systems::PlayerMovementSystem(registry);

        const bool inDungeon = game::dungeon::IsActive();

        // Overworld streaming pauses while inside an instance.
        if (!inDungeon && registry.transforms.Has(playerEntity)) {
            engine::terrain::chunks::Update(registry.transforms.Get(playerEntity).position);
        }

        bool isNetworked = engine::networking::GetLobbyState() == engine::networking::LobbyState::InLobby;
        if (!isNetworked || engine::networking::IsHost()) {
            game::systems::EnemyAISystem(registry);
            // Dungeon encounters spawn per room instead of the overworld ring.
            if (!inDungeon) game::systems::EnemySpawnSystem(registry);
        }

        game::systems::CombatSystem(registry);
        game::systems::SpellSystem(registry);
        game::systems::ProjectileSystem(registry);
        game::systems::NetworkSyncSystem(registry);

        if (registry.transforms.Has(playerEntity)) {
            const Vector3 playerPos = registry.transforms.Get(playerEntity).position;
            if (!inDungeon && IsKeyPressed(KEY_E)) {
                const int idx = game::dungeon::FindNearbyEntrance(playerPos, 7.0f);
                if (idx >= 0) {
                    const bool lobbied = engine::networking::HasRemotePeer();
                    if (lobbied && !engine::networking::IsHost()) {
                        // Host owns enter; clients wait for seed/theme sync.
                    } else {
                        game::dungeon::Enter(registry, playerEntity,
                                             game::dungeon::GetEntrances()[idx]);
                    }
                }
            } else if (inDungeon && IsKeyPressed(KEY_F8)) {
                if (!engine::networking::HasRemotePeer() || engine::networking::IsHost()) {
                    game::dungeon::BroadcastLifecycle(
                        static_cast<uint8_t>(engine::networking::DungeonOp::Exit));
                    game::dungeon::Exit(registry, playerEntity);
                }
            }
        }
        game::dungeon::Update(registry, playerEntity);

        if (registry.transforms.Has(playerEntity) && registry.cameras.Has(playerEntity)) {
            auto& t = registry.transforms.Get(playerEntity);
            auto& c = registry.cameras.Get(playerEntity);
            engine::networking::PlayerState state;
            state.steamId = engine::networking::GetLocalSteamId();
            state.x = t.position.x;
            state.y = t.position.y;
            state.z = t.position.z;
            state.yaw = c.yaw;
            engine::networking::BroadcastLocalState(state);
        }
    }

    void Update() {
        switch (g_state) {
            case State::EnterUsername:
                game::ui::handleTextInput(g_usernameBuf, g_usernameLen, (int)sizeof(g_usernameBuf));
                if (IsKeyPressed(KEY_ENTER) && g_usernameLen > 0) {
                    engine::networking::SetUsername(g_usernameBuf);
                    g_state = State::MainMenu;
                }
                break;
            case State::MainMenu:
                if (engine::networking::GetLobbyState() == engine::networking::LobbyState::InLobby) {
                    g_localReady = false; g_remoteReady = false; g_isSinglePlayer = false; g_state = State::Lobby;
                }
                break;
            case State::LobbyBrowser:
                g_refreshCooldown -= GetFrameTime();
                if (g_refreshCooldown < 0.0f) g_refreshCooldown = 0.0f;
                if (engine::networking::GetLobbyState() == engine::networking::LobbyState::InLobby) {
                    g_localReady = false; g_remoteReady = false; g_isSinglePlayer = false; g_state = State::Lobby;
                }
                break;
            case State::Lobby:
                if (g_isSinglePlayer) {
                    if (g_localReady) enterGame();
                } else if (engine::networking::IsHost() && g_localReady) {
                    engine::networking::SetLobbyData("game_started", "1");
                    enterGame();
                } else if (!engine::networking::IsHost() && engine::networking::GetLobbyData("game_started") == "1") {
                    enterGame();
                }
                break;
            case State::InGame:
                updateInGame();
                break;
        }
    }

    void Draw() {
        if (g_gameplayStarted && game::dungeon::IsActive()) {
            auto& cam = registry.cameras.Get(playerEntity).camera;
            ClearBackground(Color{6, 5, 10, 255});

            rlSetClipPlanes(0.1f, 4000.0f);
            BeginMode3D(cam);
                game::dungeon::Draw();
                game::systems::EditorDebugDrawSystem(registry);
                game::systems::Render3DSystem(registry);
                game::systems::SpellVfxRenderSystem(registry);
                game::systems::SwordViewmodelSystem(registry);
            EndMode3D();
            rlSetClipPlanes(0.01f, 1000.0f);
        } else if (g_gameplayStarted) {
            auto& cam = registry.cameras.Get(playerEntity).camera;
            // Only replace empty sky pixels  Edoes not tint terrain (no fullscreen overlay).
            // Horizon rays never hit a flat water plane, so underwater sky must be murky.
            const bool underwater = game::world::IsCameraUnderwater(cam);
            // Above water: HDRI fills the sky; clear matches haze so any gaps blend.
            ClearBackground(underwater ? Color{10, 38, 62, 255}
                                       : engine::render::sky::GetHazeColor());
            
            // Far must clear the ±3 km basin (corner-to-corner ~6 km view rays).
            rlSetClipPlanes(0.1f, 8000.0f);
            BeginMode3D(cam);
                if (!underwater) {
                    engine::render::sky::Draw(cam);
                }
                // --- RESTORED: Draw terrain heightmap chunks in 3D pass ---
                engine::terrain::chunks::Draw();
                engine::terrain::chunks::DrawGrass(cam.position);
                engine::terrain::chunks::DrawTrees(cam.position);
                game::systems::EditorDebugDrawSystem(registry);
                
                game::systems::Render3DSystem(registry);
                game::systems::SpellVfxRenderSystem(registry);
                game::world::DrawLandmarks(cam);
                game::dungeon::DrawEntrances(cam);
                game::systems::SwordViewmodelSystem(registry);
                engine::networking::PlayerState remote;
                if (engine::networking::GetRemoteState(remote)) {
                    game::systems::DrawRemotePlayer(remote);
                }
            EndMode3D();
            rlSetClipPlanes(0.01f, 1000.0f);

            if (underwater) {
                game::world::DrawUnderwaterOverlay();
            }
        } else {
            ClearBackground({15, 15, 25, 255});
        }

        switch (g_state) {
            case State::EnterUsername:
                if (game::ui::drawEnterUsername(g_usernameBuf) == game::ui::MenuEvent::ContinueUsername) {
                    if (g_usernameLen > 0) {
                        engine::networking::SetUsername(g_usernameBuf);
                        g_state = State::MainMenu;
                    }
                }
                break;
            case State::MainMenu: {
                auto evt = game::ui::drawMainMenu(g_usernameBuf, g_selectedSpellClass);
                if (evt == game::ui::MenuEvent::StartSinglePlayer) {
                    g_isSinglePlayer = true; g_localReady = false; g_remoteReady = false; g_state = State::Lobby;
                } else if (evt == game::ui::MenuEvent::CreateLobby) {
                    engine::networking::CreateLobby(); g_isSinglePlayer = false; g_localReady = false; g_remoteReady = false; g_state = State::Lobby;
                } else if (evt == game::ui::MenuEvent::JoinLobbyBrowser) {
                    engine::networking::RefreshLobbyList(); g_refreshCooldown = 2.0f; g_state = State::LobbyBrowser;
                }
                break;
            }
            case State::LobbyBrowser: {
                auto evt = game::ui::drawLobbyBrowser(g_refreshCooldown);
                if (evt == game::ui::MenuEvent::RefreshLobbies) {
                    engine::networking::RefreshLobbyList(); g_refreshCooldown = 2.0f;
                } else if (evt == game::ui::MenuEvent::BackToMainMenu) {
                    g_state = State::MainMenu;
                }
                break;
            }
            case State::Lobby: {
                auto evt = game::ui::drawLobby(g_isSinglePlayer, g_usernameBuf, g_localReady, g_remoteReady,
                                               g_selectedSpellClass);
                if (evt == game::ui::MenuEvent::ToggleReady) {
                    g_localReady = !g_localReady;
                } else if (evt == game::ui::MenuEvent::LeaveLobby) {
                    engine::networking::LeaveLobby(); g_localReady = false; g_remoteReady = false; g_state = State::MainMenu;
                }
                break;
            }
            case State::InGame:
                game::ui::DrawInGameHUD(registry, playerEntity);
                game::dungeon::DrawHUD();
                if (registry.transforms.Has(playerEntity)) {
                    game::dungeon::DrawEntrancePrompt(registry.transforms.Get(playerEntity).position);
                }
                break;
        }

        rlImGuiBegin();
        
        if (g_state == State::InGame) {
            game::systems::EditorUISystem(registry);
        }
        rlImGuiEnd();
    }

}
