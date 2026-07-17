#include "raylib.h"

int main()
{
	constexpr int screenWidth = 800;
	constexpr int screenHeight = 450;

	InitWindow(screenWidth, screenHeight, "game");
	SetTargetFPS(60);

	while (!WindowShouldClose())
	{
		BeginDrawing();
		ClearBackground(RAYWHITE);
		DrawText("Hello, Raylib!", 320, 200, 20, DARKGRAY);
		EndDrawing();
	}

	CloseWindow();
	return 0;
}

