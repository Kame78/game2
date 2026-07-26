#include "game/systems.hpp"
#include "game/enemy_model.hpp"
#include "game/character_visual.hpp"
#include "raymath.h"

namespace game::systems {

    void Render3DSystem(engine::ecs::Registry& reg) {
        float t = (float)GetTime();
        for (size_t i = 0; i < reg.renderables.data.size(); i++) {
            engine::ecs::Entity e = {reg.renderables.indexToEntity[i]};
            if (!reg.transforms.Has(e)) continue;

            auto& render = reg.renderables.data[i];
            auto& tr     = reg.transforms.Get(e);

            // Base enemies (Box visual) use the shared Quaternius zombie mesh.
            // Elites / summons / spell visuals use procedural CharacterVisual.
            if (reg.enemyAIs.Has(e) &&
                render.visual == game::CharacterVisual::Box &&
                game::enemy_model::IsReady()) {
                auto& ai = reg.enemyAIs.Get(e);

                // Anim frame advanced in EnemyAISystem (root-motion); only pose + draw here.
                if (ai.animIndex < 0) {
                    ai.animIndex = game::enemy_model::GetAnimIndex(game::enemy_model::AnimClip::Idle);
                    ai.animClip  = static_cast<int>(game::enemy_model::AnimClip::Idle);
                }
                game::enemy_model::ApplyAnimation(ai.animIndex, ai.animFrame);

                const float s = game::enemy_model::GetUniformScale();
                // Transform is cube-center (ground + half height); model pivot is at feet.
                Vector3 feet = {
                    tr.position.x,
                    tr.position.y - render.height * 0.5f,
                    tr.position.z
                };
                DrawModelEx(game::enemy_model::GetModel(),
                            feet,
                            Vector3{0.0f, 1.0f, 0.0f},
                            tr.rotation.y,
                            Vector3{s, s, s},
                            WHITE);
            } else {
                float anim = t;
                if (reg.summons.Has(e)) {
                    anim = reg.summons.Get(e).age;
                }
                game::DrawCharacterVisual(render.visual, tr.position,
                                          render.width, render.height, render.depth,
                                          render.color, render.facingYaw, anim);
            }
        }
        // Projectiles / spell VFX are drawn by SpellVfxRenderSystem.
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
