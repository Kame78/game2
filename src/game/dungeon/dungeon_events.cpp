#include "game/dungeon/dungeon_events.hpp"
#include <algorithm>
#include <cmath>

namespace game::dungeon::events {

    namespace {
        std::vector<Listener> g_listeners;
        Telemetry             g_telemetry{};
    }

    void Reset() {
        g_telemetry = Telemetry{};
    }

    void Subscribe(Listener listener) {
        if (listener) g_listeners.push_back(std::move(listener));
    }

    void Publish(const Event& ev) {
        for (const auto& listener : g_listeners) {
            listener(ev);
        }
    }

    Telemetry& GetTelemetry() { return g_telemetry; }

    void TickTelemetry(float dt, float playerHpFrac, int roomsCleared, int roomsVisited) {
        g_telemetry.playerHpFrac = playerHpFrac;
        g_telemetry.roomsCleared = roomsCleared;
        g_telemetry.roomsVisited = roomsVisited;
        g_telemetry.elapsedSec  += dt;

        const float minutes = std::max(0.05f, g_telemetry.elapsedSec / 60.0f);
        g_telemetry.roomsClearedPerMin = (float)roomsCleared / minutes;

        // Stub tension: low HP and slow clears both raise pressure.
        const float hpPressure    = 1.0f - std::clamp(playerHpFrac, 0.0f, 1.0f);
        const float clearPressure = 1.0f - std::clamp(g_telemetry.roomsClearedPerMin / 4.0f, 0.0f, 1.0f);
        g_telemetry.tension = std::clamp(0.55f * hpPressure + 0.45f * clearPressure, 0.0f, 1.0f);
    }

}  // namespace game::dungeon::events
