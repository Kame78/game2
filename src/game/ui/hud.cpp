#include "game/ui/hud.hpp"
#include "game/systems.hpp"
#include "game/spells.hpp"
#include "engine/networking.hpp"
#include "raylib.h"
#include <cmath>

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

        // Mana + spell hotbar
        if (registry.spellCasters.Has(playerEntity)) {
            auto& caster = registry.spellCasters.Get(playerEntity);
            int barW = 250, barH = 18;
            int barX = GetScreenWidth() - barW - 20;
            int barY = 52;
            float ratio = caster.mana / fmaxf(caster.manaMax, 1.0f);
            if (ratio < 0.0f) ratio = 0.0f;
            DrawRectangle(barX - 2, barY - 2, barW + 4, barH + 4, BLACK);
            DrawRectangle(barX, barY, barW, barH, DARKGRAY);
            DrawRectangle(barX, barY, (int)(barW * ratio), barH, Color{40, 120, 220, 255});
            DrawRectangleLines(barX, barY, barW, barH, SKYBLUE);
            DrawText(TextFormat("MP: %.0f / %.0f", caster.mana, caster.manaMax),
                     barX + barW / 2 - 40, barY + 1, 16, WHITE);

            if (caster.castingSpell >= 0) {
                const auto& def = game::GetSpellDef(caster.castingSpell);
                DrawText(TextFormat("Casting %s...", def.name), barX, barY + 24, 16, ORANGE);
            }

            // Class + selected spell
            auto el = (game::SpellElement)caster.selectedElement;
            Color classCol = game::ElementColor(el);
            DrawText(TextFormat("Class: %s  [Tab]", game::ElementName(el)), barX, barY + 44, 16, classCol);

            game::SpellId list[16];
            int n = game::GetSpellsForElement(el, list, 16);
            int slot = caster.selectedSlot;
            if (slot < 0) slot = 0;
            if (n > 0 && slot >= n) slot = n - 1;

            // Hotbar slots along bottom
            int slotW = 72, slotH = 52, gap = 6;
            int totalW = n * slotW + (n > 0 ? (n - 1) * gap : 0);
            int hx = (GetScreenWidth() - totalW) / 2;
            int hy = GetScreenHeight() - slotH - 18;
            for (int i = 0; i < n; i++) {
                const auto& def = game::GetSpellDef(list[i]);
                int sx = hx + i * (slotW + gap);
                bool selected = (i == slot);
                Color bg = selected
                    ? Color{(unsigned char)(classCol.r / 3), (unsigned char)(classCol.g / 3),
                            (unsigned char)(classCol.b / 3), 220}
                    : Color{20, 20, 24, 200};
                Color border = selected ? classCol : Color{90, 90, 90, 255};
                DrawRectangle(sx, hy, slotW, slotH, bg);
                DrawRectangleLines(sx, hy, slotW, slotH, border);

                int keyNum = (i == 9) ? 0 : (i + 1);
                DrawText(TextFormat("%d", keyNum), sx + 4, hy + 3, 14, LIGHTGRAY);

                const char* name = def.name;
                int tw = MeasureText(name, 12);
                if (tw > slotW - 6) {
                    DrawText(name, sx + 4, hy + 22, 10, WHITE);
                } else {
                    DrawText(name, sx + (slotW - tw) / 2, hy + 20, 12, WHITE);
                }

                float cd = caster.cooldowns[(int)list[i]];
                if (def.delivery == game::SpellDelivery::Passive) {
                    DrawText("PASSIVE", sx + 4, hy + 36, 10, Color{140, 220, 140, 255});
                } else if (cd > 0.0f) {
                    DrawRectangle(sx, hy, slotW, slotH, Color{0, 0, 0, 140});
                    const char* cdTxt = TextFormat("%.1f", cd);
                    DrawText(cdTxt, sx + (slotW - MeasureText(cdTxt, 16)) / 2, hy + 18, 16, YELLOW);
                }
            }

            if (n > 0) {
                const auto& sel = game::GetSpellDef(list[slot]);
                if (sel.delivery == game::SpellDelivery::Passive) {
                    DrawText(TextFormat("%s — always on for Ranger", sel.name),
                             hx, hy - 22, 16, classCol);
                } else {
                    DrawText(TextFormat("RMB: %s  (%.0f mana)", sel.name, sel.manaCost),
                             hx, hy - 22, 16, classCol);
                }
            }
        }

        DrawFPS(10, 10);
        DrawText("WASD | LMB melee | Tab class | 1-0 select spell | RMB cast | ~ editor | ESC mouse | F10 menu",
                 10, 30, 14, LIGHTGRAY);

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
