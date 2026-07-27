#pragma once
#include <cstdint>
#include <functional>
#include <vector>

// Thin pub/sub for dungeon lifecycle hooks + director telemetry stubs.
namespace game::dungeon::events {

    enum class Type : uint8_t {
        RoomEntered = 0,
        RoomCleared,
        AmbushTriggered,
        StageComplete,
        IntermissionStarted,
        RunComplete,
        LootClaimed,
    };

    struct Event {
        Type  type       = Type::RoomEntered;
        int   roomId     = -1;
        int   stageIndex = -1;
        float value      = 0.0f; // reward amount, ambush count, etc.
    };

    // Lightweight tension metrics for a future AI director (stubs only).
    struct Telemetry {
        float playerHpFrac        = 1.0f;
        int   roomsCleared        = 0;
        int   roomsVisited        = 0;
        float elapsedSec          = 0.0f;
        float roomsClearedPerMin  = 0.0f;
        float tension             = 0.0f; // 0..1 stub from HP + clear rate
    };

    using Listener = std::function<void(const Event&)>;

    void Reset();
    void Subscribe(Listener listener);
    void Publish(const Event& ev);

    Telemetry& GetTelemetry();
    void TickTelemetry(float dt, float playerHpFrac, int roomsCleared, int roomsVisited);

}  // namespace game::dungeon::events
