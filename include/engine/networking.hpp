#pragma once
#include <cstdint>
#include <vector>
#include "raylib.h"

namespace engine::networking {

    // Small message struct sent over the wire each tick
    struct PlayerState {
        uint64_t steamId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float yaw = 0.0f;
    };

    // Enemy state for network sync (host -> client)
    struct EnemyNetState {
        uint32_t netId = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float hpCurrent = 0.0f;
        float hpMax = 0.0f;
    };

    // Damage event (client -> host)
    struct DamageEvent {
        uint32_t netId = 0;
        float damage = 0.0f;
    };

    enum class LobbyState {
        None,
        Creating,
        InLobby,
        Joining,
    };

    bool Init();
    void Shutdown();
    void Update();

    // Lobby management
    void CreateLobby();
    void LeaveLobby();
    void OpenInviteOverlay();

    LobbyState GetLobbyState();
    uint64_t GetLocalSteamId();
    bool IsHost();

    // Player position sync
    void BroadcastLocalState(const PlayerState& state);
    bool GetRemoteState(PlayerState& out);
    bool HasRemotePeer();

    // Enemy sync (host -> client)
    void BroadcastEnemySnapshot(const std::vector<EnemyNetState>& enemies);
    bool GetEnemySnapshot(std::vector<EnemyNetState>& out);

    // Damage sync (client -> host)
    void SendDamageToHost(uint32_t netId, float damage);
    void GetPendingDamage(std::vector<DamageEvent>& out);
}
