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
    static constexpr int CHANNEL_FIREBALL = 3;
    static constexpr int CHANNEL_DUNGEON = 4;

    #pragma pack(push, 1)
    struct WirePacket {
        uint8_t type;      // 1 = position update
        float x, y, z, yaw, pitch;
        uint8_t swinging;  // 0 or 1
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
    struct WireFireball {
        uint8_t type;      // 4 = fireball spawn
        float x, y, z;
        float dirX, dirY, dirZ;
        float speed;
    };
    struct WireDungeon {
        uint8_t  type;   // 5 = dungeon sync
        uint8_t  op;     // DungeonOp
        uint32_t seed;
        uint8_t  theme;
        uint8_t  stage;
        uint8_t  pad;
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
    static std::vector<RemoteFireball> pendingFireballs;
    static std::vector<DungeonSyncMsg> pendingDungeon;

    // Username + lobby browser state
    static std::string g_username;
    static std::vector<LobbyInfo> g_lobbyList;
    static bool g_lobbyListReady = false;
    static bool g_lobbyListRefreshing = false;

    // Steam lobby-data key used to filter lobbies to this game only.
    static constexpr const char* GAME_TAG_KEY = "game_tag";
    static constexpr const char* GAME_TAG_VAL = "game2_apocalypse";
    static constexpr const char* HOST_NAME_KEY = "host_name";

    // --- Callback listener class ---
    class Listener {
    public:
        Listener()
            : lobbyCreatedCallback(this, &Listener::OnLobbyCreated),
              lobbyEnterCallback(this, &Listener::OnLobbyEnter),
              gameLobbyJoinRequestedCallback(this, &Listener::OnGameLobbyJoinRequested),
              sessionRequestCallback(this, &Listener::OnSessionRequest),
              lobbyChatUpdate(this, &Listener::OnLobbyChatUpdate),
              lobbyMatchListCallback(this, &Listener::OnLobbyMatchList) {}

        void OnLobbyCreated(LobbyCreated_t* result) {
            if (result->m_eResult == k_EResultOK) {
                currentLobby = CSteamID(result->m_ulSteamIDLobby);
                lobbyState = LobbyState::InLobby;
                printf("[net] Lobby created: %llu\n", result->m_ulSteamIDLobby);
                SteamMatchmaking()->SetLobbyJoinable(currentLobby, true);
                // Tag the lobby so the browser can filter to our game.
                SteamMatchmaking()->SetLobbyData(currentLobby, GAME_TAG_KEY, GAME_TAG_VAL);
                SteamMatchmaking()->SetLobbyData(currentLobby, HOST_NAME_KEY,
                    g_username.empty() ? "Unknown" : g_username.c_str());
            } else {
                printf("[net] Lobby create failed: %d\n", result->m_eResult);
                lobbyState = LobbyState::None;
                isHostFlag = false;
            }
        }

        void OnLobbyMatchList(LobbyMatchList_t* result) {
            g_lobbyList.clear();
            int total = static_cast<int>(result->m_nLobbiesMatching);
            if (total > 32) total = 32;
            for (int i = 0; i < total; ++i) {
                CSteamID lobbyId = SteamMatchmaking()->GetLobbyByIndex(i);
                LobbyInfo info;
                info.id          = lobbyId.ConvertToUint64();
                const char* name = SteamMatchmaking()->GetLobbyData(lobbyId, HOST_NAME_KEY);
                info.hostName    = (name && *name) ? name : "Unknown";
                info.playerCount = SteamMatchmaking()->GetNumLobbyMembers(lobbyId);
                info.maxPlayers  = SteamMatchmaking()->GetLobbyMemberLimit(lobbyId);
                g_lobbyList.push_back(std::move(info));
            }
            g_lobbyListReady = true;
            g_lobbyListRefreshing = false;
            printf("[net] Lobby list refreshed: %d lobbies\n", (int)g_lobbyList.size());
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
                WirePacket hello = {1, 0, 0, 0, 0, 0, 0};
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
        CCallback<Listener, LobbyMatchList_t> lobbyMatchListCallback;
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
                    remoteState.pitch = pkt->pitch;
                    remoteState.swinging = pkt->swinging != 0;
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

        // Receive fireball spawns from peers
        SteamNetworkingMessage_t* fMsgs[8];
        int fReceived = SteamNetworkingMessages()->ReceiveMessagesOnChannel(CHANNEL_FIREBALL, fMsgs, 8);
        for (int i = 0; i < fReceived; i++) {
            if (fMsgs[i]->GetSize() == sizeof(WireFireball)) {
                WireFireball* f = (WireFireball*)fMsgs[i]->GetData();
                if (f->type == 4) {
                    pendingFireballs.push_back({f->x, f->y, f->z, f->dirX, f->dirY, f->dirZ, f->speed});
                }
            }
            fMsgs[i]->Release();
        }

        // Receive dungeon session sync (clients apply host authority)
        SteamNetworkingMessage_t* dnMsgs[8];
        int dnReceived = SteamNetworkingMessages()->ReceiveMessagesOnChannel(CHANNEL_DUNGEON, dnMsgs, 8);
        for (int i = 0; i < dnReceived; i++) {
            if (dnMsgs[i]->GetSize() == sizeof(WireDungeon)) {
                WireDungeon* d = (WireDungeon*)dnMsgs[i]->GetData();
                if (d->type == 5) {
                    DungeonSyncMsg msg;
                    msg.op    = static_cast<DungeonOp>(d->op);
                    msg.seed  = d->seed;
                    msg.theme = d->theme;
                    msg.stage = d->stage;
                    pendingDungeon.push_back(msg);
                }
            }
            dnMsgs[i]->Release();
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

        WirePacket pkt = {1, state.x, state.y, state.z, state.yaw, state.pitch, (uint8_t)(state.swinging ? 1 : 0)};
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

    // -------- Username --------
    void SetUsername(const std::string& name) {
        g_username = name;
        // If we already have a lobby, update its host_name key too.
        if (steamInitialized && lobbyState == LobbyState::InLobby && isHostFlag) {
            SteamMatchmaking()->SetLobbyData(currentLobby, HOST_NAME_KEY,
                g_username.empty() ? "Unknown" : g_username.c_str());
        }
    }

    const std::string& GetUsername() { return g_username; }

    std::string GetSteamPersonaName() {
        if (!steamInitialized) return "";
        const char* n = SteamFriends()->GetPersonaName();
        return n ? std::string(n) : "";
    }

    // -------- Lobby browser --------
    void RefreshLobbyList() {
        if (!steamInitialized) return;
        g_lobbyList.clear();
        g_lobbyListReady = false;
        g_lobbyListRefreshing = true;
        // Filter to our game only (other Spacewar-appid users won't pollute the list).
        SteamMatchmaking()->AddRequestLobbyListStringFilter(
            GAME_TAG_KEY, GAME_TAG_VAL, k_ELobbyComparisonEqual);
        SteamMatchmaking()->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
        SteamMatchmaking()->RequestLobbyList();
    }

    bool IsLobbyListRefreshing() { return g_lobbyListRefreshing; }
    bool IsLobbyListReady() { return g_lobbyListReady; }
    const std::vector<LobbyInfo>& GetLobbyList() { return g_lobbyList; }

    void JoinLobbyById(uint64_t lobbyId) {
        if (!steamInitialized) return;
        if (lobbyState == LobbyState::InLobby) return;  // already in one
        lobbyState = LobbyState::Joining;
        isHostFlag = false;
        SteamMatchmaking()->JoinLobby(CSteamID(static_cast<uint64>(lobbyId)));
    }

    void SetLobbyData(const std::string& key, const std::string& value) {
        if (!steamInitialized || lobbyState != LobbyState::InLobby) return;
        SteamMatchmaking()->SetLobbyData(currentLobby, key.c_str(), value.c_str());
    }

    std::string GetLobbyData(const std::string& key) {
        if (!steamInitialized || lobbyState != LobbyState::InLobby) return "";
        const char* v = SteamMatchmaking()->GetLobbyData(currentLobby, key.c_str());
        return v ? std::string(v) : "";
    }

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

    void BroadcastFireball(float x, float y, float z, float dirX, float dirY, float dirZ, float speed) {
        if (!steamInitialized || lobbyState != LobbyState::InLobby) return;

        WireFireball pkt = {4, x, y, z, dirX, dirY, dirZ, speed};
        int memberCount = SteamMatchmaking()->GetNumLobbyMembers(currentLobby);
        CSteamID me = SteamUser()->GetSteamID();
        for (int i = 0; i < memberCount; i++) {
            CSteamID member = SteamMatchmaking()->GetLobbyMemberByIndex(currentLobby, i);
            if (member == me) continue;
            SteamNetworkingIdentity id = {};
            id.SetSteamID(member);
            SteamNetworkingMessages()->SendMessageToUser(
                id, &pkt, sizeof(pkt),
                k_nSteamNetworkingSend_Reliable, CHANNEL_FIREBALL);
        }
    }

    bool GetRemoteFireballs(std::vector<RemoteFireball>& out) {
        if (pendingFireballs.empty()) return false;
        out = pendingFireballs;
        pendingFireballs.clear();
        return true;
    }

    void BroadcastDungeonSync(const DungeonSyncMsg& msg) {
        if (!steamInitialized || lobbyState != LobbyState::InLobby || !isHostFlag) return;

        WireDungeon pkt = {};
        pkt.type  = 5;
        pkt.op    = static_cast<uint8_t>(msg.op);
        pkt.seed  = msg.seed;
        pkt.theme = msg.theme;
        pkt.stage = msg.stage;
        pkt.pad   = 0;

        int memberCount = SteamMatchmaking()->GetNumLobbyMembers(currentLobby);
        CSteamID me = SteamUser()->GetSteamID();
        for (int i = 0; i < memberCount; i++) {
            CSteamID member = SteamMatchmaking()->GetLobbyMemberByIndex(currentLobby, i);
            if (member == me) continue;
            SteamNetworkingIdentity id = {};
            id.SetSteamID(member);
            SteamNetworkingMessages()->SendMessageToUser(
                id, &pkt, sizeof(pkt),
                k_nSteamNetworkingSend_Reliable, CHANNEL_DUNGEON);
        }
    }

    bool GetPendingDungeonSync(DungeonSyncMsg& out) {
        if (pendingDungeon.empty()) return false;
        out = pendingDungeon.front();
        pendingDungeon.erase(pendingDungeon.begin());
        return true;
    }
}
