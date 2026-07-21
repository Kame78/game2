#include "game/game_app.hpp"
#include "raylib.h"
#include "engine/ecs/registry.hpp"
#include "game/systems.hpp"
#include "engine/input.hpp"
#include "engine/networking.hpp"

namespace Game::GameApp {

    static engine::ecs::Registry registry;
    static engine::ecs::Entity playerEntity;

    void Init() {
        engine::input::BindKey("MoveForward", KEY_W);
        engine::input::BindKey("MoveBackward", KEY_S);
        engine::input::BindKey("MoveRight", KEY_D);
        engine::input::BindKey("MoveLeft", KEY_A);
        engine::input::BindKey("Jump", KEY_SPACE);
        engine::input::LockCursor();

        // Create the Player
        playerEntity = engine::ecs::CreateEntity(registry);

        game::TransformComponent playerTransform;
        playerTransform.position = {0.0f, 2.0f, 0.0f};

        game::CameraComponent playerCam;
        playerCam.camera.position = playerTransform.position;
        playerCam.camera.target = {0.0f, 2.0f, 1.0f};
        playerCam.camera.up = {0.0f, 1.0f, 0.0f};
        playerCam.camera.fovy = 90.0f;
        playerCam.camera.projection = CAMERA_PERSPECTIVE;

        game::PlayerInputComponent playerInput;

        registry.transforms.Insert(playerEntity, playerTransform);
        registry.cameras.Insert(playerEntity, playerCam);
        registry.playerInputs.Insert(playerEntity, playerInput);

        game::HealthComponent playerHP;
        playerHP.current = 100.0f;
        playerHP.max = 100.0f;
        registry.healths.Insert(playerEntity, playerHP);

        // Create reference cubes (light blue, with collision)
        constexpr Color LIGHT_BLUE = {102, 191, 255, 255};
        struct CubeDef { Vector3 pos; float w, h, d; };
        CubeDef cubeDefs[] = {
            {{ 10.0f, 1.0f,  -5.0f}, 2.0f, 2.0f, 2.0f},
            {{-10.0f, 1.5f,  -8.0f}, 3.0f, 3.0f, 3.0f},
            {{  0.0f, 2.5f, -20.0f}, 4.0f, 5.0f, 4.0f},
            {{ 15.0f, 0.5f,  10.0f}, 1.0f, 1.0f, 1.0f},
            {{-20.0f, 3.0f,  15.0f}, 5.0f, 6.0f, 5.0f},
            {{ 25.0f, 2.0f, -25.0f}, 3.0f, 4.0f, 3.0f},
            {{-15.0f, 1.0f,  25.0f}, 2.0f, 2.0f, 2.0f},
        };
        for (auto& def : cubeDefs) {
            engine::ecs::Entity cube = engine::ecs::CreateEntity(registry);
            game::TransformComponent t;
            t.position = def.pos;
            game::RenderComponent r;
            r.color = LIGHT_BLUE;
            r.width = def.w;
            r.height = def.h;
            r.depth = def.d;
            registry.transforms.Insert(cube, t);
            registry.renderables.Insert(cube, r);
        }

        // Enemies are spawned dynamically by EnemySpawnSystem
    }

    void Update() {
        if (IsKeyPressed(KEY_ESCAPE)) engine::input::UnlockCursor();
        if (!engine::input::IsCursorLocked() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            engine::input::LockCursor();

        // --- Multiplayer hotkeys ---
        if (IsKeyPressed(KEY_F2)) engine::networking::CreateLobby();
        if (IsKeyPressed(KEY_F3)) engine::networking::OpenInviteOverlay();
        if (IsKeyPressed(KEY_F4)) engine::networking::LeaveLobby();

        game::systems::PlayerMovementSystem(registry);

        // AI and spawning only run on host (or when offline)
        bool isNetworked = engine::networking::GetLobbyState() == engine::networking::LobbyState::InLobby;
        if (!isNetworked || engine::networking::IsHost()) {
            game::systems::EnemyAISystem(registry);
            game::systems::EnemySpawnSystem(registry);
        }

        game::systems::CombatSystem(registry);
        game::systems::ProjectileSystem(registry);
        game::systems::NetworkSyncSystem(registry);

        // --- Broadcast local player state ---
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

    void Draw() {
        auto& cam = registry.cameras.Get(playerEntity).camera;

        BeginMode3D(cam);
            game::systems::Render3DSystem(registry);
            game::systems::SwordViewmodelSystem(registry);

            // Draw remote player (if connected) as a purple cube
            engine::networking::PlayerState remote;
            if (engine::networking::GetRemoteState(remote)) {
                Vector3 pos = {remote.x, remote.y - 1.0f, remote.z};  // body below eyes
                DrawCube(pos, 1.0f, 2.0f, 1.0f, PURPLE);
                DrawCubeWires(pos, 1.0f, 2.0f, 1.0f, BLACK);
            }
        EndMode3D();

        // Enemy health bars (drawn in 2D, projected from 3D)
        game::systems::HealthBarSystem(registry);

        // Crosshair
        int cx = GetScreenWidth() / 2;
        int cy = GetScreenHeight() / 2;
        DrawLine(cx - 10, cy, cx + 10, cy, WHITE);
        DrawLine(cx, cy - 10, cx, cy + 10, WHITE);

        // Player HP bar (bottom-center HUD)
        {
            float hpCurrent = 100.0f;
            float hpMax = 100.0f;
            if (registry.healths.Has(playerEntity)) {
                hpCurrent = registry.healths.Get(playerEntity).current;
                hpMax = registry.healths.Get(playerEntity).max;
            }
            int barW = 250;
            int barH = 24;
            int barX = GetScreenWidth() - barW - 20;
            int barY = 20;
            float ratio = hpCurrent / hpMax;
            if (ratio < 0.0f) ratio = 0.0f;
            Color fillColor = (ratio > 0.5f) ? GREEN : (ratio > 0.25f) ? YELLOW : RED;
            DrawRectangle(barX - 2, barY - 2, barW + 4, barH + 4, BLACK);
            DrawRectangle(barX, barY, barW, barH, DARKGRAY);
            DrawRectangle(barX, barY, (int)(barW * ratio), barH, fillColor);
            DrawRectangleLines(barX, barY, barW, barH, WHITE);
            DrawText(TextFormat("HP: %.0f / %.0f", hpCurrent, hpMax), barX + barW / 2 - 40, barY + 4, 16, WHITE);
        }

        DrawFPS(10, 10);
        DrawText("WASD move, Mouse look, LMB attack, SPACE jump, ESC free mouse", 10, 30, 16, DARKGRAY);

        // Networking HUD — prominent status box top-left
        {
            const char* netStatus = "OFFLINE  |  F2 = Host Lobby";
            Color bgColor = {60, 60, 60, 200};
            Color textColor = LIGHTGRAY;

            engine::networking::LobbyState ls = engine::networking::GetLobbyState();
            if (ls == engine::networking::LobbyState::Creating) {
                netStatus = "CREATING LOBBY...";
                bgColor = {80, 80, 20, 200};
                textColor = YELLOW;
            } else if (ls == engine::networking::LobbyState::Joining) {
                netStatus = "JOINING LOBBY...";
                bgColor = {80, 80, 20, 200};
                textColor = YELLOW;
            } else if (ls == engine::networking::LobbyState::InLobby) {
                if (engine::networking::HasRemotePeer()) {
                    netStatus = engine::networking::IsHost()
                        ? "HOST  |  Peer Connected  |  F4 Leave"
                        : "CLIENT  |  Connected to Host  |  F4 Leave";
                    bgColor = {20, 80, 20, 200};
                    textColor = GREEN;
                } else {
                    netStatus = engine::networking::IsHost()
                        ? "HOST  |  Waiting for Peer  |  F3 Invite  |  F4 Leave"
                        : "CLIENT  |  Waiting...  |  F4 Leave";
                    bgColor = {20, 60, 80, 200};
                    textColor = SKYBLUE;
                }
            }

            int textW = MeasureText(netStatus, 18);
            DrawRectangle(8, 48, textW + 16, 28, bgColor);
            DrawRectangleLines(8, 48, textW + 16, 28, textColor);
            DrawText(netStatus, 16, 53, 18, textColor);
        }
    }

    void Shutdown() {}

}
