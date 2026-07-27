#include "game/dungeon/dungeon_mask.hpp"
#include "engine/math/noise.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace game::dungeon {

namespace {

    struct Rng {
        uint64_t state;
        explicit Rng(uint64_t seed) : state(engine::math::splitmix64(seed ^ 0xD00DCAFEu)) {}
        uint64_t next() {
            state = engine::math::splitmix64(state);
            return state;
        }
        float next01() { return engine::math::randFloat01(next()); }
        int nextInt(int n) { return (n <= 0) ? 0 : (int)(next() % (uint64_t)n); }
    };

    WalkableMask makeGrid(float halfW, float halfD, float cellSize) {
        WalkableMask m;
        m.cellSize = std::max(0.75f, cellSize);
        m.halfW = halfW;
        m.halfD = halfD;
        m.nx = std::max(5, (int)ceilf((halfW * 2.0f) / m.cellSize));
        m.nz = std::max(5, (int)ceilf((halfD * 2.0f) / m.cellSize));
        // Keep odd counts so a true center cell exists.
        if ((m.nx & 1) == 0) ++m.nx;
        if ((m.nz & 1) == 0) ++m.nz;
        m.cells.assign((size_t)m.nx * (size_t)m.nz, 0);
        m.heights.assign((size_t)m.nx * (size_t)m.nz, 0.0f);
        m.ceilHeights.assign((size_t)m.nx * (size_t)m.nz, 0.0f);
        return m;
    }

    void flattenSocketHeights(WalkableMask& m) {
        if (m.heights.empty()) return;
        const int midX = m.nx / 2;
        const int midZ = m.nz / 2;
        const int half = std::max(2, (int)ceilf(4.5f / m.cellSize));
        const int blend = half + 2;

        auto flattenAxis = [&](bool posX, bool negX, bool posZ, bool negZ) {
            if (posX) {
                for (int ix = midX; ix < m.nx; ++ix)
                    for (int d = -blend; d <= blend; ++d) {
                        const int iz = midZ + d;
                        if (!m.At(ix, iz)) continue;
                        const float t = (std::abs(d) <= half) ? 0.0f
                            : std::clamp((float)(std::abs(d) - half) / 2.0f, 0.0f, 1.0f);
                        m.SetHeight(ix, iz, m.HeightAt(ix, iz) * t);
                    }
            }
            if (negX) {
                for (int ix = midX; ix >= 0; --ix)
                    for (int d = -blend; d <= blend; ++d) {
                        const int iz = midZ + d;
                        if (!m.At(ix, iz)) continue;
                        const float t = (std::abs(d) <= half) ? 0.0f
                            : std::clamp((float)(std::abs(d) - half) / 2.0f, 0.0f, 1.0f);
                        m.SetHeight(ix, iz, m.HeightAt(ix, iz) * t);
                    }
            }
            if (posZ) {
                for (int iz = midZ; iz < m.nz; ++iz)
                    for (int d = -blend; d <= blend; ++d) {
                        const int ix = midX + d;
                        if (!m.At(ix, iz)) continue;
                        const float t = (std::abs(d) <= half) ? 0.0f
                            : std::clamp((float)(std::abs(d) - half) / 2.0f, 0.0f, 1.0f);
                        m.SetHeight(ix, iz, m.HeightAt(ix, iz) * t);
                    }
            }
            if (negZ) {
                for (int iz = midZ; iz >= 0; --iz)
                    for (int d = -blend; d <= blend; ++d) {
                        const int ix = midX + d;
                        if (!m.At(ix, iz)) continue;
                        const float t = (std::abs(d) <= half) ? 0.0f
                            : std::clamp((float)(std::abs(d) - half) / 2.0f, 0.0f, 1.0f);
                        m.SetHeight(ix, iz, m.HeightAt(ix, iz) * t);
                    }
            }
        };

        flattenAxis(
            (m.sockets & SocketBit(SocketDir::PosX)) != 0,
            (m.sockets & SocketBit(SocketDir::NegX)) != 0,
            (m.sockets & SocketBit(SocketDir::PosZ)) != 0,
            (m.sockets & SocketBit(SocketDir::NegZ)) != 0);
    }

    float cellNoise(uint32_t seed, int ix, int iz, int salt) {
        const uint64_t h = engine::math::splitmix64(
            seed ^ ((uint64_t)ix * 0x9E3779B1u) ^ ((uint64_t)iz * 0x85EBCA6Bu) ^
            ((uint64_t)salt * 0xC2B2AE3Du));
        return engine::math::randFloat01(h);
    }

    void flattenSocketCeilHeights(WalkableMask& m) {
        if (m.ceilHeights.empty()) return;
        const int midX = m.nx / 2;
        const int midZ = m.nz / 2;
        const int half = std::max(2, (int)ceilf(4.5f / m.cellSize));
        const int blend = half + 2;

        auto flatten = [&](bool alongXPos, bool alongXNeg, bool alongZPos, bool alongZNeg) {
            auto band = [&](bool isX, bool positive) {
                if (isX) {
                    if (positive) {
                        for (int ix = midX; ix < m.nx; ++ix)
                            for (int d = -blend; d <= blend; ++d) {
                                const int iz = midZ + d;
                                if (!m.At(ix, iz)) continue;
                                const float t = (std::abs(d) <= half) ? 0.0f
                                    : std::clamp((float)(std::abs(d) - half) / 2.0f, 0.0f, 1.0f);
                                m.SetCeilHeight(ix, iz, m.CeilHeightAt(ix, iz) * t);
                            }
                    } else {
                        for (int ix = midX; ix >= 0; --ix)
                            for (int d = -blend; d <= blend; ++d) {
                                const int iz = midZ + d;
                                if (!m.At(ix, iz)) continue;
                                const float t = (std::abs(d) <= half) ? 0.0f
                                    : std::clamp((float)(std::abs(d) - half) / 2.0f, 0.0f, 1.0f);
                                m.SetCeilHeight(ix, iz, m.CeilHeightAt(ix, iz) * t);
                            }
                    }
                } else {
                    if (positive) {
                        for (int iz = midZ; iz < m.nz; ++iz)
                            for (int d = -blend; d <= blend; ++d) {
                                const int ix = midX + d;
                                if (!m.At(ix, iz)) continue;
                                const float t = (std::abs(d) <= half) ? 0.0f
                                    : std::clamp((float)(std::abs(d) - half) / 2.0f, 0.0f, 1.0f);
                                m.SetCeilHeight(ix, iz, m.CeilHeightAt(ix, iz) * t);
                            }
                    } else {
                        for (int iz = midZ; iz >= 0; --iz)
                            for (int d = -blend; d <= blend; ++d) {
                                const int ix = midX + d;
                                if (!m.At(ix, iz)) continue;
                                const float t = (std::abs(d) <= half) ? 0.0f
                                    : std::clamp((float)(std::abs(d) - half) / 2.0f, 0.0f, 1.0f);
                                m.SetCeilHeight(ix, iz, m.CeilHeightAt(ix, iz) * t);
                            }
                    }
                }
            };
            if (alongXPos) band(true, true);
            if (alongXNeg) band(true, false);
            if (alongZPos) band(false, true);
            if (alongZNeg) band(false, false);
        };

        flatten(
            (m.sockets & SocketBit(SocketDir::PosX)) != 0,
            (m.sockets & SocketBit(SocketDir::NegX)) != 0,
            (m.sockets & SocketBit(SocketDir::PosZ)) != 0,
            (m.sockets & SocketBit(SocketDir::NegZ)) != 0);
    }

    void generateCeilHeights(WalkableMask& m, Theme theme, SizeTier tier, uint32_t seed) {
        if (!m.Valid()) return;
        if ((int)m.ceilHeights.size() != m.nx * m.nz) {
            m.ceilHeights.assign((size_t)m.nx * (size_t)m.nz, 0.0f);
        }

        const int midX = m.nx / 2;
        const int midZ = m.nz / 2;
        const float amp =
            (tier == SizeTier::Large) ? 1.0f :
            (tier == SizeTier::Small) ? 0.6f : 0.8f;

        // Nominal clear height from base floor to nominal ceiling.
        constexpr float kNominalClear = 10.0f;
        constexpr float kMinClear = 3.6f;

        Rng rng(seed ^ 0xCE11u);

        if (theme == Theme::Cave) {
            // Irregular rock roof — mostly hangs down into the chamber.
            for (int iz = 0; iz < m.nz; ++iz) {
                for (int ix = 0; ix < m.nx; ++ix) {
                    if (!m.At(ix, iz)) {
                        m.SetCeilHeight(ix, iz, 0.0f);
                        continue;
                    }
                    const float n0 = cellNoise(seed, ix, iz, 11);
                    const float n1 = cellNoise(seed, ix / 2, iz / 2, 12);
                    const float n2 = cellNoise(seed, ix / 3, iz / 3, 13);
                    // Bias negative (stalactite / sagging vault).
                    float h = (n0 - 0.65f) * 1.4f + (n1 - 0.55f) * 2.0f + (n2 - 0.5f) * 1.6f;
                    h *= amp;

                    const float nx = ((float)ix - (float)midX) / std::max(1.0f, (float)(m.nx / 2));
                    const float nz = ((float)iz - (float)midZ) / std::max(1.0f, (float)(m.nz / 2));
                    const float edge = std::min(1.0f, sqrtf(nx * nx + nz * nz));
                    // Slightly higher near walls so openings stay tall.
                    h *= (0.75f + edge * 0.35f);
                    h = std::clamp(h, -3.2f * amp, 0.8f * amp);

                    // Keep headroom above the floor heightfield.
                    const float floorH = m.HeightAt(ix, iz);
                    const float clear = kNominalClear + h - floorH;
                    if (clear < kMinClear) h = floorH + kMinClear - kNominalClear;

                    m.SetCeilHeight(ix, iz, h);
                }
            }
        } else {
            // Masonry: mostly flat, with barrel / groin vaults (higher toward center).
            const bool vault = rng.next01() < 0.7f;
            const float vaultRise = (1.0f + rng.next01() * 1.6f) * amp;
            for (int iz = 0; iz < m.nz; ++iz) {
                for (int ix = 0; ix < m.nx; ++ix) {
                    if (!m.At(ix, iz)) {
                        m.SetCeilHeight(ix, iz, 0.0f);
                        continue;
                    }
                    float h = 0.0f;
                    if (vault) {
                        const float nx = ((float)ix - (float)midX) / std::max(1.0f, (float)(m.nx / 2));
                        const float nz = ((float)iz - (float)midZ) / std::max(1.0f, (float)(m.nz / 2));
                        const float r = std::min(1.0f, sqrtf(nx * nx + nz * nz));
                        // Dome: +rise at center, 0 at edges.
                        h = vaultRise * (1.0f - r) * (1.0f - r);
                    }
                    // Occasional coffer dips.
                    if (rng.next01() < 0.02f) h -= 0.35f * amp;

                    const float floorH = m.HeightAt(ix, iz);
                    const float clear = kNominalClear + h - floorH;
                    if (clear < kMinClear) h = floorH + kMinClear - kNominalClear;
                    m.SetCeilHeight(ix, iz, h);
                }
            }
        }

        flattenSocketCeilHeights(m);
    }

    void generateHeights(WalkableMask& m, Theme theme, SizeTier tier, uint32_t seed) {
        if (!m.Valid()) return;
        if ((int)m.heights.size() != m.nx * m.nz) {
            m.heights.assign((size_t)m.nx * (size_t)m.nz, 0.0f);
        }
        if ((int)m.ceilHeights.size() != m.nx * m.nz) {
            m.ceilHeights.assign((size_t)m.nx * (size_t)m.nz, 0.0f);
        }

        const int midX = m.nx / 2;
        const int midZ = m.nz / 2;
        const float amp =
            (tier == SizeTier::Large) ? 1.0f :
            (tier == SizeTier::Small) ? 0.65f : 0.85f;

        Rng rng(seed ^ 0x51E10u);

        if (theme == Theme::Cave) {
            const int style = rng.nextInt(3); // rolling / terraced / ridged
            for (int iz = 0; iz < m.nz; ++iz) {
                for (int ix = 0; ix < m.nx; ++ix) {
                    if (!m.At(ix, iz)) {
                        m.SetHeight(ix, iz, 0.0f);
                        continue;
                    }
                    const float n0 = cellNoise(seed, ix, iz, 1);
                    const float n1 = cellNoise(seed, ix / 2, iz / 2, 2);
                    const float n2 = cellNoise(seed, ix / 4, iz / 4, 3);
                    float h = (n0 - 0.5f) * 1.1f + (n1 - 0.5f) * 1.6f + (n2 - 0.5f) * 2.2f;
                    h *= amp;

                    const float nx = ((float)ix - (float)midX) / std::max(1.0f, (float)(m.nx / 2));
                    const float nz = ((float)iz - (float)midZ) / std::max(1.0f, (float)(m.nz / 2));
                    const float edge = std::min(1.0f, sqrtf(nx * nx + nz * nz));
                    h *= (1.0f - edge * 0.35f);

                    if (style == 1) {
                        const float step = 0.45f;
                        h = floorf(h / step + 0.5f) * step;
                    } else if (style == 2) {
                        h = ((n1 - 0.5f) * 2.8f + (n0 - 0.5f) * 0.6f) * amp;
                        if (h > 0.3f) h += 0.35f * amp;
                    }

                    h = std::clamp(h, -1.4f * amp, 2.6f * amp);
                    m.SetHeight(ix, iz, h);
                }
            }
        } else {
            for (int iz = 0; iz < m.nz; ++iz)
                for (int ix = 0; ix < m.nx; ++ix)
                    m.SetHeight(ix, iz, 0.0f);

            const int features = 1 + rng.nextInt((tier == SizeTier::Large) ? 3 : 2);
            for (int f = 0; f < features; ++f) {
                const bool raised = rng.next01() < 0.7f;
                const float h = raised ? (0.55f + rng.next01() * 0.75f) * amp
                                       : (-0.35f - rng.next01() * 0.35f) * amp;
                const int cx = 2 + rng.nextInt(std::max(1, m.nx - 4));
                const int cz = 2 + rng.nextInt(std::max(1, m.nz - 4));
                const int rw = 2 + rng.nextInt(std::max(1, m.nx / 6));
                const int rz = 2 + rng.nextInt(std::max(1, m.nz / 6));
                for (int iz = cz - rz; iz <= cz + rz; ++iz) {
                    for (int ix = cx - rw; ix <= cx + rw; ++ix) {
                        if (!m.At(ix, iz)) continue;
                        const float dx = (float)(ix - cx) / (float)std::max(1, rw);
                        const float dz = (float)(iz - cz) / (float)std::max(1, rz);
                        if (dx * dx + dz * dz > 1.05f) continue;
                        m.SetHeight(ix, iz, h);
                    }
                }
            }

            if (tier == SizeTier::Large && rng.next01() < 0.55f) {
                const int r = std::max(3, std::min(m.nx, m.nz) / 8);
                for (int iz = midZ - r; iz <= midZ + r; ++iz) {
                    for (int ix = midX - r; ix <= midX + r; ++ix) {
                        if (!m.At(ix, iz)) continue;
                        const int d2 = (ix - midX) * (ix - midX) + (iz - midZ) * (iz - midZ);
                        if (d2 <= r * r) m.SetHeight(ix, iz, 0.7f * amp);
                        else if (d2 <= (r + 1) * (r + 1)) m.SetHeight(ix, iz, 0.35f * amp);
                    }
                }
            }
        }

        flattenSocketHeights(m);
        generateCeilHeights(m, theme, tier, seed ^ 0xCEu);
    }

    void fillRect(WalkableMask& m, int x0, int z0, int x1, int z1) {
        x0 = std::max(0, x0); z0 = std::max(0, z0);
        x1 = std::min(m.nx - 1, x1); z1 = std::min(m.nz - 1, z1);
        for (int iz = z0; iz <= z1; ++iz) {
            for (int ix = x0; ix <= x1; ++ix) m.Set(ix, iz, true);
        }
    }

    void carveSocket(WalkableMask& m, SocketDir dir, float doorHalfMeters) {
        const int midX = m.nx / 2;
        const int midZ = m.nz / 2;
        // Wide enough to clear extruded wall thickness + corridor mouth.
        const int halfCells = std::max(2, (int)ceilf(doorHalfMeters / m.cellSize));

        switch (dir) {
            case SocketDir::PosX:
                for (int ix = midX; ix < m.nx; ++ix)
                    for (int d = -halfCells; d <= halfCells; ++d)
                        m.Set(ix, midZ + d, true);
                break;
            case SocketDir::NegX:
                for (int ix = midX; ix >= 0; --ix)
                    for (int d = -halfCells; d <= halfCells; ++d)
                        m.Set(ix, midZ + d, true);
                break;
            case SocketDir::PosZ:
                for (int iz = midZ; iz < m.nz; ++iz)
                    for (int d = -halfCells; d <= halfCells; ++d)
                        m.Set(midX + d, iz, true);
                break;
            case SocketDir::NegZ:
                for (int iz = midZ; iz >= 0; --iz)
                    for (int d = -halfCells; d <= halfCells; ++d)
                        m.Set(midX + d, iz, true);
                break;
        }
    }

    void openAllSockets(WalkableMask& m, uint8_t sockets, float doorHalf) {
        for (int s = 0; s < 4; ++s) {
            if (sockets & (1u << s)) carveSocket(m, static_cast<SocketDir>(s), doorHalf);
        }
    }

    void smoothOnce(WalkableMask& m) {
        WalkableMask smoothed = m;
        for (int iz = 1; iz < m.nz - 1; ++iz) {
            for (int ix = 1; ix < m.nx - 1; ++ix) {
                int n = 0;
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (m.At(ix + dx, iz + dz)) ++n;
                smoothed.Set(ix, iz, n >= 5);
            }
        }
        m = std::move(smoothed);
    }

    void digWorm(WalkableMask& m, Rng& rng, int x, int z, int steps, int rad) {
        for (int s = 0; s < steps; ++s) {
            for (int dz = -rad; dz <= rad; ++dz)
                for (int dx = -rad; dx <= rad; ++dx)
                    m.Set(x + dx, z + dz, true);
            const int dir = rng.nextInt(4);
            if (dir == 0) ++x;
            else if (dir == 1) --x;
            else if (dir == 2) ++z;
            else --z;
            x = std::clamp(x, 1, m.nx - 2);
            z = std::clamp(z, 1, m.nz - 2);
        }
    }

    void fillEllipse(WalkableMask& m, float cx, float cz, float rx, float rz, float noiseAmp, uint32_t seed) {
        for (int iz = 0; iz < m.nz; ++iz) {
            for (int ix = 0; ix < m.nx; ++ix) {
                const float nx = ((float)ix - cx) / std::max(1.0f, rx);
                const float nz = ((float)iz - cz) / std::max(1.0f, rz);
                const float d = nx * nx + nz * nz;
                const uint64_t h = engine::math::splitmix64(
                    seed ^ ((uint64_t)ix * 0x9E3779B1u) ^ ((uint64_t)iz * 0x85EBCA6Bu));
                const float n = engine::math::randFloat01(h) * noiseAmp;
                if (d < 1.0f - n) m.Set(ix, iz, true);
            }
        }
    }

    void ensureCenterConnected(WalkableMask& m) {
        const int midX = m.nx / 2;
        const int midZ = m.nz / 2;
        if (!m.At(midX, midZ)) m.Set(midX, midZ, true);

        std::vector<uint8_t> reach((size_t)m.nx * (size_t)m.nz, 0);
        std::vector<int> stack;
        stack.push_back(midZ * m.nx + midX);
        reach[(size_t)stack.back()] = 1;

        const int dx[4] = {1, -1, 0, 0};
        const int dz[4] = {0, 0, 1, -1};
        while (!stack.empty()) {
            const int idx = stack.back();
            stack.pop_back();
            const int ix = idx % m.nx;
            const int iz = idx / m.nx;
            for (int d = 0; d < 4; ++d) {
                const int nx = ix + dx[d];
                const int nz = iz + dz[d];
                if (!m.At(nx, nz)) continue;
                const int nidx = nz * m.nx + nx;
                if (reach[(size_t)nidx]) continue;
                reach[(size_t)nidx] = 1;
                stack.push_back(nidx);
            }
        }

        for (int iz = 0; iz < m.nz; ++iz) {
            for (int ix = 0; ix < m.nx; ++ix) {
                const int idx = iz * m.nx + ix;
                if (m.cells[(size_t)idx] && !reach[(size_t)idx]) m.cells[(size_t)idx] = 0;
            }
        }
    }

    WalkableMask buildMasonry(SizeTier tier, float halfW, float halfD, uint8_t sockets,
                              uint32_t seed, float cellSize) {
        Rng rng(seed ^ 0x4A501u);
        WalkableMask m = makeGrid(halfW, halfD, cellSize);

        const int morph = rng.nextInt(4); // rect / long hall / L / cross-ish
        const int midX = m.nx / 2;
        const int midZ = m.nz / 2;

        if (morph == 0) {
            fillRect(m, 1, 1, m.nx - 2, m.nz - 2);
        } else if (morph == 1) {
            // Long hall: narrow on one axis.
            if (m.nx >= m.nz) {
                const int band = std::max(3, m.nz / 3);
                fillRect(m, 1, midZ - band, m.nx - 2, midZ + band);
            } else {
                const int band = std::max(3, m.nx / 3);
                fillRect(m, midX - band, 1, midX + band, m.nz - 2);
            }
        } else if (morph == 2) {
            // L-wing: two rectangles sharing the center.
            fillRect(m, 1, midZ - m.nz / 5, m.nx - 2, midZ + m.nz / 5);
            if (rng.next01() < 0.5f) {
                fillRect(m, midX - m.nx / 5, 1, midX + m.nx / 5, midZ);
            } else {
                fillRect(m, midX - m.nx / 5, midZ, midX + m.nx / 5, m.nz - 2);
            }
        } else {
            // Cross: plus-shaped walkable area.
            const int armX = std::max(2, m.nx / 5);
            const int armZ = std::max(2, m.nz / 5);
            fillRect(m, 1, midZ - armZ, m.nx - 2, midZ + armZ);
            fillRect(m, midX - armX, 1, midX + armX, m.nz - 2);
        }

        // Optional alcoves for larger masonry rooms.
        const int alcoveTries = (m.nx >= 18 && m.nz >= 18) ? 1 + rng.nextInt(2) : 0;
        for (int a = 0; a < alcoveTries; ++a) {
            const int side = rng.nextInt(4);
            const int depth = 3 + rng.nextInt(4);
            const int width = 4 + rng.nextInt(6);
            if (side == 0) {
                fillRect(m, m.nx - 2 - depth, midZ - width / 2, m.nx - 2, midZ + width / 2);
            } else if (side == 1) {
                fillRect(m, 1, midZ - width / 2, 1 + depth, midZ + width / 2);
            } else if (side == 2) {
                fillRect(m, midX - width / 2, m.nz - 2 - depth, midX + width / 2, m.nz - 2);
            } else {
                fillRect(m, midX - width / 2, 1, midX + width / 2, 1 + depth);
            }
        }

        if (m.nx >= 28 && rng.next01() < 0.35f) {
            const int r = 1 + rng.nextInt(2);
            for (int iz = -r; iz <= r; ++iz)
                for (int ix = -r; ix <= r; ++ix)
                    if (ix * ix + iz * iz <= r * r + 1)
                        m.Set(midX + ix, midZ + iz, false);
            for (int iz = -(r + 2); iz <= (r + 2); ++iz)
                for (int ix = -(r + 2); ix <= (r + 2); ++ix) {
                    const int d2 = ix * ix + iz * iz;
                    if (d2 > r * r && d2 <= (r + 2) * (r + 2))
                        m.Set(midX + ix, midZ + iz, true);
                }
        }

        openAllSockets(m, sockets, 3.1f);
        ensureCenterConnected(m);
        openAllSockets(m, sockets, 3.1f); // re-open after connectivity trim
        m.sockets = sockets;
        generateHeights(m, Theme::Masonry, tier, seed);
        return m;
    }

    WalkableMask buildCave(SizeTier tier, float halfW, float halfD, uint8_t sockets,
                           uint32_t seed, float cellSize) {
        Rng rng(seed ^ 0xCA7Eu);
        WalkableMask m = makeGrid(halfW, halfD, cellSize);

        const int midX = m.nx / 2;
        const int midZ = m.nz / 2;
        const float rx = (float)(m.nx / 2 - 2);
        const float rz = (float)(m.nz / 2 - 2);
        const int morph = rng.nextInt(5);

        if (morph == 0) {
            // Wide cavern — irregular ellipse.
            fillEllipse(m, (float)midX, (float)midZ, rx * 0.95f, rz * 0.95f, 0.42f, seed);
        } else if (morph == 1) {
            // Twin lobes connected through the center.
            const float ox = rx * (0.35f + rng.next01() * 0.15f);
            const float oz = rz * (0.1f + rng.next01() * 0.2f);
            if (rng.next01() < 0.5f) {
                fillEllipse(m, (float)midX - ox, (float)midZ + oz, rx * 0.55f, rz * 0.55f, 0.3f, seed);
                fillEllipse(m, (float)midX + ox, (float)midZ - oz, rx * 0.55f, rz * 0.55f, 0.3f, seed ^ 0x111u);
            } else {
                fillEllipse(m, (float)midX + oz, (float)midZ - ox, rx * 0.55f, rz * 0.55f, 0.3f, seed);
                fillEllipse(m, (float)midX - oz, (float)midZ + ox, rx * 0.55f, rz * 0.55f, 0.3f, seed ^ 0x222u);
            }
            fillEllipse(m, (float)midX, (float)midZ, rx * 0.28f, rz * 0.28f, 0.1f, seed ^ 0x333u);
        } else if (morph == 2) {
            // Crescent / C-shape: ellipse with a bite taken out.
            fillEllipse(m, (float)midX, (float)midZ, rx * 0.9f, rz * 0.9f, 0.28f, seed);
            const float bx = (rng.next01() < 0.5f) ? rx * 0.55f : -rx * 0.55f;
            const float bz = (rng.next01() < 0.5f) ? rz * 0.55f : -rz * 0.55f;
            for (int iz = 0; iz < m.nz; ++iz) {
                for (int ix = 0; ix < m.nx; ++ix) {
                    const float nx = ((float)ix - ((float)midX + bx)) / std::max(1.0f, rx * 0.45f);
                    const float nz = ((float)iz - ((float)midZ + bz)) / std::max(1.0f, rz * 0.45f);
                    if (nx * nx + nz * nz < 1.0f) m.Set(ix, iz, false);
                }
            }
        } else if (morph == 3) {
            // Worm nest — small seed then many diggers.
            fillEllipse(m, (float)midX, (float)midZ, rx * 0.35f, rz * 0.35f, 0.2f, seed);
            const int worms = 5 + rng.nextInt(5);
            for (int w = 0; w < worms; ++w) {
                digWorm(m, rng, midX, midZ, 14 + rng.nextInt(22), 1 + rng.nextInt(2));
            }
        } else {
            // Pillared grotto — large chamber with rock columns.
            fillEllipse(m, (float)midX, (float)midZ, rx * 0.92f, rz * 0.92f, 0.38f, seed);
            const int pillars = 3 + rng.nextInt(4);
            for (int p = 0; p < pillars; ++p) {
                const int px = midX + rng.nextInt(std::max(1, m.nx / 2)) - m.nx / 4;
                const int pz = midZ + rng.nextInt(std::max(1, m.nz / 2)) - m.nz / 4;
                if (std::abs(px - midX) < 3 && std::abs(pz - midZ) < 3) continue;
                const int r = 1 + rng.nextInt(2);
                for (int dz = -r; dz <= r; ++dz)
                    for (int dx = -r; dx <= r; ++dx)
                        if (dx * dx + dz * dz <= r * r)
                            m.Set(px + dx, pz + dz, false);
            }
        }

        // Extra worms for irregularity on larger caves.
        if (m.nx >= 20) {
            const int extra = 1 + rng.nextInt(3);
            for (int w = 0; w < extra; ++w) {
                digWorm(m, rng,
                        midX + rng.nextInt(m.nx / 3) - m.nx / 6,
                        midZ + rng.nextInt(m.nz / 3) - m.nz / 6,
                        6 + rng.nextInt(12), 1);
            }
        }

        smoothOnce(m);
        // Never smooth away door mouths — carve after, then reconnect, then carve again.
        openAllSockets(m, sockets, 3.6f);
        ensureCenterConnected(m);
        openAllSockets(m, sockets, 3.6f);
        m.sockets = sockets;
        generateHeights(m, Theme::Cave, tier, seed);
        return m;
    }

}  // namespace

bool WalkableMask::IsDoorMouth(int ix, int iz, int dix, int diz) const {
    const int midX = nx / 2;
    const int midZ = nz / 2;
    // Match corridor half-width (~3m). Opening ≈ 2*half+1 cells.
    // half=1 @ 1.5m cells → ~4.5m clear — corridor walls seal the flanks.
    const int half = std::max(1, (int)ceilf(2.6f / std::max(0.75f, cellSize)) - 1);

    if ((sockets & SocketBit(SocketDir::PosX)) && dix == 1 && ix == nx - 1 &&
        std::abs(iz - midZ) <= half) {
        return true;
    }
    if ((sockets & SocketBit(SocketDir::NegX)) && dix == -1 && ix == 0 &&
        std::abs(iz - midZ) <= half) {
        return true;
    }
    if ((sockets & SocketBit(SocketDir::PosZ)) && diz == 1 && iz == nz - 1 &&
        std::abs(ix - midX) <= half) {
        return true;
    }
    if ((sockets & SocketBit(SocketDir::NegZ)) && diz == -1 && iz == 0 &&
        std::abs(ix - midX) <= half) {
        return true;
    }
    return false;
}

bool WalkableMask::ContainsLocal(float lx, float lz, float margin) const {
    if (!Valid()) {
        return fabsf(lx) <= halfW + margin && fabsf(lz) <= halfD + margin;
    }
    // Expand/shrink by testing nearby cells when margin is non-zero.
    const float probe = std::max(0.0f, cellSize * 0.45f);
    const float samples[5][2] = {
        {0, 0}, {probe, 0}, {-probe, 0}, {0, probe}, {0, -probe}
    };
    const int count = (fabsf(margin) < 0.01f) ? 1 : 5;
    bool any = false;
    for (int i = 0; i < count; ++i) {
        const float px = lx + samples[i][0];
        const float pz = lz + samples[i][1];
        const float u = (px + halfW) / (halfW * 2.0f);
        const float v = (pz + halfD) / (halfD * 2.0f);
        const int ix = (int)floorf(u * (float)nx);
        const int iz = (int)floorf(v * (float)nz);
        if (At(ix, iz)) any = true;
    }
    if (margin >= 0.0f) return any;
    // Negative margin: must be walkable and not near the void edge.
    if (!any) return false;
    const float u = (lx + halfW) / (halfW * 2.0f);
    const float v = (lz + halfD) / (halfD * 2.0f);
    const int ix = (int)floorf(u * (float)nx);
    const int iz = (int)floorf(v * (float)nz);
    return At(ix, iz) && At(ix - 1, iz) && At(ix + 1, iz) && At(ix, iz - 1) && At(ix, iz + 1);
}

float WalkableMask::SampleHeightLocal(float lx, float lz) const {
    if (!Valid() || heights.empty()) return 0.0f;
    if (!ContainsLocal(lx, lz, 0.0f)) return 0.0f;

    const float u = (lx + halfW) / (halfW * 2.0f) * (float)nx - 0.5f;
    const float v = (lz + halfD) / (halfD * 2.0f) * (float)nz - 0.5f;
    const int x0 = (int)floorf(u);
    const int z0 = (int)floorf(v);
    const int x1 = x0 + 1;
    const int z1 = z0 + 1;
    const float tx = u - (float)x0;
    const float tz = v - (float)z0;

    auto sample = [&](int ix, int iz) {
        if (!At(ix, iz)) return 0.0f;
        return HeightAt(ix, iz);
    };

    const float h00 = sample(x0, z0);
    const float h10 = sample(x1, z0);
    const float h01 = sample(x0, z1);
    const float h11 = sample(x1, z1);
    const float hx0 = h00 + (h10 - h00) * tx;
    const float hx1 = h01 + (h11 - h01) * tx;
    return hx0 + (hx1 - hx0) * tz;
}

bool WalkableMask::ContainsWorld(Vector3 roomCenter, Vector3 p, float margin) const {
    return ContainsLocal(p.x - roomCenter.x, p.z - roomCenter.z, margin);
}

float WalkableMask::SampleHeightWorld(Vector3 roomCenter, float x, float z) const {
    return SampleHeightLocal(x - roomCenter.x, z - roomCenter.z);
}

float WalkableMask::SampleCeilHeightLocal(float lx, float lz) const {
    if (!Valid() || ceilHeights.empty()) return 0.0f;
    if (!ContainsLocal(lx, lz, 0.0f)) return 0.0f;

    const float u = (lx + halfW) / (halfW * 2.0f) * (float)nx - 0.5f;
    const float v = (lz + halfD) / (halfD * 2.0f) * (float)nz - 0.5f;
    const int x0 = (int)floorf(u);
    const int z0 = (int)floorf(v);
    const int x1 = x0 + 1;
    const int z1 = z0 + 1;
    const float tx = u - (float)x0;
    const float tz = v - (float)z0;

    auto sample = [&](int ix, int iz) {
        if (!At(ix, iz)) return 0.0f;
        return CeilHeightAt(ix, iz);
    };

    const float h00 = sample(x0, z0);
    const float h10 = sample(x1, z0);
    const float h01 = sample(x0, z1);
    const float h11 = sample(x1, z1);
    const float hx0 = h00 + (h10 - h00) * tx;
    const float hx1 = h01 + (h11 - h01) * tx;
    return hx0 + (hx1 - hx0) * tz;
}

float WalkableMask::SampleCeilHeightWorld(Vector3 roomCenter, float x, float z) const {
    return SampleCeilHeightLocal(x - roomCenter.x, z - roomCenter.z);
}

Vector3 WalkableMask::CellCenterLocal(int ix, int iz) const {
    const float lx = -halfW + ((float)ix + 0.5f) * (halfW * 2.0f / (float)nx);
    const float lz = -halfD + ((float)iz + 0.5f) * (halfD * 2.0f / (float)nz);
    return Vector3{lx, 0.0f, lz};
}

bool WalkableMask::SampleWalkableLocal(uint64_t seed, int salt, float& outLx, float& outLz) const {
    if (!Valid()) return false;
    std::vector<int> ids;
    ids.reserve((size_t)nx * (size_t)nz / 2);
    for (int iz = 0; iz < nz; ++iz) {
        for (int ix = 0; ix < nx; ++ix) {
            if (At(ix, iz)) ids.push_back(iz * nx + ix);
        }
    }
    if (ids.empty()) return false;
    const uint64_t h = engine::math::splitmix64(seed ^ (uint64_t)(salt * 0x9E3779B1u));
    const int pick = (int)(h % (uint64_t)ids.size());
    const int ix = ids[(size_t)pick] % nx;
    const int iz = ids[(size_t)pick] / nx;
    const Vector3 c = CellCenterLocal(ix, iz);
    outLx = c.x;
    outLz = c.z;
    return true;
}

WalkableMask BuildMask(Theme theme, SizeTier tier, float halfW, float halfD,
                       uint8_t socketBits, uint32_t seed, float cellSize) {
    if (theme == Theme::Cave) {
        return buildCave(tier, halfW, halfD, socketBits, seed, cellSize);
    }
    return buildMasonry(tier, halfW, halfD, socketBits, seed, cellSize);
}

}  // namespace game::dungeon
