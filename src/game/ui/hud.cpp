#include "game/ui/hud.hpp"
#include "game/systems.hpp"
#include "engine/networking.hpp"
#include "raylib.h"

namespace game::ui {

    void DrawInGameHUD(engine::ecs::Registry& registry, engine::ecs::Entity playerEntity) {
        game::systems::HealthBarSystem(registry);

        int cx = GetScreenWidth() / 2;
        int cy = GetScreenHeight() / 2;
        DrawLine(cx - 10, cy, cx + 10, cy, WHITE);
        DrawLine(cx, cy - 10, cx, cy + 10, WHITE);

        // Player HP
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
            DrawText(TextFormat("HP: %.0f / %.0f", hpCurrent, hpMax), barX + barW / 2 - 40, barY + 4, 16, WHITE);
        }

        DrawFPS(10, 10);
        DrawText("WASD move | Mouse look | LMB attack | RMB fireball | SPACE jump | ESC free mouse | F10 menu", 10, 30, 14, LIGHTGRAY);

        // Net Status
        {
            const char* netStatus;
            Color bgColor = {60, 60, 60, 200};
            Color textColor = LIGHTGRAY;
            auto ls = engine::networking::GetLobbyState();
            if (ls == engine::networking::LobbyState::None) {
                netStatus = "SOLO";
            } else if (ls == engine::networking::LobbyState::InLobby) {
                if (engine::networking::HasRemotePeer()) {
                    netStatus = engine::networking::IsHost() ? "HOST | Peer Connected | F3 Invite" : "CLIENT | Connected";
                    bgColor = {20, 80, 20, 200}; textColor = GREEN;
                } else {
                    netStatus = engine::networking::IsHost() ? "HOST | Waiting | F3 Invite" : "CLIENT | Waiting...";
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

}
