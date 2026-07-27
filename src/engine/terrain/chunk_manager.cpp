#include "engine/terrain/chunk_manager.hpp"
#include "engine/math/noise.hpp"
#include "engine/math/hydrology.hpp"
#include "engine/jobs/thread_pool.hpp"
#include "engine/render/sky.hpp"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::terrain {

// ---------------------------------------------------------------------------
// CPU-side mesh generation. Runs on any thread (no raylib GL calls here).
// ---------------------------------------------------------------------------
struct MeshCPU {
    int vertexCount   = 0;
    int triangleCount = 0;
    std::unique_ptr<float[]>          vertices;   // xyz*vertexCount
    std::unique_ptr<float[]>          normals;    // xyz*vertexCount
    std::unique_ptr<unsigned char[]>  colors;     // rgba*vertexCount (R = moisture 0..255)
    std::unique_ptr<unsigned short[]> indices;    // 3*triangleCount
    Vector3 aabbMin = {0, 0, 0};
    Vector3 aabbMax = {0, 0, 0};
};

// Generate a heightmap mesh covering [origin, origin + size] in world space.
// Resolutions are 2^n+1 so edges nest across LODs. Vertical skirts hide residual cracks.
static MeshCPU generateMeshCPU(float originX, float originZ, float sizeX, float sizeZ,
                               int resX, int resZ) {
    const float SKIRT_DEPTH = 12.0f;

    const int surfaceVerts = resX * resZ;
    const int skirtVerts   = 2 * (resX + resZ);
    const int surfaceTris  = (resX - 1) * (resZ - 1) * 2;
    const int skirtTris    = 2 * ((resX - 1) * 2 + (resZ - 1) * 2);

    MeshCPU m;
    m.vertexCount   = surfaceVerts + skirtVerts;
    m.triangleCount = surfaceTris + skirtTris;

    m.vertices = std::make_unique<float[]>(m.vertexCount * 3);
    m.normals  = std::make_unique<float[]>(m.vertexCount * 3);
    m.colors   = std::make_unique<unsigned char[]>(m.vertexCount * 4);
    m.indices  = std::make_unique<unsigned short[]>(m.triangleCount * 3);

    float minY =  1e9f, maxY = -1e9f;
    const float invXm1 = 1.0f / static_cast<float>(resX - 1);
    const float invZm1 = 1.0f / static_cast<float>(resZ - 1);

    auto setVert = [&](int i, float wx, float wy, float wz) {
        m.vertices[i * 3 + 0] = wx;
        m.vertices[i * 3 + 1] = wy;
        m.vertices[i * 3 + 2] = wz;
        if (wy < minY) minY = wy;
        if (wy > maxY) maxY = wy;
    };

    for (int z = 0; z < resZ; ++z) {
        for (int x = 0; x < resX; ++x) {
            float wx = originX + static_cast<float>(x) * invXm1 * sizeX;
            float wz = originZ + static_cast<float>(z) * invZm1 * sizeZ;
            float wy = engine::math::WorldHeight(wx, wz);
            setVert(z * resX + x, wx, wy, wz);
        }
    }

    int idx = 0;
    for (int z = 0; z < resZ - 1; ++z) {
        for (int x = 0; x < resX - 1; ++x) {
            unsigned short tl = static_cast<unsigned short>(z * resX + x);
            unsigned short tr = static_cast<unsigned short>(tl + 1);
            unsigned short bl = static_cast<unsigned short>(tl + resX);
            unsigned short br = static_cast<unsigned short>(bl + 1);
            m.indices[idx++] = tl; m.indices[idx++] = tr; m.indices[idx++] = bl;
            m.indices[idx++] = tr; m.indices[idx++] = br; m.indices[idx++] = bl;
        }
    }

    float stepX = sizeX * invXm1;
    float stepZ = sizeZ * invZm1;
    for (int z = 0; z < resZ; ++z) {
        for (int x = 0; x < resX; ++x) {
            float hL = m.vertices[(z * resX + (x > 0        ? x - 1 : x)) * 3 + 1];
            float hR = m.vertices[(z * resX + (x < resX - 1 ? x + 1 : x)) * 3 + 1];
            float hD = m.vertices[((z > 0        ? z - 1 : z) * resX + x) * 3 + 1];
            float hU = m.vertices[((z < resZ - 1 ? z + 1 : z) * resX + x) * 3 + 1];

            Vector3 n = { (hL - hR), 2.0f * ((stepX + stepZ) * 0.5f), (hD - hU) };
            float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (len > 1e-6f) { n.x /= len; n.y /= len; n.z /= len; }
            int i = z * resX + x;
            m.normals[i * 3 + 0] = n.x;
            m.normals[i * 3 + 1] = n.y;
            m.normals[i * 3 + 2] = n.z;
        }
    }

    // Pack moisture (R), primary biome id (G), waterGate (B) for splat + debug modes.
    for (int z = 0; z < resZ; ++z) {
        for (int x = 0; x < resX; ++x) {
            int i = z * resX + x;
            float wx = m.vertices[i * 3 + 0];
            float wz = m.vertices[i * 3 + 2];
            float moist = std::clamp(engine::math::Moisture(wx, wz), 0.0f, 1.0f);
            unsigned char mb = static_cast<unsigned char>(moist * 255.0f);
            unsigned char biome =
                static_cast<unsigned char>(static_cast<int>(engine::math::PrimaryRegion(wx, wz)));
            unsigned char wg =
                static_cast<unsigned char>(std::clamp(engine::math::WaterGate(wx, wz), 0.0f, 1.0f) * 255.0f);
            m.colors[i * 4 + 0] = mb;
            m.colors[i * 4 + 1] = biome; // 0..4
            m.colors[i * 4 + 2] = wg;
            m.colors[i * 4 + 3] = 255;
        }
    }

    int skirtBase = surfaceVerts;
    auto addSkirtEdge = [&](int count, auto surfaceIndexOf, bool outwardFlip) {
        int edgeStart = skirtBase;
        for (int i = 0; i < count; ++i) {
            int si = surfaceIndexOf(i);
            float wx = m.vertices[si * 3 + 0];
            float wy = m.vertices[si * 3 + 1];
            float wz = m.vertices[si * 3 + 2];
            int vi = skirtBase + i;
            setVert(vi, wx, wy - SKIRT_DEPTH, wz);
            m.normals[vi * 3 + 0] = 0.0f;
            m.normals[vi * 3 + 1] = -1.0f;
            m.normals[vi * 3 + 2] = 0.0f;
            m.colors[vi * 4 + 0] = m.colors[si * 4 + 0];
            m.colors[vi * 4 + 1] = m.colors[si * 4 + 1];
            m.colors[vi * 4 + 2] = m.colors[si * 4 + 2];
            m.colors[vi * 4 + 3] = 255;
        }
        for (int i = 0; i < count - 1; ++i) {
            unsigned short s0 = static_cast<unsigned short>(surfaceIndexOf(i));
            unsigned short s1 = static_cast<unsigned short>(surfaceIndexOf(i + 1));
            unsigned short k0 = static_cast<unsigned short>(edgeStart + i);
            unsigned short k1 = static_cast<unsigned short>(edgeStart + i + 1);
            if (!outwardFlip) {
                m.indices[idx++] = s0; m.indices[idx++] = s1; m.indices[idx++] = k0;
                m.indices[idx++] = s1; m.indices[idx++] = k1; m.indices[idx++] = k0;
            } else {
                m.indices[idx++] = s0; m.indices[idx++] = k0; m.indices[idx++] = s1;
                m.indices[idx++] = s1; m.indices[idx++] = k0; m.indices[idx++] = k1;
            }
        }
        skirtBase += count;
    };

    addSkirtEdge(resX, [&](int i) { return i; }, false);
    addSkirtEdge(resX, [&](int i) { return (resZ - 1) * resX + i; }, true);
    addSkirtEdge(resZ, [&](int i) { return i * resX; }, true);
    addSkirtEdge(resZ, [&](int i) { return i * resX + (resX - 1); }, false);

    m.aabbMin = {originX,         minY - SKIRT_DEPTH, originZ};
    m.aabbMax = {originX + sizeX, maxY,               originZ + sizeZ};
    return m;
}

// ---------------------------------------------------------------------------
// Terrain splat materials (albedo maps + custom shader)
// ---------------------------------------------------------------------------
enum TexSlot : int {
    TEX_GRASS = 0,
    TEX_FOREST,
    TEX_MUD,
    TEX_DIRT,
    TEX_DRY,
    TEX_GRAVEL,
    TEX_ROCK,
    TEX_SNOW,
    TEX_COUNT
};

static const char* kTexFiles[TEX_COUNT] = {
    "assets/textures/terrain/grass_c.png",
    "assets/textures/terrain/forest_c.png",
    "assets/textures/terrain/mud_c.png",
    "assets/textures/terrain/dirt_c.png",
    "assets/textures/terrain/dry_c.png",
    "assets/textures/terrain/gravel_c.png",
    "assets/textures/terrain/rock_c.png",
    "assets/textures/terrain/snow_c.png",
};

static const char* kTexUniformNames[TEX_COUNT] = {
    "texGrass", "texForest", "texMud", "texDirt",
    "texDry", "texGravel", "texRock", "texSnow",
};

static Shader  g_terrainShader = {};
static Texture g_textures[TEX_COUNT] = {};
static int     g_texLocs[TEX_COUNT] = {};
static int     g_locUvScale = -1;
static int     g_locSunDir  = -1;
static int     g_locHazeColor = -1;
static int     g_locAmbientCube = -1;
static int     g_locIblStrength = -1;
static int     g_locWaterCount = -1;
static int     g_locWaterBodies = -1;
static int     g_locDebugMode = -1;
static int     g_locHazeStart = -1;
static int     g_locHazeEnd = -1;
static int     g_locHazeStrength = -1;
static int     g_locSunIntensity = -1;
static bool    g_materialsReady = false;

static bool    g_showChunkBounds = false;
static int     g_terrainDebugMode = 0;
static Vector3 g_sunDir = {0.45f, 1.0f, 0.28f};
static float   g_sunIntensity = 1.0f;
static float   g_hazeStart = 1200.0f;
static float   g_hazeEnd = 6700.0f; // start + 5500
static float   g_hazeStrength = 0.40f;

static std::string makeAssetPath(const char* relative) {
    return std::string(GetApplicationDirectory()) + relative;
}

static Texture loadTerrainTexture(const char* relative) {
    std::string path = makeAssetPath(relative);
    Image img = LoadImage(path.c_str());
    if (img.data == nullptr) {
        TraceLog(LOG_WARNING, "TERRAIN: failed to load texture %s", path.c_str());
        Image fallback = GenImageColor(4, 4, MAGENTA);
        Texture t = LoadTextureFromImage(fallback);
        UnloadImage(fallback);
        GenTextureMipmaps(&t);
        SetTextureFilter(t, TEXTURE_FILTER_ANISOTROPIC_16X);
        SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
        return t;
    }
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    Texture t = LoadTextureFromImage(img);
    UnloadImage(img);
    GenTextureMipmaps(&t);
    SetTextureFilter(t, TEXTURE_FILTER_ANISOTROPIC_16X);
    SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
    return t;
}

static void loadTerrainMaterials() {
    if (g_materialsReady) return;

    std::string vs = makeAssetPath("assets/shaders/terrain.vs");
    std::string fs = makeAssetPath("assets/shaders/terrain.fs");
    g_terrainShader = LoadShader(vs.c_str(), fs.c_str());
    if (g_terrainShader.id == 0) {
        TraceLog(LOG_WARNING, "TERRAIN: splat shader failed to load  Evertex colors only");
        return;
    }

    for (int i = 0; i < TEX_COUNT; ++i) {
        g_textures[i] = loadTerrainTexture(kTexFiles[i]);
        g_texLocs[i] = GetShaderLocation(g_terrainShader, kTexUniformNames[i]);
    }
    g_locUvScale = GetShaderLocation(g_terrainShader, "uvScale");
    g_locSunDir  = GetShaderLocation(g_terrainShader, "sunDir");
    g_locHazeColor = GetShaderLocation(g_terrainShader, "hazeColor");
    g_locAmbientCube = GetShaderLocation(g_terrainShader, "ambientCube");
    g_locIblStrength = GetShaderLocation(g_terrainShader, "iblStrength");
    g_locWaterCount  = GetShaderLocation(g_terrainShader, "waterCount");
    g_locWaterBodies = GetShaderLocation(g_terrainShader, "waterBodies");
    g_locDebugMode   = GetShaderLocation(g_terrainShader, "debugMode");
    g_locHazeStart   = GetShaderLocation(g_terrainShader, "hazeStart");
    g_locHazeEnd     = GetShaderLocation(g_terrainShader, "hazeEnd");
    g_locHazeStrength = GetShaderLocation(g_terrainShader, "hazeStrength");
    g_locSunIntensity = GetShaderLocation(g_terrainShader, "sunIntensity");
    g_materialsReady = true;
    TraceLog(LOG_INFO, "TERRAIN: loaded splat shader + %d albedo maps", TEX_COUNT);
}

static void unloadTerrainMaterials() {
    if (g_materialsReady) {
        for (int i = 0; i < TEX_COUNT; ++i) {
            if (g_textures[i].id > 0) UnloadTexture(g_textures[i]);
            g_textures[i] = {};
        }
        UnloadShader(g_terrainShader);
        g_terrainShader = {};
        g_materialsReady = false;
    }
}

static void bindTerrainMaterials() {
    if (!g_materialsReady) return;
    float uvScale = 0.09f; // ~11 m per tile
    Vector3 sun = Vector3Normalize(g_sunDir);
    Vector3 haze = engine::render::sky::GetHazeColorLinear();
    float iblStrength = 0.55f;
    SetShaderValue(g_terrainShader, g_locUvScale, &uvScale, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_terrainShader, g_locSunDir, &sun, SHADER_UNIFORM_VEC3);
    if (g_locHazeColor >= 0) {
        SetShaderValue(g_terrainShader, g_locHazeColor, &haze, SHADER_UNIFORM_VEC3);
    }
    if (g_locAmbientCube >= 0 && engine::render::sky::IsReady()) {
        SetShaderValueV(g_terrainShader, g_locAmbientCube,
                        engine::render::sky::GetAmbientCube(),
                        SHADER_UNIFORM_VEC3, 6);
    }
    if (g_locIblStrength >= 0) {
        SetShaderValue(g_terrainShader, g_locIblStrength, &iblStrength, SHADER_UNIFORM_FLOAT);
    }
    if (g_locDebugMode >= 0) {
        SetShaderValue(g_terrainShader, g_locDebugMode, &g_terrainDebugMode, SHADER_UNIFORM_INT);
    }
    if (g_locHazeStart >= 0) {
        SetShaderValue(g_terrainShader, g_locHazeStart, &g_hazeStart, SHADER_UNIFORM_FLOAT);
    }
    if (g_locHazeEnd >= 0) {
        SetShaderValue(g_terrainShader, g_locHazeEnd, &g_hazeEnd, SHADER_UNIFORM_FLOAT);
    }
    if (g_locHazeStrength >= 0) {
        SetShaderValue(g_terrainShader, g_locHazeStrength, &g_hazeStrength, SHADER_UNIFORM_FLOAT);
    }
    if (g_locSunIntensity >= 0) {
        SetShaderValue(g_terrainShader, g_locSunIntensity, &g_sunIntensity, SHADER_UNIFORM_FLOAT);
    }
    // Bind splat maps to units 1..TEX_COUNT. DrawMesh always binds material diffuse on
    // unit 0, which would stomp a map placed there. Avoid SetShaderValueTexture (batch-only).
    rlEnableShader(g_terrainShader.id);
    for (int i = 0; i < TEX_COUNT; ++i) {
        if (g_textures[i].id == 0) continue;
        const int unit = i + 1;
        rlActiveTextureSlot(unit);
        rlEnableTexture(g_textures[i].id);
        if (g_texLocs[i] >= 0) {
            SetShaderValue(g_terrainShader, g_texLocs[i], &unit, SHADER_UNIFORM_INT);
        }
    }
    rlActiveTextureSlot(0);

    // Shore wetness: nearest lakes (shader supports 12)
    int count = 0;
    float bodies[12 * 4] = {};
    if (engine::math::IsHydrologyReady()) {
        const auto& lakes = engine::math::GetLakes();
        count = static_cast<int>(std::min(lakes.size(), static_cast<size_t>(12)));
        for (int i = 0; i < count; ++i) {
            bodies[i * 4 + 0] = lakes[static_cast<size_t>(i)].x;
            bodies[i * 4 + 1] = lakes[static_cast<size_t>(i)].z;
            bodies[i * 4 + 2] = lakes[static_cast<size_t>(i)].surfaceY;
            bodies[i * 4 + 3] = std::max(lakes[static_cast<size_t>(i)].boundR,
                                         lakes[static_cast<size_t>(i)].fillRadius * 1.15f);
        }
    }
    if (g_locWaterCount >= 0) {
        SetShaderValue(g_terrainShader, g_locWaterCount, &count, SHADER_UNIFORM_INT);
    }
    if (g_locWaterBodies >= 0 && count > 0) {
        SetShaderValueV(g_terrainShader, g_locWaterBodies, bodies, SHADER_UNIFORM_VEC4, count);
    }
}

static void applyTerrainShader(Model& model) {
    if (!g_materialsReady || model.materialCount < 1) return;
    model.materials[0].shader = g_terrainShader;
    // Prevent DrawMesh from binding the default white texel on unit 0 over our splat binds.
    model.materials[0].maps[MATERIAL_MAP_ALBEDO].texture = {};
}

// ---------------------------------------------------------------------------
// Instanced grass — chunk-streamed bake, single Quaternius draw distance
// ---------------------------------------------------------------------------
// Bake only for chunks whose center is within drawDist + margin (not all LOD0–1).
// Near density stays full; instances beyond kGrassFullDensityDist are hash-thinned
// at draw time (still Quaternius meshes — no far billboards).
// ---------------------------------------------------------------------------
static bool     g_grassEnabled = true;
static float    g_grassDensity = 1.0f;
static float    g_grassMaxSlope = 0.32f;
static float    g_grassDrawDist = 50.0f;        // full Quaternius meshes — keep short for FPS
// Many seeds + fewer clumps fills the floor; mega-clusters looked sparse under the cap.
static int      g_grassClusterMin = 3;
static int      g_grassClusterMax = 6;
static float    g_grassClusterRadius = 1.35f;   // max patch disk radius (m)
static float    g_grassSeedSpacing = 1.10f;     // hex lattice spacing (m)
static float    g_grassMeadowStrength = 0.0f;   // OFF by default — was carving large empty bands
static float    g_grassMeadowScale = 0.022f;    // noise frequency
// Coverage noise — soft density variation (defaults keep ~90%+ plains seeds).
static float    g_grassCoverageStrength = 0.25f;
static float    g_grassCoverageScale = 0.028f;
static float    g_grassCoverageThreshold = 0.05f;
static float    g_grassSizeNoiseScale = 0.045f; // spatial size field frequency
static float    g_grassScaleMin = 0.78f;
static float    g_grassScaleMax = 1.28f;
static float    g_grassSink = 0.02f;            // meters into terrain

struct GrassExclusionRect {
    float minX, minZ, maxX, maxZ;
};
static std::vector<GrassExclusionRect> g_grassExclusions;

static bool inGrassExclusion(float wx, float wz) {
    for (const GrassExclusionRect& r : g_grassExclusions) {
        if (wx >= r.minX && wx <= r.maxX && wz >= r.minZ && wz <= r.maxZ) return true;
    }
    return false;
}
static constexpr float kGrassWaterGateMax = 0.12f;
// Clustered patches. Cap must not abort lattice early (that caused Z-stripes).
// Cap must fit spacing×clumps or hash-thinning starves the carpet.
static constexpr int   kGrassMaxNearPerChunk = 14000;
static constexpr int   kGrassMaxDraw = 24000; // hard cap across visible chunks
static constexpr int   kGrassChunkLodMax = 1; // terrain LOD gate (still distance-culled)
static constexpr float kGrassFullDensityDist = 28.0f; // 100% keep inside this radius
static constexpr float kGrassFarKeepFrac = 0.18f;     // keep rate at drawEnd
static float    g_grassClumpHeight = 0.323f;
static constexpr float kGrassBaseScale = 1.05f;   // Quaternius clumps are ~1.2–1.4 m tall
static constexpr float kGrassFadeMeters = 25.0f; // soft fade into max draw distance
static constexpr int   kGrassVariantCount = 4;
static chunks::GrassDrawStats g_grassDrawStats = {};

struct GrassNearInst {
    Matrix  m{};
    uint8_t variant = 0;
};

struct GrassBake {
    std::vector<GrassNearInst> near; // Quaternius clumps (varianted)
};

struct GrassVariantMesh {
    Model  model{};
    Mesh*  mesh = nullptr;
    float  height = 1.0f;
    bool   loaded = false;
};

static Shader   g_grassShader = {};
static Material g_grassClumpMaterial = {};
static GrassVariantMesh g_grassVariants[kGrassVariantCount] = {};
static Texture  g_grassTexture = {};
static bool     g_grassOwnsTexture = false;
static bool     g_grassClumpIsFallback = false;
static bool     g_grassReady = false;
static int      g_grassLocTime = -1;
static int      g_grassLocViewPos = -1;
static int      g_grassLocFadeInStart = -1;
static int      g_grassLocFadeInEnd = -1;
static int      g_grassLocFadeOutStart = -1;
static int      g_grassLocFadeOutEnd = -1;
static int      g_grassLocSunDir = -1;
static int      g_grassLocSunIntensity = -1;
static int      g_grassLocMeshHeight = -1;

static void clampGrassDrawDistance() {
    // Up to far edge of terrain LOD1 (~896 m) so slider can cover baked range.
    g_grassDrawDist = std::clamp(g_grassDrawDist, 40.0f, 500.0f);
}

// Fallback multi-plane billboard (bottom-origin) — only used if Quaternius fails.
static Model makeGrassBillboardModel(bool uAtlas) {
    constexpr float W = 0.38f;
    constexpr float H = 0.34f;
    constexpr int kPlanes = 3; // 0° / 60° / 120° — denser silhouette than a 2-plane cross
    const int vertCount = kPlanes * 4;
    const int triCount = kPlanes * 2;
    Mesh mesh = {};
    mesh.vertexCount = vertCount;
    mesh.triangleCount = triCount;
    mesh.vertices = static_cast<float*>(MemAlloc(vertCount * 3 * sizeof(float)));
    mesh.texcoords = static_cast<float*>(MemAlloc(vertCount * 2 * sizeof(float)));
    mesh.normals = static_cast<float*>(MemAlloc(vertCount * 3 * sizeof(float)));
    mesh.colors = static_cast<unsigned char*>(MemAlloc(vertCount * 4 * sizeof(unsigned char)));
    mesh.indices = static_cast<unsigned short*>(MemAlloc(triCount * 3 * sizeof(unsigned short)));

    // Atlas: bottom clumps at high V after raylib/stbi upload (image top = v~0).
    // Wider UV window ≈ one clump footprint instead of a thin blade strip.
    const float u0 = uAtlas ? 0.04f : 0.0f;
    const float u1 = uAtlas ? 0.46f : 1.0f;
    const float v0 = uAtlas ? 0.99f : 1.0f; // roots
    const float v1 = uAtlas ? 0.50f : 0.0f; // tip

    auto setV = [&](int i, float x, float y, float z, float u, float v, float nx, float nz) {
        mesh.vertices[i * 3 + 0] = x;
        mesh.vertices[i * 3 + 1] = y;
        mesh.vertices[i * 3 + 2] = z;
        mesh.texcoords[i * 2 + 0] = u;
        mesh.texcoords[i * 2 + 1] = v;
        mesh.normals[i * 3 + 0] = nx;
        mesh.normals[i * 3 + 1] = 0.0f;
        mesh.normals[i * 3 + 2] = nz;
        mesh.colors[i * 4 + 0] = 180;
        mesh.colors[i * 4 + 1] = 200;
        mesh.colors[i * 4 + 2] = 90;
        mesh.colors[i * 4 + 3] = 255;
    };

    for (int p = 0; p < kPlanes; ++p) {
        const float ang = static_cast<float>(p) * (3.14159265f / 3.0f); // 60°
        const float ca = std::cos(ang);
        const float sa = std::sin(ang);
        // Quad edges along (ca, sa); facing normal ≈ (-sa, ca).
        const float hx = ca * W;
        const float hz = sa * W;
        const float nx = -sa;
        const float nz = ca;
        const int i0 = p * 4;
        setV(i0 + 0, -hx, 0.0f, -hz, u0, v0, nx, nz);
        setV(i0 + 1,  hx, 0.0f,  hz, u1, v0, nx, nz);
        setV(i0 + 2,  hx, H,     hz, u1, v1, nx, nz);
        setV(i0 + 3, -hx, H,    -hz, u0, v1, nx, nz);
        const int t = p * 6;
        mesh.indices[t + 0] = static_cast<unsigned short>(i0 + 0);
        mesh.indices[t + 1] = static_cast<unsigned short>(i0 + 1);
        mesh.indices[t + 2] = static_cast<unsigned short>(i0 + 2);
        mesh.indices[t + 3] = static_cast<unsigned short>(i0 + 0);
        mesh.indices[t + 4] = static_cast<unsigned short>(i0 + 2);
        mesh.indices[t + 5] = static_cast<unsigned short>(i0 + 3);
    }

    UploadMesh(&mesh, false);
    return LoadModelFromMesh(mesh);
}

static float measureMeshHeightY(const Mesh& mesh) {
    if (mesh.vertices == nullptr || mesh.vertexCount <= 0) return 0.323f;
    float minY = mesh.vertices[1];
    float maxY = minY;
    for (int i = 0; i < mesh.vertexCount; ++i) {
        const float y = mesh.vertices[i * 3 + 1];
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
    }
    const float h = maxY - minY;
    return (h > 0.05f) ? h : 0.323f;
}

// Shift mesh so the lowest vertex sits on Y=0 (ground pivot for instancing).
static void snapMeshToGroundPivot(Mesh& mesh) {
    if (mesh.vertices == nullptr || mesh.vertexCount <= 0) return;
    float minY = mesh.vertices[1];
    for (int i = 1; i < mesh.vertexCount; ++i) {
        minY = std::min(minY, mesh.vertices[i * 3 + 1]);
    }
    if (std::fabs(minY) < 1e-5f) return;
    for (int i = 0; i < mesh.vertexCount; ++i) {
        mesh.vertices[i * 3 + 1] -= minY;
    }
}

static void bindGrassShaderLocs() {
    g_grassShader.locs[SHADER_LOC_MATRIX_MVP] =
        GetShaderLocation(g_grassShader, "mvp");
    g_grassShader.locs[SHADER_LOC_MATRIX_MODEL] =
        GetShaderLocationAttrib(g_grassShader, "instanceTransform");
    g_grassShader.locs[SHADER_LOC_VERTEX_POSITION] =
        GetShaderLocationAttrib(g_grassShader, "vertexPosition");
    g_grassShader.locs[SHADER_LOC_VERTEX_TEXCOORD01] =
        GetShaderLocationAttrib(g_grassShader, "vertexTexCoord");
    g_grassShader.locs[SHADER_LOC_VERTEX_NORMAL] =
        GetShaderLocationAttrib(g_grassShader, "vertexNormal");
    g_grassShader.locs[SHADER_LOC_VERTEX_COLOR] =
        GetShaderLocationAttrib(g_grassShader, "vertexColor");
    g_grassShader.locs[SHADER_LOC_COLOR_DIFFUSE] =
        GetShaderLocation(g_grassShader, "colDiffuse");
    g_grassShader.locs[SHADER_LOC_MAP_DIFFUSE] =
        GetShaderLocation(g_grassShader, "texture0");

    g_grassLocTime = GetShaderLocation(g_grassShader, "uTime");
    g_grassLocViewPos = GetShaderLocation(g_grassShader, "viewPos");
    g_grassLocFadeInStart = GetShaderLocation(g_grassShader, "fadeInStart");
    g_grassLocFadeInEnd = GetShaderLocation(g_grassShader, "fadeInEnd");
    g_grassLocFadeOutStart = GetShaderLocation(g_grassShader, "fadeOutStart");
    g_grassLocFadeOutEnd = GetShaderLocation(g_grassShader, "fadeOutEnd");
    g_grassLocSunDir = GetShaderLocation(g_grassShader, "sunDir");
    g_grassLocSunIntensity = GetShaderLocation(g_grassShader, "sunIntensity");
    g_grassLocMeshHeight = GetShaderLocation(g_grassShader, "meshHeight");
}

static void loadGrassMaterials() {
    if (g_grassReady) return;
    clampGrassDrawDistance();

    std::string vs = makeAssetPath("assets/shaders/grass.vs");
    std::string fs = makeAssetPath("assets/shaders/grass.fs");
    g_grassShader = LoadShader(vs.c_str(), fs.c_str());
    if (g_grassShader.id == 0) {
        TraceLog(LOG_ERROR, "GRASS: shader failed to load (%s / %s)", vs.c_str(), fs.c_str());
        return;
    }
    bindGrassShaderLocs();
    if (g_grassShader.locs[SHADER_LOC_MATRIX_MODEL] < 0) {
        TraceLog(LOG_ERROR, "GRASS: instanceTransform attrib missing — instancing disabled");
        UnloadShader(g_grassShader);
        g_grassShader = {};
        return;
    }

    auto loadSolidGrassTex = [&]() {
        Image fallback = GenImageColor(4, 4, Color{160, 190, 80, 255});
        g_grassTexture = LoadTextureFromImage(fallback);
        UnloadImage(fallback);
        g_grassOwnsTexture = true;
    };

    // Prefer Quaternius stylized atlas; fall back to older albedo.
    std::string texPath = makeAssetPath("assets/models/grass/quaternius/textures/Grass.png");
    Image img = LoadImage(texPath.c_str());
    if (img.data == nullptr) {
        texPath = makeAssetPath("assets/models/grass/grass_albedo.png");
        img = LoadImage(texPath.c_str());
    }
    bool haveAtlas = img.data != nullptr;
    if (haveAtlas) {
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        if (img.width > 512 || img.height > 512) ImageResize(&img, 512, 512);
        g_grassTexture = LoadTextureFromImage(img);
        UnloadImage(img);
        GenTextureMipmaps(&g_grassTexture);
        SetTextureFilter(g_grassTexture, TEXTURE_FILTER_BILINEAR);
        g_grassOwnsTexture = true;
        TraceLog(LOG_INFO, "GRASS: texture %s", texPath.c_str());
    } else {
        TraceLog(LOG_WARNING, "GRASS: albedo missing, using solid green");
        loadSolidGrassTex();
    }

    static const char* kVariantPaths[kGrassVariantCount] = {
        "assets/models/grass/quaternius/Grass_Common_Short.obj",
        "assets/models/grass/quaternius/Grass_Common_Tall.obj",
        "assets/models/grass/quaternius/Grass_Wispy_Short.obj",
        "assets/models/grass/quaternius/Grass_Wispy_Tall.obj",
    };
    static const char* kVariantNames[kGrassVariantCount] = {
        "Common_Short", "Common_Tall", "Wispy_Short", "Wispy_Tall",
    };

    g_grassClumpIsFallback = false;
    int loaded = 0;
    float heightSum = 0.0f;
    for (int i = 0; i < kGrassVariantCount; ++i) {
        std::string modelPath = makeAssetPath(kVariantPaths[i]);
        Model model = LoadModel(modelPath.c_str());
        if (model.meshCount <= 0 || model.meshes == nullptr ||
            model.meshes[0].vertexCount <= 0) {
            TraceLog(LOG_WARNING, "GRASS: failed to load variant %s (%s)",
                     kVariantNames[i], modelPath.c_str());
            if (model.meshCount > 0) UnloadModel(model);
            g_grassVariants[i] = {};
            continue;
        }
        // Snap pivot on CPU then push new positions to GPU VBO 0 (positions).
        snapMeshToGroundPivot(model.meshes[0]);
        if (model.meshes[0].vboId[0] > 0 && model.meshes[0].vertices != nullptr) {
            UpdateMeshBuffer(model.meshes[0], 0, model.meshes[0].vertices,
                             model.meshes[0].vertexCount * 3 * static_cast<int>(sizeof(float)), 0);
        }
        // Drop per-model Grass.png copies — drawing uses shared g_grassTexture.
        // Never UnloadTexture the raylib default 1x1 (breaks untextured draws / landmarks).
        for (int mi = 0; mi < model.materialCount; ++mi) {
            Texture& tex = model.materials[mi].maps[MATERIAL_MAP_ALBEDO].texture;
            if (tex.id > 0 && (tex.width > 1 || tex.height > 1)) {
                UnloadTexture(tex);
            }
            tex = {};
        }

        g_grassVariants[i].model = model;
        g_grassVariants[i].mesh = &g_grassVariants[i].model.meshes[0];
        g_grassVariants[i].height = measureMeshHeightY(*g_grassVariants[i].mesh);
        g_grassVariants[i].loaded = true;
        heightSum += g_grassVariants[i].height;
        ++loaded;
        TraceLog(LOG_INFO, "GRASS: variant %s (%d verts, %d tris, h=%.3f)",
                 kVariantNames[i],
                 g_grassVariants[i].mesh->vertexCount,
                 g_grassVariants[i].mesh->triangleCount,
                 g_grassVariants[i].height);
    }

    if (loaded == 0) {
        TraceLog(LOG_ERROR, "GRASS: no Quaternius variants loaded — using billboard fallback");
        g_grassVariants[0].model = makeGrassBillboardModel(haveAtlas);
        g_grassVariants[0].mesh = &g_grassVariants[0].model.meshes[0];
        g_grassVariants[0].height = 0.34f;
        g_grassVariants[0].loaded = true;
        g_grassClumpIsFallback = true;
        loaded = 1;
        heightSum = 0.34f;
        for (int i = 1; i < kGrassVariantCount; ++i) {
            g_grassVariants[i] = g_grassVariants[0];
            g_grassVariants[i].loaded = true;
        }
    } else {
        // Fill any missing slots with Common_Short (or first loaded).
        int donor = 0;
        for (int i = 0; i < kGrassVariantCount; ++i) {
            if (g_grassVariants[i].loaded) { donor = i; break; }
        }
        for (int i = 0; i < kGrassVariantCount; ++i) {
            if (!g_grassVariants[i].loaded) {
                g_grassVariants[i].mesh = g_grassVariants[donor].mesh;
                g_grassVariants[i].height = g_grassVariants[donor].height;
                g_grassVariants[i].loaded = true; // shared mesh pointer; model empty
                TraceLog(LOG_WARNING, "GRASS: variant %d missing — sharing %s",
                         i, kVariantNames[donor]);
            }
        }
    }

    g_grassClumpHeight = heightSum / static_cast<float>(std::max(loaded, 1));

    g_grassClumpMaterial = LoadMaterialDefault();
    g_grassClumpMaterial.shader = g_grassShader;
    g_grassClumpMaterial.maps[MATERIAL_MAP_ALBEDO].texture = g_grassTexture;
    g_grassClumpMaterial.maps[MATERIAL_MAP_ALBEDO].color = WHITE;

    g_grassReady = true;
    TraceLog(LOG_INFO,
             "GRASS: Quaternius pack ready (%d variants%s) drawDist=%.0fm (bake in draw range)",
             loaded, g_grassClumpIsFallback ? ", fallback" : "", g_grassDrawDist);
}

static void unloadGrassMaterials() {
    if (!g_grassReady && g_grassShader.id == 0) return;

    auto detachMat = [](Material& mat) {
        if (!mat.maps) return;
        mat.maps[MATERIAL_MAP_ALBEDO].texture = {};
        mat.shader.id = rlGetShaderIdDefault();
        UnloadMaterial(mat);
        mat = {};
    };
    detachMat(g_grassClumpMaterial);

    auto unloadModelSafe = [](Model& model) {
        if (model.meshCount <= 0) return;
        for (int i = 0; i < model.materialCount; ++i) {
            model.materials[i].maps[MATERIAL_MAP_ALBEDO].texture = {};
            model.materials[i].shader.id = rlGetShaderIdDefault();
        }
        UnloadModel(model);
        model = {};
    };
    for (int i = 0; i < kGrassVariantCount; ++i) {
        // Only unload if this slot owns a distinct model (meshCount > 0).
        if (g_grassVariants[i].model.meshCount > 0) {
            unloadModelSafe(g_grassVariants[i].model);
        }
        g_grassVariants[i] = {};
    }
    g_grassClumpIsFallback = false;

    if (g_grassOwnsTexture && g_grassTexture.id > 0) UnloadTexture(g_grassTexture);
    g_grassTexture = {};
    g_grassOwnsTexture = false;

    if (g_grassShader.id > 0) UnloadShader(g_grassShader);
    g_grassShader = {};
    g_grassReady = false;
    g_grassClumpHeight = 0.323f;
}

static Matrix makeGrassTransform(float x, float y, float z, float yaw, float scaleY, float scaleXZ) {
    Matrix S = MatrixScale(scaleXZ, scaleY, scaleXZ);
    Matrix R = MatrixRotateY(yaw);
    Matrix T = MatrixTranslate(x, y, z);
    return MatrixMultiply(MatrixMultiply(S, R), T);
}

static inline Vector3 grassOrigin(const Matrix& m) {
    return {m.m12, m.m13, m.m14};
}

// Smooth value-noise sample at one frequency (bilinear + smoothstep).
static float valueNoise2D(float x, float z, uint64_t s) {
    const int ix = static_cast<int>(std::floor(x));
    const int iz = static_cast<int>(std::floor(z));
    const float fx = x - static_cast<float>(ix);
    const float fz = z - static_cast<float>(iz);
    const float a = engine::math::randFloat01(engine::math::hash2D(s, ix, iz));
    const float b = engine::math::randFloat01(engine::math::hash2D(s, ix + 1, iz));
    const float c = engine::math::randFloat01(engine::math::hash2D(s, ix, iz + 1));
    const float d = engine::math::randFloat01(engine::math::hash2D(s, ix + 1, iz + 1));
    const float u = fx * fx * (3.0f - 2.0f * fx);
    const float v = fz * fz * (3.0f - 2.0f * fz);
    return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}

// Soft meadow mask (0..1). Domain-rotated FBM so clearings aren't axis-aligned stripes.
static float meadowMask(float x, float z, uint64_t seed, float baseFreq) {
    // Rotate ~37° to break world-axis banding from chunk grids / value lattice.
    constexpr float c = 0.7986f; // cos
    constexpr float sRot = 0.6018f; // sin
    const float rx = x * c - z * sRot;
    const float rz = x * sRot + z * c;

    float n = 0.0f;
    float amp = 1.0f;
    float freq = std::clamp(baseFreq, 0.005f, 0.12f);
    float sum = 0.0f;
    uint64_t s = seed ^ 0xBEEF045FULL;
    for (int o = 0; o < 4; ++o) {
        // Slight per-octave shear further reduces lattice artifacts.
        const float ox = rx * freq + static_cast<float>(o) * 17.13f;
        const float oz = rz * freq * 1.07f + static_cast<float>(o) * 9.71f;
        n += valueNoise2D(ox, oz, s) * amp;
        sum += amp;
        amp *= 0.5f;
        freq *= 2.03f;
        s = engine::math::splitmix64(s);
    }
    return (sum > 1e-5f) ? (n / sum) : 0.5f;
}

// Spatial noise → grass mesh variant (biased toward short common for carpet).
// 0 Common_Short, 1 Common_Tall, 2 Wispy_Short, 3 Wispy_Tall
static int pickGrassVariant(float x, float z) {
    const uint64_t ws = engine::math::GetWorldConfig().seed;
    const float a = meadowMask(x, z, ws ^ 0x67A55EEDULL, 0.032f);
    const float b = valueNoise2D(x * 0.085f + 3.1f, z * 0.085f - 1.7f, ws ^ 0xB00B1E5ULL);
    const float t = a * 0.68f + b * 0.32f;
    if (t < 0.40f) return 0;
    if (t < 0.62f) return 2;
    if (t < 0.82f) return 1;
    return 3;
}

// Coverage field (independent seed from meadow). strength=0 → always 1 (full carpet).
static float coverageNoise(float x, float z, uint64_t worldSeed) {
    if (g_grassCoverageStrength < 1e-4f) return 1.0f;
    const float raw = meadowMask(x, z, worldSeed ^ 0xC0AEA11CULL, g_grassCoverageScale);
    return 1.0f - g_grassCoverageStrength + g_grassCoverageStrength * raw;
}

// Spatial size field remapped later into [scaleMin, scaleMax].
static float sizeNoise(float x, float z, uint64_t worldSeed) {
    return meadowMask(x, z, worldSeed ^ 0x51AEB00BULL, g_grassSizeNoiseScale);
}

// Low-freq vigor for whole-patch radius (shares size family, slower).
static float clusterVigorNoise(float x, float z, uint64_t worldSeed) {
    return meadowMask(x, z, worldSeed ^ 0xA1C05555ULL, g_grassSizeNoiseScale * 0.35f);
}

static void clampGrassClusterKnobs() {
    g_grassClusterMin = std::clamp(g_grassClusterMin, 1, 20);
    g_grassClusterMax = std::clamp(g_grassClusterMax, g_grassClusterMin, 24);
    g_grassClusterRadius = std::clamp(g_grassClusterRadius, 0.25f, 4.0f);
    g_grassSeedSpacing = std::clamp(g_grassSeedSpacing, 0.70f, 12.0f);
    g_grassMeadowStrength = std::clamp(g_grassMeadowStrength, 0.0f, 1.0f);
    g_grassMeadowScale = std::clamp(g_grassMeadowScale, 0.008f, 0.10f);
    g_grassCoverageStrength = std::clamp(g_grassCoverageStrength, 0.0f, 1.0f);
    g_grassCoverageScale = std::clamp(g_grassCoverageScale, 0.008f, 0.12f);
    g_grassCoverageThreshold = std::clamp(g_grassCoverageThreshold, 0.0f, 0.85f);
    g_grassSizeNoiseScale = std::clamp(g_grassSizeNoiseScale, 0.008f, 0.15f);
    float sMin = std::clamp(g_grassScaleMin, 0.35f, 2.5f);
    float sMax = std::clamp(g_grassScaleMax, 0.35f, 2.5f);
    if (sMax < sMin) std::swap(sMin, sMax);
    g_grassScaleMin = sMin;
    g_grassScaleMax = sMax;
    g_grassSink = std::clamp(g_grassSink, 0.0f, 0.15f);
}

// Biome / water / slope gate. Uses continuous blend weights (not PrimaryRegion
// winner-take-all), so soft Voronoi borders don't flicker into bare stripes.
static bool grassSiteOk(float wx, float wz, float maxSlope, float* biomeScaleOut) {
    if (inGrassExclusion(wx, wz)) return false;
    if (engine::math::WaterGate(wx, wz) > kGrassWaterGateMax) return false;

    const auto w = engine::math::SampleRegion(wx, wz);
    // Cover ground wherever plains (or light wetlands) dominate the blend.
    const float grassWeight = w.plains + w.wetlands * 0.55f + w.hills * 0.12f;
    if (grassWeight < 0.22f) return false; // mountains / deep water cells
    if (w.mountains > 0.55f) return false;

    if (engine::math::TerrainSlope(wx, wz) > maxSlope) return false;
    const float groundY = engine::math::WorldHeight(wx, wz);
    if (groundY < engine::math::LocalWaterLevel(wx, wz) + 0.2f) return false;

    if (biomeScaleOut) {
        *biomeScaleOut = (w.wetlands > w.plains) ? 1.10f : 1.0f;
    }
    return true;
}

// Ground one clump at (wx,wz). Re-checks water/slope; yaw from hash, scale from size noise.
static bool placeGrassClump(float wx, float wz, float maxSlope, float scaleBase,
                            float biomeScale, uint64_t h0, Matrix* out) {
    if (inGrassExclusion(wx, wz)) return false;
    if (engine::math::WaterGate(wx, wz) > kGrassWaterGateMax) return false;
    if (engine::math::TerrainSlope(wx, wz) > maxSlope) return false;
    const float groundY = engine::math::WorldHeight(wx, wz);
    if (groundY < engine::math::LocalWaterLevel(wx, wz) + 0.2f) return false;
    const float wy = groundY - g_grassSink;

    uint64_t h1 = engine::math::splitmix64(h0);
    const float yaw = engine::math::randFloat01(h1) * 6.2831853f;
    uint64_t h2 = engine::math::splitmix64(h1);
    const float s0 = g_grassScaleMin;
    const float s1 = g_grassScaleMax;
    const uint64_t worldSeed = engine::math::GetWorldConfig().seed;
    // Spatial size field + small hash jitter (~±7%) so neighbors aren't identical stamps.
    const float sn = sizeNoise(wx, wz, worldSeed);
    const float jitter = 0.93f + engine::math::randFloat01(h2) * 0.14f;
    const float scaleMul = (s0 + sn * (s1 - s0)) * biomeScale * jitter;
    const float scaleY = scaleBase * scaleMul;
    // Slight XZ variance so clumps aren't perfect cylinders.
    const float xzJitter = 0.92f +
        engine::math::randFloat01(engine::math::splitmix64(h2 ^ 0xA5ULL)) * 0.16f;
    const float scaleXZ = scaleBase * scaleMul * xzJitter;
    *out = makeGrassTransform(wx, wy, wz, yaw, scaleY, scaleXZ);
    return true;
}

struct GrassSeedCand {
    float    x = 0.0f;
    float    z = 0.0f;
    uint64_t h = 0;
    float    biomeScale = 1.0f;
    float    coverage = 1.0f;
};

// Hex lattice → filter → uniform subsample under budget → clusters.
// IMPORTANT: never abort the lattice mid-row when the instance cap fills — that
// left entire +Z halves of each chunk bare (visible world-aligned stripes).
static void fillGrassClusters(std::vector<GrassNearInst>& out, float originX, float originZ,
                              float size, float densityMul, float scaleBase,
                              int maxPerChunk, int cMin, int cMax, uint64_t seedTag,
                              bool assignVariant) {
    out.clear();
    if (densityMul < 0.05f || maxPerChunk <= 0) return;
    clampGrassClusterKnobs();

    const float sp = std::max(0.70f, g_grassSeedSpacing);
    const float rowH = sp * 0.8660254f; // √3/2
    const uint64_t worldSeed = engine::math::GetWorldConfig().seed;
    const uint64_t seed = worldSeed ^ seedTag;
    const float maxSlope = g_grassMaxSlope;
    const float meadowThresh = g_grassMeadowStrength * 0.50f;
    const float covThresh = g_grassCoverageThreshold;
    cMin = std::clamp(cMin, 1, 20);
    cMax = std::clamp(cMax, cMin, 24);
    // Keep patches overlapping neighboring seeds so the floor reads as carpet.
    const float rMax = std::max(g_grassClusterRadius, sp * 0.85f);
    const float rMin = rMax * 0.78f;
    const float jitter = sp * 0.40f;

    const float margin = jitter + 0.5f;
    const int iz0 = static_cast<int>(std::floor((originZ - margin) / rowH)) - 1;
    const int iz1 = static_cast<int>(std::floor((originZ + size + margin) / rowH)) + 1;
    const int ix0 = static_cast<int>(std::floor((originX - margin) / sp)) - 1;
    const int ix1 = static_cast<int>(std::floor((originX + size + margin) / sp)) + 1;

    std::vector<GrassSeedCand> cands;
    cands.reserve(static_cast<size_t>((iz1 - iz0 + 1) * (ix1 - ix0 + 1)));

    // Pass 1: gather every valid seed in the chunk (no instance cap here).
    for (int iz = iz0; iz <= iz1; ++iz) {
        const float rowOff = (iz & 1) ? (sp * 0.5f) : 0.0f;
        for (int ix = ix0; ix <= ix1; ++ix) {
            uint64_t h0 = engine::math::hash2D(seed, ix, iz);
            const float jx = (engine::math::randFloat01(h0) - 0.5f) * 2.0f * jitter;
            const float jz =
                (engine::math::randFloat01(engine::math::splitmix64(h0)) - 0.5f) * 2.0f * jitter;
            const float sx = static_cast<float>(ix) * sp + rowOff + jx;
            const float sz = static_cast<float>(iz) * rowH + jz;

            if (sx < originX || sx >= originX + size || sz < originZ || sz >= originZ + size) {
                continue;
            }

            if (meadowThresh > 1e-4f &&
                meadowMask(sx, sz, worldSeed, g_grassMeadowScale) < meadowThresh) {
                continue;
            }

            const float cov = coverageNoise(sx, sz, worldSeed);
            if (cov < covThresh) continue;

            float biomeScale = 1.0f;
            if (!grassSiteOk(sx, sz, maxSlope, &biomeScale)) continue;

            cands.push_back({sx, sz, h0, biomeScale, cov});
        }
    }

    if (cands.empty()) return;

    // Pass 2: if clumps would exceed the cap, keep a uniform random subset of seeds
    // (hash-ordered), NOT "first rows until full".
    // Use full densityMul (not ×0.55) so editor density actually packs the lawn.
    const float tDens = std::clamp(densityMul, 0.0f, 2.0f);
    const int avgClumps = std::max(1, (cMin + cMax) / 2);
    // Budget seeds first so thinning doesn't leave a sparse lattice.
    const int maxSeeds = std::max(1, maxPerChunk / avgClumps);

    if (static_cast<int>(cands.size()) > maxSeeds) {
        // Deterministic partial shuffle by sorting on a hash of position.
        std::sort(cands.begin(), cands.end(), [seed](const GrassSeedCand& a, const GrassSeedCand& b) {
            const uint64_t ha = engine::math::hash2D(
                seed ^ 0xA11CEULL,
                static_cast<int32_t>(std::floor(a.x * 10.0f)),
                static_cast<int32_t>(std::floor(a.z * 10.0f)));
            const uint64_t hb = engine::math::hash2D(
                seed ^ 0xA11CEULL,
                static_cast<int32_t>(std::floor(b.x * 10.0f)),
                static_cast<int32_t>(std::floor(b.z * 10.0f)));
            return ha < hb;
        });
        cands.resize(static_cast<size_t>(maxSeeds));
    }

    out.reserve(static_cast<size_t>(std::min(
        maxPerChunk, static_cast<int>(cands.size()) * (cMax + 2))));

    // Pass 3: place clusters for the (possibly thinned) seeds.
    for (const GrassSeedCand& cand : cands) {
        if (static_cast<int>(out.size()) >= maxPerChunk) break;

        // Coverage mostly affects radius; keep clump counts high for carpet.
        const float covMul = 0.70f + 0.30f * std::clamp(cand.coverage, 0.0f, 1.0f);

        uint64_t h1 = engine::math::splitmix64(cand.h ^ 0xC1A55ULL);
        const int span = std::max(0, cMax - cMin);
        const int baseN = cMin +
            static_cast<int>(engine::math::randFloat01(h1) * static_cast<float>(span + 1));
        const int bonus = static_cast<int>(std::lround(tDens * 1.5f));
        const int nClumps = std::clamp(baseN + bonus, cMin, cMax + 3);

        uint64_t h2 = engine::math::splitmix64(h1);
        const float vigor = clusterVigorNoise(cand.x, cand.z, worldSeed);
        float clusterR = rMin + engine::math::randFloat01(h2) * (rMax - rMin);
        clusterR *= covMul * (0.85f + 0.30f * vigor);

        {
            Matrix m{};
            if (placeGrassClump(cand.x, cand.z, maxSlope, scaleBase, cand.biomeScale, h2, &m)) {
                const uint8_t var = assignVariant
                    ? static_cast<uint8_t>(pickGrassVariant(cand.x, cand.z))
                    : 0;
                out.push_back({m, var});
            }
        }

        for (int c = 1; c < nClumps && static_cast<int>(out.size()) < maxPerChunk; ++c) {
            uint64_t hc = engine::math::splitmix64(h2 + static_cast<uint64_t>(c) * 0x9E37ULL);
            const float ang = engine::math::randFloat01(hc) * 6.2831853f;
            const float r = std::sqrt(engine::math::randFloat01(engine::math::splitmix64(hc))) *
                            clusterR;
            const float wx = cand.x + std::cos(ang) * r;
            const float wz = cand.z + std::sin(ang) * r;
            Matrix m{};
            if (placeGrassClump(wx, wz, maxSlope, scaleBase, cand.biomeScale, hc, &m)) {
                const uint8_t var = assignVariant
                    ? static_cast<uint8_t>(pickGrassVariant(wx, wz))
                    : 0;
                out.push_back({m, var});
            }
        }
    }
}

static void fillGrassNearClusters(std::vector<GrassNearInst>& out, float originX, float originZ,
                                  float size, float densityMul) {
    fillGrassClusters(out, originX, originZ, size, densityMul, kGrassBaseScale,
                      kGrassMaxNearPerChunk, g_grassClusterMin, g_grassClusterMax,
                      0x67A55ULL, true);
}

// Deterministic per-chunk grass bake (worker-safe).
// Only runs when chunk center is within drawDist + margin of the player.
static GrassBake generateGrassCPU(float originX, float originZ, float size, int lod,
                                  float chunkCenterDist, bool* bakedOut) {
    GrassBake bake;
    if (bakedOut) *bakedOut = false;
    if (lod > kGrassChunkLodMax) return bake;
    const float bakeR = g_grassDrawDist + size * 1.25f;
    if (chunkCenterDist > bakeR) return bake;
    const float density = std::clamp(g_grassDensity, 0.0f, 2.0f);
    if (density < 0.01f) {
        if (bakedOut) *bakedOut = true;
        return bake;
    }

    fillGrassNearClusters(bake.near, originX, originZ, size, density);
    if (bakedOut) *bakedOut = true;
    return bake;
}

// ---------------------------------------------------------------------------
// Instanced trees / undergrowth — Quaternius, chunk-streamed like grass
// ---------------------------------------------------------------------------
static bool     g_treesEnabled = true;
static float    g_treeDensity = 1.0f;
static float    g_treeMaxSlope = 0.38f;
static float    g_treeDrawDist = 220.0f;
static float    g_treeSeedSpacing = 14.0f;   // hills ~12–18; plains thinned by chance
static float    g_bushSeedSpacing = 8.0f;
static float    g_treeScaleMin = 0.85f;
static float    g_treeScaleMax = 1.25f;
static float    g_treeSink = 0.04f;
static constexpr float kNatureWaterGateMax = 0.12f;
static constexpr int   kNatureMaxTreesPerChunk = 80;
static constexpr int   kNatureMaxBushesPerChunk = 200;
static constexpr int   kNatureMaxDraw = 6000;
static constexpr int   kNatureChunkLodMax = 1;
static constexpr float kNatureFadeMeters = 40.0f;
static constexpr float kTreeBaseScale = 1.0f;
static constexpr float kBushBaseScale = 0.95f;
static chunks::TreeDrawStats g_treeDrawStats = {};

// 0–4 CommonTree, 5–9 Pine, 10–12 Dead, 13–14 Twisted, 15–21 undergrowth
static constexpr int kNatureVariantCount = 22;

struct NatureInst {
    Matrix  m{};
    uint8_t variant = 0;
};

struct NatureBake {
    std::vector<NatureInst> trees;
    std::vector<NatureInst> bushes;
};

struct NatureVariant {
    Model model{};
    float height = 1.0f;
    bool  loaded = false;
};

static Shader g_foliageShader = {};
static NatureVariant g_natureVariants[kNatureVariantCount] = {};
static bool g_natureReady = false;
static std::unordered_map<std::string, Texture> g_natureTexCache;
static int  g_foliageLocViewPos = -1;
static int  g_foliageLocFadeInStart = -1;
static int  g_foliageLocFadeInEnd = -1;
static int  g_foliageLocFadeOutStart = -1;
static int  g_foliageLocFadeOutEnd = -1;
static int  g_foliageLocSunDir = -1;
static int  g_foliageLocSunIntensity = -1;

static void clampTreeKnobs() {
    g_treeDrawDist = std::clamp(g_treeDrawDist, 60.0f, 500.0f);
    g_treeDensity = std::clamp(g_treeDensity, 0.0f, 2.0f);
    g_treeMaxSlope = std::clamp(g_treeMaxSlope, 0.05f, 1.0f);
    g_treeSeedSpacing = std::clamp(g_treeSeedSpacing, 6.0f, 50.0f);
    g_bushSeedSpacing = std::clamp(g_bushSeedSpacing, 3.0f, 30.0f);
    float sMin = std::clamp(g_treeScaleMin, 0.35f, 2.5f);
    float sMax = std::clamp(g_treeScaleMax, 0.35f, 2.5f);
    if (sMax < sMin) std::swap(sMin, sMax);
    g_treeScaleMin = sMin;
    g_treeScaleMax = sMax;
    g_treeSink = std::clamp(g_treeSink, 0.0f, 0.25f);
}

static void bindFoliageShaderLocs() {
    g_foliageShader.locs[SHADER_LOC_MATRIX_MVP] =
        GetShaderLocation(g_foliageShader, "mvp");
    g_foliageShader.locs[SHADER_LOC_MATRIX_MODEL] =
        GetShaderLocationAttrib(g_foliageShader, "instanceTransform");
    g_foliageShader.locs[SHADER_LOC_VERTEX_POSITION] =
        GetShaderLocationAttrib(g_foliageShader, "vertexPosition");
    g_foliageShader.locs[SHADER_LOC_VERTEX_TEXCOORD01] =
        GetShaderLocationAttrib(g_foliageShader, "vertexTexCoord");
    g_foliageShader.locs[SHADER_LOC_VERTEX_NORMAL] =
        GetShaderLocationAttrib(g_foliageShader, "vertexNormal");
    g_foliageShader.locs[SHADER_LOC_VERTEX_COLOR] =
        GetShaderLocationAttrib(g_foliageShader, "vertexColor");
    g_foliageShader.locs[SHADER_LOC_COLOR_DIFFUSE] =
        GetShaderLocation(g_foliageShader, "colDiffuse");
    g_foliageShader.locs[SHADER_LOC_MAP_DIFFUSE] =
        GetShaderLocation(g_foliageShader, "texture0");

    g_foliageLocViewPos = GetShaderLocation(g_foliageShader, "viewPos");
    g_foliageLocFadeInStart = GetShaderLocation(g_foliageShader, "fadeInStart");
    g_foliageLocFadeInEnd = GetShaderLocation(g_foliageShader, "fadeInEnd");
    g_foliageLocFadeOutStart = GetShaderLocation(g_foliageShader, "fadeOutStart");
    g_foliageLocFadeOutEnd = GetShaderLocation(g_foliageShader, "fadeOutEnd");
    g_foliageLocSunDir = GetShaderLocation(g_foliageShader, "sunDir");
    g_foliageLocSunIntensity = GetShaderLocation(g_foliageShader, "sunIntensity");
}

static float measureModelHeightY(const Model& model) {
    float minY = 1e9f;
    float maxY = -1e9f;
    bool any = false;
    for (int mi = 0; mi < model.meshCount; ++mi) {
        const Mesh& mesh = model.meshes[mi];
        if (mesh.vertices == nullptr || mesh.vertexCount <= 0) continue;
        for (int i = 0; i < mesh.vertexCount; ++i) {
            const float y = mesh.vertices[i * 3 + 1];
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
            any = true;
        }
    }
    if (!any) return 1.0f;
    const float h = maxY - minY;
    return (h > 0.05f) ? h : 1.0f;
}

static void snapModelToGroundPivot(Model& model) {
    float minY = 1e9f;
    bool any = false;
    for (int mi = 0; mi < model.meshCount; ++mi) {
        Mesh& mesh = model.meshes[mi];
        if (mesh.vertices == nullptr || mesh.vertexCount <= 0) continue;
        for (int i = 0; i < mesh.vertexCount; ++i) {
            minY = std::min(minY, mesh.vertices[i * 3 + 1]);
            any = true;
        }
    }
    if (!any || std::fabs(minY) < 1e-5f) return;
    for (int mi = 0; mi < model.meshCount; ++mi) {
        Mesh& mesh = model.meshes[mi];
        if (mesh.vertices == nullptr || mesh.vertexCount <= 0) continue;
        for (int i = 0; i < mesh.vertexCount; ++i) {
            mesh.vertices[i * 3 + 1] -= minY;
        }
        if (mesh.vboId != nullptr && mesh.vboId[0] > 0) {
            UpdateMeshBuffer(mesh, 0, mesh.vertices,
                             mesh.vertexCount * 3 * static_cast<int>(sizeof(float)), 0);
        }
    }
}

static void loadNatureMaterials() {
    if (g_natureReady) return;
    clampTreeKnobs();

    std::string vs = makeAssetPath("assets/shaders/foliage.vs");
    std::string fs = makeAssetPath("assets/shaders/foliage.fs");
    g_foliageShader = LoadShader(vs.c_str(), fs.c_str());
    if (g_foliageShader.id == 0) {
        TraceLog(LOG_ERROR, "TREES: foliage shader failed (%s / %s)", vs.c_str(), fs.c_str());
        return;
    }
    bindFoliageShaderLocs();
    if (g_foliageShader.locs[SHADER_LOC_MATRIX_MODEL] < 0) {
        TraceLog(LOG_ERROR, "TREES: instanceTransform attrib missing — trees disabled");
        UnloadShader(g_foliageShader);
        g_foliageShader = {};
        return;
    }

    static const char* kNaturePaths[kNatureVariantCount] = {
        "assets/models/nature/quaternius/CommonTree_1.obj",
        "assets/models/nature/quaternius/CommonTree_2.obj",
        "assets/models/nature/quaternius/CommonTree_3.obj",
        "assets/models/nature/quaternius/CommonTree_4.obj",
        "assets/models/nature/quaternius/CommonTree_5.obj",
        "assets/models/nature/quaternius/Pine_1.obj",
        "assets/models/nature/quaternius/Pine_2.obj",
        "assets/models/nature/quaternius/Pine_3.obj",
        "assets/models/nature/quaternius/Pine_4.obj",
        "assets/models/nature/quaternius/Pine_5.obj",
        "assets/models/nature/quaternius/DeadTree_1.obj",
        "assets/models/nature/quaternius/DeadTree_2.obj",
        "assets/models/nature/quaternius/DeadTree_3.obj",
        "assets/models/nature/quaternius/TwistedTree_1.obj",
        "assets/models/nature/quaternius/TwistedTree_2.obj",
        "assets/models/nature/quaternius/Bush_Common.obj",
        "assets/models/nature/quaternius/Bush_Common_Flowers.obj",
        "assets/models/nature/quaternius/Clover_1.obj",
        "assets/models/nature/quaternius/Clover_2.obj",
        "assets/models/nature/quaternius/Fern_1.obj",
        "assets/models/nature/quaternius/Plant_1.obj",
        "assets/models/nature/quaternius/Plant_7.obj",
    };

    auto cachedNatureTex = [](const char* relPath) -> Texture {
        const std::string key(relPath);
        auto it = g_natureTexCache.find(key);
        if (it != g_natureTexCache.end()) return it->second;
        Texture t = LoadTexture(makeAssetPath(relPath).c_str());
        if (t.id > 0) {
            GenTextureMipmaps(&t);
            SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
        }
        g_natureTexCache.emplace(key, t);
        return t;
    };

    // Shared albedo paths (relative to assets/) — avoids N× duplicate VRAM uploads.
    auto assignSharedAlbedos = [&](Model& model, int variant) {
        for (int mi = 0; mi < model.materialCount; ++mi) {
            Texture& tex = model.materials[mi].maps[MATERIAL_MAP_ALBEDO].texture;
            // Skip raylib's default 1x1 white — UnloadTexture(id=1) nukes all untextured draws.
            if (tex.id > 0 && (tex.width > 1 || tex.height > 1)) {
                UnloadTexture(tex);
            }
            tex = {};
        }
        auto setMat = [&](int mi, const char* rel) {
            if (mi < 0 || mi >= model.materialCount) return;
            model.materials[mi].maps[MATERIAL_MAP_ALBEDO].texture = cachedNatureTex(rel);
        };
        if (variant < 5) {
            setMat(0, "assets/models/nature/quaternius/textures/Bark_NormalTree.png");
            setMat(1, "assets/models/nature/quaternius/textures/Leaves_NormalTree_C.png");
        } else if (variant < 10) {
            setMat(0, "assets/models/nature/quaternius/textures/Bark_NormalTree.png");
            setMat(1, "assets/models/nature/quaternius/textures/Leaf_Pine_C.png");
        } else if (variant < 13) {
            setMat(0, "assets/models/nature/quaternius/textures/Bark_DeadTree.png");
        } else if (variant < 15) {
            setMat(0, "assets/models/nature/quaternius/textures/Bark_TwistedTree.png");
            setMat(1, "assets/models/nature/quaternius/textures/Leaves_TwistedTree_C.png");
        } else if (variant == 15) {
            setMat(0, "assets/models/nature/quaternius/textures/Leaves_TwistedTree_C.png");
        } else if (variant == 16) {
            setMat(0, "assets/models/nature/quaternius/textures/Flowers.png");
            setMat(1, "assets/models/nature/quaternius/textures/Leaves_NormalTree_C.png");
        } else {
            setMat(0, "assets/models/nature/quaternius/textures/Leaves.png");
        }
    };

    int loaded = 0;
    for (int i = 0; i < kNatureVariantCount; ++i) {
        std::string modelPath = makeAssetPath(kNaturePaths[i]);
        Model model = LoadModel(modelPath.c_str());
        if (model.meshCount <= 0 || model.meshes == nullptr) {
            TraceLog(LOG_WARNING, "TREES: failed to load %s", modelPath.c_str());
            if (model.meshCount > 0) UnloadModel(model);
            g_natureVariants[i] = {};
            continue;
        }
        snapModelToGroundPivot(model);
        assignSharedAlbedos(model, i);
        for (int mi = 0; mi < model.materialCount; ++mi) {
            model.materials[mi].shader = g_foliageShader;
        }
        g_natureVariants[i].model = model;
        g_natureVariants[i].height = measureModelHeightY(model);
        g_natureVariants[i].loaded = true;
        ++loaded;
        TraceLog(LOG_INFO, "TREES: variant %d meshes=%d h=%.2f (%s)",
                 i, model.meshCount, g_natureVariants[i].height, kNaturePaths[i]);
    }

    // Fill missing slots from a donor in the same family when possible.
    auto donorFor = [](int i) -> int {
        if (i < 5) return 0;
        if (i < 10) return 5;
        if (i < 13) return 10;
        if (i < 15) return 13;
        return 15;
    };
    for (int i = 0; i < kNatureVariantCount; ++i) {
        if (g_natureVariants[i].loaded) continue;
        const int d = donorFor(i);
        if (!g_natureVariants[d].loaded) {
            // Find any loaded
            for (int j = 0; j < kNatureVariantCount; ++j) {
                if (g_natureVariants[j].loaded) {
                    g_natureVariants[i].model = {};
                    g_natureVariants[i].model.meshCount = 0;
                    // Share by shallow copy of mesh pointers is unsafe for Unload —
                    // leave unloaded and skip at draw.
                    break;
                }
            }
            continue;
        }
        // Don't share Model ownership; leave empty (draw skips unloaded).
        TraceLog(LOG_WARNING, "TREES: variant %d missing", i);
    }

    g_natureReady = loaded > 0;
    TraceLog(LOG_INFO, "TREES: ready (%d/%d variants) drawDist=%.0fm",
             loaded, kNatureVariantCount, g_treeDrawDist);
}

static void unloadNatureMaterials() {
    if (!g_natureReady && g_foliageShader.id == 0) return;
    for (int i = 0; i < kNatureVariantCount; ++i) {
        if (g_natureVariants[i].model.meshCount > 0) {
            for (int mi = 0; mi < g_natureVariants[i].model.materialCount; ++mi) {
                // Shared cache owns textures — detach before UnloadModel.
                g_natureVariants[i].model.materials[mi].maps[MATERIAL_MAP_ALBEDO].texture = {};
                g_natureVariants[i].model.materials[mi].shader.id = rlGetShaderIdDefault();
            }
            UnloadModel(g_natureVariants[i].model);
        }
        g_natureVariants[i] = {};
    }
    for (auto& [key, tex] : g_natureTexCache) {
        (void)key;
        if (tex.id > 0) UnloadTexture(tex);
    }
    g_natureTexCache.clear();
    if (g_foliageShader.id > 0) UnloadShader(g_foliageShader);
    g_foliageShader = {};
    g_natureReady = false;
}

static Matrix makeNatureTransform(float x, float y, float z, float yaw, float scale) {
    Matrix S = MatrixScale(scale, scale, scale);
    Matrix R = MatrixRotateY(yaw);
    Matrix T = MatrixTranslate(x, y, z);
    return MatrixMultiply(MatrixMultiply(S, R), T);
}

static bool inExclusionPad(float wx, float wz) {
    const auto& ex = engine::math::GetHydrologyExclusions();
    for (const auto& e : ex) {
        const float dx = wx - e.x;
        const float dz = wz - e.z;
        const float r = e.radius;
        if (dx * dx + dz * dz < r * r) return true;
    }
    return false;
}

struct NatureSiteInfo {
    float plains = 0.0f;
    float hills = 0.0f;
    float mountains = 0.0f;
    float wetlands = 0.0f;
    float foothill = 0.0f;
};

static bool natureSiteOk(float wx, float wz, float maxSlope, bool forTree, NatureSiteInfo* out) {
    if (engine::math::WaterGate(wx, wz) > kNatureWaterGateMax) return false;
    if (inExclusionPad(wx, wz)) return false;

    const auto w = engine::math::SampleRegion(wx, wz);
    if (w.water > 0.40f) return false;
    if (w.mountains > 0.55f) return false; // steep peaks
    if (engine::math::TerrainSlope(wx, wz) > maxSlope) return false;
    const float groundY = engine::math::WorldHeight(wx, wz);
    if (groundY < engine::math::LocalWaterLevel(wx, wz) + 0.35f) return false;

    const float foothill =
        (w.mountains > 0.12f && w.mountains <= 0.55f) ? w.mountains : 0.0f;
    const float land = w.plains + w.hills + foothill + w.wetlands * 0.55f;
    if (land < 0.18f) return false;

    if (forTree) {
        // Wetlands: bushes/ferns only (very few trees handled by spawn chance).
        if (w.wetlands > 0.60f && w.plains < 0.20f && w.hills < 0.20f && foothill < 0.10f) {
            return false;
        }
    }

    if (out) {
        out->plains = w.plains;
        out->hills = w.hills;
        out->mountains = w.mountains;
        out->wetlands = w.wetlands;
        out->foothill = foothill;
    }
    return true;
}

static int pickTreeVariant(const NatureSiteInfo& site, uint64_t h) {
    const float rare = engine::math::randFloat01(h);
    uint64_t h1 = engine::math::splitmix64(h);
    if (rare < 0.025f) {
        return 10 + static_cast<int>(engine::math::randFloat01(h1) * 3.0f) % 3;
    }
    if (rare < 0.045f) {
        return 13 + static_cast<int>(engine::math::randFloat01(h1) * 2.0f) % 2;
    }
    const float pineBias = std::clamp(site.hills * 0.32f + site.foothill * 0.82f, 0.0f, 0.92f);
    uint64_t h2 = engine::math::splitmix64(h1);
    if (engine::math::randFloat01(h2) < pineBias) {
        return 5 + static_cast<int>(engine::math::randFloat01(engine::math::splitmix64(h2)) * 5.0f) % 5;
    }
    return static_cast<int>(engine::math::randFloat01(engine::math::splitmix64(h2 ^ 0xC0FFEE11ULL)) * 5.0f) % 5;
}

static int pickBushVariant(const NatureSiteInfo& site, uint64_t h) {
    const float t = engine::math::randFloat01(h);
    // Wetlands prefer fern/clover/plant; hills/plains prefer bushes.
    if (site.wetlands > 0.40f) {
        if (t < 0.35f) return 19; // Fern
        if (t < 0.55f) return 17; // Clover_1
        if (t < 0.70f) return 18; // Clover_2
        if (t < 0.85f) return 20; // Plant_1
        return 21;                // Plant_7
    }
    if (t < 0.42f) return 15; // Bush_Common
    if (t < 0.58f) return 16; // Bush_Common_Flowers
    if (t < 0.72f) return 17; // Clover_1
    if (t < 0.82f) return 18; // Clover_2
    if (t < 0.92f) return 19; // Fern
    if (t < 0.97f) return 20; // Plant_1
    return 21;                // Plant_7
}

static float treeSpawnChance(const NatureSiteInfo& site) {
    // Plains sparse (~8–15% seeds), hills moderate (~40–60%), foothills present.
    float c = site.plains * 0.12f + site.hills * 0.52f + site.foothill * 0.42f;
    c += site.wetlands * 0.03f; // almost none in wetlands
    return std::clamp(c * g_treeDensity, 0.0f, 0.90f);
}

static float bushSpawnChance(const NatureSiteInfo& site) {
    float c = site.plains * 0.18f + site.hills * 0.48f + site.wetlands * 0.55f +
              site.foothill * 0.22f;
    return std::clamp(c * g_treeDensity, 0.0f, 0.92f);
}

static bool placeNatureInstance(float wx, float wz, float maxSlope, float baseScale,
                                uint64_t h0, Matrix* out) {
    if (engine::math::WaterGate(wx, wz) > kNatureWaterGateMax) return false;
    if (engine::math::TerrainSlope(wx, wz) > maxSlope) return false;
    const float groundY = engine::math::WorldHeight(wx, wz);
    if (groundY < engine::math::LocalWaterLevel(wx, wz) + 0.35f) return false;
    const float wy = groundY - g_treeSink;

    uint64_t h1 = engine::math::splitmix64(h0);
    const float yaw = engine::math::randFloat01(h1) * 6.2831853f;
    uint64_t h2 = engine::math::splitmix64(h1);
    const float sn = meadowMask(wx, wz, engine::math::GetWorldConfig().seed ^ 0x7EEE5ULL, 0.028f);
    const float jitter = 0.94f + engine::math::randFloat01(h2) * 0.12f;
    const float scale =
        baseScale * (g_treeScaleMin + sn * (g_treeScaleMax - g_treeScaleMin)) * jitter;
    *out = makeNatureTransform(wx, wy, wz, yaw, scale);
    return true;
}

static void fillNatureLattice(std::vector<NatureInst>& out, float originX, float originZ,
                              float size, float spacing, int maxPerChunk, bool forTrees,
                              uint64_t seedTag) {
    out.clear();
    if (maxPerChunk <= 0 || g_treeDensity < 0.01f) return;
    clampTreeKnobs();

    const float sp = std::max(forTrees ? 6.0f : 3.0f, spacing);
    const float rowH = sp * 0.8660254f;
    const uint64_t worldSeed = engine::math::GetWorldConfig().seed;
    const uint64_t seed = worldSeed ^ seedTag;
    const float maxSlope = g_treeMaxSlope;
    const float jitter = sp * 0.35f;
    const float margin = jitter + 0.5f;
    const float baseScale = forTrees ? kTreeBaseScale : kBushBaseScale;

    const int iz0 = static_cast<int>(std::floor((originZ - margin) / rowH)) - 1;
    const int iz1 = static_cast<int>(std::floor((originZ + size + margin) / rowH)) + 1;
    const int ix0 = static_cast<int>(std::floor((originX - margin) / sp)) - 1;
    const int ix1 = static_cast<int>(std::floor((originX + size + margin) / sp)) + 1;

    struct Cand {
        float x, z;
        uint64_t h;
        NatureSiteInfo site;
    };
    std::vector<Cand> cands;
    cands.reserve(static_cast<size_t>((iz1 - iz0 + 1) * (ix1 - ix0 + 1)));

    for (int iz = iz0; iz <= iz1; ++iz) {
        const float rowOff = (iz & 1) ? (sp * 0.5f) : 0.0f;
        for (int ix = ix0; ix <= ix1; ++ix) {
            uint64_t h0 = engine::math::hash2D(seed, ix, iz);
            const float jx = (engine::math::randFloat01(h0) - 0.5f) * 2.0f * jitter;
            const float jz =
                (engine::math::randFloat01(engine::math::splitmix64(h0)) - 0.5f) * 2.0f * jitter;
            const float sx = static_cast<float>(ix) * sp + rowOff + jx;
            const float sz = static_cast<float>(iz) * rowH + jz;
            if (sx < originX || sx >= originX + size || sz < originZ || sz >= originZ + size) {
                continue;
            }

            NatureSiteInfo site{};
            if (!natureSiteOk(sx, sz, maxSlope, forTrees, &site)) continue;

            const float chance = forTrees ? treeSpawnChance(site) : bushSpawnChance(site);
            if (engine::math::randFloat01(engine::math::splitmix64(h0 ^ 0x51A11ULL)) > chance) {
                continue;
            }

            cands.push_back({sx, sz, h0, site});
        }
    }

    if (cands.empty()) return;

    if (static_cast<int>(cands.size()) > maxPerChunk) {
        std::sort(cands.begin(), cands.end(), [seed](const Cand& a, const Cand& b) {
            const uint64_t ha = engine::math::hash2D(
                seed ^ 0xA11CEULL,
                static_cast<int32_t>(std::floor(a.x * 10.0f)),
                static_cast<int32_t>(std::floor(a.z * 10.0f)));
            const uint64_t hb = engine::math::hash2D(
                seed ^ 0xA11CEULL,
                static_cast<int32_t>(std::floor(b.x * 10.0f)),
                static_cast<int32_t>(std::floor(b.z * 10.0f)));
            return ha < hb;
        });
        cands.resize(static_cast<size_t>(maxPerChunk));
    }

    out.reserve(cands.size());
    for (const Cand& cand : cands) {
        Matrix m{};
        if (!placeNatureInstance(cand.x, cand.z, maxSlope, baseScale, cand.h, &m)) continue;
        const int var = forTrees ? pickTreeVariant(cand.site, cand.h ^ 0x7EEEULL)
                                 : pickBushVariant(cand.site, cand.h ^ 0xB055ULL);
        out.push_back({m, static_cast<uint8_t>(std::clamp(var, 0, kNatureVariantCount - 1))});
    }
}

static NatureBake generateNatureCPU(float originX, float originZ, float size, int lod,
                                    float chunkCenterDist, bool* bakedOut) {
    NatureBake bake;
    if (bakedOut) *bakedOut = false;
    if (lod > kNatureChunkLodMax) return bake;
    const float bakeR = g_treeDrawDist + size * 1.25f;
    if (chunkCenterDist > bakeR) return bake;
    if (g_treeDensity < 0.01f) {
        if (bakedOut) *bakedOut = true;
        return bake;
    }

    fillNatureLattice(bake.trees, originX, originZ, size, g_treeSeedSpacing,
                      kNatureMaxTreesPerChunk, true, 0x7EEE0001ULL);
    fillNatureLattice(bake.bushes, originX, originZ, size, g_bushSeedSpacing,
                      kNatureMaxBushesPerChunk, false, 0xB0550002ULL);
    if (bakedOut) *bakedOut = true;
    return bake;
}

// ---------------------------------------------------------------------------
// GPU upload. MAIN THREAD ONLY (raylib calls into OpenGL).
// ---------------------------------------------------------------------------
static Model uploadMeshToGPU(const MeshCPU& cpu) {
    Mesh mesh = {};
    mesh.vertexCount   = cpu.vertexCount;
    mesh.triangleCount = cpu.triangleCount;

    size_t vSize = cpu.vertexCount * 3 * sizeof(float);
    size_t cSize = cpu.vertexCount * 4 * sizeof(unsigned char);
    size_t iSize = cpu.triangleCount * 3 * sizeof(unsigned short);
    size_t tSize = cpu.vertexCount * 2 * sizeof(float);

    mesh.vertices  = static_cast<float*>(MemAlloc(static_cast<unsigned int>(vSize)));
    mesh.normals   = static_cast<float*>(MemAlloc(static_cast<unsigned int>(vSize)));
    mesh.colors    = static_cast<unsigned char*>(MemAlloc(static_cast<unsigned int>(cSize)));
    mesh.indices   = static_cast<unsigned short*>(MemAlloc(static_cast<unsigned int>(iSize)));
    mesh.texcoords = static_cast<float*>(MemAlloc(static_cast<unsigned int>(tSize)));

    std::memcpy(mesh.vertices, cpu.vertices.get(), vSize);
    std::memcpy(mesh.normals,  cpu.normals.get(),  vSize);
    std::memcpy(mesh.colors,   cpu.colors.get(),   cSize);
    std::memcpy(mesh.indices,  cpu.indices.get(),  iSize);

    // World-XZ UVs (shader primarily uses fragPosition.xz; keep valid UVs for raylib)
    for (int i = 0; i < cpu.vertexCount; ++i) {
        mesh.texcoords[i * 2 + 0] = cpu.vertices[i * 3 + 0] * 0.09f;
        mesh.texcoords[i * 2 + 1] = cpu.vertices[i * 3 + 2] * 0.09f;
    }

    UploadMesh(&mesh, false);
    Model model = LoadModelFromMesh(mesh);
    applyTerrainShader(model);
    return model;
}

// ---------------------------------------------------------------------------
// Streaming state
// ---------------------------------------------------------------------------
static inline uint64_t packCoord(int32_t x, int32_t z) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32)
         |  static_cast<uint64_t>(static_cast<uint32_t>(z));
}

struct ChunkSlot {
    ChunkCoord coord = {0, 0};
    Model      model = {};
    bool       modelLoaded = false;
    int        lod = 0;
    Vector3    aabbMin = {0, 0, 0};
    Vector3    aabbMax = {0, 0, 0};
    GrassBake  grass;
    NatureBake nature;
    bool       grassBaked = false;   // attempted bake while in grass range
    bool       natureBaked = false;  // attempted bake while in tree range
    // Pre-bucketed on worker — main thread only moves these.
    std::vector<Matrix> grassDraw[kGrassVariantCount];
    std::vector<Matrix> natureDraw[kNatureVariantCount];
};

static void bucketGrassDraw(const GrassBake& grass, std::vector<Matrix> out[kGrassVariantCount]) {
    for (int v = 0; v < kGrassVariantCount; ++v) out[v].clear();
    for (const GrassNearInst& inst : grass.near) {
        const int v = std::clamp(static_cast<int>(inst.variant), 0, kGrassVariantCount - 1);
        out[v].push_back(inst.m);
    }
    for (int v = 0; v < kGrassVariantCount; ++v) out[v].shrink_to_fit();
}

static void bucketNatureDraw(const NatureBake& nature, std::vector<Matrix> out[kNatureVariantCount]) {
    for (int v = 0; v < kNatureVariantCount; ++v) out[v].clear();
    auto bucket = [&](const std::vector<NatureInst>& list) {
        for (const NatureInst& inst : list) {
            const int v = std::clamp(static_cast<int>(inst.variant), 0, kNatureVariantCount - 1);
            out[v].push_back(inst.m);
        }
    };
    bucket(nature.trees);
    bucket(nature.bushes);
    for (int v = 0; v < kNatureVariantCount; ++v) out[v].shrink_to_fit();
}

static std::unordered_map<uint64_t, std::unique_ptr<ChunkSlot>> g_chunks;

// Cross-thread queue of finished CPU meshes waiting for GPU upload.
struct PendingUpload {
    ChunkCoord coord;
    int        lod;
    MeshCPU    mesh;
    GrassBake  grass;
    NatureBake nature;
    bool       grassBaked = false;
    bool       natureBaked = false;
    std::vector<Matrix> grassDraw[kGrassVariantCount];
    std::vector<Matrix> natureDraw[kNatureVariantCount];
};
static std::mutex                 g_pendingMutex;
static std::vector<PendingUpload> g_pendingUploads;

// Chunk coords currently in-flight on the thread pool. Prevents double-enqueue.
static std::unordered_set<uint64_t> g_inFlight;

// GPU upload budget per frame — mesh UploadMesh only (draw lists built on worker).
static constexpr int GPU_UPLOAD_BUDGET_PER_FRAME = 2;
// Don't enqueue the entire LOAD_RADIUS ring at once — that stalls startup for minutes.
static constexpr int MAX_CHUNK_GEN_IN_FLIGHT = 8;
static constexpr int MAX_CHUNK_GEN_SUBMIT_PER_FRAME = 2;

// ---------------------------------------------------------------------------
// Far horizon ring — cheap silhouette beyond LOAD_RADIUS (mountains without
// streaming thousands of real chunks).
// ---------------------------------------------------------------------------
static Model g_horizonModel = {};
static bool  g_horizonLoaded = false;
static int   g_horizonAnchorCx = std::numeric_limits<int>::max();
static int   g_horizonAnchorCz = std::numeric_limits<int>::max();
static constexpr int kHorizonAng = 96;
static constexpr int kHorizonRad = 6;

static MeshCPU generateHorizonRingCPU(float centerX, float centerZ,
                                      float rInner, float rOuter) {
    const int nA = kHorizonAng;
    const int nR = kHorizonRad + 1;
    MeshCPU m;
    m.vertexCount = nA * nR;
    m.triangleCount = nA * kHorizonRad * 2;
    m.vertices = std::make_unique<float[]>(m.vertexCount * 3);
    m.normals  = std::make_unique<float[]>(m.vertexCount * 3);
    m.colors   = std::make_unique<unsigned char[]>(m.vertexCount * 4);
    m.indices  = std::make_unique<unsigned short[]>(m.triangleCount * 3);

    float minY = 1e9f, maxY = -1e9f;
    constexpr float kPi2 = 6.28318530718f;
    for (int ir = 0; ir < nR; ++ir) {
        const float t = static_cast<float>(ir) / static_cast<float>(kHorizonRad);
        const float r = rInner + (rOuter - rInner) * t;
        for (int ia = 0; ia < nA; ++ia) {
            const float ang = (static_cast<float>(ia) / static_cast<float>(nA)) * kPi2;
            const float wx = centerX + std::cos(ang) * r;
            const float wz = centerZ + std::sin(ang) * r;
            const float wy = engine::math::WorldHeight(wx, wz);
            const int i = ir * nA + ia;
            m.vertices[i * 3 + 0] = wx;
            m.vertices[i * 3 + 1] = wy;
            m.vertices[i * 3 + 2] = wz;
            if (wy < minY) minY = wy;
            if (wy > maxY) maxY = wy;
            // Rock-ish vertex color so splat still reads at distance
            m.colors[i * 4 + 0] = 40;
            m.colors[i * 4 + 1] = 90;
            m.colors[i * 4 + 2] = 160;
            m.colors[i * 4 + 3] = 255;
        }
    }

    int idx = 0;
    for (int ir = 0; ir < kHorizonRad; ++ir) {
        for (int ia = 0; ia < nA; ++ia) {
            const int a0 = ir * nA + ia;
            const int a1 = ir * nA + ((ia + 1) % nA);
            const int b0 = (ir + 1) * nA + ia;
            const int b1 = (ir + 1) * nA + ((ia + 1) % nA);
            m.indices[idx++] = static_cast<unsigned short>(a0);
            m.indices[idx++] = static_cast<unsigned short>(b0);
            m.indices[idx++] = static_cast<unsigned short>(b1);
            m.indices[idx++] = static_cast<unsigned short>(a0);
            m.indices[idx++] = static_cast<unsigned short>(b1);
            m.indices[idx++] = static_cast<unsigned short>(a1);
        }
    }

    // Flat-ish upward normals (silhouette; lighting secondary)
    for (int i = 0; i < m.vertexCount; ++i) {
        m.normals[i * 3 + 0] = 0.0f;
        m.normals[i * 3 + 1] = 1.0f;
        m.normals[i * 3 + 2] = 0.0f;
    }
    m.aabbMin = {centerX - rOuter, minY, centerZ - rOuter};
    m.aabbMax = {centerX + rOuter, maxY, centerZ + rOuter};
    return m;
}

static void rebuildHorizonAround(int pcx, int pcz) {
    const float S = engine::math::WorldConfig::CHUNK_SIZE;
    const float cx = (static_cast<float>(pcx) + 0.5f) * S;
    const float cz = (static_cast<float>(pcz) + 0.5f) * S;
    const float rInner = static_cast<float>(engine::math::WorldConfig::LOAD_RADIUS) * S * 0.92f;
    const float rOuter = engine::math::WorldConfig::WORLD_HALF_EXTENT * 0.98f;
    MeshCPU cpu = generateHorizonRingCPU(cx, cz, rInner, rOuter);
    if (g_horizonLoaded) {
        UnloadModel(g_horizonModel);
        g_horizonModel = {};
        g_horizonLoaded = false;
    }
    g_horizonModel = uploadMeshToGPU(cpu);
    g_horizonLoaded = true;
    g_horizonAnchorCx = pcx;
    g_horizonAnchorCz = pcz;
}

static void updateHorizon(int pcx, int pcz) {
    if (!g_horizonLoaded ||
        std::abs(pcx - g_horizonAnchorCx) >= 2 ||
        std::abs(pcz - g_horizonAnchorCz) >= 2) {
        rebuildHorizonAround(pcx, pcz);
    }
}

// LOD switch hysteresis: avoid thrashing meshes at ring boundaries.
static int stableLOD(int dist, int currentLod, bool hasChunk) {
    int target = engine::math::GetLODForDistance(dist);
    if (!hasChunk) return target;
    if (target == currentLod) return currentLod;
    if (target > currentLod) {
        // Coarsen only once firmly inside the coarser band
        return engine::math::GetLODForDistance(std::max(0, dist - 1)) >= target
                   ? target
                   : currentLod;
    }
    // Refine only once firmly inside the finer band
    return engine::math::GetLODForDistance(dist + 1) <= target ? target : currentLod;
}

// Frustum test using the active 3D modelview/projection (call inside BeginMode3D).
static bool aabbInFrustum(Vector3 mn, Vector3 mx) {
    Matrix mvp = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());

    float minNdcX =  1e9f, maxNdcX = -1e9f;
    float minNdcY =  1e9f, maxNdcY = -1e9f;
    float minNdcZ =  1e9f, maxNdcZ = -1e9f;
    bool anyInFront = false;

    for (int i = 0; i < 8; ++i) {
        Vector3 c = {
            (i & 1) ? mx.x : mn.x,
            (i & 2) ? mx.y : mn.y,
            (i & 4) ? mx.z : mn.z
        };
        float x = mvp.m0 * c.x + mvp.m4 * c.y + mvp.m8  * c.z + mvp.m12;
        float y = mvp.m1 * c.x + mvp.m5 * c.y + mvp.m9  * c.z + mvp.m13;
        float z = mvp.m2 * c.x + mvp.m6 * c.y + mvp.m10 * c.z + mvp.m14;
        float w = mvp.m3 * c.x + mvp.m7 * c.y + mvp.m11 * c.z + mvp.m15;
        if (w <= 0.0001f) continue;
        anyInFront = true;
        float invW = 1.0f / w;
        float nx = x * invW;
        float ny = y * invW;
        float nz = z * invW;
        if (nx < minNdcX) minNdcX = nx;
        if (nx > maxNdcX) maxNdcX = nx;
        if (ny < minNdcY) minNdcY = ny;
        if (ny > maxNdcY) maxNdcY = ny;
        if (nz < minNdcZ) minNdcZ = nz;
        if (nz > maxNdcZ) maxNdcZ = nz;
    }

    if (!anyInFront) return false;
    if (maxNdcX < -1.0f || minNdcX > 1.0f) return false;
    if (maxNdcY < -1.0f || minNdcY > 1.0f) return false;
    if (maxNdcZ < -1.0f || minNdcZ > 1.0f) return false;
    return true;
}

// Worker task: build CPU mesh at given LOD resolution, push to pending queue.
static void generateChunkAsync(ChunkCoord coord, int lod, float playerX, float playerZ) {
    const float S = engine::math::WorldConfig::CHUNK_SIZE;
    const int   R = engine::math::GetResolutionForLOD(lod);
    float originX = coord.x * S;
    float originZ = coord.z * S;
    const float cx = originX + S * 0.5f;
    const float cz = originZ + S * 0.5f;
    const float dx = cx - playerX;
    const float dz = cz - playerZ;
    const float centerDist = std::sqrt(dx * dx + dz * dz);

    MeshCPU cpu = generateMeshCPU(originX, originZ, S, S, R, R);
    bool grassBaked = false;
    bool natureBaked = false;
    GrassBake grass = generateGrassCPU(originX, originZ, S, lod, centerDist, &grassBaked);
    NatureBake nature = generateNatureCPU(originX, originZ, S, lod, centerDist, &natureBaked);

    PendingUpload up;
    up.coord = coord;
    up.lod = lod;
    up.mesh = std::move(cpu);
    up.grass = std::move(grass);
    up.nature = std::move(nature);
    up.grassBaked = grassBaked;
    up.natureBaked = natureBaked;
    bucketGrassDraw(up.grass, up.grassDraw);
    bucketNatureDraw(up.nature, up.natureDraw);

    std::lock_guard<std::mutex> lock(g_pendingMutex);
    g_pendingUploads.push_back(std::move(up));
}

// Blocking version used during Init() to prewarm spawn area at LOD 0.
static void generateChunkBlocking(ChunkCoord coord) {
    const float S = engine::math::WorldConfig::CHUNK_SIZE;
    const int   R = engine::math::WorldConfig::CHUNK_RESOLUTION;
    float originX = coord.x * S;
    float originZ = coord.z * S;

    MeshCPU cpu = generateMeshCPU(originX, originZ, S, S, R, R);
    Model model = uploadMeshToGPU(cpu);

    auto slot = std::make_unique<ChunkSlot>();
    slot->coord       = coord;
    slot->model       = model;
    slot->modelLoaded = true;
    slot->lod         = 0;
    slot->aabbMin     = cpu.aabbMin;
    slot->aabbMax     = cpu.aabbMax;
    bool grassBaked = false;
    bool natureBaked = false;
    slot->grass       = generateGrassCPU(originX, originZ, S, 0, 0.0f, &grassBaked);
    slot->nature      = generateNatureCPU(originX, originZ, S, 0, 0.0f, &natureBaked);
    slot->grassBaked  = grassBaked;
    slot->natureBaked = natureBaked;
    bucketGrassDraw(slot->grass, slot->grassDraw);
    bucketNatureDraw(slot->nature, slot->natureDraw);
    g_chunks[packCoord(coord.x, coord.z)] = std::move(slot);
}

namespace chunks {

void Init() {
    loadTerrainMaterials();
    loadGrassMaterials();
    loadNatureMaterials();

    // Prewarm a small spawn patch synchronously (full grass). Distant rings stream in.
    const int PREWARM = 1;  // 3x3 = 9 chunks — was 5x5 with heavy grass (very slow)
    for (int dz = -PREWARM; dz <= PREWARM; ++dz) {
        for (int dx = -PREWARM; dx <= PREWARM; ++dx) {
            generateChunkBlocking({dx, dz});
        }
    }
    updateHorizon(0, 0);
}

void Update(Vector3 playerPos) {
    const float S = engine::math::WorldConfig::CHUNK_SIZE;
    const int   R = engine::math::WorldConfig::LOAD_RADIUS;

    int pcx = static_cast<int>(std::floor(playerPos.x / S));
    int pcz = static_cast<int>(std::floor(playerPos.z / S));
    const float grassBakeR = g_grassDrawDist + S * 1.25f;
    const float treeBakeR = g_treeDrawDist + S * 1.25f;

    // 1. Radial Distance Priority Queueing (closest to player first!)
    std::unordered_set<uint64_t> wanted;
    wanted.reserve(static_cast<size_t>((2 * R + 1) * (2 * R + 1)));

    struct ChunkTask {
        ChunkCoord coord;
        int        dist;
        int        targetLOD;
    };
    std::vector<ChunkTask> tasks;
    tasks.reserve((2 * R + 1) * (2 * R + 1));

    for (int dz = -R; dz <= R; ++dz) {
        for (int dx = -R; dx <= R; ++dx) {
            ChunkCoord c = {pcx + dx, pcz + dz};
            uint64_t k = packCoord(c.x, c.z);
            wanted.insert(k);

            int dist = std::max(std::abs(dx), std::abs(dz));
            auto it = g_chunks.find(k);
            int currentLod = (it != g_chunks.end()) ? it->second->lod : 0;
            int targetLOD = stableLOD(dist, currentLod, it != g_chunks.end());

            const float cx = (static_cast<float>(c.x) + 0.5f) * S;
            const float cz = (static_cast<float>(c.z) + 0.5f) * S;
            const float cdx = cx - playerPos.x;
            const float cdz = cz - playerPos.z;
            const float centerDist = std::sqrt(cdx * cdx + cdz * cdz);

            bool needsGeneration = false;
            if (it == g_chunks.end()) {
                needsGeneration = true;
            } else if (it->second->lod != targetLOD) {
                needsGeneration = true;
            } else {
                // Entered foliage bake range after streaming in without bake.
                if (targetLOD <= kGrassChunkLodMax && centerDist <= grassBakeR &&
                    !it->second->grassBaked) {
                    needsGeneration = true;
                }
                if (targetLOD <= kNatureChunkLodMax && centerDist <= treeBakeR &&
                    !it->second->natureBaked) {
                    needsGeneration = true;
                }
            }

            if (needsGeneration && g_inFlight.find(k) == g_inFlight.end()) {
                tasks.push_back({c, dist, targetLOD});
            }
        }
    }

    // Sort tasks radially so ground under player generates FIRST!
    std::sort(tasks.begin(), tasks.end(), [](const ChunkTask& a, const ChunkTask& b) {
        return a.dist < b.dist;
    });

    // Throttle: only keep a small in-flight window so we don't queue 4000+ jobs
    // (that made "loading into VRAM" feel endless while RAM/CPU thrashed).
    const int inFlight = static_cast<int>(g_inFlight.size());
    const int room = std::max(0, MAX_CHUNK_GEN_IN_FLIGHT - inFlight);
    const int submitCap = std::min(room, MAX_CHUNK_GEN_SUBMIT_PER_FRAME);
    int submitted = 0;
    const float px = playerPos.x;
    const float pz = playerPos.z;
    for (const auto& task : tasks) {
        if (submitted >= submitCap) break;
        uint64_t k = packCoord(task.coord.x, task.coord.z);
        g_inFlight.insert(k);
        ChunkCoord c = task.coord;
        int targetLOD = task.targetLOD;
        engine::jobs::GetGlobalPool().Submit([c, targetLOD, px, pz] {
            generateChunkAsync(c, targetLOD, px, pz);
        });
        ++submitted;
    }

    // 2. Unload chunks outside UNLOAD_RADIUS (hysteresis buffer prevents edge popping).
    const int UR = engine::math::WorldConfig::UNLOAD_RADIUS;
    for (auto it = g_chunks.begin(); it != g_chunks.end(); ) {
        int dx = std::abs(it->second->coord.x - pcx);
        int dz = std::abs(it->second->coord.z - pcz);
        int dist = std::max(dx, dz);
        if (dist > UR) {
            if (it->second->modelLoaded) UnloadModel(it->second->model);
            it = g_chunks.erase(it);
        } else {
            ++it;
        }
    }

    // 3. Priority GPU Uploads (upload closest finished meshes first!)
    std::vector<PendingUpload> readyBatch;
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        if (!g_pendingUploads.empty()) {
            std::sort(g_pendingUploads.begin(), g_pendingUploads.end(), [pcx, pcz](const PendingUpload& a, const PendingUpload& b) {
                int distA = std::max(std::abs(a.coord.x - pcx), std::abs(a.coord.z - pcz));
                int distB = std::max(std::abs(b.coord.x - pcx), std::abs(b.coord.z - pcz));
                return distA > distB; // back() will pop closest distance first
            });
            int budget = GPU_UPLOAD_BUDGET_PER_FRAME;
            while (!g_pendingUploads.empty() && budget-- > 0) {
                readyBatch.push_back(std::move(g_pendingUploads.back()));
                g_pendingUploads.pop_back();
            }
        }
    }

    for (auto& up : readyBatch) {
        uint64_t k = packCoord(up.coord.x, up.coord.z);
        g_inFlight.erase(k);

        // Chunk was unloaded while worker was building it — drop mesh
        if (wanted.find(k) == wanted.end()) continue;

        Model model = uploadMeshToGPU(up.mesh);

        auto applySlot = [&](ChunkSlot& slot) {
            slot.coord       = up.coord;
            slot.model       = model;
            slot.modelLoaded = true;
            slot.lod         = up.lod;
            slot.aabbMin     = up.mesh.aabbMin;
            slot.aabbMax     = up.mesh.aabbMax;
            slot.grass       = std::move(up.grass);
            slot.nature      = std::move(up.nature);
            slot.grassBaked  = up.grassBaked;
            slot.natureBaked = up.natureBaked;
            for (int v = 0; v < kGrassVariantCount; ++v) {
                slot.grassDraw[v] = std::move(up.grassDraw[v]);
            }
            for (int v = 0; v < kNatureVariantCount; ++v) {
                slot.natureDraw[v] = std::move(up.natureDraw[v]);
            }
        };

        auto it = g_chunks.find(k);
        if (it != g_chunks.end()) {
            if (it->second->modelLoaded) UnloadModel(it->second->model);
            applySlot(*it->second);
        } else {
            auto slot = std::make_unique<ChunkSlot>();
            applySlot(*slot);
            g_chunks[k] = std::move(slot);
        }
    }

    updateHorizon(pcx, pcz);
}

void Draw() {
    static bool logged = false;
    if (!logged) {
        TraceLog(LOG_INFO, "TERRAIN: chunks::Draw() called, %zu chunks loaded", g_chunks.size());
        logged = true;
    }

    bindTerrainMaterials();

    rlDisableBackfaceCulling();
    for (auto& [key, slot] : g_chunks) {
        if (!slot->modelLoaded) continue;
        if (!aabbInFrustum(slot->aabbMin, slot->aabbMax)) continue;
        // Keep DrawMesh from rebinding a default diffuse over splat unit state.
        if (slot->model.materialCount > 0) {
            slot->model.materials[0].maps[MATERIAL_MAP_ALBEDO].texture = {};
            slot->model.materials[0].shader = g_terrainShader;
        }
        DrawModel(slot->model, {0.0f, 0.0f, 0.0f}, 1.0f, WHITE);
    }
    if (g_horizonLoaded && g_horizonModel.meshCount > 0) {
        if (g_horizonModel.materialCount > 0) {
            g_horizonModel.materials[0].maps[MATERIAL_MAP_ALBEDO].texture = {};
            g_horizonModel.materials[0].shader = g_terrainShader;
        }
        // Horizon is a large ring — frustum test the AABB if available via mesh bounds.
        DrawModel(g_horizonModel, {0.0f, 0.0f, 0.0f}, 1.0f, WHITE);
    }
    rlEnableBackfaceCulling();
}

static void pushGrassBandUniforms(Vector3 viewPos,
                                  float fadeInStart, float fadeInEnd,
                                  float fadeOutStart, float fadeOutEnd,
                                  float meshH) {
    const float t = static_cast<float>(GetTime());
    Vector3 sun = Vector3Normalize(g_sunDir);
    if (g_grassLocTime >= 0) {
        SetShaderValue(g_grassShader, g_grassLocTime, &t, SHADER_UNIFORM_FLOAT);
    }
    if (g_grassLocViewPos >= 0) {
        SetShaderValue(g_grassShader, g_grassLocViewPos, &viewPos, SHADER_UNIFORM_VEC3);
    }
    if (g_grassLocFadeInStart >= 0) {
        SetShaderValue(g_grassShader, g_grassLocFadeInStart, &fadeInStart, SHADER_UNIFORM_FLOAT);
    }
    if (g_grassLocFadeInEnd >= 0) {
        SetShaderValue(g_grassShader, g_grassLocFadeInEnd, &fadeInEnd, SHADER_UNIFORM_FLOAT);
    }
    if (g_grassLocFadeOutStart >= 0) {
        SetShaderValue(g_grassShader, g_grassLocFadeOutStart, &fadeOutStart, SHADER_UNIFORM_FLOAT);
    }
    if (g_grassLocFadeOutEnd >= 0) {
        SetShaderValue(g_grassShader, g_grassLocFadeOutEnd, &fadeOutEnd, SHADER_UNIFORM_FLOAT);
    }
    if (g_grassLocSunDir >= 0) {
        SetShaderValue(g_grassShader, g_grassLocSunDir, &sun, SHADER_UNIFORM_VEC3);
    }
    if (g_grassLocSunIntensity >= 0) {
        SetShaderValue(g_grassShader, g_grassLocSunIntensity, &g_sunIntensity, SHADER_UNIFORM_FLOAT);
    }
    if (g_grassLocMeshHeight >= 0) {
        SetShaderValue(g_grassShader, g_grassLocMeshHeight, &meshH, SHADER_UNIFORM_FLOAT);
    }
}

void DrawGrass(Vector3 viewPos) {
    g_grassDrawStats = {};

    // Always refresh baked totals (cheap) so editor counters stay valid when disabled.
    for (const auto& [key, slot] : g_chunks) {
        (void)key;
        if (slot->lod > kGrassChunkLodMax) continue;
        g_grassDrawStats.baked += slot->grass.near.size();
    }

    if (!g_grassEnabled || !g_grassReady) return;
    bool anyVariant = false;
    for (int i = 0; i < kGrassVariantCount; ++i) {
        if (g_grassVariants[i].mesh && g_grassVariants[i].mesh->vaoId != 0) {
            anyVariant = true;
            break;
        }
    }
    if (!anyVariant) return;

    clampGrassDrawDistance();
    const float drawEnd = g_grassDrawDist;
    const float fadeOutStart = std::max(2.0f, drawEnd - kGrassFadeMeters);
    const float chunkCull = drawEnd + engine::math::WorldConfig::CHUNK_SIZE * 0.75f;
    const float chunkCullSq = chunkCull * chunkCull;
    constexpr float padY = 2.0f;
    const float S = engine::math::WorldConfig::CHUNK_SIZE;

    // Collect visible chunks (frustum + distance), closest first — draw lists are pre-baked.
    struct VisChunk {
        ChunkSlot* slot = nullptr;
        float      distSq = 0.0f;
    };
    static std::vector<VisChunk> vis;
    vis.clear();
    vis.reserve(64);

    for (auto& [key, slot] : g_chunks) {
        (void)key;
        if (slot->lod > kGrassChunkLodMax) continue;
        bool any = false;
        for (int v = 0; v < kGrassVariantCount; ++v) {
            if (!slot->grassDraw[v].empty()) { any = true; break; }
        }
        if (!any) continue;

        const float cx = (static_cast<float>(slot->coord.x) + 0.5f) * S;
        const float cz = (static_cast<float>(slot->coord.z) + 0.5f) * S;
        const float cdx = cx - viewPos.x;
        const float cdz = cz - viewPos.z;
        const float d2 = cdx * cdx + cdz * cdz;
        if (d2 > chunkCullSq) continue;

        Vector3 mn = slot->aabbMin;
        Vector3 mx = slot->aabbMax;
        mx.y += padY;
        if (!aabbInFrustum(mn, mx)) continue;

        vis.push_back({slot.get(), d2});
    }

    std::sort(vis.begin(), vis.end(), [](const VisChunk& a, const VisChunk& b) {
        return a.distSq < b.distSq;
    });

    rlDisableBackfaceCulling();

    // Scratch for distance density thinning (near = full, far = sparse Quaternius).
    static std::vector<Matrix> thinned;
    thinned.clear();
    thinned.reserve(4096);
    const uint64_t thinSeed = engine::math::GetWorldConfig().seed ^ 0x67A55D15ULL;

    size_t drawn = 0;
    size_t tris = 0;
    for (const VisChunk& vc : vis) {
        if (static_cast<int>(drawn) >= kGrassMaxDraw) break;
        ChunkSlot& slot = *vc.slot;
        for (int v = 0; v < kGrassVariantCount; ++v) {
            std::vector<Matrix>& list = slot.grassDraw[v];
            if (list.empty()) continue;
            Mesh* mesh = g_grassVariants[v].mesh;
            if (!mesh || mesh->vaoId == 0) continue;

            thinned.clear();
            thinned.reserve(list.size());
            for (const Matrix& m : list) {
                const float wx = m.m12;
                const float wz = m.m14;
                const float dx = wx - viewPos.x;
                const float dz = wz - viewPos.z;
                const float d = std::sqrt(dx * dx + dz * dz);
                if (d > drawEnd) continue;
                if (d > kGrassFullDensityDist) {
                    const float span = std::max(1.0f, drawEnd - kGrassFullDensityDist);
                    float t = (d - kGrassFullDensityDist) / span;
                    t = std::clamp(t, 0.0f, 1.0f);
                    // Smoothstep falloff — full near density, sparse at horizon.
                    const float s = t * t * (3.0f - 2.0f * t);
                    const float keep = 1.0f - s * (1.0f - kGrassFarKeepFrac);
                    const uint64_t h = engine::math::hash2D(
                        thinSeed,
                        static_cast<int32_t>(std::floor(wx * 4.0f)),
                        static_cast<int32_t>(std::floor(wz * 4.0f)));
                    if (engine::math::randFloat01(h) > keep) continue;
                }
                thinned.push_back(m);
            }
            if (thinned.empty()) continue;

            int count = static_cast<int>(thinned.size());
            const int room = kGrassMaxDraw - static_cast<int>(drawn);
            if (room <= 0) break;
            if (count > room) count = room;

            const float meshH = g_grassVariants[v].height > 0.05f
                ? g_grassVariants[v].height : g_grassClumpHeight;
            pushGrassBandUniforms(viewPos, -1.0f, -1.0f, fadeOutStart, drawEnd, meshH);
            DrawMeshInstanced(*mesh, g_grassClumpMaterial, thinned.data(), count);

            drawn += static_cast<size_t>(count);
            tris += static_cast<size_t>(count) *
                static_cast<size_t>(mesh->triangleCount > 0 ? mesh->triangleCount : 80);
        }
    }

    g_grassDrawStats.drawn = drawn;
    g_grassDrawStats.approxTris = tris;

    rlEnableBackfaceCulling();
}

static void pushFoliageUniforms(Vector3 viewPos,
                                float fadeInStart, float fadeInEnd,
                                float fadeOutStart, float fadeOutEnd) {
    Vector3 sun = Vector3Normalize(g_sunDir);
    if (g_foliageLocViewPos >= 0) {
        SetShaderValue(g_foliageShader, g_foliageLocViewPos, &viewPos, SHADER_UNIFORM_VEC3);
    }
    if (g_foliageLocFadeInStart >= 0) {
        SetShaderValue(g_foliageShader, g_foliageLocFadeInStart, &fadeInStart, SHADER_UNIFORM_FLOAT);
    }
    if (g_foliageLocFadeInEnd >= 0) {
        SetShaderValue(g_foliageShader, g_foliageLocFadeInEnd, &fadeInEnd, SHADER_UNIFORM_FLOAT);
    }
    if (g_foliageLocFadeOutStart >= 0) {
        SetShaderValue(g_foliageShader, g_foliageLocFadeOutStart, &fadeOutStart, SHADER_UNIFORM_FLOAT);
    }
    if (g_foliageLocFadeOutEnd >= 0) {
        SetShaderValue(g_foliageShader, g_foliageLocFadeOutEnd, &fadeOutEnd, SHADER_UNIFORM_FLOAT);
    }
    if (g_foliageLocSunDir >= 0) {
        SetShaderValue(g_foliageShader, g_foliageLocSunDir, &sun, SHADER_UNIFORM_VEC3);
    }
    if (g_foliageLocSunIntensity >= 0) {
        SetShaderValue(g_foliageShader, g_foliageLocSunIntensity, &g_sunIntensity, SHADER_UNIFORM_FLOAT);
    }
}

void DrawTrees(Vector3 viewPos) {
    g_treeDrawStats = {};

    for (const auto& [key, slot] : g_chunks) {
        (void)key;
        if (slot->lod > kNatureChunkLodMax) continue;
        g_treeDrawStats.bakedTrees += slot->nature.trees.size();
        g_treeDrawStats.bakedBushes += slot->nature.bushes.size();
    }

    if (!g_treesEnabled || !g_natureReady) return;

    clampTreeKnobs();
    const float drawEnd = g_treeDrawDist;
    const float fadeOutStart = std::max(2.0f, drawEnd - kNatureFadeMeters);
    const float chunkCull = drawEnd + engine::math::WorldConfig::CHUNK_SIZE * 0.75f;
    const float chunkCullSq = chunkCull * chunkCull;
    constexpr float padY = 18.0f;
    const float S = engine::math::WorldConfig::CHUNK_SIZE;

    struct VisChunk {
        ChunkSlot* slot = nullptr;
        float      distSq = 0.0f;
    };
    static std::vector<VisChunk> vis;
    vis.clear();
    vis.reserve(64);

    for (auto& [key, slot] : g_chunks) {
        (void)key;
        if (slot->lod > kNatureChunkLodMax) continue;
        bool any = false;
        for (int v = 0; v < kNatureVariantCount; ++v) {
            if (!slot->natureDraw[v].empty()) { any = true; break; }
        }
        if (!any) continue;

        const float cx = (static_cast<float>(slot->coord.x) + 0.5f) * S;
        const float cz = (static_cast<float>(slot->coord.z) + 0.5f) * S;
        const float cdx = cx - viewPos.x;
        const float cdz = cz - viewPos.z;
        const float d2 = cdx * cdx + cdz * cdz;
        if (d2 > chunkCullSq) continue;

        Vector3 mn = slot->aabbMin;
        Vector3 mx = slot->aabbMax;
        mx.y += padY;
        if (!aabbInFrustum(mn, mx)) continue;

        vis.push_back({slot.get(), d2});
    }

    std::sort(vis.begin(), vis.end(), [](const VisChunk& a, const VisChunk& b) {
        return a.distSq < b.distSq;
    });

    rlDisableBackfaceCulling();
    pushFoliageUniforms(viewPos, -1.0f, -1.0f, fadeOutStart, drawEnd);

    // Gather by variant across visible chunks → one DrawMeshInstanced per variant mesh.
    static std::vector<Matrix> batch[kNatureVariantCount];
    for (int v = 0; v < kNatureVariantCount; ++v) {
        batch[v].clear();
        batch[v].reserve(256);
    }

    size_t drawn = 0;
    for (const VisChunk& vc : vis) {
        if (static_cast<int>(drawn) >= kNatureMaxDraw) break;
        ChunkSlot& slot = *vc.slot;
        for (int v = 0; v < kNatureVariantCount; ++v) {
            std::vector<Matrix>& list = slot.natureDraw[v];
            if (list.empty()) continue;
            if (!g_natureVariants[v].loaded) continue;
            const int room = kNatureMaxDraw - static_cast<int>(drawn);
            if (room <= 0) break;
            int take = static_cast<int>(list.size());
            if (take > room) take = room;
            batch[v].insert(batch[v].end(), list.begin(), list.begin() + take);
            drawn += static_cast<size_t>(take);
        }
    }

    size_t tris = 0;
    for (int v = 0; v < kNatureVariantCount; ++v) {
        if (batch[v].empty()) continue;
        NatureVariant& nv = g_natureVariants[v];
        if (!nv.loaded || nv.model.meshCount <= 0) continue;
        const int count = static_cast<int>(batch[v].size());
        for (int mi = 0; mi < nv.model.meshCount; ++mi) {
            Mesh& mesh = nv.model.meshes[mi];
            if (mesh.vaoId == 0) continue;
            const int matIdx = (nv.model.meshMaterial != nullptr)
                ? nv.model.meshMaterial[mi] : 0;
            Material& mat = nv.model.materials[std::clamp(matIdx, 0, nv.model.materialCount - 1)];
            mat.shader = g_foliageShader;
            DrawMeshInstanced(mesh, mat, batch[v].data(), count);
            tris += static_cast<size_t>(count) *
                static_cast<size_t>(mesh.triangleCount > 0 ? mesh.triangleCount : 100);
        }
    }

    g_treeDrawStats.drawn = drawn;
    g_treeDrawStats.approxTris = tris;
    rlEnableBackfaceCulling();
}

void Shutdown() {
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        g_pendingUploads.clear();
    }
    for (auto& [key, slot] : g_chunks) {
        if (slot->modelLoaded) UnloadModel(slot->model);
    }
    g_chunks.clear();
    g_inFlight.clear();
    if (g_horizonLoaded) {
        UnloadModel(g_horizonModel);
        g_horizonModel = {};
        g_horizonLoaded = false;
    }
    unloadGrassMaterials();
    unloadNatureMaterials();
    unloadTerrainMaterials();
}

size_t LoadedChunkCount() { return g_chunks.size(); }

size_t PendingUploadCount() {
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    return g_pendingUploads.size();
}

size_t GrassInstanceCount() {
    size_t n = 0;
    for (const auto& [key, slot] : g_chunks) {
        (void)key;
        n += slot->grass.near.size();
    }
    return n;
}

size_t TreeInstanceCount() {
    size_t n = 0;
    for (const auto& [key, slot] : g_chunks) {
        (void)key;
        n += slot->nature.trees.size();
    }
    return n;
}

size_t BushInstanceCount() {
    size_t n = 0;
    for (const auto& [key, slot] : g_chunks) {
        (void)key;
        n += slot->nature.bushes.size();
    }
    return n;
}

const GrassDrawStats& GetGrassDrawStats() { return g_grassDrawStats; }
const TreeDrawStats& GetTreeDrawStats() { return g_treeDrawStats; }

void ReloadAround(Vector3 center, int radiusChunks) {
    if (radiusChunks < 0) radiusChunks = 0;
    const float S = engine::math::WorldConfig::CHUNK_SIZE;
    const int pcx = static_cast<int>(std::floor(center.x / S));
    const int pcz = static_cast<int>(std::floor(center.z / S));

    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        g_pendingUploads.erase(
            std::remove_if(g_pendingUploads.begin(), g_pendingUploads.end(),
                           [&](const PendingUpload& up) {
                               const int dx = std::abs(up.coord.x - pcx);
                               const int dz = std::abs(up.coord.z - pcz);
                               return std::max(dx, dz) <= radiusChunks;
                           }),
            g_pendingUploads.end());
    }

    for (auto it = g_chunks.begin(); it != g_chunks.end(); ) {
        const int dx = std::abs(it->second->coord.x - pcx);
        const int dz = std::abs(it->second->coord.z - pcz);
        if (std::max(dx, dz) <= radiusChunks) {
            g_inFlight.erase(it->first);
            if (it->second->modelLoaded) UnloadModel(it->second->model);
            it = g_chunks.erase(it);
        } else {
            ++it;
        }
    }
}

void DrawDebug() {
    if (!g_showChunkBounds) return;
    const float S = engine::math::WorldConfig::CHUNK_SIZE;
    for (auto& [key, slot] : g_chunks) {
        (void)key;
        const float x0 = static_cast<float>(slot->coord.x) * S;
        const float z0 = static_cast<float>(slot->coord.z) * S;
        const float y0 = slot->aabbMin.y;
        const float y1 = slot->aabbMax.y;
        Color c = slot->modelLoaded ? Color{80, 200, 255, 180} : Color{255, 80, 80, 180};
        DrawCubeWires(Vector3{x0 + S * 0.5f, (y0 + y1) * 0.5f, z0 + S * 0.5f},
                      S, std::max(2.0f, y1 - y0), S, c);
    }
}

void SetShowChunkBounds(bool show) { g_showChunkBounds = show; }
bool GetShowChunkBounds() { return g_showChunkBounds; }

void SetTerrainDebugMode(int mode) { g_terrainDebugMode = std::clamp(mode, 0, 4); }
int  GetTerrainDebugMode() { return g_terrainDebugMode; }

void SetSunDirection(Vector3 dir) {
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len > 1e-6f) {
        g_sunDir = {dir.x / len, dir.y / len, dir.z / len};
    }
}
Vector3 GetSunDirection() { return Vector3Normalize(g_sunDir); }

void SetSunIntensity(float intensity) { g_sunIntensity = std::clamp(intensity, 0.0f, 4.0f); }
float GetSunIntensity() { return g_sunIntensity; }

void SetHazeDistance(float start, float end) {
    g_hazeStart = std::max(0.0f, start);
    g_hazeEnd = std::max(g_hazeStart + 1.0f, end);
}
void SetHazeStrength(float strength) { g_hazeStrength = std::clamp(strength, 0.0f, 1.0f); }
float GetHazeStart() { return g_hazeStart; }
float GetHazeEnd() { return g_hazeEnd; }
float GetHazeStrength() { return g_hazeStrength; }

void SetGrassEnabled(bool enabled) { g_grassEnabled = enabled; }
bool GetGrassEnabled() { return g_grassEnabled; }

void SetGrassDensity(float density) { g_grassDensity = std::clamp(density, 0.0f, 2.0f); }
float GetGrassDensity() { return g_grassDensity; }

void SetGrassMaxSlope(float slope) { g_grassMaxSlope = std::clamp(slope, 0.05f, 1.0f); }
float GetGrassMaxSlope() { return g_grassMaxSlope; }

void SetGrassDrawDistance(float meters) {
    g_grassDrawDist = meters;
    clampGrassDrawDistance();
}
float GetGrassDrawDistance() { return g_grassDrawDist; }

// Deprecated stubs — all map to the single draw distance.
void SetGrassNearDistance(float meters) { SetGrassDrawDistance(meters); }
float GetGrassNearDistance() { return g_grassDrawDist; }
void SetGrassMidDistance(float meters) { SetGrassDrawDistance(meters); }
float GetGrassMidDistance() { return g_grassDrawDist; }
void SetGrassFarDistance(float meters) { SetGrassDrawDistance(meters); }
float GetGrassFarDistance() { return g_grassDrawDist; }

void SetGrassClusterMin(int n) {
    g_grassClusterMin = n;
    clampGrassClusterKnobs();
}
int GetGrassClusterMin() { return g_grassClusterMin; }
void SetGrassClusterMax(int n) {
    g_grassClusterMax = n;
    clampGrassClusterKnobs();
}
int GetGrassClusterMax() { return g_grassClusterMax; }

void SetGrassClusterRadius(float meters) {
    g_grassClusterRadius = meters;
    clampGrassClusterKnobs();
}
float GetGrassClusterRadius() { return g_grassClusterRadius; }

void SetGrassSeedSpacing(float meters) {
    g_grassSeedSpacing = meters;
    clampGrassClusterKnobs();
}
float GetGrassSeedSpacing() { return g_grassSeedSpacing; }

void SetGrassMeadowStrength(float strength) {
    g_grassMeadowStrength = strength;
    clampGrassClusterKnobs();
}
float GetGrassMeadowStrength() { return g_grassMeadowStrength; }

void SetGrassMeadowScale(float scale) {
    g_grassMeadowScale = scale;
    clampGrassClusterKnobs();
}
float GetGrassMeadowScale() { return g_grassMeadowScale; }

void SetGrassCoverageStrength(float strength) {
    g_grassCoverageStrength = strength;
    clampGrassClusterKnobs();
}
float GetGrassCoverageStrength() { return g_grassCoverageStrength; }

void SetGrassCoverageScale(float scale) {
    g_grassCoverageScale = scale;
    clampGrassClusterKnobs();
}
float GetGrassCoverageScale() { return g_grassCoverageScale; }

void SetGrassCoverageThreshold(float threshold) {
    g_grassCoverageThreshold = threshold;
    clampGrassClusterKnobs();
}
float GetGrassCoverageThreshold() { return g_grassCoverageThreshold; }

void SetGrassSizeNoiseScale(float scale) {
    g_grassSizeNoiseScale = scale;
    clampGrassClusterKnobs();
}
float GetGrassSizeNoiseScale() { return g_grassSizeNoiseScale; }

void SetGrassScaleMin(float mul) {
    g_grassScaleMin = mul;
    clampGrassClusterKnobs();
}
float GetGrassScaleMin() { return g_grassScaleMin; }
void SetGrassScaleMax(float mul) {
    g_grassScaleMax = mul;
    clampGrassClusterKnobs();
}
float GetGrassScaleMax() { return g_grassScaleMax; }

void SetGrassSinkCm(float cm) {
    g_grassSink = cm * 0.01f;
    clampGrassClusterKnobs();
}
float GetGrassSinkCm() { return g_grassSink * 100.0f; }

void ClearGrassExclusions() { g_grassExclusions.clear(); }

void AddGrassExclusionRect(float minX, float minZ, float maxX, float maxZ) {
    if (maxX < minX) std::swap(maxX, minX);
    if (maxZ < minZ) std::swap(maxZ, minZ);
    g_grassExclusions.push_back({minX, minZ, maxX, maxZ});
}

void SetTreesEnabled(bool enabled) { g_treesEnabled = enabled; }
bool GetTreesEnabled() { return g_treesEnabled; }

void SetTreeDensity(float density) {
    g_treeDensity = density;
    clampTreeKnobs();
}
float GetTreeDensity() { return g_treeDensity; }

void SetTreeMaxSlope(float slope) {
    g_treeMaxSlope = slope;
    clampTreeKnobs();
}
float GetTreeMaxSlope() { return g_treeMaxSlope; }

void SetTreeDrawDistance(float meters) {
    g_treeDrawDist = meters;
    clampTreeKnobs();
}
float GetTreeDrawDistance() { return g_treeDrawDist; }

void SetTreeSeedSpacing(float meters) {
    g_treeSeedSpacing = meters;
    clampTreeKnobs();
}
float GetTreeSeedSpacing() { return g_treeSeedSpacing; }

void SetBushSeedSpacing(float meters) {
    g_bushSeedSpacing = meters;
    clampTreeKnobs();
}
float GetBushSeedSpacing() { return g_bushSeedSpacing; }

void SetTreeScaleMin(float mul) {
    g_treeScaleMin = mul;
    clampTreeKnobs();
}
float GetTreeScaleMin() { return g_treeScaleMin; }

void SetTreeScaleMax(float mul) {
    g_treeScaleMax = mul;
    clampTreeKnobs();
}
float GetTreeScaleMax() { return g_treeScaleMax; }

void SetTreeSinkCm(float cm) {
    g_treeSink = cm * 0.01f;
    clampTreeKnobs();
}
float GetTreeSinkCm() { return g_treeSink * 100.0f; }

}  // namespace chunks
}  // namespace engine::terrain
