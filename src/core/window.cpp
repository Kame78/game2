#include "core/window.hpp"
#include "raylib.h"

namespace core::window {
    void Init(int width, int height, const std::string& title) {
        // Must be set before InitWindow — requests MSAA 4x (driver may fall back).
        ::SetConfigFlags(FLAG_MSAA_4X_HINT);
        ::InitWindow(width, height, title.c_str());
        ::SetTargetFPS(60);
    }

    void Close() { ::CloseWindow(); }
    bool ShouldClose() {return ::WindowShouldClose(); }
    void BeginFrame() { ::BeginDrawing(); ::ClearBackground(RAYWHITE); }
    void EndFrame() { ::EndDrawing(); }
    float GetDeltaTime() {return ::GetFrameTime(); }
}