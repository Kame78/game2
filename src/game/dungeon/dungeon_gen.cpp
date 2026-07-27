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
    SizeTier pickSizeTier(RoomType type, Rng& rng, const GenProfile& profile) {
        switch (type) {
            case RoomType::Boss:
                return SizeTier::Large;
            case RoomType::Combat:
            case RoomType::Elite:
                return (rng.next01() < profile.largeCombatChance) ? SizeTier::Large
                                                                   : SizeTier::Medium;
            case RoomType::Entrance:
            case RoomType::Extract:
            case RoomType::SafeHaven:
                return (rng.next01() < 0.55f) ? SizeTier::Small : SizeTier::Medium;
            case RoomType::Treasure:
            case RoomType::Secret:
            case RoomType::Vault:
                return (rng.next01() < 0.65f) ? SizeTier::Small : SizeTier::Medium;
            default:
                return SizeTier::Medium;
        }
    }

    float spanForTier(SizeTier tier, RoomType type, Rng& rng, const GenProfile& profile) {
        float lo = profile.mediumSpanMin;
        float hi = profile.mediumSpanMax;
        if (type == RoomType::Boss) {
            lo = profile.bossSpanMin;
            hi = profile.bossSpanMax;
        } else if (tier == SizeTier::Small) {
            lo = profile.smallSpanMin;
            hi = profile.smallSpanMax;
        } else if (tier == SizeTier::Large) {
            lo = profile.largeSpanMin;
            hi = profile.largeSpanMax;
        }
        return lo + rng.next01() * std::max(0.0f, hi - lo);
    }

    void applyFootprint(Room& r, Rng& rng, const GenProfile& profile) {
        r.size = pickSizeTier(r.type, rng, profile);
        const float span = spanForTier(r.size, r.type, rng, profile);
        // Slight aspect variation so halls don't all read as perfect squares.
        const float aspect = 0.85f + rng.next01() * 0.3f;
        r.halfW = (span * 0.5f) * aspect;
        r.halfD = (span * 0.5f) / aspect;
        // Long combat halls bias one axis.
        if ((r.type == RoomType::Combat || r.type == RoomType::Elite) && rng.next01() < 0.3f) {
            if (rng.next01() < 0.5f) r.halfW *= 1.25f;
            else                     r.halfD *= 1.25f;
        }
        // Keep corridor gaps positive against GenProfile.cell.
        const float maxHalf = profile.cell * 0.5f - 8.0f;
        r.halfW = std::min(r.halfW, maxHalf);
        r.halfD = std::min(r.halfD, maxHalf);
        r.shape = RoomShape::Rect;
        r.shapeVariant = 0;
    }

    FloorStyle pickFloor(RoomType type, Rng& rng) {
        switch (type) {
            case RoomType::SafeHaven:
            case RoomType::Secret:
                return (rng.next01() < 0.65f) ? FloorStyle::Mosaic : FloorStyle::Checker;
            case RoomType::Elite:
            case RoomType::Boss:
                return (rng.next01() < 0.7f) ? FloorStyle::BloodRing : FloorStyle::Cracked;
            case RoomType::Combat:
                if (rng.next01() < 0.45f) return FloorStyle::Cracked;
                if (rng.next01() < 0.35f) return FloorStyle::Flooded;
                return FloorStyle::Checker;
            case RoomType::Extract:
                return FloorStyle::Mosaic;
            case RoomType::Treasure:
            case RoomType::Vault:
                return (rng.next01() < 0.5f) ? FloorStyle::Mosaic : FloorStyle::Checker;
            default:
                return FloorStyle::Checker;
        }
    }

    CorridorStyle pickCorridorStyle(const Room& a, const Room& b, Rng& rng) {
        if (a.type == RoomType::Boss || b.type == RoomType::Boss) return CorridorStyle::Choke;
        if (a.type == RoomType::Extract || b.type == RoomType::Extract) return CorridorStyle::Standard;
        const float u = rng.next01();
        if (u < 0.22f) return CorridorStyle::Ruined;
        if (u < 0.40f) return CorridorStyle::Choke;
        if (u < 0.58f) return CorridorStyle::Flooded;
        return CorridorStyle::Standard;
    }

    Transition makeLink(const Room& a, const Room& b, float cell, CorridorStyle style) {
        Transition t;
        t.fromRoom = a.id;
        t.toRoom   = b.id;
        t.alongX   = (a.cellZ == b.cellZ);
        t.style    = style;

        // Center on the actual gap between facing AABB edges — NOT the midpoint of
        // room centers. Unequal room sizes previously left a black seam on the
        // larger room's side (corridor was shifted toward the smaller room).
        (void)cell;
        if (t.alongX) {
            float x0, x1;
            if (a.center.x <= b.center.x) {
                x0 = a.center.x + a.halfW;
                x1 = b.center.x - b.halfW;
            } else {
                x0 = b.center.x + b.halfW;
                x1 = a.center.x - a.halfW;
            }
            t.center.x = (x0 + x1) * 0.5f;
            t.center.z = (a.center.z + b.center.z) * 0.5f;
            t.halfLen  = std::max(2.0f, (x1 - x0) * 0.5f);
        } else {
            float z0, z1;
            if (a.center.z <= b.center.z) {
                z0 = a.center.z + a.halfD;
                z1 = b.center.z - b.halfD;
            } else {
                z0 = b.center.z + b.halfD;
                z1 = a.center.z - a.halfD;
            }
            t.center.z = (z0 + z1) * 0.5f;
            t.center.x = (a.center.x + b.center.x) * 0.5f;
            t.halfLen  = std::max(2.0f, (z1 - z0) * 0.5f);
        }
        t.center.y = a.center.y;

        switch (style) {
            case CorridorStyle::Choke:   t.halfWidth = 1.85f; break;
            case CorridorStyle::Ruined:  t.halfWidth = 3.4f;  break;
            case CorridorStyle::Flooded: t.halfWidth = 3.1f;  break;
            default:                     t.halfWidth = 3.0f;  break;
        }
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
    for (auto& r : layout.rooms) applyFootprint(r, rng, profile);

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

            applyFootprint(r, rng, profile);
            layout.rooms.push_back(r);
            occupied[cellKey(nx, nz)] = r.id;
            break;
        }
    }

    // --- Floors (silhouettes come from theme masks at stamp time) ---
    for (auto& r : layout.rooms) {
        r.floor = pickFloor(r.type, rng);
        r.shape = RoomShape::Rect;
        r.shapeVariant = 0;
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
        const CorridorStyle style = pickCorridorStyle(layout.rooms[i], layout.rooms[i + 1], rng);
        Transition t = makeLink(layout.rooms[i], layout.rooms[i + 1], profile.cell, style);
        // The way into the extract room stays sealed until the boss is dead.
        if (layout.rooms[i + 1].type == RoomType::Extract && layout.bossRoom >= 0) {
            t.type      = TransitionType::LockedGate;
            t.locked    = true;
            t.requires_ = GateRequirement::BossDead;
            t.style     = CorridorStyle::Standard;
            t.halfWidth = 3.0f;
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

            const CorridorStyle style = pickCorridorStyle(host, branch, rng);
            Transition t = makeLink(host, branch, profile.cell, style);
            if (branch.type == RoomType::Secret) {
                t.type      = TransitionType::HiddenPassage;
                t.locked    = true;
                t.requires_ = GateRequirement::Search;
                t.style     = CorridorStyle::Standard;
            } else if (branch.type == RoomType::Vault) {
                t.type      = TransitionType::LockedGate;
                t.locked    = true;
                t.requires_ = GateRequirement::Key;
                t.style     = CorridorStyle::Standard;
                t.halfWidth = 3.0f;
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
