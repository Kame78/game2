#include "game/systems.hpp"
#include "raymath.h"

namespace game::systems {

    void Render3DSystem(engine::ecs::Registry& reg) {
        for (size_t i = 0; i < reg.renderables.data.size(); i++) {
            engine::ecs::Entity e = {reg.renderables.indexToEntity[i]};
            if (!reg.transforms.Has(e)) continue;

            auto& render = reg.renderables.data[i];
            auto& t      = reg.transforms.Get(e);

            DrawCube(t.position, render.width, render.height, render.depth, render.color);
            DrawCubeWires(t.position, render.width, render.height, render.depth, BLACK);
        }

        // Render projectiles as glowing spheres
        for (size_t i = 0; i < reg.projectiles.data.size(); i++) {
            engine::ecs::Entity e = {reg.projectiles.indexToEntity[i]};
            if (!reg.transforms.Has(e)) continue;
            auto& t = reg.transforms.Get(e);
            auto& p = reg.projectiles.data[i];
            DrawSphere(t.position, p.radius, ORANGE);
            DrawSphereWires(t.position, p.radius + 0.05f, 6, 6, RED);
        }
    }

    void HealthBarSystem(engine::ecs::Registry& reg) {
        if (reg.playerInputs.data.empty()) return;
        engine::ecs::Entity player = {reg.playerInputs.indexToEntity[0]};
        if (!reg.cameras.Has(player)) return;

        auto& cam = reg.cameras.Get(player).camera;

        // Draw health bars above all damageable entities (except local player)
        for (size_t i = 0; i < reg.healths.data.size(); i++) {
            engine::ecs::Entity e = {reg.healths.indexToEntity[i]};
            if (e == player) continue;
            if (!reg.transforms.Has(e) || !reg.renderables.Has(e)) continue;

            auto& hp  = reg.healths.data[i];
            auto& pos = reg.transforms.Get(e).position;
            auto& r   = reg.renderables.Get(e);

            // Don't show bar if full HP
            if (hp.current >= hp.max) continue;

            float ratio = hp.current / hp.max;
            if (ratio < 0.0f) ratio = 0.0f;

            // Project world position above entity to screen
            Vector3 barWorldPos = {pos.x, pos.y + r.height * 0.5f + 0.4f, pos.z};
            Vector2 screenPos = GetWorldToScreen(barWorldPos, cam);

            // Skip if behind camera
            if (screenPos.x < -100 || screenPos.x > GetScreenWidth() + 100 ||
                screenPos.y < -100 || screenPos.y > GetScreenHeight() + 100) continue;

            int barW = 60;
            int barH = 8;
            int barX = (int)screenPos.x - barW / 2;
            int barY = (int)screenPos.y - barH / 2;

            Color barColor = (ratio > 0.5f) ? GREEN : (ratio > 0.25f) ? YELLOW : RED;

            DrawRectangle(barX, barY, barW, barH, DARKGRAY);
            DrawRectangle(barX, barY, (int)(barW * ratio), barH, barColor);
            DrawRectangleLines(barX, barY, barW, barH, BLACK);
        }
    }

}
