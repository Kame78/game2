#pragma once
#include <string>

namespace core::window {
    void Init(int width, int height, const std::string& title);
    void Close();
    bool ShouldClose();
    void BeginFrame();
    void EndFrame();
    float GetDeltaTime();
}