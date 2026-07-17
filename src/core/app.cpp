#include "core/app.hpp"
#include "raylib.h"
#include "game/game_app.hpp"

namespace Core::App {

    int Run() {
        constexpr int screenWidth = 1920;
        constexpr int screenHeight = 1080;

        InitWindow(screenWidth, screenHeight, "game");
        SetTargetFPS(60);

        Game::GameApp::Init();

        while(!WindowShouldClose())
        {
            Game::GameApp::Update();
            BeginDrawing();
            ClearBackground(RAYWHITE);
            Game::GameApp::Draw();
            EndDrawing();
        
        }

        Game::GameApp::Shutdown();
        CloseWindow();

        return 0;
    
    }
}