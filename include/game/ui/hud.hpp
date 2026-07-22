#pragma once
#include "engine/ecs/registry.hpp"

// --- NEW: HUD Rendering Header ---
namespace game::ui {
    void DrawInGameHUD(engine::ecs::Registry& registry, engine::ecs::Entity playerEntity);
}
