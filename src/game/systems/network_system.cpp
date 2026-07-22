#include "game/systems.hpp"
#include "game/factories/entity_factory.hpp"
#include "engine/networking.hpp"
#include "raymath.h"
#include <vector>

namespace game::systems {

    static int syncTickCounter = 0;

    void NetworkSyncSystem(engine::ecs::Registry& reg) {
        if (engine::networking::GetLobbyState() != engine::networking::LobbyState::InLobby) return;
        if (!engine::networking::HasRemotePeer()) return;

        if (engine::networking::IsHost()) {
            // HOST: broadcast enemy snapshot every 3 frames
            syncTickCounter++;
            if (syncTickCounter >= 3) {
                syncTickCounter = 0;
                std::vector<engine::networking::EnemyNetState> snapshot;
                for (size_t i = 0; i < reg.enemyAIs.data.size(); i++) {
                    engine::ecs::Entity e = {reg.enemyAIs.indexToEntity[i]};
                    if (!reg.transforms.Has(e) || !reg.healths.Has(e)) continue;
                    auto& t = reg.transforms.Get(e);
                    auto& hp = reg.healths.Get(e);
                    engine::networking::EnemyNetState s;
                    s.netId = reg.enemyAIs.data[i].netId;
                    s.x = t.position.x;
                    s.y = t.position.y;
                    s.z = t.position.z;
                    s.hpCurrent = hp.current;
                    s.hpMax = hp.max;
                    snapshot.push_back(s);
                }
                engine::networking::BroadcastEnemySnapshot(snapshot);
            }

            // HOST: apply damage from remote clients
            std::vector<engine::networking::DamageEvent> dmg;
            engine::networking::GetPendingDamage(dmg);
            for (auto& d : dmg) {
                for (size_t i = 0; i < reg.enemyAIs.data.size(); i++) {
                    if (reg.enemyAIs.data[i].netId == d.netId) {
                        engine::ecs::Entity e = {reg.enemyAIs.indexToEntity[i]};
                        if (reg.healths.Has(e)) {
                            reg.healths.Get(e).current -= d.damage;
                        }
                    }
                }
            }
        } else {
            // CLIENT: receive enemy snapshot from host
            std::vector<engine::networking::EnemyNetState> snapshot;
            if (engine::networking::GetEnemySnapshot(snapshot)) {
                for (auto& s : snapshot) {
                    bool found = false;
                    for (size_t i = 0; i < reg.enemyAIs.data.size(); i++) {
                        if (reg.enemyAIs.data[i].netId == s.netId) {
                            engine::ecs::Entity e = {reg.enemyAIs.indexToEntity[i]};
                            if (reg.transforms.Has(e) && reg.healths.Has(e)) {
                                reg.transforms.Get(e).position = {s.x, s.y, s.z};
                                reg.healths.Get(e).current = s.hpCurrent;
                            }
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        factories::EntityFactory::CreateEnemy(reg, {s.x, s.y, s.z}, s.netId);
                    }
                }
            }
        }
    }

    void DrawRemotePlayer(const engine::networking::PlayerState& remote) {
        Vector3 pos = {remote.x, remote.y - 1.0f, remote.z};
        DrawCube(pos, 1.0f, 2.0f, 1.0f, PURPLE);
        DrawCubeWires(pos, 1.0f, 2.0f, 1.0f, BLACK);
    }

    void SpawnRemoteFireballs(engine::ecs::Registry& reg) {
        // Placeholder for remote fireball network synchronization if needed
    }

}
