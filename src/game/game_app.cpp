#include "game/game_app.hpp"
#include "raylib.h"

namespace Game::GameApp {

    void Init() {

    }

    void Update() {

    }

    void Draw() {
        const char* text = "Hello from the new GameApp Architechture";
        int fontSize = 20;
        int textWidth = MeasureText(text, fontSize);

        int x = GetScreenWidth() / 2 - textWidth / 2;
        int y = GetScreenHeight() / 2 - fontSize / 2;

        DrawText(text, x, y, fontSize, DARKGRAY);
    }

    void Shutdown() {

    }
    
}