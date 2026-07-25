#include "engine/math/hydrology.hpp"
#include "engine/math/noise.hpp"
#include "raylib.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <vector>

namespace engine::math {

namespace {

constexpr float kMinLakeSpacing = 900.0f;
constexpr int   kMaxLakes       = 10;
// Soft lake disc — keep in sync with noise.cpp kWaterBodyCoreR (200) + ShoreW (100)
constexpr float kLakeBodyOuterR = 300.0f;
constexpr float kRiverStep      = 32.0f;  // denser polyline → fewer broken gaps
constexpr int   kMaxRiverSteps  = 85;     // reach lakes with smaller steps
constexpr int   kMaxRivers      = 36;

constexpr float kHashCell       = 256.0f;
constexpr int   kHashDim        = 28; // covers ±3584 m

std::vector<LakeSite>  g_lakes;
std::vector<RiverPath> g_rivers;
bool                   g_ready = false;

struct ExclusionZone {
    float x, z, radius;
};
std::vector<ExclusionZone> g_exclusions;
std::vector<HydrologyExclusion> g_exclusionsPublic; // mirrored for const API


// Flattened river segments for fast carve queries
struct RiverSeg {
    float ax, az, bx, bz;
    float halfW;
};
std::vector<RiverSeg> g_riverSegs;

// Spatial hash: cell → feature indices
std::vector<int> g_lakeHash[kHashDim * kHashDim];
std::vector<int> g_segHash[kHashDim * kHashDim];

float smoothstep(float edge0, float edge1, float x) {
    float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float dist2(float ax, float az, float bx, float bz) {
    const float dx = ax - bx;
    const float dz = az - bz;
    return dx * dx + dz * dz;
}

int hashIndex(float x, float z) {
    const float half = kHashDim * kHashCell * 0.5f;
    int cx = static_cast<int>((x + half) / kHashCell);
    int cz = static_cast<int>((z + half) / kHashCell);
    cx = std::clamp(cx, 0, kHashDim - 1);
    cz = std::clamp(cz, 0, kHashDim - 1);
    return cz * kHashDim + cx;
}

void hashInsertRange(std::vector<int>* buckets, int id, float x, float z, float radius) {
    const float half = kHashDim * kHashCell * 0.5f;
    int minX = static_cast<int>((x - radius + half) / kHashCell);
    int maxX = static_cast<int>((x + radius + half) / kHashCell);
    int minZ = static_cast<int>((z - radius + half) / kHashCell);
    int maxZ = static_cast<int>((z + radius + half) / kHashCell);
    minX = std::clamp(minX, 0, kHashDim - 1);
    maxX = std::clamp(maxX, 0, kHashDim - 1);
    minZ = std::clamp(minZ, 0, kHashDim - 1);
    maxZ = std::clamp(maxZ, 0, kHashDim - 1);
    for (int cz = minZ; cz <= maxZ; ++cz) {
        for (int cx = minX; cx <= maxX; ++cx) {
            buckets[cz * kHashDim + cx].push_back(id);
        }
    }
}

void clearSpatialHash() {
    for (int i = 0; i < kHashDim * kHashDim; ++i) {
        g_lakeHash[i].clear();
        g_segHash[i].clear();
    }
}

void buildSpatialHash() {
    clearSpatialHash();
    for (int i = 0; i < static_cast<int>(g_lakes.size()); ++i) {
        const LakeSite& lake = g_lakes[i];
        hashInsertRange(g_lakeHash, i, lake.x, lake.z, lake.boundR * 1.45f + 16.0f);
    }
    for (int i = 0; i < static_cast<int>(g_riverSegs.size()); ++i) {
        const RiverSeg& s = g_riverSegs[i];
        const float mx = (s.ax + s.bx) * 0.5f;
        const float mz = (s.az + s.bz) * 0.5f;
        const float rad = 0.5f * std::sqrt(dist2(s.ax, s.az, s.bx, s.bz)) + s.halfW * 3.2f;
        hashInsertRange(g_segHash, i, mx, mz, rad);
    }
}

float planHeight(float x, float z) {
    return LandSurfaceHeight(x, z);
}

bool hitsExclusion(float x, float z, float pad) {
    for (const ExclusionZone& ex : g_exclusions) {
        const float r = ex.radius + pad;
        if (dist2(x, z, ex.x, ex.z) < r * r) return true;
    }
    return false;
}

bool lakeHitsExclusion(const LakeSite& lake) {
    // Reject if the lake disc would overlap a protected pad (spawn, cities, etc.)
    for (const ExclusionZone& ex : g_exclusions) {
        const float r = ex.radius + lake.boundR * 0.92f;
        if (dist2(lake.x, lake.z, ex.x, ex.z) < r * r) return true;
    }
    return false;
}

float distToSegment(float px, float pz, float ax, float az, float bx, float bz) {
    const float abx = bx - ax;
    const float abz = bz - az;
    const float apx = px - ax;
    const float apz = pz - az;
    const float ab2 = abx * abx + abz * abz;
    const float t = (ab2 > 1.0e-6f) ? std::clamp((apx * abx + apz * abz) / ab2, 0.0f, 1.0f) : 0.0f;
    const float cx = ax + abx * t;
    const float cz = az + abz * t;
    return std::sqrt(dist2(px, pz, cx, cz));
}

float lakeCoverageLocal(const LakeSite& lake, float x, float z) {
    float dx = x - lake.x;
    float dz = z - lake.z;
    const float c = std::cos(lake.angle);
    const float s = std::sin(lake.angle);
    const float lx = dx * c + dz * s;
    const float lz = -dx * s + dz * c;
    const float nx = lx / std::max(lake.radiusA, 1.0f);
    const float nz = lz / std::max(lake.radiusB, 1.0f);
    const float r = std::sqrt(nx * nx + nz * nz);
    if (r < 1.0e-5f) return 0.0f;

    const float ang = std::atan2(nz, nx);
    // Multi-harmonic shoreline warp (cheap, deterministic)
    float warp = 1.0f
        + lake.warpAmp * std::sin(ang * lake.warpFreq + lake.phase)
        + lake.warpAmp * 0.55f * std::sin(ang * (lake.warpFreq * 1.7f) + lake.phase * 1.3f)
        + lake.warpAmp * 0.25f * std::sin(lx * 0.031f + lake.phase) * std::cos(lz * 0.027f);
    warp = std::clamp(warp, 0.55f, 1.55f);
    return r / warp;
}

void snapToDepression(LakeSite& lake) {
    // Walk downhill so the carved bowl sits in a natural low.
    float bx = lake.x;
    float bz = lake.z;
    float bh = planHeight(bx, bz);
    static const float kDirs[8][2] = {
        {1, 0}, {-1, 0}, {0, 1}, {0, -1},
        {0.7071f, 0.7071f}, {-0.7071f, 0.7071f},
        {0.7071f, -0.7071f}, {-0.7071f, -0.7071f},
    };
    for (int step = 0; step < 10; ++step) {
        float bestX = bx;
        float bestZ = bz;
        float bestH = bh;
        for (const auto& d : kDirs) {
            for (float dist : {40.0f, 80.0f, 130.0f}) {
                const float tx = bx + d[0] * dist;
                const float tz = bz + d[1] * dist;
                if (MountainMask(tx, tz) > 0.55f) continue;
                const float th = planHeight(tx, tz);
                if (th + 0.25f < bestH) {
                    bestH = th;
                    bestX = tx;
                    bestZ = tz;
                }
            }
        }
        if (bestX == bx && bestZ == bz) break;
        bx = bestX;
        bz = bestZ;
        bh = bestH;
    }
    lake.x = bx;
    lake.z = bz;
}

float sampleRimHeight(const LakeSite& lake) {
    float sum = 0.0f;
    float lo = 1.0e30f;
    constexpr int kSamples = 8;
    const float r = std::max(lake.radiusA, lake.radiusB) * 0.92f;
    for (int i = 0; i < kSamples; ++i) {
        const float ang = static_cast<float>(i) * (6.2831853f / kSamples);
        const float h = planHeight(lake.x + std::cos(ang) * r, lake.z + std::sin(ang) * r);
        sum += h;
        lo = std::min(lo, h);
    }
    // Bias toward lower shores so water doesn't flood high banks
    const float avg = sum / static_cast<float>(kSamples);
    return avg * 0.65f + lo * 0.35f;
}

// Dry-land shelf around a lake disc — used once to set the water table.
// Sample just outside the soft dig fringe so heights are undug shore, not bed.
float sampleDryShelfHeight(float cx, float cz, float boundR) {
    float sum = 0.0f;
    float lo = 1.0e30f;
    int n = 0;
    constexpr int kSamples = 8;
    const float r0 = boundR + 40.0f;
    for (int i = 0; i < kSamples; ++i) {
        const float ang = static_cast<float>(i) * (6.2831853f / static_cast<float>(kSamples));
        const float ca = std::cos(ang);
        const float sa = std::sin(ang);
        for (float extra = 0.0f; extra <= 240.0f + 1.0e-3f; extra += 80.0f) {
            const float x = cx + ca * (r0 + extra);
            const float z = cz + sa * (r0 + extra);
            const WorldRegion reg = PrimaryRegion(x, z);
            // Voronoi Water cells are large; dig is only the disc — don't skip shore.
            if (reg == WorldRegion::Mountains) continue;
            if (MountainMask(x, z) > 0.35f) continue;
            const float h = LandSurfaceHeight(x, z);
            sum += h;
            lo = std::min(lo, h);
            ++n;
            break;
        }
    }
    if (n <= 0) return 14.0f; // ~land shelf fallback
    const float avg = sum / static_cast<float>(n);
    return avg * 0.60f + lo * 0.40f; // bias toward lower shores
}

bool tooCloseToExisting(float x, float z, float minDist) {
    for (const LakeSite& o : g_lakes) {
        const float need = minDist + o.boundR * 0.25f;
        if (dist2(x, z, o.x, o.z) < need * need) return true;
    }
    return false;
}

void finalizeLakeLevels(LakeSite& lake) {
    lake.boundR = std::max(lake.radiusA, lake.radiusB) * 1.55f;
    const float floorH = planHeight(lake.x, lake.z);
    const float rimH = sampleRimHeight(lake);
    const float relief = std::max(0.0f, rimH - floorH);

    constexpr float kShoreMargin = 1.35f;
    float surface = rimH - kShoreMargin;

    const float basinCap = floorH + std::max(relief * 0.55f, relief - kShoreMargin);
    surface = std::min(surface, basinCap);
    surface = std::min(surface, rimH - 0.85f);
    surface = std::max(surface, floorH + 0.85f);

    lake.surfaceY = surface;
    // Modest dig depth — extreme bowls left empty water and sheer walls
    lake.depth = std::clamp(relief * 0.55f + 3.0f + lake.depth * 0.25f, 3.5f, 9.0f);
}

bool riverHitsExclusion(const RiverPath& path) {
    const float pad = path.halfWidth + 24.0f;
    for (const Vector2& p : path.points) {
        if (hitsExclusion(p.x, p.y, pad)) return true;
    }
    return false;
}

float sampleBasinRoughness(const LakeSite& lake) {
    // Max−min height across basin samples — high = alpine/uneven, reject
    float lo = 1.0e30f;
    float hi = -1.0e30f;
    const float r = std::max(lake.radiusA, lake.radiusB) * 0.85f;
    for (int i = 0; i < 16; ++i) {
        const float ang = (6.2831853f * static_cast<float>(i)) / 16.0f;
        const float h = planHeight(lake.x + std::cos(ang) * r, lake.z + std::sin(ang) * r);
        lo = std::min(lo, h);
        hi = std::max(hi, h);
    }
    lo = std::min(lo, planHeight(lake.x, lake.z));
    hi = std::max(hi, planHeight(lake.x, lake.z));
    return hi - lo;
}

void tryAddVoronoiLake(LakeSite lake) {
    const float half = WorldConfig::WORLD_HALF_EXTENT - 200.0f;
    if (lake.x < -half || lake.x > half || lake.z < -half || lake.z > half) return;

    // Stay clear of N/S mountain approach (arced front, not raw |z|)
    if (NsAlpineDepth(lake.x, lake.z) > WorldConfig::WORLD_HALF_EXTENT - 1250.0f) return;

    lake.boundR = std::max(lake.radiusA, lake.radiusB) * 1.40f;

    // Only require the *center* stay off landmark pads. Full disc vs boundR was
    // rejecting every km-scale lake (church 420 + boundR ~1400 ≈ 1.7 km kill radius).
    if (hitsExclusion(lake.x, lake.z, 160.0f)) return;

    if (MountainMask(lake.x, lake.z) > 0.25f) return;
    const RegionWeights reg = SampleRegion(lake.x, lake.z);
    if (reg.primary == WorldRegion::Mountains) return;

    const float floorH = planHeight(lake.x, lake.z);
    if (floorH > 55.0f) return;

    // Authored basin — carve supplies the bowl; depression fill sets final surfaceY
    lake.surfaceY = floorH + 2.4f;
    lake.depth = std::clamp(lake.depth, 5.5f, 10.0f);

    if (tooCloseToExisting(lake.x, lake.z, kMinLakeSpacing + lake.boundR * 0.15f)) return;

    g_lakes.push_back(lake);
}

void tryAddLake(LakeSite lake) {
    const float half = WorldConfig::WORLD_HALF_EXTENT - 150.0f;
    if (lake.x < -half || lake.x > half || lake.z < -half || lake.z > half) return;

    snapToDepression(lake);

    // Reject before finalize so boundR is still a good disc estimate
    lake.boundR = std::max(lake.radiusA, lake.radiusB) * 1.55f;
    if (hitsExclusion(lake.x, lake.z, 40.0f)) return;
    if (lakeHitsExclusion(lake)) return;

    // Region layout: no lakes in mountain belts; prefer wetlands / plains
    const RegionWeights reg = SampleRegion(lake.x, lake.z);
    if (reg.mountains > 0.35f) return;
    if (reg.primary == WorldRegion::Mountains) return;
    if (reg.wetlands < 0.12f && reg.plains < 0.35f && Moisture(lake.x, lake.z) < 0.40f) return;

    // Keep lakes in low/rolling land — high alpine bowls look broken on uneven slopes
    if (MountainMask(lake.x, lake.z) > 0.22f) return;
    const float floorH = planHeight(lake.x, lake.z);
    if (floorH > 52.0f) return; // ~40m above land shelf
    if (Moisture(lake.x, lake.z) < 0.20f) return;
    if (sampleBasinRoughness(lake) > 18.0f) return; // too uneven across the footprint

    // Require a real depression (rim above floor) — not a flat stamp
    const float rimH = sampleRimHeight(lake);
    if (rimH < floorH + 2.0f) return;
    if (rimH - floorH > 22.0f) return; // cliff-walled hole, not a pond

    finalizeLakeLevels(lake);

    // Surface must sit clearly below the rim after capping
    if (lake.surfaceY > rimH - 0.75f) return;
    if (rimH - lake.surfaceY < 0.6f) return;

    if (tooCloseToExisting(lake.x, lake.z, kMinLakeSpacing + lake.boundR * 0.25f)) return;
    if (lakeHitsExclusion(lake)) return;

    g_lakes.push_back(lake);
}

RiverPath smoothRiverPath(RiverPath path) {
    // One Chaikin pass — cuts sharp 45° kinks without densifying too much
    if (path.points.size() < 4) return path;
    std::vector<Vector2> out;
    out.reserve(path.points.size() * 2);
    out.push_back(path.points.front());
    for (size_t i = 0; i + 1 < path.points.size(); ++i) {
        const Vector2& a = path.points[i];
        const Vector2& b = path.points[i + 1];
        out.push_back(Vector2{a.x * 0.75f + b.x * 0.25f, a.y * 0.75f + b.y * 0.25f});
        out.push_back(Vector2{a.x * 0.25f + b.x * 0.75f, a.y * 0.25f + b.y * 0.75f});
    }
    out.push_back(path.points.back());
    path.points = std::move(out);
    return path;
}

RiverPath traceRiver(float sx, float sz, const LakeSite& target, float halfWidth) {
    RiverPath path;
    path.halfWidth = halfWidth;
    path.points.push_back(Vector2{sx, sz});

    float x = sx;
    float z = sz;
    const float goalR = std::min(target.radiusA, target.radiusB) * 0.75f;
    const float goalR2 = goalR * goalR;
    const float nearR2 = (goalR * 2.4f) * (goalR * 2.4f);

    static const float kDir[8][2] = {
        {1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, -1.0f},
        {0.7071f, 0.7071f}, {-0.7071f, 0.7071f},
        {0.7071f, -0.7071f}, {-0.7071f, -0.7071f},
    };

    for (int step = 0; step < kMaxRiverSteps; ++step) {
        if (dist2(x, z, target.x, target.z) <= goalR2) {
            path.points.push_back(Vector2{target.x, target.z});
            break;
        }

        float toLakeX = target.x - x;
        float toLakeZ = target.z - z;
        float toLen = std::sqrt(toLakeX * toLakeX + toLakeZ * toLakeZ);
        if (toLen > 1.0f) {
            toLakeX /= toLen;
            toLakeZ /= toLen;
        }

        float bestX = x;
        float bestZ = z;
        float bestScore = planHeight(x, z);
        bool moved = false;

        for (const auto& d : kDir) {
            const float nx = x + d[0] * kRiverStep;
            const float nz = z + d[1] * kRiverStep;
            const float nh = planHeight(nx, nz);
            const float closer = toLakeX * d[0] + toLakeZ * d[1];
            // Stronger pull toward the lake so paths stay continuous into the basin
            const float score = nh - closer * 2.4f;
            if (!moved || score < bestScore) {
                bestScore = score;
                bestX = nx;
                bestZ = nz;
                moved = true;
            }
        }

        if (!moved || (bestX == x && bestZ == z)) {
            bestX = x + toLakeX * kRiverStep;
            bestZ = z + toLakeZ * kRiverStep;
        }

        if (dist2(bestX, bestZ, x, z) < 1.0f) break;
        x = bestX;
        z = bestZ;
        path.points.push_back(Vector2{x, z});
    }

    // If we got close but didn't enter the rim, snap the last point into the lake
    if (!path.points.empty()) {
        const Vector2& last = path.points.back();
        const float d2 = dist2(last.x, last.y, target.x, target.z);
        if (d2 > goalR2 && d2 < nearR2) {
            path.points.push_back(Vector2{target.x, target.z});
        }
    }

    if (path.points.size() < 3) {
        path.points.clear();
        return path;
    }
    return smoothRiverPath(std::move(path));
}

void buildRiverSegs() {
    g_riverSegs.clear();
    for (const RiverPath& river : g_rivers) {
        if (river.points.size() < 2) continue;
        // Decimate: keep every other point for carve (still dense enough)
        for (size_t i = 1; i < river.points.size(); i += 1) {
            const Vector2& a = river.points[i - 1];
            const Vector2& b = river.points[i];
            // Skip tiny segments
            if (dist2(a.x, a.y, b.x, b.y) < 4.0f) continue;
            g_riverSegs.push_back(RiverSeg{a.x, a.y, b.x, b.y, river.halfWidth});
        }
    }
}

int g_riverCount = 0;

bool tryPushRiver(RiverPath&& path) {
    if (path.points.size() < 3) return false;
    if (g_riverCount >= kMaxRivers) return false;
    if (riverHitsExclusion(path)) return false;
    g_rivers.push_back(std::move(path));
    ++g_riverCount;
    return true;
}

void buildFeederRivers(uint64_t seed) {
    // Feeders: start on higher ground around each lake, flow downhill into the basin.
    for (size_t li = 0; li < g_lakes.size(); ++li) {
        const LakeSite& lake = g_lakes[li];
        const uint64_t lh = hash2D(seed ^ 0xA11CEULL, static_cast<int>(li), 7);
        int streams = 1;
        if (randFloat01(lh) > 0.35f) streams = 2;
        if (lake.boundR > 260.0f && randFloat01(splitmix64(lh)) > 0.4f) streams = 3;

        const float lakeH = planHeight(lake.x, lake.z);

        for (int s = 0; s < streams; ++s) {
            if (g_riverCount >= kMaxRivers) return;

            const uint64_t sh = hash2D(seed ^ 0xBEEFULL, static_cast<int>(li), s + 1);
            bool placed = false;

            for (int attempt = 0; attempt < 4 && !placed; ++attempt) {
                const uint64_t ah = splitmix64(sh ^ static_cast<uint64_t>(attempt * 97));
                const float ang = randFloat01(ah) * 6.2831853f;
                const float dist = lake.boundR + 180.0f + randFloat01(splitmix64(ah)) * 850.0f;
                const float sx = lake.x + std::cos(ang) * dist;
                const float sz = lake.z + std::sin(ang) * dist;

                float bestX = sx;
                float bestZ = sz;
                float bestH = planHeight(sx, sz);
                for (int t = 0; t < 8; ++t) {
                    const uint64_t th = hash2D(ah, t, 3);
                    const float jitter = 70.0f + randFloat01(th) * 200.0f;
                    const float ja = randFloat01(splitmix64(th)) * 6.2831853f;
                    const float tx = sx + std::cos(ja) * jitter;
                    const float tz = sz + std::sin(ja) * jitter;
                    const float tht = planHeight(tx, tz);
                    if (tht > bestH && MountainMask(tx, tz) < 0.78f) {
                        bestH = tht;
                        bestX = tx;
                        bestZ = tz;
                    }
                }

                // Must start clearly above the lake bed surface (pre-carve land).
                if (bestH < lakeH + 2.0f) continue;
                if (hitsExclusion(bestX, bestZ, 30.0f)) continue;

                // ~2× former creek widths so ribbons read as rivers
                const float halfW = 8.0f + randFloat01(splitmix64(ah ^ 33ULL)) * 8.0f;
                placed = tryPushRiver(traceRiver(bestX, bestZ, lake, halfW));
            }
        }
    }
}

void buildInterLakeLinks(uint64_t seed) {
    // Optional short channels between nearby lakes — higher basin drains toward lower.
    for (size_t i = 0; i < g_lakes.size(); ++i) {
        if (g_riverCount >= kMaxRivers) return;

        int bestJ = -1;
        float bestD2 = 1200.0f * 1200.0f;
        for (size_t j = i + 1; j < g_lakes.size(); ++j) {
            const float d2 = dist2(g_lakes[i].x, g_lakes[i].z, g_lakes[j].x, g_lakes[j].z);
            const float minSep = (g_lakes[i].boundR + g_lakes[j].boundR) * 0.9f;
            if (d2 < bestD2 && d2 > minSep * minSep) {
                bestD2 = d2;
                bestJ = static_cast<int>(j);
            }
        }
        if (bestJ < 0) continue;

        const uint64_t lh = hash2D(seed ^ 0xC0FFEEULL, static_cast<int>(i), bestJ);
        if (randFloat01(lh) < 0.45f) continue; // not every pair

        const LakeSite& a = g_lakes[i];
        const LakeSite& b = g_lakes[bestJ];
        const float ha = planHeight(a.x, a.z);
        const float hb = planHeight(b.x, b.z);
        if (std::fabs(ha - hb) < 1.0f) continue;

        const LakeSite& src = (ha > hb) ? a : b;
        const LakeSite& dst = (ha > hb) ? b : a;

        float dx = dst.x - src.x;
        float dz = dst.z - src.z;
        float len = std::sqrt(dx * dx + dz * dz);
        if (len < 1.0f) continue;
        dx /= len;
        dz /= len;

        // Leave the source near its rim, flow into the destination basin.
        const float sx = src.x + dx * (std::min(src.radiusA, src.radiusB) * 0.95f);
        const float sz = src.z + dz * (std::min(src.radiusA, src.radiusB) * 0.95f);
        const float halfW = 9.0f + randFloat01(splitmix64(lh)) * 6.0f;
        tryPushRiver(traceRiver(sx, sz, dst, halfW));
    }
}

void buildRivers(uint64_t seed) {
    g_rivers.clear();
    g_riverCount = 0;
    buildFeederRivers(seed);
    buildInterLakeLinks(seed);
}

// Generation-time "fluid": pour water at the lake seed until it spills.
// Sets surfaceY to the spill elevation and fillRadius to the flooded extent.
void depressionFillLake(LakeSite& lake) {
    const float searchR = std::max(lake.boundR * 1.75f, std::max(lake.radiusA, lake.radiusB) * 2.1f);
    const float cell = std::clamp(searchR * 0.045f, 5.0f, 10.0f);
    int n = static_cast<int>(std::ceil((2.0f * searchR) / cell));
    n = std::clamp(n, 20, 128); // allow finer grids for km-scale Voronoi lakes

    const float step = (2.0f * searchR) / static_cast<float>(n);
    const float x0 = lake.x - searchR;
    const float z0 = lake.z - searchR;
    const int stride = n + 1;
    const int vertCount = stride * stride;

    std::vector<float> h(static_cast<size_t>(vertCount));
    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) {
            const float x = x0 + static_cast<float>(i) * step;
            const float z = z0 + static_cast<float>(j) * step;
            h[static_cast<size_t>(j * stride + i)] = WorldHeight(x, z);
        }
    }

    const int ci = std::clamp(static_cast<int>((lake.x - x0) / step), 1, n - 1);
    const int cj = std::clamp(static_cast<int>((lake.z - z0) / step), 1, n - 1);
    const float seedH = h[static_cast<size_t>(cj * stride + ci)];

    // Rising water: raise level to successive rim notches until spill/domain edge
    float level = seedH + 0.35f;
    float spill = level;
    float maxWetR = 0.0f;
    std::vector<uint8_t> wet(static_cast<size_t>(n * n), 0);

    auto floodBelow = [&](float waterLevel, bool* escaped) {
        std::fill(wet.begin(), wet.end(), 0);
        *escaped = false;
        maxWetR = 0.0f;
        std::queue<int> q;
        auto tryPush = [&](int i, int j) {
            if (i < 0 || j < 0 || i >= n || j >= n) return;
            const int idx = j * n + i;
            if (wet[static_cast<size_t>(idx)]) return;
            // Cell center height ≈ average of corners
            const float h00 = h[static_cast<size_t>(j * stride + i)];
            const float h10 = h[static_cast<size_t>(j * stride + i + 1)];
            const float h01 = h[static_cast<size_t>((j + 1) * stride + i)];
            const float h11 = h[static_cast<size_t>((j + 1) * stride + i + 1)];
            const float cellH = 0.25f * (h00 + h10 + h01 + h11);
            if (cellH >= waterLevel) return;
            wet[static_cast<size_t>(idx)] = 1;
            q.push(idx);
            const float cx = x0 + (static_cast<float>(i) + 0.5f) * step;
            const float cz = z0 + (static_cast<float>(j) + 0.5f) * step;
            const float dx = cx - lake.x;
            const float dz = cz - lake.z;
            maxWetR = std::max(maxWetR, std::sqrt(dx * dx + dz * dz));
            if (i == 0 || j == 0 || i == n - 1 || j == n - 1) *escaped = true;
        };

        // Seed around center
        for (int dj = -1; dj <= 1; ++dj)
            for (int di = -1; di <= 1; ++di)
                tryPush(ci + di, cj + dj);

        while (!q.empty()) {
            const int idx = q.front();
            q.pop();
            const int i = idx % n;
            const int j = idx / n;
            tryPush(i + 1, j);
            tryPush(i - 1, j);
            tryPush(i, j + 1);
            tryPush(i, j - 1);
            tryPush(i + 1, j + 1);
            tryPush(i - 1, j + 1);
            tryPush(i + 1, j - 1);
            tryPush(i - 1, j - 1);
        }
    };

    auto minRimHeight = [&](float waterLevel) -> float {
        float rim = 1.0e30f;
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                if (!wet[static_cast<size_t>(j * n + i)]) continue;
                // Look at dry neighbors — their height is a rim candidate
                const int nbr[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (int k = 0; k < 4; ++k) {
                    const int ni = i + nbr[k][0];
                    const int nj = j + nbr[k][1];
                    if (ni < 0 || nj < 0 || ni >= n || nj >= n) continue;
                    if (wet[static_cast<size_t>(nj * n + ni)]) continue;
                    const float h00 = h[static_cast<size_t>(nj * stride + ni)];
                    const float h10 = h[static_cast<size_t>(nj * stride + ni + 1)];
                    const float h01 = h[static_cast<size_t>((nj + 1) * stride + ni)];
                    const float h11 = h[static_cast<size_t>((nj + 1) * stride + ni + 1)];
                    const float cellH = 0.25f * (h00 + h10 + h01 + h11);
                    if (cellH >= waterLevel) rim = std::min(rim, cellH);
                }
            }
        }
        return rim;
    };

    bool escaped = false;
    for (int iter = 0; iter < 48; ++iter) {
        floodBelow(level, &escaped);
        if (escaped) {
            spill = level;
            break;
        }
        const float rim = minRimHeight(level);
        if (rim > 1.0e29f) {
            spill = level;
            break;
        }
        // Next notch — water rises to the lowest rim
        if (rim <= level + 0.05f) {
            spill = rim;
            break;
        }
        // Don't flood the whole search domain into a plains sheet
        if (maxWetR > searchR * 0.92f) {
            spill = level;
            break;
        }
        level = rim;
        spill = rim;
    }

    // Final flood: fill almost to the spill so the basin isn't a deep empty hole
    float waterY = spill - 0.08f;
    waterY = std::max(waterY, seedH + 0.75f);
    floodBelow(waterY + 0.02f, &escaped);

    lake.surfaceY = waterY;
    lake.fillRadius = std::max(maxWetR + cell * 2.0f, std::max(lake.radiusA, lake.radiusB) * 0.85f);
    lake.boundR = std::max(lake.boundR, lake.fillRadius * 1.05f);
    // Cap reported depth so the next carve pass stays shallow/blendable
    lake.depth = std::clamp(waterY - seedH, 2.5f, 8.5f);
}

void simulateLakeDepressionFills() {
    // Carve uses surfaceY; iterating lets the berm and pool settle together
    for (int pass = 0; pass < 2; ++pass) {
        for (LakeSite& lake : g_lakes) {
            depressionFillLake(lake);
        }
        // Rebuild hash pads after boundR / fillRadius updates
        if (pass == 0) {
            clearSpatialHash();
            buildSpatialHash();
        }
    }
    TraceLog(LOG_INFO, "HYDROLOGY: depression-fill simulated for %d lakes",
             static_cast<int>(g_lakes.size()));
}

}  // namespace

void ClearHydrology() {
    g_lakes.clear();
    g_rivers.clear();
    g_riverSegs.clear();
    clearSpatialHash();
    g_ready = false;
}

void ClearHydrologyExclusions() {
    g_exclusions.clear();
    g_exclusionsPublic.clear();
}

void AddHydrologyExclusion(float x, float z, float radius) {
    if (radius <= 0.0f) return;
    g_exclusions.push_back(ExclusionZone{x, z, radius});
    g_exclusionsPublic.push_back(HydrologyExclusion{x, z, radius});
}

const std::vector<HydrologyExclusion>& GetHydrologyExclusions() {
    return g_exclusionsPublic;
}

bool IsHydrologyReady() { return g_ready; }

const std::vector<LakeSite>& GetLakes() { return g_lakes; }
const std::vector<RiverPath>& GetRivers() { return g_rivers; }

LakeSite* GetLakeMutable(size_t index) {
    if (index >= g_lakes.size()) return nullptr;
    return &g_lakes[index];
}

float LakeCoverage(const LakeSite& lake, float x, float z) {
    return lakeCoverageLocal(lake, x, z);
}

float LakeRimRadius(const LakeSite& lake, float angleRad) {
    // Rim in world meters along ellipse + warp (must match LakeCoverage harmonics)
    const float c = std::cos(lake.angle);
    const float s = std::sin(lake.angle);
    // Direction in ellipse space for this world angle
    const float wx = std::cos(angleRad);
    const float wz = std::sin(angleRad);
    const float lx = wx * c + wz * s;
    const float lz = -wx * s + wz * c;
    const float denom = std::sqrt(
        (lx * lx) / (lake.radiusA * lake.radiusA) +
        (lz * lz) / (lake.radiusB * lake.radiusB));
    if (denom < 1.0e-5f) return lake.radiusA;

    float base = 1.0f / denom;
    // Local coords of the unwarped ellipse rim (for the spatial warp term)
    const float lxRim = lx * base;
    const float lzRim = lz * base;
    const float ang = std::atan2(lz / lake.radiusB, lx / lake.radiusA);
    float warp = 1.0f
        + lake.warpAmp * std::sin(ang * lake.warpFreq + lake.phase)
        + lake.warpAmp * 0.55f * std::sin(ang * (lake.warpFreq * 1.7f) + lake.phase * 1.3f)
        + lake.warpAmp * 0.25f * std::sin(lxRim * 0.031f + lake.phase) * std::cos(lzRim * 0.027f);
    warp = std::clamp(warp, 0.55f, 1.55f);
    return base * warp;
}

void BuildHydrology(uint64_t seed) {
    ClearHydrology();

    // Water biomes = soft lake discs at Voronoi Water sites:
    //   dry shelf → waterLevel (slightly below) → sunken bed (noise waterGate).
    // Cell type stays Water for classification; flooded footprint is ~pond/lake sized.
    constexpr float kShoreMargin = 1.5f;
    constexpr float kWaterDepth  = 6.0f; // keep in sync with noise.cpp kBiomeWaterDepth
    // Mountain approach start on arced depth (matches noise.cpp kMountainApproachBand ≈ 1450;
    // keep ~200 m interior margin so lakes stay off the soft foothills fade).
    constexpr float kAlpineDepth = WorldConfig::WORLD_HALF_EXTENT - 1250.0f;

    static const float kFallbackAnchors[][2] = {
        { 1100.0f,   450.0f },
        {-1100.0f,  -300.0f },
        {  500.0f, -1100.0f },
    };

    auto pushBiomeFill = [&](float x, float z, uint64_t h) {
        if (static_cast<int>(g_lakes.size()) >= kMaxLakes) return;
        if (NsAlpineDepth(x, z) > kAlpineDepth) return;
        // Only reject if the cell *center* sits on a protected pad (spawn/city).
        if (hitsExclusion(x, z, 120.0f)) return;
        if (tooCloseToExisting(x, z, kMinLakeSpacing * 0.5f)) return;

        LakeSite lake{};
        lake.x = x;
        lake.z = z;
        lake.angle = randFloat01(splitmix64(h)) * 6.2831853f;
        lake.phase = randFloat01(splitmix64(h ^ 0x11ULL)) * 6.2831853f;
        lake.warpFreq = 1.2f + randFloat01(splitmix64(h ^ 0x22ULL)) * 1.2f;
        lake.warpAmp = 0.12f + randFloat01(splitmix64(h ^ 0x33ULL)) * 0.10f;

        // Soft disc ~200 m core / ~300 m outer (≈400–600 m across). Ellipse guides mesh + shore warp.
        const float major = kLakeBodyOuterR * (0.82f + randFloat01(splitmix64(h ^ 0x44ULL)) * 0.18f);
        const float aspect = 0.72f + randFloat01(splitmix64(h ^ 0x55ULL)) * 0.22f;
        lake.radiusA = major;
        lake.radiusB = major * aspect;
        lake.fillRadius = kLakeBodyOuterR * (1.00f + randFloat01(splitmix64(h ^ 0x66ULL)) * 0.08f);
        lake.boundR = std::max({lake.radiusA, lake.radiusB, lake.fillRadius}) * 1.08f;
        lake.depth = kWaterDepth;

        // waterLevel from the local undug shelf at the cell site (bed + depth),
        // pulled down slightly toward the surrounding dry rim when that rim is higher.
        // Guarantees: bed < waterLevel < localShelf (filled bowl, not under the map).
        const float bedH = LandSurfaceHeight(lake.x, lake.z);
        const float localShelf = bedH + kWaterDepth;
        const float rimH = sampleDryShelfHeight(lake.x, lake.z, lake.boundR);
        float waterLevel = localShelf - kShoreMargin;
        if (rimH > localShelf + 0.5f) {
            // Shore sits above the cell — pull the table toward the lower rim-ish shelf
            waterLevel = std::min(waterLevel, rimH - kShoreMargin);
        }
        waterLevel = std::clamp(waterLevel, bedH + 2.5f, localShelf - 0.5f);
        lake.surfaceY = waterLevel;

        g_lakes.push_back(lake);
        TraceLog(LOG_INFO,
                 "HYDROLOGY: biome fill at (%.0f, %.0f) waterLevel=%.1f bed=%.1f localShelf=%.1f rim=%.1f depth=%.1f fillR=%.0f",
                 lake.x, lake.z, lake.surfaceY, bedH, localShelf, rimH, lake.depth, lake.fillRadius);
    };

    const auto cells = CollectBiomeCells(WorldConfig::WORLD_HALF_EXTENT - 100.0f);
    int waterCells = 0;
    for (const BiomeCellInfo& cell : cells) {
        if (cell.biome != WorldRegion::Water) continue;
        ++waterCells;
        TraceLog(LOG_INFO, "HYDROLOGY: Water biome cell (%d,%d) site=(%.0f, %.0f)",
                 cell.cx, cell.cz, cell.x, cell.z);
        const uint64_t h = hash2D(seed ^ 0xA7E11ULL, cell.cx, cell.cz);
        pushBiomeFill(cell.x, cell.z, h);
    }

    if (g_lakes.empty()) {
        TraceLog(LOG_WARNING, "HYDROLOGY: 0 water biomes — placing fallback fills at anchors");
        for (int i = 0; i < 3; ++i) {
            const uint64_t h = hash2D(seed ^ 0xFA11BACFull, i, 0);
            pushBiomeFill(kFallbackAnchors[i][0], kFallbackAnchors[i][1], h);
        }
    }

    buildRivers(seed);
    buildRiverSegs();
    buildSpatialHash();
    g_ready = true;

    TraceLog(LOG_INFO,
             "HYDROLOGY: %d water-biome fills (%d water cells), %d rivers (no depression-fill)",
             static_cast<int>(g_lakes.size()), waterCells,
             static_cast<int>(g_rivers.size()));
}

float ApplyHydrologyCarve(float x, float z, float height) {
    if (!g_ready) return height;

    // Never carve protected pads (spawn church, city plazas, etc.)
    if (hitsExclusion(x, z, 0.0f)) return height;

    float h = height;
    const int cell = hashIndex(x, z);

    // Soft lake disc — keep in sync with noise.cpp kWaterBodyCoreR / ShoreW.
    constexpr float kCoreR  = 200.0f;
    constexpr float kShoreW = 100.0f;
    // Keep bed clearly under the translucent surface (was 0.35 — z-fight / land poke-through).
    constexpr float kMinClearance = 1.65f;

    // Enforce bed under each biome water table (kills mid-lake land islands).
    // LandSurfaceHeight digs a bowl; this clamps residual peaks to lake.surfaceY.
    const std::vector<int>& lakeIds = g_lakeHash[cell];
    float nearestLakeY = WaterLevel();
    float bestLakeGate = 0.0f;
    for (int id : lakeIds) {
        const LakeSite& lake = g_lakes[static_cast<size_t>(id)];
        const float dx = x - lake.x;
        const float dz = z - lake.z;
        const float dist = std::sqrt(dx * dx + dz * dz);
        const float g = 1.0f - smoothstep(kCoreR, kCoreR + kShoreW, dist);
        const float gate = g * g;
        if (gate < 1.0e-4f) continue;
        if (gate > bestLakeGate) {
            bestLakeGate = gate;
            nearestLakeY = lake.surfaceY;
        }
        // Target bowl: shallow near shore, ~depth in the core.
        const float dig = std::max(lake.depth, 4.0f);
        const float target = lake.surfaceY
            - (kMinClearance + gate * (dig - kMinClearance));
        h = h * (1.0f - gate) + std::min(h, target) * gate;
        // Hard guarantee once we're clearly inside open water.
        if (gate > 0.28f) {
            h = std::min(h, lake.surfaceY - kMinClearance);
        }
        if (gate > 0.55f) {
            h = std::min(h, lake.surfaceY - std::min(dig * 0.55f, dig - 0.5f));
        }
    }

    // River channels
    const std::vector<int>& segIds = g_segHash[cell];
    float bestDist = 1.0e30f;
    float bestHalf = 5.0f;
    for (int id : segIds) {
        const RiverSeg& s = g_riverSegs[id];
        const float d = distToSegment(x, z, s.ax, s.az, s.bx, s.bz);
        if (d < bestDist) {
            bestDist = d;
            bestHalf = s.halfW;
        }
    }
    // Prefer nearby biome table when the hash cell missed (wide channel near shore)
    if (bestLakeGate < 1.0e-4f) {
        for (const LakeSite& lake : g_lakes) {
            const float dx = x - lake.x;
            const float dz = z - lake.z;
            if (dx * dx + dz * dz < lake.boundR * lake.boundR * 2.25f) {
                nearestLakeY = lake.surfaceY;
                break;
            }
        }
    }
    if (bestDist < bestHalf * 2.85f) {
        const float t = 1.0f - smoothstep(bestHalf * 0.15f, bestHalf * 2.85f, bestDist);
        const float dig = (3.6f + bestHalf * 0.72f) * (t * t);
        h -= dig;
        const float channelFloor = nearestLakeY - 1.15f - bestHalf * 0.12f;
        if (h < channelFloor - 5.0f) h = channelFloor - 5.0f;
    }

    return h;
}

}  // namespace engine::math
