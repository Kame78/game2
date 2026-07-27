#include "game/world/panel_build.hpp"
#include "engine/data/json.hpp"
#include "engine/math/noise.hpp"
#include "engine/terrain/chunk_manager.hpp"
#include "raymath.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace game::world::panel_build {

namespace {

using Piece = building_panels::Piece;
using Style = building_panels::Style;

constexpr float kEpsPos = 0.35f;
constexpr float kEpsYaw = 5.0f;
constexpr float kRemoveRadius = 3.5f;
constexpr const char* kDefaultPath = "assets/data/buildings_placed.json";

bool g_ready = false;
bool g_enabled = false;

Piece g_selPiece = Piece::Floor;
Style g_selStyle = Style::Stone;
float g_yawDeg = 0.0f;
bool  g_userYaw = false;
int   g_lastEdgeKey = 0x7fffffff;

std::vector<PlacedPiece> g_placed;

bool  g_ghostValid = false;
PlacedPiece g_ghost{};

bool isFloorFamily(Piece p) {
    return p == Piece::Floor || p == Piece::Stairs || p == Piece::RoofPyramid;
}
bool isRoofSlope(Piece p) { return p == Piece::RoofSlope; }
bool isWallFamily(Piece p) {
    return p == Piece::Wall || p == Piece::WallDoor || p == Piece::WallWindow ||
           p == Piece::Gable || p == Piece::GableRamp || p == Piece::WallRise;
}
bool isPillar(Piece p) { return p == Piece::Pillar; }

Vector3 roofRidgeDir(float yawDeg) {
    float r = yawDeg * DEG2RAD;
    return {std::sin(r), 0.0f, std::cos(r)};
}

float snapBayCenter(float v, float G) {
    return (std::floor(v / G) + 0.5f) * G;
}
float snapGridLine(float v, float G) {
    return std::round(v / G) * G;
}

float normalizeYaw(float y) {
    while (y < 0.0f) y += 360.0f;
    while (y >= 360.0f) y -= 360.0f;
    int q = (int)std::lround(y / 90.0f) & 3;
    return (float)q * 90.0f;
}

// Snap RoofSlope so tiles cascade eave→ridge (same yaw) or meet an opposite rake at the ridge.
bool trySnapRoofToNeighbor(const Vector3& hit, float aimY, PlacedPiece& out) {
    const float G = building_panels::Grid();
    const float rise = building_panels::RoofRise();
    const float attachR = G * 1.35f;

    const PlacedPiece* best = nullptr;
    float bestD = attachR;

    for (const PlacedPiece& p : g_placed) {
        if (p.piece != Piece::RoofSlope) continue;
        float dx = hit.x - p.pos.x;
        float dz = hit.z - p.pos.z;
        float d = std::sqrt(dx * dx + dz * dz);
        float dy = std::fabs(aimY - (p.pos.y + rise * 0.5f));
        float score = d + dy * 0.35f;
        if (score < bestD) {
            bestD = score;
            best = &p;
        }
    }
    if (!best) return false;

    Vector3 ridge = roofRidgeDir(best->yawDeg);
    Vector3 toHit = {hit.x - best->pos.x, 0.0f, hit.z - best->pos.z};
    float along = toHit.x * ridge.x + toHit.z * ridge.z;
    Vector3 side = {-ridge.z, 0.0f, ridge.x};
    float across = toHit.x * side.x + toHit.z * side.z;

    out.piece = Piece::RoofSlope;
    out.style = g_selStyle;

    if (along > G * 0.25f && std::fabs(across) < G * 0.75f) {
        out.pos = {best->pos.x + ridge.x * G, best->pos.y + rise, best->pos.z + ridge.z * G};
        out.yawDeg = normalizeYaw(best->yawDeg);
        if (!g_userYaw) g_yawDeg = out.yawDeg;
        return true;
    }
    if (along < -G * 0.25f && std::fabs(across) < G * 0.75f) {
        out.pos = {best->pos.x - ridge.x * G, best->pos.y - rise, best->pos.z - ridge.z * G};
        out.yawDeg = normalizeYaw(best->yawDeg);
        if (!g_userYaw) g_yawDeg = out.yawDeg;
        return true;
    }
    if (std::fabs(across) > G * 0.25f) {
        float s = (across > 0.0f) ? 1.0f : -1.0f;
        out.pos = {best->pos.x + side.x * s * G, best->pos.y, best->pos.z + side.z * s * G};
        out.yawDeg = normalizeYaw(best->yawDeg);
        if (!g_userYaw) g_yawDeg = out.yawDeg;
        return true;
    }
    if (along > G * 0.15f) {
        float oppYaw = normalizeYaw(best->yawDeg + 180.0f);
        Vector3 oppRidge = roofRidgeDir(oppYaw);
        Vector3 bestRidge = {
            best->pos.x + ridge.x * (G * 0.5f),
            best->pos.y + rise,
            best->pos.z + ridge.z * (G * 0.5f)};
        out.pos = {
            snapBayCenter(bestRidge.x + oppRidge.x * (G * 0.5f), G),
            best->pos.y,
            snapBayCenter(bestRidge.z + oppRidge.z * (G * 0.5f), G)};
        out.yawDeg = g_userYaw ? normalizeYaw(g_yawDeg) : oppYaw;
        if (!g_userYaw) g_yawDeg = out.yawDeg;
        return true;
    }
    return false;
}

float foundationY(float x, float z) {
    return engine::math::WorldHeight(x, z);
}

// Story index from aim height above foundation.
int storyFromHeight(float y, float foundation, float WH) {
    float rel = y - foundation;
    if (rel < WH * 0.35f) return 0;
    return std::max(0, (int)std::floor((rel + WH * 0.35f) / WH));
}

// True if wall sits on the perimeter of bay centered at (bx,bz).
bool wallBordersBay(const PlacedPiece& w, float bx, float bz, float G) {
    const float half = G * 0.5f;
    const float tol = 0.55f;
    // Vertical edge (constant X)
    if (std::fabs(std::fabs(w.pos.x - bx) - half) < tol && std::fabs(w.pos.z - bz) < half + tol)
        return true;
    // Horizontal edge (constant Z)
    if (std::fabs(std::fabs(w.pos.z - bz) - half) < tol && std::fabs(w.pos.x - bx) < half + tol)
        return true;
    return false;
}

float findStackY(float x, float z, float aimY, Piece placing) {
    const float G = building_panels::Grid();
    const float WH = building_panels::WallHeight();
    float base = foundationY(x, z);
    float highestFloor = base;
    float highestWallTop = base;

    for (const PlacedPiece& p : g_placed) {
        if (p.piece == Piece::Floor) {
            if (std::fabs(p.pos.x - x) <= kEpsPos && std::fabs(p.pos.z - z) <= kEpsPos)
                highestFloor = std::max(highestFloor, p.pos.y);
        }
        if (isWallFamily(p.piece) && wallBordersBay(p, x, z, G)) {
            float top = p.pos.y + WH;
            if (p.piece == Piece::WallRise || p.piece == Piece::Gable)
                top = p.pos.y + building_panels::RoofRise();
            else if (p.piece == Piece::GableRamp)
                top = p.pos.y + 2.0f * building_panels::RoofRise();
            highestWallTop = std::max(highestWallTop, top);
        }
        if (p.piece == Piece::RoofSlope) {
            // Roof tile occupies this bay or an adjacent bay — eave height and ridge height
            float dx = std::fabs(p.pos.x - x);
            float dz = std::fabs(p.pos.z - z);
            if (dx < G * 0.6f && dz < G * 0.6f) {
                highestFloor = std::max(highestFloor, p.pos.y);
                highestWallTop = std::max(highestWallTop, p.pos.y + building_panels::RoofRise());
            }
        }
        // Pillars at bay corners also support a roof level
        if (isPillar(p.piece)) {
            const float half = G * 0.5f;
            if (std::fabs(std::fabs(p.pos.x - x) - half) < 0.6f &&
                std::fabs(std::fabs(p.pos.z - z) - half) < 0.6f) {
                highestWallTop = std::max(highestWallTop, p.pos.y + WH);
            }
        }
    }

    int story = storyFromHeight(aimY, base, WH);
    float fromAim = base + (float)story * WH;

    // Prefer the highest support at or below where you're looking (with slack so
    // aiming near a wall top / second-story top snaps up onto it).
    float y = fromAim;
    if (aimY >= highestFloor - 0.75f)
        y = std::max(y, highestFloor);
    if (aimY >= highestWallTop - WH * 0.85f)
        y = std::max(y, highestWallTop);

    // Looking clearly above current tops → allow empty story from aim
    if (aimY > highestWallTop + 0.4f)
        y = std::max(y, fromAim);

    (void)placing;
    return y;
}

// Aim height at a bay column: closest point on the look ray to the vertical line (x,z).
float aimHeightAtColumn(const Camera3D& cam, float x, float z, float fallbackY) {
    Vector3 dir = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    // Ray: o + t d. Closest to line (x, y free, z):
    // Minimize (ox+t*dx - x)^2 + (oz+t*dz - z)^2
    float dx = dir.x, dz = dir.z;
    float denom = dx * dx + dz * dz;
    float t;
    if (denom < 1e-6f) {
        t = 8.0f;
    } else {
        t = ((x - cam.position.x) * dx + (z - cam.position.z) * dz) / denom;
        t = std::clamp(t, 1.0f, 120.0f);
    }
    return cam.position.y + dir.y * t;
}

Vector3 lookHitTerrain(const Camera3D& cam, float maxDist = 400.0f) {
    Vector3 dir = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    Vector3 best = Vector3Add(cam.position, Vector3Scale(dir, maxDist));
    float bestT = maxDist;

    auto considerPoint = [&](float t, Vector3 p) {
        if (t < 0.5f || t >= bestT) return;
        bestT = t;
        best = p;
    };

    // Piece AABB hits (full wall height — not just mid-sphere)
    const float G = building_panels::Grid();
    const float WH = building_panels::WallHeight();
    for (float t = 1.0f; t < maxDist; t += 0.35f) {
        Vector3 p = Vector3Add(cam.position, Vector3Scale(dir, t));
        for (const PlacedPiece& pl : g_placed) {
            float hx, hy0, hy1, hz;
            if (isWallFamily(pl.piece)) {
                hx = G * 0.55f;
                hz = G * 0.55f;
                // Thin along facing — approximate with square footprint for pick
                hy0 = pl.pos.y;
                hy1 = pl.pos.y + WH;
                if (p.x >= pl.pos.x - hx && p.x <= pl.pos.x + hx &&
                    p.z >= pl.pos.z - hz && p.z <= pl.pos.z + hz &&
                    p.y >= hy0 - 0.2f && p.y <= hy1 + 0.2f) {
                    considerPoint(t, p);
                }
            } else if (pl.piece == Piece::Floor || pl.piece == Piece::RoofSlope) {
                hx = G * 0.5f;
                hz = G * 0.5f;
                float y0 = pl.pos.y - 0.5f;
                float y1 = pl.pos.y + ((pl.piece == Piece::RoofSlope)
                                           ? building_panels::RoofRise() + 0.5f
                                           : 1.2f);
                if (p.x >= pl.pos.x - hx && p.x <= pl.pos.x + hx &&
                    p.z >= pl.pos.z - hz && p.z <= pl.pos.z + hz &&
                    p.y >= y0 && p.y <= y1) {
                    considerPoint(t, p);
                }
            } else if (pl.piece == Piece::WallRise) {
                hx = G * 0.55f;
                hz = G * 0.55f;
                float h = building_panels::RoofRise();
                if (p.x >= pl.pos.x - hx && p.x <= pl.pos.x + hx &&
                    p.z >= pl.pos.z - hz && p.z <= pl.pos.z + hz &&
                    p.y >= pl.pos.y - 0.2f && p.y <= pl.pos.y + h + 0.2f) {
                    considerPoint(t, p);
                }
            }
        }
        float ground = engine::math::WorldHeight(p.x, p.z);
        if (p.y <= ground + 0.5f) {
            considerPoint(t, {p.x, ground, p.z});
            break;
        }
    }
    return best;
}

void computeSnap(const Camera3D& cam, const Vector3& hit, Piece piece, PlacedPiece& out) {
    const float G = building_panels::Grid();
    const float WH = building_panels::WallHeight();
    out.piece = piece;
    out.style = g_selStyle;

    if (isRoofSlope(piece)) {
        float aimY = aimHeightAtColumn(cam, hit.x, hit.z, hit.y);
        if (hit.y > aimY) aimY = hit.y;
        if (trySnapRoofToNeighbor(hit, aimY, out)) return;

        // Free place: bay center on wall/floor support; yaw from user or look
        float x = snapBayCenter(hit.x, G);
        float z = snapBayCenter(hit.z, G);
        float y = findStackY(x, z, aimY, piece);
        out.pos = {x, y, z};
        if (!g_userYaw) {
            // Face uphill along look (ridge away from camera horizontally)
            Vector3 dir = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
            float yaw = std::atan2(dir.x, dir.z) * RAD2DEG;
            g_yawDeg = normalizeYaw(yaw);
        }
        out.yawDeg = normalizeYaw(g_yawDeg);
        return;
    }

    if (isPillar(piece)) {
        float x = snapGridLine(hit.x, G);
        float z = snapGridLine(hit.z, G);
        float fy = foundationY(x, z);
        float aimY = aimHeightAtColumn(cam, x, z, hit.y);
        float stack = findStackY(x, z, aimY, piece);
        int st = storyFromHeight(std::max(aimY, stack), fy, WH);
        out.pos = {x, fy + (float)st * WH, z};
        out.yawDeg = g_userYaw ? normalizeYaw(g_yawDeg) : 0.0f;
        return;
    }

    if (isWallFamily(piece)) {
        float lineX = snapGridLine(hit.x, G);
        float lineZ = snapGridLine(hit.z, G);
        float distX = std::fabs(hit.x - lineX);
        float distZ = std::fabs(hit.z - lineZ);

        float autoYaw = 0.0f;
        float x, z;
        int edgeKey;
        if (distX <= distZ) {
            x = lineX;
            z = snapBayCenter(hit.z, G);
            autoYaw = (hit.x >= lineX) ? 90.0f : -90.0f;
            edgeKey = (int)std::lround(lineX / G) * 100000 + (int)std::lround(z / G);
        } else {
            x = snapBayCenter(hit.x, G);
            z = lineZ;
            autoYaw = (hit.z >= lineZ) ? 0.0f : 180.0f;
            edgeKey = (int)std::lround(x / G) * 100000 + (int)std::lround(lineZ / G) + 50000;
        }

        if (edgeKey != g_lastEdgeKey) {
            g_lastEdgeKey = edgeKey;
            g_userYaw = false;
        }

        float fy = foundationY(x, z);
        float yawUse = g_userYaw ? normalizeYaw(g_yawDeg) : normalizeYaw(autoYaw);
        if (!g_userYaw) g_yawDeg = yawUse;
        float yawRad = yawUse * DEG2RAD;
        float inwardX = x + std::sin(yawRad) * (G * 0.5f);
        float inwardZ = z + std::cos(yawRad) * (G * 0.5f);
        float aimY = aimHeightAtColumn(cam, x, z, hit.y);
        for (const PlacedPiece& p : g_placed) {
            if (p.piece != Piece::Floor) continue;
            if (std::fabs(p.pos.x - inwardX) < kEpsPos && std::fabs(p.pos.z - inwardZ) < kEpsPos) {
                fy = p.pos.y;
                break;
            }
        }
        int story = storyFromHeight(aimY, fy, WH);
        // Climb onto existing wall of same edge (full walls or short WallRise stacks)
        for (const PlacedPiece& p : g_placed) {
            if (!isWallFamily(p.piece)) continue;
            if (std::fabs(p.pos.x - x) > kEpsPos || std::fabs(p.pos.z - z) > kEpsPos) continue;
            float ph = (p.piece == Piece::WallRise) ? building_panels::RoofRise() : WH;
            if (p.piece == Piece::Gable) ph = building_panels::RoofRise();
            if (p.piece == Piece::GableRamp) ph = 2.0f * building_panels::RoofRise();
            if (aimY >= p.pos.y + ph * 0.4f) {
                if (piece == Piece::WallRise || piece == Piece::Gable || piece == Piece::GableRamp) {
                    out.pos = {x, p.pos.y + ((p.piece == Piece::WallRise) ? building_panels::RoofRise()
                                          : (p.piece == Piece::Gable)     ? building_panels::RoofRise()
                                          : (p.piece == Piece::GableRamp) ? 2.0f * building_panels::RoofRise()
                                                                         : WH),
                               z};
                    out.yawDeg = yawUse;
                    return;
                }
                story = std::max(story, storyFromHeight(p.pos.y + ph + 0.1f, fy, WH));
            }
        }
        // WallRise sits in RoofRise increments above foundation / floor
        if (piece == Piece::WallRise) {
            float rise = building_panels::RoofRise();
            int step = std::max(0, (int)std::floor((aimY - fy + rise * 0.35f) / rise));
            out.pos = {x, fy + (float)step * rise, z};
            out.yawDeg = yawUse;
            return;
        }
        out.pos = {x, fy + (float)story * WH, z};
        out.yawDeg = yawUse;
        return;
    }

    // Floor / roof / stairs family
    float x = snapBayCenter(hit.x, G);
    float z = snapBayCenter(hit.z, G);
    float aimY = aimHeightAtColumn(cam, x, z, hit.y);
    // Prefer hit.y when the ray actually struck a high piece surface
    if (hit.y > aimY) aimY = hit.y;
    float y = findStackY(x, z, aimY, piece);
    out.pos = {x, y, z};
    out.yawDeg = normalizeYaw(g_yawDeg);
}

bool sameSlot(const PlacedPiece& a, const PlacedPiece& b) {
    if (a.piece != b.piece) return false;
    if (std::fabs(a.pos.x - b.pos.x) > kEpsPos) return false;
    if (std::fabs(a.pos.y - b.pos.y) > kEpsPos) return false;
    if (std::fabs(a.pos.z - b.pos.z) > kEpsPos) return false;
    float dy = std::fabs(normalizeYaw(a.yawDeg) - normalizeYaw(b.yawDeg));
    if (dy > 180.0f) dy = 360.0f - dy;
    return dy <= kEpsYaw;
}

void addFloorGrassExclusion(const PlacedPiece& p) {
    if (p.piece != Piece::Floor) return;
    const float G = building_panels::Grid();
    const float pad = 0.4f;
    const float half = G * 0.5f;
    engine::terrain::chunks::AddGrassExclusionRect(
        p.pos.x - half - pad, p.pos.z - half - pad,
        p.pos.x + half + pad, p.pos.z + half + pad);
}

void rebuildGrassExclusionsFromFloors() {
    // Church footprint re-registered in InitBuildingPanels; append floor bays.
    for (const PlacedPiece& p : g_placed) {
        addFloorGrassExclusion(p);
    }
}

}  // namespace

const char* PieceName(Piece p) {
    switch (p) {
    case Piece::Floor: return "Floor";
    case Piece::Wall: return "Wall";
    case Piece::WallDoor: return "WallDoor";
    case Piece::WallWindow: return "WallWindow";
    case Piece::Pillar: return "Pillar";
    case Piece::RoofSlope: return "RoofSlope";
    case Piece::Gable: return "Gable";
    case Piece::GableRamp: return "GableRamp";
    case Piece::WallRise: return "WallRise";
    case Piece::RoofPyramid: return "RoofPyramid";
    case Piece::Stairs: return "Stairs";
    default: return "Floor";
    }
}

const char* StyleName(Style s) {
    switch (s) {
    case Style::Wood: return "Wood";
    case Style::RoofDark: return "RoofDark";
    case Style::Stone:
    default: return "Stone";
    }
}

Piece PieceFromName(const char* name) {
    if (!name) return Piece::Floor;
    for (int i = 0; i < (int)Piece::Count; ++i) {
        if (std::strcmp(name, PieceName((Piece)i)) == 0) return (Piece)i;
    }
    return Piece::Floor;
}

Style StyleFromName(const char* name) {
    if (!name) return Style::Stone;
    if (std::strcmp(name, "Wood") == 0) return Style::Wood;
    if (std::strcmp(name, "RoofDark") == 0) return Style::RoofDark;
    return Style::Stone;
}

void Init() {
    if (g_ready) return;
    g_ready = true;
    g_enabled = false;
    g_placed.clear();
    g_ghostValid = false;
    Load(kDefaultPath);
    rebuildGrassExclusionsFromFloors();
}

void Shutdown() {
    if (!g_ready) return;
    if (!g_placed.empty()) Save(kDefaultPath);
    g_placed.clear();
    g_ghostValid = false;
    g_enabled = false;
    g_ready = false;
}

void SetEnabled(bool enabled) { g_enabled = enabled && g_ready; }
bool IsEnabled() { return g_enabled; }

void SetSelectedPiece(Piece piece) {
    g_selPiece = piece;
    g_userYaw = false;
}
Piece GetSelectedPiece() { return g_selPiece; }

void SetSelectedStyle(Style style) { g_selStyle = style; }
Style GetSelectedStyle() { return g_selStyle; }

void RotateYaw(int steps) {
    g_yawDeg = normalizeYaw(g_yawDeg + (float)steps * 90.0f);
    g_userYaw = true;
}
float GetYawDeg() { return normalizeYaw(g_yawDeg); }

void Update(const Camera3D& cam) {
    g_ghostValid = false;
    if (!g_enabled || !g_ready) return;

    if (IsKeyPressed(KEY_Q)) RotateYaw(-1);
    if (IsKeyPressed(KEY_E)) RotateYaw(+1);

    Vector3 hit = lookHitTerrain(cam);
    computeSnap(cam, hit, g_selPiece, g_ghost);
    g_ghost.style = g_selStyle;
    g_ghostValid = true;
}

bool TryPlace() {
    if (!g_enabled || !g_ghostValid) return false;
    for (const PlacedPiece& p : g_placed) {
        if (sameSlot(p, g_ghost)) return false;
    }
    g_placed.push_back(g_ghost);
    addFloorGrassExclusion(g_ghost);
    return true;
}

bool TryRemove() {
    if (!g_enabled || !g_ready) return false;
    // Use ghost position as aim center when valid, else fail
    Vector3 aim = g_ghostValid ? g_ghost.pos : Vector3{0, 0, 0};
    if (!g_ghostValid) return false;

    int best = -1;
    float bestD = kRemoveRadius;
    for (int i = 0; i < (int)g_placed.size(); ++i) {
        float d = Vector3Distance(g_placed[i].pos, aim);
        // Prefer matching selected piece type slightly
        if (g_placed[i].piece == g_selPiece) d *= 0.85f;
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    if (best < 0) return false;
    g_placed.erase(g_placed.begin() + best);
    return true;
}

void ClearAll() {
    g_placed.clear();
    g_ghostValid = false;
}

void Draw() {
    if (!g_ready) return;
    for (const PlacedPiece& p : g_placed) {
        building_panels::Draw(p.piece, p.pos, p.yawDeg, p.style);
    }
    if (g_enabled && g_ghostValid) {
        BeginBlendMode(BLEND_ALPHA);
        building_panels::Draw(g_ghost.piece, g_ghost.pos, g_ghost.yawDeg, g_ghost.style,
                              Color{120, 255, 160, 140});
        EndBlendMode();
        // Snap marker
        DrawCubeWires(g_ghost.pos, 0.4f, 0.4f, 0.4f, Color{80, 255, 120, 255});
    }
}

bool Load(const char* path) {
    if (!path) path = kDefaultPath;
    engine::data::Json root = engine::data::LoadJsonFile(path);
    if (!root.IsObject()) return false;
    const engine::data::Json& arr = root.Get("pieces");
    if (!arr.IsArray()) return false;

    g_placed.clear();
    g_placed.reserve(arr.Size());
    for (size_t i = 0; i < arr.Size(); ++i) {
        const engine::data::Json& e = arr.At(i);
        if (!e.IsObject()) continue;
        PlacedPiece p;
        p.piece = PieceFromName(e.Str("piece", "Floor"));
        p.style = StyleFromName(e.Str("style", "Stone"));
        p.pos.x = e.Float("x", 0.0f);
        p.pos.y = e.Float("y", 0.0f);
        p.pos.z = e.Float("z", 0.0f);
        p.yawDeg = normalizeYaw(e.Float("yaw", 0.0f));
        g_placed.push_back(p);
        addFloorGrassExclusion(p);
    }
    return true;
}

bool Save(const char* path) {
    if (!path) path = kDefaultPath;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "{\n  \"pieces\": [\n";
    for (size_t i = 0; i < g_placed.size(); ++i) {
        const PlacedPiece& p = g_placed[i];
        out << "    { \"piece\": \"" << PieceName(p.piece) << "\", \"style\": \""
            << StyleName(p.style) << "\", \"x\": " << p.pos.x << ", \"y\": " << p.pos.y
            << ", \"z\": " << p.pos.z << ", \"yaw\": " << p.yawDeg << " }";
        if (i + 1 < g_placed.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n}\n";
    return (bool)out;
}

int PlacedCount() { return (int)g_placed.size(); }
const PlacedPiece* PlacedData() { return g_placed.empty() ? nullptr : g_placed.data(); }

}  // namespace game::world::panel_build
