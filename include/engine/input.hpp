#pragma once
#include <string>
#include "raylib.h"

namespace engine::input {
    void BindKey(const std::string& action, int raylibKey);
    bool IsActionDown(const std::string& action);
    bool IsActionPressed(const std::string& action);
    Vector2 GetMouseDelta();
    void LockCursor();
    void UnlockCursor();
    bool IsCursorLocked();
}