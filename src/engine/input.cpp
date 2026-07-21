#include "engine/input.hpp"
#include <unordered_map>

namespace engine::input {
    static std::unordered_map<std::string, int> keyBindings;
    static bool cursorLocked = false;

    void BindKey(const std::string& action, int raylibKey) { keyBindings[action] = raylibKey; }
    
    bool IsActionDown(const std::string& action) {
        auto it = keyBindings.find(action);
        if (it != keyBindings.end()) return ::IsKeyDown(it->second);
        return false;
    }
    
    bool IsActionPressed(const std::string& action) {
        auto it = keyBindings.find(action);
        if (it != keyBindings.end()) return ::IsKeyPressed(it->second);
        return false;
    }
    
    Vector2 GetMouseDelta() { return ::GetMouseDelta(); }
    void LockCursor() { ::DisableCursor(); cursorLocked = true; }
    void UnlockCursor() { ::EnableCursor(); cursorLocked = false; }
    bool IsCursorLocked() { return cursorLocked; }
}