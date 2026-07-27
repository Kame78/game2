#include "core/app.hpp"
#include "raylib.h"
#include "game/game_app.hpp"
#include "engine/networking.hpp"

namespace Core::App {

    int Run() {
        // Silence raylib FILEIO/VAO INFO spam — chunk streaming logs every UploadMesh
        // and that console I/O alone hitchs hard while the overworld fills VRAM.
        SetTraceLogLevel(LOG_WARNING);
        // Must be set before InitWindow — requests MSAA 4x (driver may fall back).
        SetConfigFlags(FLAG_MSAA_4X_HINT);
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