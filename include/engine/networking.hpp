#pragma once
#include <cstdint>
#include <string>
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
        float pitch = 0.0f;
        bool swinging = false;
    };

    struct RemoteFireball {
        float x, y, z;
        float dirX, dirY, dirZ;
        float speed;
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

    // Fireball sync (broadcast to all peers)
    void BroadcastFireball(float x, float y, float z, float dirX, float dirY, float dirZ, float speed);
    bool GetRemoteFireballs(std::vector<RemoteFireball>& out);

    // Damage sync (client -> host)
    void SendDamageToHost(uint32_t netId, float damage);
    void GetPendingDamage(std::vector<DamageEvent>& out);

    // -------- Username (used in lobby browser and player nametags) --------
    void SetUsername(const std::string& name);
    const std::string& GetUsername();
    // If Steam is initialized, returns the Steam persona name; otherwise "".
    std::string GetSteamPersonaName();

    // -------- Lobby browser --------
    struct LobbyInfo {
        uint64_t    id           = 0;
        std::string hostName;
        int         playerCount  = 0;
        int         maxPlayers   = 0;
    };

    // Ask Steam for the current list of open lobbies (filtered to our game tag).
    // Result arrives asynchronously; poll with IsLobbyListReady().
    void RefreshLobbyList();
    bool IsLobbyListRefreshing();
    bool IsLobbyListReady();
    const std::vector<LobbyInfo>& GetLobbyList();

    // Directly join a lobby by its uint64 id (from LobbyInfo).
    void JoinLobbyById(uint64_t lobbyId);

    // Lobby metadata helpers (thin wrappers over SteamMatchmaking lobby data).
    void SetLobbyData(const std::string& key, const std::string& value);
    std::string GetLobbyData(const std::string& key);
}
