#include "game/world/building_panels.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <cmath>
#include <cstring>
#include <vector>

namespace game::world::building_panels {

namespace {

constexpr float kGrid       = 4.0f;
constexpr float kWallH      = 3.0f;
constexpr float kDoorH      = 2.6f;
constexpr float kDoorW      = 1.6f;
constexpr float kWindowW    = 1.4f;
constexpr float kWindowH    = 1.2f;
constexpr float kWindowSill = 1.0f;
constexpr float kWallThick  = 0.28f;
constexpr float kFloorThick = 0.22f;
constexpr float kTrim       = 0.08f;
constexpr float kPillarW    = 0.55f;
constexpr float kRoofRise   = 1.5f;
constexpr float kPyramidH   = 3.5f;
constexpr float kPyramidOverhang = 0.35f;

// World meters → UV (1.0 = one texture repeat per meter).
constexpr float kUvScale = 0.85f;
constexpr int   kTexSize = 256;

// Near-white fill so albedo texture reads; trim slightly darkened (vertex multiply).
constexpr Color kFillTint = {235, 232, 225, 255};
constexpr Color kTrimTint = {150, 140, 125, 255};

Texture2D g_texStone{};
Texture2D g_texWood{};
Texture2D g_texRoof{};
bool g_texReady = false;

Color styleFill(Style /*s*/) { return kFillTint; }
Color styleTrim(Style /*s*/) { return kTrimTint; }

Texture2D& textureFor(Style s) {
    switch (s) {
    case Style::Wood:     return g_texWood;
    case Style::RoofDark: return g_texRoof;
    case Style::Stone:
    default:              return g_texStone;
    }
}

// ---- Procedural tileable textures ----

static float hash2(int x, int y) {
    unsigned n = (unsigned)(x * 374761393u + y * 668265263u);
    n = (n ^ (n >> 13)) * 1274126177u;
    return (float)(n & 0xFFFFu) / 65535.0f;
}

static float valueNoiseTile(float x, float y, int period) {
    // x,y in [0, period)
    int x0 = ((int)std::floor(x) % period + period) % period;
    int y0 = ((int)std::floor(y) % period + period) % period;
    int x1 = (x0 + 1) % period;
    int y1 = (y0 + 1) % period;
    float fx = x - std::floor(x);
    float fy = y - std::floor(y);
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    float v00 = hash2(x0, y0);
    float v10 = hash2(x1, y0);
    float v01 = hash2(x0, y1);
    float v11 = hash2(x1, y1);
    float a = v00 + (v10 - v00) * fx;
    float b = v01 + (v11 - v01) * fx;
    return a + (b - a) * fy;
}

static float fbmTile(float x, float y, int period, int octaves) {
    float sum = 0.0f, amp = 0.5f, freq = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        int p = (int)std::max(2, (int)((float)period / freq));
        sum += amp * valueNoiseTile(x * freq, y * freq, p);
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return (norm > 0.0f) ? (sum / norm) : 0.0f;
}

static unsigned char clampU8(float v) {
    if (v < 0.0f) return 0;
    if (v > 255.0f) return 255;
    return (unsigned char)(v + 0.5f);
}

static Texture2D uploadTileable(Image img) {
    Texture2D tex = LoadTextureFromImage(img);
    UnloadImage(img);
    SetTextureWrap(tex, TEXTURE_WRAP_REPEAT);
    SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
    return tex;
}

static Texture2D makeStoneTexture() {
    Image img = GenImageColor(kTexSize, kTexSize, BLANK);
    auto* data = (unsigned char*)img.data;
    for (int y = 0; y < kTexSize; ++y) {
        for (int x = 0; x < kTexSize; ++x) {
            float u = (float)x;
            float v = (float)y;
            float n = fbmTile(u * 0.08f, v * 0.08f, 32, 4);
            float n2 = fbmTile(u * 0.25f, v * 0.25f, 64, 3);
            float gx = std::fabs(std::fmod(u + n * 6.0f, 48.0f) - 24.0f);
            float gy = std::fabs(std::fmod(v + n2 * 6.0f, 28.0f) - 14.0f);
            float mortar = (gx > 21.0f || gy > 12.0f) ? 0.82f : 1.0f;
            float base = (0.52f + n * 0.22f + n2 * 0.08f) * mortar;
            if (hash2(x, y) > 0.97f) base *= 0.75f;
            int i = (y * kTexSize + x) * 4;
            data[i + 0] = clampU8(base * 210.0f);
            data[i + 1] = clampU8(base * 200.0f);
            data[i + 2] = clampU8(base * 185.0f);
            data[i + 3] = 255;
        }
    }
    return uploadTileable(img);
}

static Texture2D makeWoodTexture() {
    Image img = GenImageColor(kTexSize, kTexSize, BLANK);
    auto* data = (unsigned char*)img.data;
    for (int y = 0; y < kTexSize; ++y) {
        for (int x = 0; x < kTexSize; ++x) {
            float u = (float)x;
            float v = (float)y;
            // Vertical grain
            float grain = fbmTile(u * 0.035f, v * 0.35f, 32, 4);
            float ring = std::sin((u + grain * 18.0f) * 0.22f) * 0.5f + 0.5f;
            float n = fbmTile(u * 0.1f, v * 0.1f, 64, 3);
            float base = 0.35f + grain * 0.25f + ring * 0.12f + n * 0.06f;
            // Occasional darker knot
            if (hash2(x / 8, y / 8) > 0.93f && hash2(x, y) > 0.6f) base *= 0.65f;
            int i = (y * kTexSize + x) * 4;
            data[i + 0] = clampU8(base * 175.0f);
            data[i + 1] = clampU8(base * 115.0f);
            data[i + 2] = clampU8(base * 65.0f);
            data[i + 3] = 255;
        }
    }
    return uploadTileable(img);
}

static Texture2D makeRoofTexture() {
    Image img = GenImageColor(kTexSize, kTexSize, BLANK);
    auto* data = (unsigned char*)img.data;
    for (int y = 0; y < kTexSize; ++y) {
        for (int x = 0; x < kTexSize; ++x) {
            float u = (float)x;
            float v = (float)y;
            float n = fbmTile(u * 0.12f, v * 0.12f, 32, 4);
            // Horizontal shingle rows
            float row = std::fmod(v + n * 3.0f, 18.0f);
            float shingle = (row < 2.0f) ? 0.75f : (0.9f + n * 0.15f);
            float colBreak = std::fabs(std::fmod(u + (int)(v / 18.0f) * 9.0f, 22.0f) - 11.0f);
            if (colBreak > 10.0f) shingle *= 0.88f;
            float base = 0.28f * shingle;
            int i = (y * kTexSize + x) * 4;
            data[i + 0] = clampU8(base * 160.0f);
            data[i + 1] = clampU8(base * 95.0f);
            data[i + 2] = clampU8(base * 70.0f);
            data[i + 3] = 255;
        }
    }
    return uploadTileable(img);
}

void initTextures() {
    if (g_texReady) return;
    g_texStone = makeStoneTexture();
    g_texWood  = makeWoodTexture();
    g_texRoof  = makeRoofTexture();
    g_texReady = true;
}

void shutdownTextures() {
    if (!g_texReady) return;
    UnloadTexture(g_texStone);
    UnloadTexture(g_texWood);
    UnloadTexture(g_texRoof);
    g_texStone = {};
    g_texWood = {};
    g_texRoof = {};
    g_texReady = false;
}

void bindTexture(Model& model, Style style) {
    if (model.materialCount <= 0) return;
    Texture2D& tex = textureFor(style);
    for (int i = 0; i < model.materialCount; ++i) {
        model.materials[i].maps[MATERIAL_MAP_ALBEDO].texture = tex;
        model.materials[i].maps[MATERIAL_MAP_ALBEDO].color = WHITE;
    }
}

void clearBoundTextures(Model& model) {
    // Avoid UnloadModel freeing shared style textures.
    for (int i = 0; i < model.materialCount; ++i) {
        model.materials[i].maps[MATERIAL_MAP_ALBEDO].texture = {};
    }
}

// ---- Mesh builder with UVs ----

struct MeshCPU {
    std::vector<Vector3> positions;
    std::vector<Vector3> normals;
    std::vector<Vector2> uvs;
    std::vector<Color>   colors;
    std::vector<unsigned short> indices;

    static Vector2 projectUV(Vector3 p, Vector3 n) {
        float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
        if (ay >= ax && ay >= az)
            return {p.x * kUvScale, p.z * kUvScale};
        if (ax >= az)
            return {p.z * kUvScale, p.y * kUvScale};
        return {p.x * kUvScale, p.y * kUvScale};
    }

    void addTri(Vector3 a, Vector3 b, Vector3 c, Color col) {
        Vector3 e1 = Vector3Subtract(b, a);
        Vector3 e2 = Vector3Subtract(c, a);
        Vector3 n  = Vector3Normalize(Vector3CrossProduct(e1, e2));
        if (n.x == 0.0f && n.y == 0.0f && n.z == 0.0f) n = {0, 1, 0};
        unsigned short base = (unsigned short)positions.size();
        positions.push_back(a);
        positions.push_back(b);
        positions.push_back(c);
        normals.push_back(n);
        normals.push_back(n);
        normals.push_back(n);
        uvs.push_back(projectUV(a, n));
        uvs.push_back(projectUV(b, n));
        uvs.push_back(projectUV(c, n));
        colors.push_back(col);
        colors.push_back(col);
        colors.push_back(col);
        indices.push_back(base);
        indices.push_back((unsigned short)(base + 1));
        indices.push_back((unsigned short)(base + 2));
    }

    void addQuad(Vector3 a, Vector3 b, Vector3 c, Vector3 d, Color col) {
        addTri(a, b, c, col);
        addTri(a, c, d, col);
    }

    void addBox(Vector3 center, Vector3 size, Color col) {
        const float hx = size.x * 0.5f;
        const float hy = size.y * 0.5f;
        const float hz = size.z * 0.5f;
        Vector3 p000 = {center.x - hx, center.y - hy, center.z - hz};
        Vector3 p001 = {center.x - hx, center.y - hy, center.z + hz};
        Vector3 p010 = {center.x - hx, center.y + hy, center.z - hz};
        Vector3 p011 = {center.x - hx, center.y + hy, center.z + hz};
        Vector3 p100 = {center.x + hx, center.y - hy, center.z - hz};
        Vector3 p101 = {center.x + hx, center.y - hy, center.z + hz};
        Vector3 p110 = {center.x + hx, center.y + hy, center.z - hz};
        Vector3 p111 = {center.x + hx, center.y + hy, center.z + hz};
        addQuad(p100, p000, p010, p110, col); // -Z
        addQuad(p001, p101, p111, p011, col); // +Z
        addQuad(p000, p001, p011, p010, col); // -X
        addQuad(p101, p100, p110, p111, col); // +X
        addQuad(p011, p111, p110, p010, col); // +Y
        addQuad(p000, p100, p101, p001, col); // -Y
    }

    Model upload() const {
        Mesh mesh = {0};
        const int vCount = (int)positions.size();
        const int tCount = (int)indices.size() / 3;
        mesh.vertexCount   = vCount;
        mesh.triangleCount = tCount;

        mesh.vertices  = (float*)MemAlloc((unsigned int)(vCount * 3 * (int)sizeof(float)));
        mesh.normals   = (float*)MemAlloc((unsigned int)(vCount * 3 * (int)sizeof(float)));
        mesh.texcoords = (float*)MemAlloc((unsigned int)(vCount * 2 * (int)sizeof(float)));
        mesh.colors    = (unsigned char*)MemAlloc((unsigned int)(vCount * 4 * (int)sizeof(unsigned char)));
        mesh.indices   = (unsigned short*)MemAlloc((unsigned int)(tCount * 3 * (int)sizeof(unsigned short)));

        for (int i = 0; i < vCount; ++i) {
            mesh.vertices[i * 3 + 0] = positions[i].x;
            mesh.vertices[i * 3 + 1] = positions[i].y;
            mesh.vertices[i * 3 + 2] = positions[i].z;
            mesh.normals[i * 3 + 0]  = normals[i].x;
            mesh.normals[i * 3 + 1]  = normals[i].y;
            mesh.normals[i * 3 + 2]  = normals[i].z;
            mesh.texcoords[i * 2 + 0] = uvs[i].x;
            mesh.texcoords[i * 2 + 1] = uvs[i].y;
            mesh.colors[i * 4 + 0]   = colors[i].r;
            mesh.colors[i * 4 + 1]   = colors[i].g;
            mesh.colors[i * 4 + 2]   = colors[i].b;
            mesh.colors[i * 4 + 3]   = colors[i].a;
        }
        std::memcpy(mesh.indices, indices.data(), indices.size() * sizeof(unsigned short));

        UploadMesh(&mesh, false);
        return LoadModelFromMesh(mesh);
    }
};

MeshCPU buildFloor(Color fill, Color trim) {
    MeshCPU m;
    const float half = kGrid * 0.5f;
    m.addBox({0, kFloorThick * 0.5f, 0}, {kGrid, kFloorThick, kGrid}, fill);
    const float t = kTrim;
    const float y = kFloorThick + t * 0.5f;
    m.addBox({0, y, -half + t * 0.5f}, {kGrid, t, t}, trim);
    m.addBox({0, y,  half - t * 0.5f}, {kGrid, t, t}, trim);
    m.addBox({-half + t * 0.5f, y, 0}, {t, t, kGrid - 2 * t}, trim);
    m.addBox({ half - t * 0.5f, y, 0}, {t, t, kGrid - 2 * t}, trim);
    return m;
}

MeshCPU buildWall(Color fill, Color trim, bool door, bool window) {
    MeshCPU m;
    const float halfW = kGrid * 0.5f;
    const float zc = kWallThick * 0.5f;

    if (!door && !window) {
        m.addBox({0, kWallH * 0.5f, zc}, {kGrid, kWallH, kWallThick}, fill);
    } else if (door) {
        const float side = (kGrid - kDoorW) * 0.5f;
        m.addBox({-(halfW - side * 0.5f), kWallH * 0.5f, zc}, {side, kWallH, kWallThick}, fill);
        m.addBox({ (halfW - side * 0.5f), kWallH * 0.5f, zc}, {side, kWallH, kWallThick}, fill);
        const float lintelH = kWallH - kDoorH;
        m.addBox({0, kDoorH + lintelH * 0.5f, zc}, {kDoorW, lintelH, kWallThick}, fill);
        m.addBox({-kDoorW * 0.5f, kDoorH * 0.5f, zc + kWallThick * 0.5f + kTrim * 0.5f},
                 {kTrim, kDoorH, kTrim}, trim);
        m.addBox({ kDoorW * 0.5f, kDoorH * 0.5f, zc + kWallThick * 0.5f + kTrim * 0.5f},
                 {kTrim, kDoorH, kTrim}, trim);
        m.addBox({0, kDoorH, zc + kWallThick * 0.5f + kTrim * 0.5f},
                 {kDoorW + kTrim, kTrim, kTrim}, trim);
    } else {
        const float side = (kGrid - kWindowW) * 0.5f;
        const float below = kWindowSill;
        const float above = kWallH - (kWindowSill + kWindowH);
        m.addBox({-(halfW - side * 0.5f), kWallH * 0.5f, zc}, {side, kWallH, kWallThick}, fill);
        m.addBox({ (halfW - side * 0.5f), kWallH * 0.5f, zc}, {side, kWallH, kWallThick}, fill);
        m.addBox({0, below * 0.5f, zc}, {kWindowW, below, kWallThick}, fill);
        m.addBox({0, kWindowSill + kWindowH + above * 0.5f, zc}, {kWindowW, above, kWallThick}, fill);
        const float fz = zc + kWallThick * 0.5f + kTrim * 0.5f;
        m.addBox({-kWindowW * 0.5f, kWindowSill + kWindowH * 0.5f, fz}, {kTrim, kWindowH, kTrim}, trim);
        m.addBox({ kWindowW * 0.5f, kWindowSill + kWindowH * 0.5f, fz}, {kTrim, kWindowH, kTrim}, trim);
        m.addBox({0, kWindowSill, fz}, {kWindowW, kTrim, kTrim}, trim);
        m.addBox({0, kWindowSill + kWindowH, fz}, {kWindowW, kTrim, kTrim}, trim);
        m.addBox({0, kWindowSill + kWindowH * 0.5f, fz}, {kTrim * 0.7f, kWindowH, kTrim}, trim);
        m.addBox({0, kWindowSill + kWindowH * 0.5f, fz}, {kWindowW, kTrim * 0.7f, kTrim}, trim);
    }

    const float fz = zc + kWallThick * 0.5f + kTrim * 0.5f;
    m.addBox({0, kWallH - kTrim * 0.5f, fz}, {kGrid, kTrim, kTrim}, trim);
    m.addBox({-halfW + kTrim * 0.5f, kWallH * 0.5f, fz}, {kTrim, kWallH, kTrim}, trim);
    m.addBox({ halfW - kTrim * 0.5f, kWallH * 0.5f, fz}, {kTrim, kWallH, kTrim}, trim);
    return m;
}

MeshCPU buildPillar(Color fill, Color trim) {
    MeshCPU m;
    m.addBox({0, kWallH * 0.5f, 0}, {kPillarW, kWallH, kPillarW}, fill);
    m.addBox({0, kTrim * 0.5f, 0}, {kPillarW + kTrim * 2, kTrim, kPillarW + kTrim * 2}, trim);
    m.addBox({0, kWallH - kTrim * 0.5f, 0}, {kPillarW + kTrim * 2, kTrim, kPillarW + kTrim * 2}, trim);
    return m;
}

MeshCPU buildRoofSlope(Color fill, Color trim) {
    MeshCPU m;
    const float half = kGrid * 0.5f;
    const float rise = kRoofRise;
    const float thick = 0.18f;

    Vector3 a  = {-half, thick, -half};
    Vector3 b  = { half, thick, -half};
    Vector3 c  = { half, rise + thick, half};
    Vector3 d  = {-half, rise + thick, half};
    Vector3 a2 = {-half, 0, -half};
    Vector3 b2 = { half, 0, -half};
    Vector3 c2 = { half, rise, half};
    Vector3 d2 = {-half, rise, half};

    m.addQuad(a, b, c, d, fill);
    m.addQuad(b2, a2, d2, c2, fill);
    m.addQuad(a2, a, d, d2, fill);
    m.addQuad(b, b2, c2, c, fill);
    m.addQuad(a2, b2, b, a, fill);
    m.addQuad(d, c, c2, d2, fill);

    m.addBox({0, thick * 0.5f, -half}, {kGrid, thick, kTrim}, trim);
    m.addBox({0, rise + thick * 0.5f, half}, {kGrid, thick, kTrim}, trim);
    return m;
}

MeshCPU buildGable(Color fill, Color trim) {
    MeshCPU m;
    // One-bay right triangle. Origin = wall-center bottom at mid-base.
    // Low at local -X (eave), high at local +X (ridge). Pitch = RoofRise/Grid.
    const float half = kGrid * 0.5f;
    const float rise = kRoofRise;
    const float z0 = 0.0f;
    const float z1 = kWallThick;

    Vector3 lo0 = {-half, 0, z0};
    Vector3 hi0 = { half, 0, z0};
    Vector3 pk0 = { half, rise, z0};
    Vector3 lo1 = {-half, 0, z1};
    Vector3 hi1 = { half, 0, z1};
    Vector3 pk1 = { half, rise, z1};

    m.addTri(lo0, hi0, pk0, fill);
    m.addTri(hi1, lo1, pk1, fill);
    m.addQuad(lo0, lo1, hi1, hi0, fill);       // base
    m.addQuad(hi0, hi1, pk1, pk0, fill);       // vertical (ridge)
    m.addQuad(lo0, pk0, pk1, lo1, fill);       // hypotenuse rake

    const float fz = z1 + kTrim * 0.5f;
    m.addBox({0, kTrim * 0.5f, fz}, {kGrid, kTrim, kTrim}, trim);
    m.addBox({half, rise * 0.5f, fz}, {kTrim, rise, kTrim}, trim);
    return m;
}

MeshCPU buildGableRamp(Color fill, Color trim) {
    MeshCPU m;
    // Trapezoid continuing a gable cascade: left height = Rise, right = 2*Rise.
    // Place at y = previousGable.y (i.e. eaveY + (ix-1)*Rise) for bay ix >= 1.
    const float half = kGrid * 0.5f;
    const float r = kRoofRise;
    const float z0 = 0.0f;
    const float z1 = kWallThick;

    Vector3 bl0 = {-half, 0, z0};
    Vector3 br0 = { half, 0, z0};
    Vector3 tr0 = { half, 2.0f * r, z0};
    Vector3 tl0 = {-half, r, z0};
    Vector3 bl1 = {-half, 0, z1};
    Vector3 br1 = { half, 0, z1};
    Vector3 tr1 = { half, 2.0f * r, z1};
    Vector3 tl1 = {-half, r, z1};

    m.addQuad(bl0, br0, tr0, tl0, fill); // exterior
    m.addQuad(br1, bl1, tl1, tr1, fill); // interior
    m.addQuad(bl0, bl1, br1, br0, fill); // base
    m.addQuad(tl0, tr0, tr1, tl1, fill); // top rake
    m.addQuad(bl0, tl0, tl1, bl1, fill); // left
    m.addQuad(br0, br1, tr1, tr0, fill); // right

    const float fz = z1 + kTrim * 0.5f;
    m.addBox({0, kTrim * 0.5f, fz}, {kGrid, kTrim, kTrim}, trim);
    return m;
}

MeshCPU buildWallRise(Color fill, Color trim) {
    MeshCPU m;
    // Short wall matching one gable/roof bay: Grid wide × RoofRise tall.
    const float halfW = kGrid * 0.5f;
    const float zc = kWallThick * 0.5f;
    const float h = kRoofRise;
    m.addBox({0, h * 0.5f, zc}, {kGrid, h, kWallThick}, fill);
    const float fz = zc + kWallThick * 0.5f + kTrim * 0.5f;
    m.addBox({0, h - kTrim * 0.5f, fz}, {kGrid, kTrim, kTrim}, trim);
    m.addBox({-halfW + kTrim * 0.5f, h * 0.5f, fz}, {kTrim, h, kTrim}, trim);
    m.addBox({ halfW - kTrim * 0.5f, h * 0.5f, fz}, {kTrim, h, kTrim}, trim);
    return m;
}

MeshCPU buildRoofPyramid(Color fill, Color trim) {
    MeshCPU m;
    const float half = kGrid + kPyramidOverhang;
    const float h = kPyramidH;
    const float thick = 0.16f;

    Vector3 n = {-half, 0, -half};
    Vector3 e = { half, 0, -half};
    Vector3 s = { half, 0,  half};
    Vector3 w = {-half, 0,  half};
    Vector3 pk = {0, h, 0};

    auto addFace = [&](Vector3 a, Vector3 b, Vector3 peak) {
        m.addTri(a, b, peak, fill);
        Vector3 a2 = {a.x * 0.92f, a.y + thick, a.z * 0.92f};
        Vector3 b2 = {b.x * 0.92f, b.y + thick, b.z * 0.92f};
        Vector3 pk2 = {peak.x, peak.y - thick, peak.z};
        m.addTri(b2, a2, pk2, fill);
    };
    addFace(n, e, pk);
    addFace(e, s, pk);
    addFace(s, w, pk);
    addFace(w, n, pk);

    m.addBox({0, thick * 0.5f, -half}, {half * 2.0f, thick, kTrim}, trim);
    m.addBox({0, thick * 0.5f,  half}, {half * 2.0f, thick, kTrim}, trim);
    m.addBox({-half, thick * 0.5f, 0}, {kTrim, thick, half * 2.0f}, trim);
    m.addBox({ half, thick * 0.5f, 0}, {kTrim, thick, half * 2.0f}, trim);
    m.addBox({0, h, 0}, {kTrim * 2.0f, kTrim, kTrim * 2.0f}, trim);
    return m;
}

MeshCPU buildStairs(Color fill, Color trim) {
    MeshCPU m;
    const int steps = 6;
    const float stepD = kGrid / (float)steps;
    const float stepH = kWallH / (float)steps;
    const float w = kGrid * 0.55f;
    for (int i = 0; i < steps; ++i) {
        float z = stepD * ((float)i + 0.5f);
        float y = stepH * ((float)i + 0.5f);
        m.addBox({0, y, z}, {w, stepH, stepD}, fill);
    }
    m.addBox({-w * 0.5f, kWallH * 0.35f, kGrid * 0.5f}, {kTrim, kWallH * 0.7f, kGrid}, trim);
    m.addBox({ w * 0.5f, kWallH * 0.35f, kGrid * 0.5f}, {kTrim, kWallH * 0.7f, kGrid}, trim);
    return m;
}

struct PieceModels {
    Model model{};
    bool  ready = false;
};

PieceModels g_pieces[(int)Piece::Count];
bool g_ready = false;

void buildPiece(Piece piece) {
    PieceModels& pm = g_pieces[(int)piece];
    Color fill = styleFill(Style::Stone);
    Color trim = styleTrim(Style::Stone);
    MeshCPU cpu;
    switch (piece) {
    case Piece::Floor:       cpu = buildFloor(fill, trim); break;
    case Piece::Wall:        cpu = buildWall(fill, trim, false, false); break;
    case Piece::WallDoor:    cpu = buildWall(fill, trim, true, false); break;
    case Piece::WallWindow:  cpu = buildWall(fill, trim, false, true); break;
    case Piece::Pillar:      cpu = buildPillar(fill, trim); break;
    case Piece::RoofSlope:   cpu = buildRoofSlope(fill, trim); break;
    case Piece::Gable:       cpu = buildGable(fill, trim); break;
    case Piece::GableRamp:   cpu = buildGableRamp(fill, trim); break;
    case Piece::WallRise:    cpu = buildWallRise(fill, trim); break;
    case Piece::RoofPyramid: cpu = buildRoofPyramid(fill, trim); break;
    case Piece::Stairs:      cpu = buildStairs(fill, trim); break;
    default:                 cpu = buildFloor(fill, trim); break;
    }
    pm.model = cpu.upload();
    bindTexture(pm.model, Style::Stone);
    pm.ready = true;
}

void unloadPiece(PieceModels& pm) {
    if (!pm.ready) return;
    clearBoundTextures(pm.model);
    UnloadModel(pm.model);
    pm = {};
}

}  // namespace

float Grid()              { return kGrid; }
float WallHeight()        { return kWallH; }
float DoorHeight()        { return kDoorH; }
float RoofRise()          { return kRoofRise; }
float RoofPyramidHeight() { return kPyramidH; }

void Init() {
    if (g_ready) return;
    initTextures();
    for (int i = 0; i < (int)Piece::Count; ++i) {
        buildPiece((Piece)i);
    }
    g_ready = true;
}

void Shutdown() {
    if (!g_ready) return;
    for (int i = 0; i < (int)Piece::Count; ++i) {
        unloadPiece(g_pieces[i]);
    }
    shutdownTextures();
    g_ready = false;
}

void Draw(Piece piece, Vector3 pos, float yawDeg, Style style, Color tint, bool mirrorX) {
    if (!g_ready) return;
    int idx = (int)piece;
    if (idx < 0 || idx >= (int)Piece::Count) return;
    Model& model = g_pieces[idx].model;
    if (model.meshCount <= 0) return;

    bindTexture(model, style);

    Matrix local = mirrorX ? MatrixScale(-1.0f, 1.0f, 1.0f) : MatrixIdentity();
    Matrix transform = MatrixMultiply(local, MatrixRotateY(yawDeg * DEG2RAD));
    transform = MatrixMultiply(transform, MatrixTranslate(pos.x, pos.y, pos.z));
    model.transform = transform;
    if (mirrorX) rlDisableBackfaceCulling();
    DrawModel(model, {0, 0, 0}, 1.0f, tint);
    if (mirrorX) rlEnableBackfaceCulling();
    model.transform = MatrixIdentity();
}

}  // namespace game::world::building_panels
