#include "core/window.hpp"
#include "raylib.h"

namespace core::window {
    void Init(int width, int height, const std::string& title) {
        ::InitWindow(width, height, title.c_str());
        ::SetTargetFPS(60);
    }

    void Close() { ::CloseWindow(); }
    bool ShouldClose() {return ::WindowShouldClose(); }
    void BeginFrame() { ::BeginDrawing(); ::ClearBackground(RAYWHITE); }
    void EndFrame() { ::EndDrawing(); }
    float GetDeltaTime() {return ::GetFrameTime(); }
}