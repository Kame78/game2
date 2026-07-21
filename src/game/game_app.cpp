#include "game/game_app.hpp"
#include "raylib.h"
#include "rlgl.h"
#include "engine/ecs/registry.hpp"
#include "game/systems.hpp"
#include "engine/input.hpp"
#include "engine/networking.hpp"
#include "engine/math/noise.hpp"
#include "engine/terrain/chunk_manager.hpp"
#include "game/world/world_gen.hpp"
#include <cstring>
#include <cstdio>

namespace Game::GameApp {

    // ---------- State machine ----------
    enum class State {
        EnterUsername,
        MainMenu,
        LobbyBrowser,
        Lobby,          // Pre-game lobby with ready-up
        InGame,
    };
    static State g_state = State::EnterUsername;

    // ---------- ECS + gameplay state ----------
    static engine::ecs::Registry registry;
    static engine::ecs::Entity   playerEntity;
    static bool                  g_gameplayStarted = false;  // true once player is spawned

    // ---------- Menu state ----------
    static char  g_usernameBuf[32] = "";
    static int   g_usernameLen     = 0;
    static float g_refreshCooldown = 0.0f;
    static bool  g_localReady      = false;
    static bool  g_remoteReady     = false;
    static bool  g_isSinglePlayer  = false;

    // ---------- Small UI helpers ----------
    static bool button(const char* label, int x, int y, int w, int h,
                       Color bgIdle = {50, 50, 70, 230},
                       Color bgHover = {80, 80, 110, 240}) {
        Vector2 m = GetMousePosition();
        bool hovered = m.x >= x && m.x < x + w && m.y >= y && m.y < y + h;
        DrawRectangle(x, y, w, h, hovered ? bgHover : bgIdle);
        DrawRectangleLines(x, y, w, h, WHITE);
        int fs = 20;
        int tw = MeasureText(label, fs);
        DrawText(label, x + (w - tw) / 2, y + (h - fs) / 2, fs, WHITE);
        return hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    }

    static void handleTextInput(char* buf, int& len, int max) {
        int c;
        while ((c = GetCharPressed()) != 0) {
            if (c >= 32 && c < 127 && len < max - 1) {
                buf[len++] = (char)c;
                buf[len] = 0;
            }
        }
        if (IsKeyPressed(KEY_BACKSPACE) && len > 0) {
            buf[--len] = 0;
        }
    }

    // ---------- Gameplay lifecycle ----------
    static void spawnPlayerEntity() {
        playerEntity = engine::ecs::CreateEntity(registry);

        game::TransformComponent playerTransform;
        float spawnGround = engine::math::WorldHeight(0.0f, 0.0f);
        playerTransform.position = {0.0f, spawnGround + 2.0f, 0.0f};

        game::CameraComponent playerCam;
        playerCam.camera.position = playerTransform.position;
        playerCam.camera.target   = {0.0f, playerTransform.position.y, 1.0f};
        playerCam.camera.up       = {0.0f, 1.0f, 0.0f};
        playerCam.camera.fovy     = 90.0f;
        playerCam.camera.projection = CAMERA_PERSPECTIVE;

        game::PlayerInputComponent playerInput;

        registry.transforms.Insert(playerEntity, playerTransform);
        registry.cameras.Insert(playerEntity, playerCam);
        registry.playerInputs.Insert(playerEntity, playerInput);

        game::HealthComponent playerHP;
        playerHP.current = 100.0f;
        playerHP.max     = 100.0f;
        registry.healths.Insert(playerEntity, playerHP);

        g_gameplayStarted = true;
    }

    static void enterGame() {
        if (!g_gameplayStarted) spawnPlayerEntity();
        else {
            // Reset player if re-entering (e.g. left game then rejoined)
            auto& t  = registry.transforms.Get(playerEntity);
            auto& hp = registry.healths.Get(playerEntity);
            float spawnGround = engine::math::WorldHeight(0.0f, 0.0f);
            t.position = {0.0f, spawnGround + 2.0f, 0.0f};
            hp.current = hp.max;
        }
        engine::input::LockCursor();
        g_state = State::InGame;
    }

    static void leaveGame() {
        engine::input::UnlockCursor();
        engine::networking::LeaveLobby();
        g_state = State::MainMenu;
    }

    // ---------- Init / Shutdown ----------
    void Init() {
        engine::input::BindKey("MoveForward",  KEY_W);
        engine::input::BindKey("MoveBackward", KEY_S);
        engine::input::BindKey("MoveRight",    KEY_D);
        engine::input::BindKey("MoveLeft",     KEY_A);
        engine::input::BindKey("Jump",         KEY_SPACE);

        // World setup — height modifier BEFORE terrain generation so landmarks sit flat.
        game::world::InstallHeightModifier();
        rlSetClipPlanes(0.1, 10000.0);
        engine::terrain::chunks::Init();

        // Prefill username from Steam persona name (they can edit it).
        std::string steamName = engine::networking::GetSteamPersonaName();
        if (!steamName.empty()) {
            std::strncpy(g_usernameBuf, steamName.c_str(), sizeof(g_usernameBuf) - 1);
            g_usernameBuf[sizeof(g_usernameBuf) - 1] = 0;
            g_usernameLen = (int)std::strlen(g_usernameBuf);
        }

        engine::input::UnlockCursor();  // menu needs a visible cursor
        g_state = State::EnterUsername;
    }

    void Shutdown() {
        engine::terrain::chunks::Shutdown();
    }

    // ---------- Menu screens ----------
    static void updateEnterUsername() {
        handleTextInput(g_usernameBuf, g_usernameLen, (int)sizeof(g_usernameBuf));
        if (IsKeyPressed(KEY_ENTER) && g_usernameLen > 0) {
            engine::networking::SetUsername(g_usernameBuf);
            g_state = State::MainMenu;
        }
    }

    static void drawEnterUsername() {
        int W = GetScreenWidth(), H = GetScreenHeight();
        // Dim overlay
        DrawRectangle(0, 0, W, H, {0, 0, 0, 180});

        const char* title = "ENTER YOUR NAME";
        DrawText(title, W/2 - MeasureText(title, 32)/2, H/2 - 140, 32, WHITE);

        // Text box
        int boxW = 400, boxH = 44;
        int bx = W/2 - boxW/2, by = H/2 - 40;
        DrawRectangle(bx, by, boxW, boxH, {30, 30, 45, 255});
        DrawRectangleLines(bx, by, boxW, boxH, WHITE);
        // Blinking cursor
        int tx = bx + 12;
        DrawText(g_usernameBuf, tx, by + 12, 22, WHITE);
        int textW = MeasureText(g_usernameBuf, 22);
        if ((int)(GetTime() * 2.0) % 2 == 0) {
            DrawRectangle(tx + textW + 2, by + 12, 2, 22, WHITE);
        }

        const char* hint = "Press ENTER to continue";
        DrawText(hint, W/2 - MeasureText(hint, 18)/2, H/2 + 30, 18, LIGHTGRAY);

        if (button("Continue", W/2 - 100, H/2 + 70, 200, 40)) {
            if (g_usernameLen > 0) {
                engine::networking::SetUsername(g_usernameBuf);
                g_state = State::MainMenu;
            }
        }
    }

    static void updateMainMenu() {
        // Watch for lobby state changes — if we joined via Steam invite, go to lobby room.
        if (engine::networking::GetLobbyState() == engine::networking::LobbyState::InLobby
            && g_state == State::MainMenu) {
            g_localReady  = false;
            g_remoteReady = false;
            g_isSinglePlayer = false;
            g_state = State::Lobby;
        }
    }

    static void drawMainMenu() {
        int W = GetScreenWidth(), H = GetScreenHeight();
        DrawRectangle(0, 0, W, H, {0, 0, 0, 160});

        const char* title = "APOCALYPSE";
        DrawText(title, W/2 - MeasureText(title, 56)/2, 80, 56, WHITE);
        const char* sub = TextFormat("Playing as: %s", g_usernameBuf);
        DrawText(sub, W/2 - MeasureText(sub, 18)/2, 145, 18, LIGHTGRAY);

        int bw = 300, bh = 56, gap = 16;
        int by = H/2 - 60;
        int bx = W/2 - bw/2;

        if (button("Single Player", bx, by, bw, bh)) {
            g_isSinglePlayer = true;
            g_localReady  = false;
            g_remoteReady = false;
            g_state = State::Lobby;
        }
        if (button("Create Lobby (Multiplayer)", bx, by + (bh + gap), bw, bh)) {
            engine::networking::CreateLobby();
            g_isSinglePlayer = false;
            g_localReady  = false;
            g_remoteReady = false;
            g_state = State::Lobby;
        }
        if (button("Join Lobby (Browser)", bx, by + 2 * (bh + gap), bw, bh)) {
            engine::networking::RefreshLobbyList();
            g_refreshCooldown = 2.0f;
            g_state = State::LobbyBrowser;
        }

        // Steam status footer
        Color footerColor = engine::networking::GetLocalSteamId() != 0 ? GREEN : GRAY;
        const char* steamStatus = engine::networking::GetLocalSteamId() != 0
            ? "Steam: Connected"
            : "Steam: Offline (multiplayer unavailable)";
        DrawText(steamStatus, 12, H - 24, 16, footerColor);
    }

    static void updateLobbyBrowser() {
        g_refreshCooldown -= GetFrameTime();
        if (g_refreshCooldown < 0.0f) g_refreshCooldown = 0.0f;

        // If we successfully joined a lobby, go to the lobby room (not directly to game).
        if (engine::networking::GetLobbyState() == engine::networking::LobbyState::InLobby) {
            g_localReady  = false;
            g_remoteReady = false;
            g_isSinglePlayer = false;
            g_state = State::Lobby;
        }
    }

    static void drawLobbyBrowser() {
        int W = GetScreenWidth(), H = GetScreenHeight();
        DrawRectangle(0, 0, W, H, {0, 0, 0, 180});

        const char* title = "LOBBY BROWSER";
        DrawText(title, W/2 - MeasureText(title, 40)/2, 40, 40, WHITE);

        // Status line
        const char* status;
        auto ls = engine::networking::GetLobbyState();
        if (ls == engine::networking::LobbyState::Joining) status = "Joining lobby...";
        else if (engine::networking::IsLobbyListRefreshing()) status = "Searching for lobbies...";
        else if (engine::networking::IsLobbyListReady() && engine::networking::GetLobbyList().empty()) status = "No open lobbies found.";
        else status = "";
        if (*status) {
            DrawText(status, W/2 - MeasureText(status, 20)/2, 100, 20, YELLOW);
        }

        // Lobby list (scrollable-ish, capped at 12 visible)
        int listX = W/2 - 300, listY = 140, listW = 600;
        int rowH = 44, rowGap = 6;
        const auto& lobbies = engine::networking::GetLobbyList();
        int shown = (int)lobbies.size();
        if (shown > 12) shown = 12;
        for (int i = 0; i < shown; ++i) {
            const auto& lob = lobbies[i];
            int ry = listY + i * (rowH + rowGap);
            char label[128];
            std::snprintf(label, sizeof(label), "%s   (%d/%d players)   [Join]",
                          lob.hostName.c_str(), lob.playerCount, lob.maxPlayers);
            if (button(label, listX, ry, listW, rowH)) {
                engine::networking::JoinLobbyById(lob.id);
            }
        }

        // Bottom buttons
        int by = H - 80;
        bool canRefresh = g_refreshCooldown <= 0.0f && !engine::networking::IsLobbyListRefreshing();
        Color refreshBg = canRefresh ? Color{50, 80, 50, 230} : Color{40, 40, 40, 200};
        Color refreshHov = canRefresh ? Color{80, 120, 80, 240} : Color{40, 40, 40, 200};
        if (button("Refresh", W/2 - 220, by, 200, 44, refreshBg, refreshHov) && canRefresh) {
            engine::networking::RefreshLobbyList();
            g_refreshCooldown = 2.0f;
        }
        if (button("Back", W/2 + 20, by, 200, 44)) {
            g_state = State::MainMenu;
        }
    }

    // ---------- Pre-game Lobby (L4D-style ready-up) ----------
    static void updateLobby() {
        // In single player, auto-start when ready is clicked.
        // In multiplayer, start only when ALL players are ready.
        // For now (2 player max): local + remote both ready → start.
        if (g_isSinglePlayer) {
            if (g_localReady) enterGame();
        } else {
            // TODO: send/receive ready state over network (using lobby metadata for simplicity).
            // For now, host can force-start once ready.
            if (g_localReady && (engine::networking::IsHost() || g_remoteReady)) {
                enterGame();
            }
        }
    }

    static void drawLobby() {
        int W = GetScreenWidth(), H = GetScreenHeight();
        DrawRectangle(0, 0, W, H, {0, 0, 0, 180});

        const char* title = g_isSinglePlayer ? "SINGLE PLAYER" : "GAME LOBBY";
        DrawText(title, W/2 - MeasureText(title, 44)/2, 60, 44, WHITE);

        // Show who's in the lobby
        int panelX = W/2 - 250, panelY = 140, panelW = 500;
        DrawRectangle(panelX, panelY, panelW, 200, {30, 30, 50, 220});
        DrawRectangleLines(panelX, panelY, panelW, 200, WHITE);

        // Local player row
        const char* localLabel = TextFormat("  %s  (You)%s",
            g_usernameBuf, g_localReady ? "  [READY]" : "");
        Color localColor = g_localReady ? GREEN : WHITE;
        DrawText(localLabel, panelX + 16, panelY + 20, 22, localColor);

        // Remote player row (multiplayer only)
        if (!g_isSinglePlayer) {
            bool hasPeer = engine::networking::HasRemotePeer();
            const char* remoteLabel;
            Color remoteColor;
            if (hasPeer) {
                remoteLabel = TextFormat("  Connected%s", g_remoteReady ? "  [READY]" : "");
                remoteColor = g_remoteReady ? GREEN : SKYBLUE;
            } else {
                remoteLabel = "  Waiting for player...";
                remoteColor = GRAY;
            }
            DrawText(remoteLabel, panelX + 16, panelY + 60, 22, remoteColor);

            // Invite button
            if (!hasPeer) {
                if (button("Invite Friend (F3)", panelX + 16, panelY + 100, 220, 36)) {
                    engine::networking::OpenInviteOverlay();
                }
            }
        }

        // Mode info
        const char* modeInfo = g_isSinglePlayer
            ? "Press Ready to start the game."
            : (engine::networking::IsHost()
                ? "Host: Press Ready to start when all players are in."
                : "Wait for the host to start the game.");
        DrawText(modeInfo, W/2 - MeasureText(modeInfo, 16)/2, panelY + 160, 16, LIGHTGRAY);

        // Bottom buttons
        int btnY = panelY + 220 + 30;
        Color readyBg  = g_localReady ? Color{20, 80, 20, 230} : Color{50, 50, 70, 230};
        Color readyHov = g_localReady ? Color{30, 110, 30, 240} : Color{80, 80, 110, 240};
        const char* readyLabel = g_localReady ? "READY!" : "Ready Up";
        if (button(readyLabel, W/2 - 220, btnY, 200, 50, readyBg, readyHov)) {
            g_localReady = !g_localReady;
        }
        if (button("Leave", W/2 + 20, btnY, 200, 50)) {
            engine::networking::LeaveLobby();
            g_localReady  = false;
            g_remoteReady = false;
            g_state = State::MainMenu;
        }

        // F3 shortcut for invite
        if (IsKeyPressed(KEY_F3)) engine::networking::OpenInviteOverlay();
    }

    // ---------- Gameplay (existing per-frame logic) ----------
    static void updateInGame() {
        if (IsKeyPressed(KEY_ESCAPE)) engine::input::UnlockCursor();
        if (!engine::input::IsCursorLocked() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            engine::input::LockCursor();
        if (IsKeyPressed(KEY_F10)) { leaveGame(); return; }
        if (IsKeyPressed(KEY_F3))  engine::networking::OpenInviteOverlay();

        game::systems::PlayerMovementSystem(registry);

        if (registry.transforms.Has(playerEntity)) {
            engine::terrain::chunks::Update(registry.transforms.Get(playerEntity).position);
        }

        bool isNetworked = engine::networking::GetLobbyState() == engine::networking::LobbyState::InLobby;
        if (!isNetworked || engine::networking::IsHost()) {
            game::systems::EnemyAISystem(registry);
            game::systems::EnemySpawnSystem(registry);
        }

        game::systems::CombatSystem(registry);
        game::systems::ProjectileSystem(registry);
        game::systems::NetworkSyncSystem(registry);

        // Broadcast local player state
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

    static void drawInGameWorld() {
        auto& cam = registry.cameras.Get(playerEntity).camera;
        BeginMode3D(cam);
            game::systems::Render3DSystem(registry);
            game::world::DrawLandmarks(cam);
            game::systems::SwordViewmodelSystem(registry);

            engine::networking::PlayerState remote;
            if (engine::networking::GetRemoteState(remote)) {
                Vector3 pos = {remote.x, remote.y - 1.0f, remote.z};
                DrawCube(pos, 1.0f, 2.0f, 1.0f, PURPLE);
                DrawCubeWires(pos, 1.0f, 2.0f, 1.0f, BLACK);
            }
        EndMode3D();
    }

    static void drawInGameHUD() {
        game::systems::HealthBarSystem(registry);

        int cx = GetScreenWidth() / 2;
        int cy = GetScreenHeight() / 2;
        DrawLine(cx - 10, cy, cx + 10, cy, WHITE);
        DrawLine(cx, cy - 10, cx, cy + 10, WHITE);

        // Player HP (top-right)
        {
            float hpCurrent = 100.0f, hpMax = 100.0f;
            if (registry.healths.Has(playerEntity)) {
                hpCurrent = registry.healths.Get(playerEntity).current;
                hpMax     = registry.healths.Get(playerEntity).max;
            }
            int barW = 250, barH = 24;
            int barX = GetScreenWidth() - barW - 20;
            int barY = 20;
            float ratio = hpCurrent / hpMax;
            if (ratio < 0.0f) ratio = 0.0f;
            Color fillColor = (ratio > 0.5f) ? GREEN : (ratio > 0.25f) ? YELLOW : RED;
            DrawRectangle(barX - 2, barY - 2, barW + 4, barH + 4, BLACK);
            DrawRectangle(barX, barY, barW, barH, DARKGRAY);
            DrawRectangle(barX, barY, (int)(barW * ratio), barH, fillColor);
            DrawRectangleLines(barX, barY, barW, barH, WHITE);
            DrawText(TextFormat("HP: %.0f / %.0f", hpCurrent, hpMax),
                     barX + barW / 2 - 40, barY + 4, 16, WHITE);
        }

        DrawFPS(10, 10);
        DrawText("WASD move | Mouse look | LMB attack | RMB fireball | SPACE jump | ESC free mouse | F10 menu",
                 10, 30, 14, LIGHTGRAY);

        // Networking status
        {
            const char* netStatus;
            Color bgColor = {60, 60, 60, 200};
            Color textColor = LIGHTGRAY;
            auto ls = engine::networking::GetLobbyState();
            if (ls == engine::networking::LobbyState::None) {
                netStatus = "SOLO";
            } else if (ls == engine::networking::LobbyState::InLobby) {
                if (engine::networking::HasRemotePeer()) {
                    netStatus = engine::networking::IsHost()
                        ? "HOST | Peer Connected | F3 Invite"
                        : "CLIENT | Connected";
                    bgColor = {20, 80, 20, 200}; textColor = GREEN;
                } else {
                    netStatus = engine::networking::IsHost()
                        ? "HOST | Waiting | F3 Invite"
                        : "CLIENT | Waiting...";
                    bgColor = {20, 60, 80, 200}; textColor = SKYBLUE;
                }
            } else {
                netStatus = "CONNECTING...";
                bgColor = {80, 80, 20, 200}; textColor = YELLOW;
            }
            int textW = MeasureText(netStatus, 16);
            DrawRectangle(8, 48, textW + 16, 24, bgColor);
            DrawRectangleLines(8, 48, textW + 16, 24, textColor);
            DrawText(netStatus, 16, 52, 16, textColor);
        }
    }

    // ---------- Public entry points ----------
    void Update() {
        switch (g_state) {
            case State::EnterUsername: updateEnterUsername(); break;
            case State::MainMenu:      updateMainMenu();      break;
            case State::LobbyBrowser:  updateLobbyBrowser();  break;
            case State::Lobby:         updateLobby();         break;
            case State::InGame:        updateInGame();        break;
        }
    }

    void Draw() {
        // Always draw the world as a backdrop (once gameplay has been initialized).
        if (g_gameplayStarted) {
            drawInGameWorld();
        } else {
            // Before any gameplay starts, show a simple dark backdrop.
            ClearBackground({15, 15, 25, 255});
        }

        switch (g_state) {
            case State::EnterUsername: drawEnterUsername(); break;
            case State::MainMenu:      drawMainMenu();      break;
            case State::LobbyBrowser:  drawLobbyBrowser();  break;
            case State::Lobby:         drawLobby();         break;
            case State::InGame:        drawInGameHUD();     break;
        }
    }

}
