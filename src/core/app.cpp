#include "core/app.hpp"
#include "raylib.h"
#include "game/game_app.hpp"
#include "engine/networking.hpp"

namespace Core::App {

    int Run() {
        InitWindow(1280, 720, "game");
        SetTargetFPS(60);

        engine::networking::Init();  // OK to fail — game plays offline

        Game::GameApp::Init();

        while(!WindowShouldClose())
        {
            engine::networking::Update();
            Game::GameApp::Update();
            BeginDrawing();
            ClearBackground({40, 40, 50, 255});
            Game::GameApp::Draw();
            EndDrawing();
        
        }

        Game::GameApp::Shutdown();
        engine::networking::Shutdown();
        CloseWindow();

        return 0;
    
    }
}