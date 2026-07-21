#include "engine/networking.hpp"
#include "steam/steam_api.h"
#include "steam/isteamnetworkingmessages.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace engine::networking {

    // Networking channel ID for our position updates
    static constexpr int CHANNEL_POSITION = 0;
    static constexpr int CHANNEL_ENEMIES = 1;
    static constexpr int CHANNEL_DAMAGE = 2;

    #pragma pack(push, 1)
    struct WirePacket {
        uint8_t type;      // 1 = position update
        float x, y, z, yaw;
    };
    struct WireEnemyEntry {
        uint32_t netId;
        float x, y, z;
        float hpCurrent, hpMax;
    };
    struct WireDamage {
        uint8_t type;      // 3 = damage event
        uint32_t netId;
        float damage;
    };
    #pragma pack(pop)

    static bool steamInitialized = false;
    static bool isHostFlag = false;
    static LobbyState lobbyState = LobbyState::None;
    static CSteamID currentLobby;
    static PlayerState remoteState;
    static bool haveRemote = false;
    static std::vector<EnemyNetState> latestSnapshot;
    static bool newSnapshotReady = false;
    static std::vector<DamageEvent> pendingDamage;

    // --- Callback listener class ---
    class Listener {
    public:
        Listener()
            : lobbyCreatedCallback(this, &Listener::OnLobbyCreated),
              lobbyEnterCallback(this, &Listener::OnLobbyEnter),
              gameLobbyJoinRequestedCallback(this, &Listener::OnGameLobbyJoinRequested),
              sessionRequestCallback(this, &Listener::OnSessionRequest),
              lobbyChatUpdate(this, &Listener::OnLobbyChatUpdate) {}

        void OnLobbyCreated(LobbyCreated_t* result) {
            if (result->m_eResult == k_EResultOK) {
                currentLobby = CSteamID(result->m_ulSteamIDLobby);
                lobbyState = LobbyState::InLobby;
                printf("[net] Lobby created: %llu\n", result->m_ulSteamIDLobby);
                SteamMatchmaking()->SetLobbyJoinable(currentLobby, true);
            } else {
                printf("[net] Lobby create failed: %d\n", result->m_eResult);
                lobbyState = LobbyState::None;
            }
        }

        void OnLobbyEnter(LobbyEnter_t* result) {
            currentLobby = CSteamID(result->m_ulSteamIDLobby);
            lobbyState = LobbyState::InLobby;
            printf("[net] Entered lobby: %llu\n", result->m_ulSteamIDLobby);

            // Send initial hello to all other members so P2P session opens
            int count = SteamMatchmaking()->GetNumLobbyMembers(currentLobby);
            CSteamID me = SteamUser()->GetSteamID();
            for (int i = 0; i < count; i++) {
                CSteamID member = SteamMatchmaking()->GetLobbyMemberByIndex(currentLobby, i);
                if (member == me) continue;
                WirePacket hello = {1, 0, 0, 0, 0};
                SteamNetworkingIdentity id = {};
                id.SetSteamID(member);
                SteamNetworkingMessages()->SendMessageToUser(
                    id, &hello, sizeof(hello),
                    k_nSteamNetworkingSend_Unreliable, CHANNEL_POSITION);
            }
        }

        // Fired when user accepts an invite from friend list
        void OnGameLobbyJoinRequested(GameLobbyJoinRequested_t* req) {
            printf("[net] Joining lobby via invite: %llu\n", req->m_steamIDLobby.ConvertToUint64());
            lobbyState = LobbyState::Joining;
            SteamMatchmaking()->JoinLobby(req->m_steamIDLobby);
        }

        // Auto-accept incoming P2P session requests
        void OnSessionRequest(SteamNetworkingMessagesSessionRequest_t* req) {
            SteamNetworkingMessages()->AcceptSessionWithUser(req->m_identityRemote);
            printf("[net] Accepted P2P session\n");
        }

        void OnLobbyChatUpdate(LobbyChatUpdate_t* update) {
            if (update->m_rgfChatMemberStateChange & k_EChatMemberStateChangeEntered) {
                printf("[net] Peer entered lobby: %llu\n", update->m_ulSteamIDUserChanged);
            }
            if (update->m_rgfChatMemberStateChange & (k_EChatMemberStateChangeLeft | k_EChatMemberStateChangeDisconnected)) {
                printf("[net] Peer left lobby: %llu\n", update->m_ulSteamIDUserChanged);
                haveRemote = false;
            }
        }

    private:
        CCallback<Listener, LobbyCreated_t> lobbyCreatedCallback;
        CCallback<Listener, LobbyEnter_t> lobbyEnterCallback;
        CCallback<Listener, GameLobbyJoinRequested_t> gameLobbyJoinRequestedCallback;
        CCallback<Listener, SteamNetworkingMessagesSessionRequest_t> sessionRequestCallback;
        CCallback<Listener, LobbyChatUpdate_t> lobbyChatUpdate;
    };

    static Listener* listener = nullptr;

    bool Init() {
        if (!SteamAPI_Init()) {
            printf("[net] SteamAPI_Init failed. Is Steam running? Is steam_appid.txt present?\n");
            return false;
        }
        steamInitialized = true;
        listener = new Listener();

        // Initialize networking messages
        SteamNetworkingUtils()->InitRelayNetworkAccess();

        printf("[net] Steam initialized. Local user: %llu (%s)\n",
               SteamUser()->GetSteamID().ConvertToUint64(),
               SteamFriends()->GetPersonaName());
        return true;
    }

    void Shutdown() {
        if (!steamInitialized) return;
        if (lobbyState == LobbyState::InLobby) {
            SteamMatchmaking()->LeaveLobby(currentLobby);
        }
        delete listener;
        listener = nullptr;
        SteamAPI_Shutdown();
        steamInitialized = false;
    }

    void Update() {
        if (!steamInitialized) return;
        SteamAPI_RunCallbacks();

        // Receive player position messages
        SteamNetworkingMessage_t* msgs[16];
        int received = SteamNetworkingMessages()->ReceiveMessagesOnChannel(CHANNEL_POSITION, msgs, 16);
        for (int i = 0; i < received; i++) {
            if (msgs[i]->GetSize() == sizeof(WirePacket)) {
                WirePacket* pkt = (WirePacket*)msgs[i]->GetData();
                if (pkt->type == 1) {
                    remoteState.steamId = msgs[i]->m_identityPeer.GetSteamID64();
                    remoteState.x = pkt->x;
                    remoteState.y = pkt->y;
                    remoteState.z = pkt->z;
                    remoteState.yaw = pkt->yaw;
                    haveRemote = true;
                }
            }
            msgs[i]->Release();
        }

        // Receive enemy snapshots (client reads from host)
        SteamNetworkingMessage_t* eMsgs[4];
        int eReceived = SteamNetworkingMessages()->ReceiveMessagesOnChannel(CHANNEL_ENEMIES, eMsgs, 4);
        for (int i = 0; i < eReceived; i++) {
            size_t sz = eMsgs[i]->GetSize();
            if (sz >= 2) {
                const uint8_t* data = (const uint8_t*)eMsgs[i]->GetData();
                uint16_t count = 0;
                memcpy(&count, data, 2);
                if (sz == 2 + count * sizeof(WireEnemyEntry)) {
                    latestSnapshot.clear();
                    const WireEnemyEntry* entries = (const WireEnemyEntry*)(data + 2);
                    for (uint16_t j = 0; j < count; j++) {
                        EnemyNetState s;
                        s.netId = entries[j].netId;
                        s.x = entries[j].x;
                        s.y = entries[j].y;
                        s.z = entries[j].z;
                        s.hpCurrent = entries[j].hpCurrent;
                        s.hpMax = entries[j].hpMax;
                        latestSnapshot.push_back(s);
                    }
                    newSnapshotReady = true;
                }
            }
            eMsgs[i]->Release();
        }

        // Receive damage events (host reads from clients)
        SteamNetworkingMessage_t* dMsgs[16];
        int dReceived = SteamNetworkingMessages()->ReceiveMessagesOnChannel(CHANNEL_DAMAGE, dMsgs, 16);
        for (int i = 0; i < dReceived; i++) {
            if (dMsgs[i]->GetSize() == sizeof(WireDamage)) {
                WireDamage* d = (WireDamage*)dMsgs[i]->GetData();
                if (d->type == 3) {
                    pendingDamage.push_back({d->netId, d->damage});
                }
            }
            dMsgs[i]->Release();
        }
    }

    void CreateLobby() {
        if (!steamInitialized) return;
        if (lobbyState != LobbyState::None) return;
        lobbyState = LobbyState::Creating;
        isHostFlag = true;
        SteamMatchmaking()->CreateLobby(k_ELobbyTypeFriendsOnly, 4);
    }

    void LeaveLobby() {
        if (!steamInitialized || lobbyState != LobbyState::InLobby) return;
        SteamMatchmaking()->LeaveLobby(currentLobby);
        lobbyState = LobbyState::None;
        isHostFlag = false;
        haveRemote = false;
    }

    void OpenInviteOverlay() {
        if (!steamInitialized || lobbyState != LobbyState::InLobby) return;
        SteamFriends()->ActivateGameOverlayInviteDialog(currentLobby);
    }

    LobbyState GetLobbyState() { return lobbyState; }

    uint64_t GetLocalSteamId() {
        if (!steamInitialized) return 0;
        return SteamUser()->GetSteamID().ConvertToUint64();
    }

    void BroadcastLocalState(const PlayerState& state) {
        if (!steamInitialized || lobbyState != LobbyState::InLobby) return;

        WirePacket pkt = {1, state.x, state.y, state.z, state.yaw};
        int count = SteamMatchmaking()->GetNumLobbyMembers(currentLobby);
        CSteamID me = SteamUser()->GetSteamID();
        for (int i = 0; i < count; i++) {
            CSteamID member = SteamMatchmaking()->GetLobbyMemberByIndex(currentLobby, i);
            if (member == me) continue;
            SteamNetworkingIdentity id = {};
            id.SetSteamID(member);
            SteamNetworkingMessages()->SendMessageToUser(
                id, &pkt, sizeof(pkt),
                k_nSteamNetworkingSend_UnreliableNoDelay, CHANNEL_POSITION);
        }
    }

    bool GetRemoteState(PlayerState& out) {
        if (!haveRemote) return false;
        out = remoteState;
        return true;
    }

    bool HasRemotePeer() { return haveRemote; }

    bool IsHost() { return isHostFlag; }

    void BroadcastEnemySnapshot(const std::vector<EnemyNetState>& enemies) {
        if (!steamInitialized || lobbyState != LobbyState::InLobby) return;

        uint16_t count = (uint16_t)enemies.size();
        size_t sz = 2 + count * sizeof(WireEnemyEntry);
        std::vector<uint8_t> buf(sz);
        memcpy(buf.data(), &count, 2);
        WireEnemyEntry* entries = (WireEnemyEntry*)(buf.data() + 2);
        for (uint16_t i = 0; i < count; i++) {
            entries[i] = {enemies[i].netId, enemies[i].x, enemies[i].y, enemies[i].z,
                          enemies[i].hpCurrent, enemies[i].hpMax};
        }

        int memberCount = SteamMatchmaking()->GetNumLobbyMembers(currentLobby);
        CSteamID me = SteamUser()->GetSteamID();
        for (int i = 0; i < memberCount; i++) {
            CSteamID member = SteamMatchmaking()->GetLobbyMemberByIndex(currentLobby, i);
            if (member == me) continue;
            SteamNetworkingIdentity id = {};
            id.SetSteamID(member);
            SteamNetworkingMessages()->SendMessageToUser(
                id, buf.data(), (uint32_t)sz,
                k_nSteamNetworkingSend_UnreliableNoDelay, CHANNEL_ENEMIES);
        }
    }

    bool GetEnemySnapshot(std::vector<EnemyNetState>& out) {
        if (!newSnapshotReady) return false;
        out = latestSnapshot;
        newSnapshotReady = false;
        return true;
    }

    void SendDamageToHost(uint32_t netId, float damage) {
        if (!steamInitialized || lobbyState != LobbyState::InLobby || isHostFlag) return;

        WireDamage pkt = {3, netId, damage};
        int memberCount = SteamMatchmaking()->GetNumLobbyMembers(currentLobby);
        CSteamID me = SteamUser()->GetSteamID();
        for (int i = 0; i < memberCount; i++) {
            CSteamID member = SteamMatchmaking()->GetLobbyMemberByIndex(currentLobby, i);
            if (member == me) continue;
            SteamNetworkingIdentity id = {};
            id.SetSteamID(member);
            SteamNetworkingMessages()->SendMessageToUser(
                id, &pkt, sizeof(pkt),
                k_nSteamNetworkingSend_Reliable, CHANNEL_DAMAGE);
            break;  // Only send to host (first non-self member)
        }
    }

    void GetPendingDamage(std::vector<DamageEvent>& out) {
        out = pendingDamage;
        pendingDamage.clear();
    }
}
