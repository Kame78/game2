#include "game/systems.hpp"
#include "engine/input.hpp"
#include "imgui.h"

namespace game::systems {

    bool g_showEditor = false;
    float g_playerMoveSpeed = 10.0f;

    void EditorInputSystem(engine::ecs::Registry& reg) {
        // Toggle editor with ~ (KEY_GRAVE), F2, or P
        if (IsKeyPressed(KEY_GRAVE) || IsKeyPressed(KEY_F2) || IsKeyPressed(KEY_P)) {
            g_showEditor = !g_showEditor;
            if (g_showEditor) {
                engine::input::UnlockCursor();
            } else {
                engine::input::LockCursor();
            }
        }

        if (g_showEditor) {
            // While editor is open, press Left Alt to toggle mouse look vs mouse cursor
            if (IsKeyPressed(KEY_LEFT_ALT)) {
                if (engine::input::IsCursorLocked()) engine::input::UnlockCursor();
                else engine::input::LockCursor();
            }
        }
    }

    void EditorUISystem(engine::ecs::Registry& reg) {
        if (!g_showEditor) return;

        // --- MODIFIED: Force top-left window position, size, and uncollapsed state ---
        ImGui::SetNextWindowPos(ImVec2(20, 40), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(340, 280), ImGuiCond_Always);
        ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);

        // --- MODIFIED: Remove &g_showEditor to prevent ImGui from auto-closing it ---
        if (!ImGui::Begin("Developer Editor (~ / F2 / P)")) {
            ImGui::End();
            return;
        }

        ImGui::Text("Press LEFT ALT to toggle mouse lock.");
        ImGui::Separator();
        
        if (!reg.playerInputs.data.empty()) {
            engine::ecs::Entity playerE = {reg.playerInputs.indexToEntity[0]};
            if (reg.playerInputs.Has(playerE)) {
                auto& input = reg.playerInputs.Get(playerE);
                ImGui::Checkbox("Flight Mode", &input.isFlying);
                ImGui::Checkbox("NoClip", &input.noClip);
            }
        }

        ImGui::SliderFloat("Move Speed", &g_playerMoveSpeed, 5.0f, 100.0f);
        
        // Developer Mode Coordinates Readout
        ImGui::Separator();
        ImGui::Text("Dev Coordinates:");
        if (!reg.transforms.data.empty()) {
            engine::ecs::Entity playerE = {reg.transforms.indexToEntity[0]};
            if (reg.transforms.Has(playerE)) {
                auto& t = reg.transforms.Get(playerE);
                ImGui::Text("Position: X: %.2f | Y: %.2f | Z: %.2f", t.position.x, t.position.y, t.position.z);
            }
        }
        if (!reg.cameras.data.empty()) {
            engine::ecs::Entity playerE = {reg.cameras.indexToEntity[0]};
            if (reg.cameras.Has(playerE)) {
                auto& c = reg.cameras.Get(playerE);
                ImGui::Text("Facing Yaw: %.2f rad", c.yaw);
            }
        }

        ImGui::End();
    }

}
