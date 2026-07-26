#include "game/dungeon/dungeon.hpp"
#include "engine/math/noise.hpp"
#include <algorithm>
#include <unordered_map>

namespace game::dungeon {

namespace {

    struct Rng {
        uint64_t state;

        explicit Rng(uint64_t seed) : state(engine::math::splitmix64(seed ^ 0x5DEECE66DULL)) {}

        uint64_t next() {
            state = engine::math::splitmix64(state);
            return state;
        }
        float next01() { return engine::math::randFloat01(next()); }
        int   nextInt(int n) { return (n <= 0) ? 0 : (int)(next() % (uint64_t)n); }
    };

    constexpr int kDirX[4] = { 1, -1, 0,  0 };
    constexpr int kDirZ[4] = { 0,  0, 1, -1 };

    inline int64_t cellKey(int x, int z) {
        return ((int64_t)x << 32) ^ (uint32_t)z;
    }

    // Boss rooms need headroom for large tiers; extract/haven stay compact.
    void applyFootprint(Room& r) {
        switch (r.type) {
            case RoomType::Boss:      r.halfW = r.halfD = 20.0f; break;
            case RoomType::Entrance:  r.halfW = r.halfD = 12.0f; break;
            case RoomType::Extract:   r.halfW = r.halfD = 12.0f; break;
            case RoomType::SafeHaven: r.halfW = r.halfD = 12.0f; break;
            case RoomType::Treasure:  r.halfW = r.halfD = 11.0f; break;
            case RoomType::Secret:    r.halfW = r.halfD = 10.0f; break;
            case RoomType::Vault:     r.halfW = r.halfD = 11.0f; break;
            default:                  r.halfW = r.halfD = 14.0f; break;
        }
    }

    Transition makeLink(const Room& a, const Room& b, float cell) {
        Transition t;
        t.fromRoom = a.id;
        t.toRoom   = b.id;
        t.alongX   = (a.cellZ == b.cellZ);

        t.center.x = (a.center.x + b.center.x) * 0.5f;
        t.center.y = a.center.y;
        t.center.z = (a.center.z + b.center.z) * 0.5f;

        // Corridor spans the gap between the two room walls.
        const float gap = t.alongX ? (cell - a.halfW - b.halfW)
                                   : (cell - a.halfD - b.halfD);
        t.halfLen   = std::max(2.0f, gap * 0.5f);
        t.halfWidth = 3.0f;
        return t;
    }

}  // namespace

Layout Generate(uint32_t seed, const GenProfile& profile) {
    Layout layout;
    layout.seed = seed;

    Rng rng(seed);

    const int pathLen = profile.minPathRooms +
                        rng.nextInt(std::max(1, profile.maxPathRooms - profile.minPathRooms + 1));

    std::unordered_map<int64_t, int> occupied;  // cell -> room id

    // --- Critical path: random walk on a grid, never revisiting a cell ---
    std::vector<Room> path;
    int cx = 0, cz = 0;
    int lastDir = -1;

    for (int i = 0; i < pathLen; ++i) {
        Room r;
        r.id    = (int)path.size();
        r.cellX = cx;
        r.cellZ = cz;
        r.onCriticalPath = true;
        path.push_back(r);
        occupied[cellKey(cx, cz)] = r.id;

        if (i == pathLen - 1) break;

        // Prefer continuing forward, but allow turns; never step onto a used cell.
        int order[4] = {0, 1, 2, 3};
        for (int k = 3; k > 0; --k) std::swap(order[k], order[rng.nextInt(k + 1)]);

        int chosen = -1;
        for (int k = 0; k < 4; ++k) {
            const int d = order[k];
            // Avoid immediate backtracking into the previous cell.
            if (lastDir >= 0 && kDirX[d] == -kDirX[lastDir] && kDirZ[d] == -kDirZ[lastDir]) continue;
            const int nx = cx + kDirX[d];
            const int nz = cz + kDirZ[d];
            if (occupied.count(cellKey(nx, nz))) continue;
            chosen = d;
            break;
        }
        if (chosen < 0) break;  // boxed in — shorter dungeon, still valid

        lastDir = chosen;
        cx += kDirX[chosen];
        cz += kDirZ[chosen];
    }

    const int n = (int)path.size();

    // --- Assign room types along the path ---
    path[0].type = RoomType::Entrance;
    if (n >= 2) path[n - 1].type = RoomType::Extract;
    if (n >= 3) path[n - 2].type = RoomType::Boss;

    const int firstMid = 1;
    const int lastMid  = n - 3;  // inclusive
    for (int i = firstMid; i <= lastMid; ++i) path[i].type = RoomType::Combat;

    if (lastMid >= firstMid) {
        const int midCount = lastMid - firstMid + 1;
        // One elite pack and one safe haven in the middle stretch.
        const int elite = firstMid + rng.nextInt(midCount);
        path[elite].type = RoomType::Elite;
        if (midCount >= 3) {
            int haven = firstMid + rng.nextInt(midCount);
            for (int guard = 0; guard < 4 && haven == elite; ++guard) {
                haven = firstMid + rng.nextInt(midCount);
            }
            if (haven != elite) path[haven].type = RoomType::SafeHaven;
        }
    }

    layout.rooms = path;
    for (auto& r : layout.rooms) applyFootprint(r);

    // --- Side branches off the critical path ---
    // Optional content lives here: treasure, hidden rooms, and key-locked vaults.
    const int branchTarget = 1 + rng.nextInt(std::max(1, profile.maxBranches));
    bool secretPlaced = false;
    bool vaultPlaced  = false;

    for (int b = 0; b < branchTarget && n > 2; ++b) {
        const int hostIdx = 1 + rng.nextInt(std::max(1, n - 2));  // not entrance / extract
        const Room host = layout.rooms[hostIdx];

        int order[4] = {0, 1, 2, 3};
        for (int k = 3; k > 0; --k) std::swap(order[k], order[rng.nextInt(k + 1)]);

        for (int k = 0; k < 4; ++k) {
            const int nx = host.cellX + kDirX[order[k]];
            const int nz = host.cellZ + kDirZ[order[k]];
            if (occupied.count(cellKey(nx, nz))) continue;

            Room r;
            r.id    = (int)layout.rooms.size();
            r.cellX = nx;
            r.cellZ = nz;
            r.onCriticalPath = false;

            if (!secretPlaced && rng.next01() < profile.secretChance) {
                r.type = RoomType::Secret;
                secretPlaced = true;
            } else if (!vaultPlaced && rng.next01() < profile.vaultChance) {
                r.type = RoomType::Vault;
                vaultPlaced = true;
            } else {
                r.type = (rng.next01() < 0.5f) ? RoomType::Treasure : RoomType::Combat;
            }

            applyFootprint(r);
            layout.rooms.push_back(r);
            occupied[cellKey(nx, nz)] = r.id;
            break;
        }
    }

    // --- World placement ---
    for (auto& r : layout.rooms) {
        r.center.x = (float)r.cellX * profile.cell;
        r.center.y = 0.0f;
        r.center.z = (float)r.cellZ * profile.cell;
    }

    layout.entranceRoom = 0;
    for (const auto& r : layout.rooms) {
        if (r.type == RoomType::Boss)    layout.bossRoom    = r.id;
        if (r.type == RoomType::Extract) layout.extractRoom = r.id;
    }

    // --- Links: critical path chain, then branch spurs ---
    for (int i = 0; i + 1 < n; ++i) {
        Transition t = makeLink(layout.rooms[i], layout.rooms[i + 1], profile.cell);
        // The way into the extract room stays sealed until the boss is dead.
        if (layout.rooms[i + 1].type == RoomType::Extract && layout.bossRoom >= 0) {
            t.type     = TransitionType::LockedGate;
            t.locked   = true;
            t.requires_ = GateRequirement::BossDead;
        } else if (layout.rooms[i + 1].type == RoomType::Boss) {
            t.type = TransitionType::Door;  // framed threshold into the arena
        }
        layout.links.push_back(t);
    }

    for (int i = n; i < (int)layout.rooms.size(); ++i) {
        const Room& branch = layout.rooms[i];
        // Branches attach to whichever neighbouring cell is on the path.
        for (int d = 0; d < 4; ++d) {
            const auto it = occupied.find(cellKey(branch.cellX + kDirX[d], branch.cellZ + kDirZ[d]));
            if (it == occupied.end()) continue;
            const Room& host = layout.rooms[it->second];
            if (!host.onCriticalPath) continue;

            Transition t = makeLink(host, branch, profile.cell);
            if (branch.type == RoomType::Secret) {
                t.type      = TransitionType::HiddenPassage;
                t.locked    = true;
                t.requires_ = GateRequirement::Search;
            } else if (branch.type == RoomType::Vault) {
                t.type      = TransitionType::LockedGate;
                t.locked    = true;
                t.requires_ = GateRequirement::Key;
            }
            layout.links.push_back(t);
            break;
        }
    }

    // The vault key sits in a combat room on the critical path, so the optional
    // reward always costs a fight.
    if (vaultPlaced) {
        std::vector<int> candidates;
        for (int i = 1; i < n; ++i) {
            if (layout.rooms[i].type == RoomType::Combat ||
                layout.rooms[i].type == RoomType::Elite) {
                candidates.push_back(i);
            }
        }
        if (!candidates.empty()) {
            layout.keyRoom = candidates[rng.nextInt((int)candidates.size())];
        } else {
            // No fight rooms generated — drop the lock rather than soft-lock the vault.
            for (auto& link : layout.links) {
                if (link.requires_ == GateRequirement::Key) {
                    link.locked    = false;
                    link.requires_ = GateRequirement::None;
                    link.type      = TransitionType::Corridor;
                }
            }
        }
    }

    return layout;
}

}  // namespace game::dungeon
