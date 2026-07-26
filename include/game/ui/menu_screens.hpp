#pragma once
#include "raylib.h"
#include <cstdint>

// --- NEW: Encapsulated Menu Screens & Events Header ---
namespace game::ui {

    enum class MenuEvent {
        None,
        ContinueUsername,
        StartSinglePlayer,
        CreateLobby,
        JoinLobbyBrowser,
        RefreshLobbies,
        BackToMainMenu,
        ToggleReady,
        LeaveLobby,
    };

    bool button(const char* label, int x, int y, int w, int h,
                Color bgIdle = Color{50, 50, 70, 230},
                Color bgHover = Color{80, 80, 110, 240});

    void handleTextInput(char* buf, int& len, int max);

    MenuEvent drawEnterUsername(const char* usernameBuf);
    // selectedElement: SpellElement index (Fire/Water/Necro/Priest). Mutated by class picker.
    MenuEvent drawMainMenu(const char* usernameBuf, uint8_t& selectedElement);
    MenuEvent drawLobbyBrowser(float refreshCooldown);
    MenuEvent drawLobby(bool isSinglePlayer, const char* usernameBuf, bool localReady, bool remoteReady,
                        uint8_t selectedElement);

}
