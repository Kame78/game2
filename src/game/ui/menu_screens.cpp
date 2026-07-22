#include "game/ui/menu_screens.hpp"
#include "engine/networking.hpp"
#include <cstdio>

namespace game::ui {

    bool button(const char* label, int x, int y, int w, int h, Color bgIdle, Color bgHover) {
        Vector2 m = GetMousePosition();
        bool hovered = m.x >= x && m.x < x + w && m.y >= y && m.y < y + h;
        DrawRectangle(x, y, w, h, hovered ? bgHover : bgIdle);
        DrawRectangleLines(x, y, w, h, WHITE);
        int fs = 20;
        int tw = MeasureText(label, fs);
        DrawText(label, x + (w - tw) / 2, y + (h - fs) / 2, fs, WHITE);
        return hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    }

    void handleTextInput(char* buf, int& len, int max) {
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

    MenuEvent drawEnterUsername(const char* usernameBuf) {
        int W = GetScreenWidth(), H = GetScreenHeight();
        DrawRectangle(0, 0, W, H, Color{0, 0, 0, 180});

        const char* title = "ENTER YOUR NAME";
        DrawText(title, W/2 - MeasureText(title, 32)/2, H/2 - 140, 32, WHITE);

        int boxW = 400, boxH = 44;
        int bx = W/2 - boxW/2, by = H/2 - 40;
        DrawRectangle(bx, by, boxW, boxH, Color{30, 30, 45, 255});
        DrawRectangleLines(bx, by, boxW, boxH, WHITE);

        int tx = bx + 12;
        DrawText(usernameBuf, tx, by + 12, 22, WHITE);
        int textW = MeasureText(usernameBuf, 22);
        if ((int)(GetTime() * 2.0) % 2 == 0) {
            DrawRectangle(tx + textW + 2, by + 12, 2, 22, WHITE);
        }

        const char* hint = "Press ENTER to continue";
        DrawText(hint, W/2 - MeasureText(hint, 18)/2, H/2 + 30, 18, LIGHTGRAY);

        if (button("Continue", W/2 - 100, H/2 + 70, 200, 40)) {
            return MenuEvent::ContinueUsername;
        }
        return MenuEvent::None;
    }

    MenuEvent drawMainMenu(const char* usernameBuf) {
        int W = GetScreenWidth(), H = GetScreenHeight();
        DrawRectangle(0, 0, W, H, Color{0, 0, 0, 160});

        const char* title = "APOCALYPSE";
        DrawText(title, W/2 - MeasureText(title, 56)/2, 80, 56, WHITE);
        const char* sub = TextFormat("Playing as: %s", usernameBuf);
        DrawText(sub, W/2 - MeasureText(sub, 18)/2, 145, 18, LIGHTGRAY);

        Color footerColor = engine::networking::GetLocalSteamId() != 0 ? GREEN : GRAY;
        const char* steamStatus = engine::networking::GetLocalSteamId() != 0
            ? "Steam: Connected"
            : "Steam: Offline (multiplayer unavailable)";
        DrawText(steamStatus, 12, H - 24, 16, footerColor);

        int bw = 300, bh = 56, gap = 16, by = H/2 - 60, bx = W/2 - bw/2;
        if (button("Single Player", bx, by, bw, bh)) {
            return MenuEvent::StartSinglePlayer;
        }
        if (button("Create Lobby (Multiplayer)", bx, by + (bh + gap), bw, bh)) {
            return MenuEvent::CreateLobby;
        }
        if (button("Join Lobby (Browser)", bx, by + 2 * (bh + gap), bw, bh)) {
            return MenuEvent::JoinLobbyBrowser;
        }

        return MenuEvent::None;
    }

    MenuEvent drawLobbyBrowser(float refreshCooldown) {
        int W = GetScreenWidth(), H = GetScreenHeight();
        DrawRectangle(0, 0, W, H, Color{0, 0, 0, 180});

        const char* title = "LOBBY BROWSER";
        DrawText(title, W/2 - MeasureText(title, 40)/2, 40, 40, WHITE);

        const char* status;
        auto ls = engine::networking::GetLobbyState();
        if (ls == engine::networking::LobbyState::Joining) status = "Joining lobby...";
        else if (engine::networking::IsLobbyListRefreshing()) status = "Searching for lobbies...";
        else if (engine::networking::IsLobbyListReady() && engine::networking::GetLobbyList().empty()) status = "No open lobbies found.";
        else status = "";
        if (*status) {
            DrawText(status, W/2 - MeasureText(status, 20)/2, 100, 20, YELLOW);
        }

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

        int by = H - 80;
        bool canRefresh = refreshCooldown <= 0.0f && !engine::networking::IsLobbyListRefreshing();
        Color refreshBg = canRefresh ? Color{50, 80, 50, 230} : Color{40, 40, 40, 200};
        Color refreshHov = canRefresh ? Color{80, 120, 80, 240} : Color{40, 40, 40, 200};
        if (button("Refresh", W/2 - 220, by, 200, 44, refreshBg, refreshHov) && canRefresh) {
            return MenuEvent::RefreshLobbies;
        }
        if (button("Back", W/2 + 20, by, 200, 44)) {
            return MenuEvent::BackToMainMenu;
        }

        return MenuEvent::None;
    }

    MenuEvent drawLobby(bool isSinglePlayer, const char* usernameBuf, bool localReady, bool remoteReady) {
        int W = GetScreenWidth(), H = GetScreenHeight();
        DrawRectangle(0, 0, W, H, Color{0, 0, 0, 180});

        const char* title = isSinglePlayer ? "SINGLE PLAYER" : "GAME LOBBY";
        DrawText(title, W/2 - MeasureText(title, 44)/2, 60, 44, WHITE);

        int panelX = W/2 - 250, panelY = 140, panelW = 500;
        DrawRectangle(panelX, panelY, panelW, 200, Color{30, 30, 50, 220});
        DrawRectangleLines(panelX, panelY, panelW, 200, WHITE);

        const char* localLabel = TextFormat("  %s  (You)%s", usernameBuf, localReady ? "  [READY]" : "");
        Color localColor = localReady ? GREEN : WHITE;
        DrawText(localLabel, panelX + 16, panelY + 20, 22, localColor);

        if (!isSinglePlayer) {
            bool hasPeer = engine::networking::HasRemotePeer();
            const char* remoteLabel;
            Color remoteColor;
            if (hasPeer) {
                remoteLabel = TextFormat("  Connected%s", remoteReady ? "  [READY]" : "");
                remoteColor = remoteReady ? GREEN : SKYBLUE;
            } else {
                remoteLabel = "  Waiting for player...";
                remoteColor = GRAY;
            }
            DrawText(remoteLabel, panelX + 16, panelY + 60, 22, remoteColor);

            if (!hasPeer) {
                if (button("Invite Friend (F3)", panelX + 16, panelY + 100, 220, 36)) {
                    engine::networking::OpenInviteOverlay();
                }
            }
        }

        const char* modeInfo = isSinglePlayer
            ? "Press Ready to start the game."
            : (engine::networking::IsHost()
                ? "Host: Press Ready to start when all players are in."
                : "Wait for the host to start the game.");
        DrawText(modeInfo, W/2 - MeasureText(modeInfo, 16)/2, panelY + 160, 16, LIGHTGRAY);

        int btnY = panelY + 220 + 30;
        Color readyBg  = localReady ? Color{20, 80, 20, 230} : Color{50, 50, 70, 230};
        Color readyHov = localReady ? Color{30, 110, 30, 240} : Color{80, 80, 110, 240};
        const char* readyLabel = localReady ? "READY!" : "Ready Up";
        if (button(readyLabel, W/2 - 220, btnY, 200, 50, readyBg, readyHov)) {
            return MenuEvent::ToggleReady;
        }
        if (button("Leave", W/2 + 20, btnY, 200, 50)) {
            return MenuEvent::LeaveLobby;
        }
        if (IsKeyPressed(KEY_F3)) engine::networking::OpenInviteOverlay();

        return MenuEvent::None;
    }

}
