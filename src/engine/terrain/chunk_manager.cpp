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
#include <cstdlib>
#include <cstring>
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
// Instanced grass — chunk-streamed bake + distance LODs at draw
// ---------------------------------------------------------------------------
// Near: full Poly Haven clump mesh. Mid/Far: shared cross-quad impostor.
// Bake runs with terrain chunks (LOD0 only); lists unload with the chunk.
// ---------------------------------------------------------------------------
static bool     g_grassEnabled = true;
static float    g_grassDensity = 1.0f;
static float    g_grassMaxSlope = 0.32f;
static float    g_grassNearDist = 20.0f;
static float    g_grassMidDist = 45.0f;
static float    g_grassFarDist = 100.0f;
static float    g_grassNearDensMul = 1.40f;
static float    g_grassMidDensMul = 0.55f;
static float    g_grassFarDensMul = 0.18f;
// Cluster / meadow / scale knobs — radius ≥ ~0.7× spacing so patches merge.
static int      g_grassClusterMin = 6;
static int      g_grassClusterMax = 12;
static float    g_grassClusterRadius = 2.40f;   // max patch disk radius (m)
static float    g_grassSeedSpacing = 2.60f;     // hex lattice spacing (m)
static float    g_grassMeadowStrength = 0.0f;   // OFF by default — was carving large empty bands
static float    g_grassMeadowScale = 0.022f;    // noise frequency
static float    g_grassScaleMin = 0.78f;
static float    g_grassScaleMax = 1.28f;
static float    g_grassSink = 0.02f;            // meters into terrain
static constexpr float kGrassWaterGateMax = 0.12f;
// Near clustered patches. Cap must not abort lattice early (that caused Z-stripes).
static constexpr int   kGrassMaxNearPerChunk = 4800;
static constexpr int   kGrassMaxMidPerChunk  = 220;
static constexpr int   kGrassMaxFarPerChunk  = 80;
static constexpr int   kGrassMaxNearDraw = 7500;
static constexpr int   kGrassMaxMidDraw  = 3200;
static constexpr int   kGrassMaxFarDraw  = 1600;
static constexpr int   kGrassChunkLodMax = 0; // bake only on terrain LOD0 chunks
static float    g_grassClumpHeight = 0.323f;
static float    g_grassImpostorHeight = 0.32f;
static constexpr float kGrassBaseScale = 3.9f;
static constexpr float kGrassImpostorScale = 4.2f;
static chunks::GrassDrawStats g_grassDrawStats = {};

struct GrassBake {
    std::vector<Matrix> near; // full clump mesh
    std::vector<Matrix> mid;  // impostor
    std::vector<Matrix> far;  // sparse impostor
};

static Shader   g_grassShader = {};
static Material g_grassClumpMaterial = {};
static Material g_grassImpostorMaterial = {};
static Model    g_grassClumpModel = {};
static Model    g_grassImpostorModel = {};
static Mesh*    g_grassClumpMesh = nullptr;
static Mesh*    g_grassImpostorMesh = nullptr;
static Texture  g_grassTexture = {};
static bool     g_grassOwnsTexture = false;
static bool     g_grassClumpIsFallback = false;
static bool     g_grassReady = false;
static int      g_grassLocTime = -1;
static int      g_grassLocViewPos = -1;
static int      g_grassLocFadeStart = -1;
static int      g_grassLocFadeEnd = -1;
static int      g_grassLocSunDir = -1;
static int      g_grassLocSunIntensity = -1;
static int      g_grassLocMeshHeight = -1;

static void clampGrassLodDistances() {
    g_grassNearDist = std::clamp(g_grassNearDist, 6.0f, 80.0f);
    g_grassMidDist = std::clamp(g_grassMidDist, g_grassNearDist + 4.0f, 200.0f);
    g_grassFarDist = std::clamp(g_grassFarDist, g_grassMidDist + 8.0f, 400.0f);
}

// Crossed-quad impostor (bottom-origin). uAtlas=true samples a bottom clump on the albedo.
static Model makeGrassBillboardModel(bool uAtlas) {
    constexpr float W = 0.22f;
    constexpr float H = 0.32f;
    Mesh mesh = {};
    mesh.vertexCount = 8;
    mesh.triangleCount = 4;
    mesh.vertices = static_cast<float*>(MemAlloc(8 * 3 * sizeof(float)));
    mesh.texcoords = static_cast<float*>(MemAlloc(8 * 2 * sizeof(float)));
    mesh.normals = static_cast<float*>(MemAlloc(8 * 3 * sizeof(float)));
    mesh.colors = static_cast<unsigned char*>(MemAlloc(8 * 4 * sizeof(unsigned char)));
    mesh.indices = static_cast<unsigned short*>(MemAlloc(4 * 3 * sizeof(unsigned short)));

    // Atlas: bottom clumps live at high V after raylib/stbi upload (image top = v~0).
    const float u0 = uAtlas ? 0.08f : 0.0f;
    const float u1 = uAtlas ? 0.28f : 1.0f;
    const float v0 = uAtlas ? 0.98f : 1.0f; // bottom of card → grass roots
    const float v1 = uAtlas ? 0.62f : 0.0f; // tip

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

    setV(0, -W, 0.0f, 0.0f, u0, v0, 0.0f, 1.0f);
    setV(1,  W, 0.0f, 0.0f, u1, v0, 0.0f, 1.0f);
    setV(2,  W, H,    0.0f, u1, v1, 0.0f, 1.0f);
    setV(3, -W, H,    0.0f, u0, v1, 0.0f, 1.0f);
    setV(4, 0.0f, 0.0f, -W, u0, v0, 1.0f, 0.0f);
    setV(5, 0.0f, 0.0f,  W, u1, v0, 1.0f, 0.0f);
    setV(6, 0.0f, H,     W, u1, v1, 1.0f, 0.0f);
    setV(7, 0.0f, H,    -W, u0, v1, 1.0f, 0.0f);

    const unsigned short idx[] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    std::memcpy(mesh.indices, idx, sizeof(idx));
    UploadMesh(&mesh, false);
    g_grassImpostorHeight = H;
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
    g_grassLocFadeStart = GetShaderLocation(g_grassShader, "fadeStart");
    g_grassLocFadeEnd = GetShaderLocation(g_grassShader, "fadeEnd");
    g_grassLocSunDir = GetShaderLocation(g_grassShader, "sunDir");
    g_grassLocSunIntensity = GetShaderLocation(g_grassShader, "sunIntensity");
    g_grassLocMeshHeight = GetShaderLocation(g_grassShader, "meshHeight");
}

static void loadGrassMaterials() {
    if (g_grassReady) return;
    clampGrassLodDistances();

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
        Image fallback = GenImageColor(4, 4, Color{180, 200, 90, 255});
        g_grassTexture = LoadTextureFromImage(fallback);
        UnloadImage(fallback);
        g_grassOwnsTexture = true;
    };

    std::string texPath = makeAssetPath("assets/models/grass/grass_albedo.png");
    Image img = LoadImage(texPath.c_str());
    bool haveAtlas = img.data != nullptr;
    if (haveAtlas) {
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        if (img.width > 512 || img.height > 512) ImageResize(&img, 512, 512);
        g_grassTexture = LoadTextureFromImage(img);
        UnloadImage(img);
        GenTextureMipmaps(&g_grassTexture);
        SetTextureFilter(g_grassTexture, TEXTURE_FILTER_BILINEAR);
        g_grassOwnsTexture = true;
    } else {
        TraceLog(LOG_WARNING, "GRASS: albedo missing (%s), using solid green", texPath.c_str());
        loadSolidGrassTex();
    }

    // Always build impostor mesh (mid/far + clump fallback).
    g_grassImpostorModel = makeGrassBillboardModel(haveAtlas);
    g_grassImpostorMesh = &g_grassImpostorModel.meshes[0];

    std::string modelPath = makeAssetPath("assets/models/grass/grass_medium.obj");
    g_grassClumpModel = LoadModel(modelPath.c_str());
    g_grassClumpIsFallback = false;
    if (g_grassClumpModel.meshCount <= 0 || g_grassClumpModel.meshes == nullptr ||
        g_grassClumpModel.meshes[0].vertexCount <= 0) {
        TraceLog(LOG_ERROR,
                 "GRASS: failed to load model '%s' — near LOD uses billboard impostor. "
                 "Check assets/ next to the exe.",
                 modelPath.c_str());
        if (g_grassClumpModel.meshCount > 0) UnloadModel(g_grassClumpModel);
        g_grassClumpModel = makeGrassBillboardModel(haveAtlas);
        g_grassClumpIsFallback = true;
        g_grassClumpHeight = g_grassImpostorHeight;
    } else {
        g_grassClumpHeight = measureMeshHeightY(g_grassClumpModel.meshes[0]);
        TraceLog(LOG_INFO, "GRASS: loaded clump %s (%d verts, %d tris, h=%.3f)",
                 modelPath.c_str(),
                 g_grassClumpModel.meshes[0].vertexCount,
                 g_grassClumpModel.meshes[0].triangleCount,
                 g_grassClumpHeight);
    }
    g_grassClumpMesh = &g_grassClumpModel.meshes[0];

    g_grassClumpMaterial = LoadMaterialDefault();
    g_grassClumpMaterial.shader = g_grassShader;
    g_grassClumpMaterial.maps[MATERIAL_MAP_ALBEDO].texture = g_grassTexture;
    g_grassClumpMaterial.maps[MATERIAL_MAP_ALBEDO].color = WHITE;

    g_grassImpostorMaterial = LoadMaterialDefault();
    g_grassImpostorMaterial.shader = g_grassShader;
    g_grassImpostorMaterial.maps[MATERIAL_MAP_ALBEDO].texture = g_grassTexture;
    g_grassImpostorMaterial.maps[MATERIAL_MAP_ALBEDO].color = WHITE;

    g_grassReady = true;
    TraceLog(LOG_INFO,
             "GRASS: LOD ready near<=%.0fm (clump) mid<=%.0fm far<=%.0fm | caps %d/%d/%d per chunk",
             g_grassNearDist, g_grassMidDist, g_grassFarDist,
             kGrassMaxNearPerChunk, kGrassMaxMidPerChunk, kGrassMaxFarPerChunk);
}

static void unloadGrassMaterials() {
    if (!g_grassReady && g_grassClumpModel.meshCount == 0 && g_grassShader.id == 0) return;

    auto detachMat = [](Material& mat) {
        if (!mat.maps) return;
        mat.maps[MATERIAL_MAP_ALBEDO].texture = {};
        mat.shader.id = rlGetShaderIdDefault();
        UnloadMaterial(mat);
        mat = {};
    };
    detachMat(g_grassClumpMaterial);
    detachMat(g_grassImpostorMaterial);

    auto unloadModelSafe = [](Model& model) {
        if (model.meshCount <= 0) return;
        for (int i = 0; i < model.materialCount; ++i) {
            model.materials[i].maps[MATERIAL_MAP_ALBEDO].texture = {};
            model.materials[i].shader.id = rlGetShaderIdDefault();
        }
        UnloadModel(model);
        model = {};
    };
    unloadModelSafe(g_grassClumpModel);
    unloadModelSafe(g_grassImpostorModel);
    g_grassClumpMesh = nullptr;
    g_grassImpostorMesh = nullptr;
    g_grassClumpIsFallback = false;

    if (g_grassOwnsTexture && g_grassTexture.id > 0) UnloadTexture(g_grassTexture);
    g_grassTexture = {};
    g_grassOwnsTexture = false;

    if (g_grassShader.id > 0) UnloadShader(g_grassShader);
    g_grassShader = {};
    g_grassReady = false;
    g_grassClumpHeight = 0.323f;
    g_grassImpostorHeight = 0.32f;
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

static void clampGrassClusterKnobs() {
    g_grassClusterMin = std::clamp(g_grassClusterMin, 1, 20);
    g_grassClusterMax = std::clamp(g_grassClusterMax, g_grassClusterMin, 24);
    g_grassClusterRadius = std::clamp(g_grassClusterRadius, 0.25f, 4.0f);
    g_grassSeedSpacing = std::clamp(g_grassSeedSpacing, 1.2f, 12.0f);
    g_grassMeadowStrength = std::clamp(g_grassMeadowStrength, 0.0f, 1.0f);
    g_grassMeadowScale = std::clamp(g_grassMeadowScale, 0.008f, 0.10f);
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

// Ground one clump at (wx,wz). Re-checks water/slope; yaw/scale from hash.
static bool placeGrassClump(float wx, float wz, float maxSlope, float scaleBase,
                            float biomeScale, uint64_t h0, Matrix* out) {
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
    const float scaleMul =
        (s0 + engine::math::randFloat01(h2) * (s1 - s0)) * biomeScale;
    const float scaleY = scaleBase * scaleMul;
    // Slight XZ variance so clumps aren't perfect cylinders.
    const float xzJitter = 0.92f +
        engine::math::randFloat01(engine::math::splitmix64(h2 ^ 0xA5ULL)) * 0.16f;
    const float scaleXZ = scaleBase * scaleMul * xzJitter;
    *out = makeGrassTransform(wx, wy, wz, yaw, scaleY, scaleXZ);
    return true;
}

// Mid/far: single impostors on a coarse grid (cheap).
static void fillGrassBand(std::vector<Matrix>& out, float originX, float originZ, float size,
                          float spacing, int maxCount, float scaleBase, uint64_t seedTag) {
    out.clear();
    if (spacing < 0.4f || maxCount <= 0) return;
    const int cells = std::max(1, static_cast<int>(std::floor(size / spacing)));
    out.reserve(static_cast<size_t>(std::min(cells * cells, maxCount)));
    const uint64_t seed = engine::math::GetWorldConfig().seed ^ seedTag;
    const float maxSlope = g_grassMaxSlope;

    for (int iz = 0; iz < cells && static_cast<int>(out.size()) < maxCount; ++iz) {
        for (int ix = 0; ix < cells && static_cast<int>(out.size()) < maxCount; ++ix) {
            const int32_t gx = static_cast<int32_t>(std::floor(originX)) + ix * 17 + iz * 3;
            const int32_t gz = static_cast<int32_t>(std::floor(originZ)) + iz * 17 + ix * 5;
            uint64_t h0 = engine::math::hash2D(seed, gx, gz);
            const float jx = engine::math::randFloat01(h0);
            const float jz = engine::math::randFloat01(engine::math::splitmix64(h0));
            const float wx = originX + (static_cast<float>(ix) + jx) * (size / static_cast<float>(cells));
            const float wz = originZ + (static_cast<float>(iz) + jz) * (size / static_cast<float>(cells));
            float biomeScale = 1.0f;
            if (!grassSiteOk(wx, wz, maxSlope, &biomeScale)) continue;
            Matrix m{};
            if (placeGrassClump(wx, wz, maxSlope, scaleBase, biomeScale, h0, &m)) {
                out.push_back(m);
            }
        }
    }
}

struct GrassSeedCand {
    float    x = 0.0f;
    float    z = 0.0f;
    uint64_t h = 0;
    float    biomeScale = 1.0f;
};

// Near: world hex lattice → filter → uniform subsample under budget → clusters.
// IMPORTANT: never abort the lattice mid-row when the instance cap fills — that
// left entire +Z halves of each chunk bare (visible world-aligned stripes).
static void fillGrassNearClusters(std::vector<Matrix>& out, float originX, float originZ,
                                  float size, float densityMul) {
    out.clear();
    if (densityMul < 0.05f) return;
    clampGrassClusterKnobs();

    const float sp = std::max(1.2f, g_grassSeedSpacing);
    const float rowH = sp * 0.8660254f; // √3/2
    const uint64_t worldSeed = engine::math::GetWorldConfig().seed;
    const uint64_t seed = worldSeed ^ 0x67A55ULL;
    const float maxSlope = g_grassMaxSlope;
    const float meadowThresh = g_grassMeadowStrength * 0.50f;
    const int cMin = g_grassClusterMin;
    const int cMax = g_grassClusterMax;
    const float rMax = std::max(g_grassClusterRadius, sp * 0.65f);
    const float rMin = rMax * 0.72f;
    const float jitter = sp * 0.45f;

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

            float biomeScale = 1.0f;
            if (!grassSiteOk(sx, sz, maxSlope, &biomeScale)) continue;

            cands.push_back({sx, sz, h0, biomeScale});
        }
    }

    if (cands.empty()) return;

    // Pass 2: if clumps would exceed the cap, keep a uniform random subset of seeds
    // (hash-ordered), NOT "first rows until full".
    const float tDens = std::clamp(densityMul * 0.55f, 0.0f, 1.0f);
    const int avgClumps = std::max(1, (cMin + cMax + static_cast<int>(tDens * 3.0f)) / 2);
    const int maxSeeds = std::max(1, kGrassMaxNearPerChunk / avgClumps);

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
        kGrassMaxNearPerChunk, static_cast<int>(cands.size()) * (cMax + 2))));

    // Pass 3: place clusters for the (possibly thinned) seeds.
    for (const GrassSeedCand& cand : cands) {
        if (static_cast<int>(out.size()) >= kGrassMaxNearPerChunk) break;

        uint64_t h1 = engine::math::splitmix64(cand.h ^ 0xC1A55ULL);
        const int span = std::max(0, cMax - cMin);
        const int baseN = cMin +
            static_cast<int>(engine::math::randFloat01(h1) * static_cast<float>(span + 1));
        const int bonus = static_cast<int>(tDens * 3.0f);
        const int nClumps = std::clamp(baseN + bonus, cMin, cMax + 2);

        uint64_t h2 = engine::math::splitmix64(h1);
        const float clusterR = rMin + engine::math::randFloat01(h2) * (rMax - rMin);

        {
            Matrix m{};
            if (placeGrassClump(cand.x, cand.z, maxSlope, kGrassBaseScale, cand.biomeScale, h2, &m)) {
                out.push_back(m);
            }
        }

        for (int c = 1; c < nClumps && static_cast<int>(out.size()) < kGrassMaxNearPerChunk; ++c) {
            uint64_t hc = engine::math::splitmix64(h2 + static_cast<uint64_t>(c) * 0x9E37ULL);
            const float ang = engine::math::randFloat01(hc) * 6.2831853f;
            const float r = std::sqrt(engine::math::randFloat01(engine::math::splitmix64(hc))) *
                            clusterR;
            const float wx = cand.x + std::cos(ang) * r;
            const float wz = cand.z + std::sin(ang) * r;
            Matrix m{};
            if (placeGrassClump(wx, wz, maxSlope, kGrassBaseScale, cand.biomeScale, hc, &m)) {
                out.push_back(m);
            }
        }
    }
}

// Deterministic per-chunk grass bake (worker-safe). Near = clusters; mid/far = sparse impostors.
static GrassBake generateGrassCPU(float originX, float originZ, float size, int lod) {
    GrassBake bake;
    if (lod > kGrassChunkLodMax) return bake;
    const float density = std::clamp(g_grassDensity, 0.0f, 2.0f);
    if (density < 0.01f) return bake;

    const float nearD = std::max(0.05f, density * g_grassNearDensMul);
    const float midD  = std::max(0.05f, density * g_grassMidDensMul);
    const float farD  = std::max(0.05f, density * g_grassFarDensMul);

    fillGrassNearClusters(bake.near, originX, originZ, size, nearD);

    // Mid/far stay single-instance grids — do not blow LOD budgets.
    const float midSpacing = 3.20f / std::sqrt(midD);
    const float farSpacing = 6.50f / std::sqrt(farD);
    fillGrassBand(bake.mid, originX, originZ, size, midSpacing, kGrassMaxMidPerChunk,
                  kGrassImpostorScale, 0x91C3FULL);
    fillGrassBand(bake.far, originX, originZ, size, farSpacing, kGrassMaxFarPerChunk,
                  kGrassImpostorScale * 1.15f, 0xC0FFEEULL);
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
};

static std::unordered_map<uint64_t, std::unique_ptr<ChunkSlot>> g_chunks;

// Cross-thread queue of finished CPU meshes waiting for GPU upload.
struct PendingUpload {
    ChunkCoord coord;
    int        lod;
    MeshCPU    mesh;
    GrassBake  grass;
};
static std::mutex                 g_pendingMutex;
static std::vector<PendingUpload> g_pendingUploads;

// Chunk coords currently in-flight on the thread pool. Prevents double-enqueue.
static std::unordered_set<uint64_t> g_inFlight;

// GPU upload budget per frame (smooth streaming across many chunks).
static constexpr int GPU_UPLOAD_BUDGET_PER_FRAME = 16;

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
static void generateChunkAsync(ChunkCoord coord, int lod) {
    const float S = engine::math::WorldConfig::CHUNK_SIZE;
    const int   R = engine::math::GetResolutionForLOD(lod);
    float originX = coord.x * S;
    float originZ = coord.z * S;

    MeshCPU cpu = generateMeshCPU(originX, originZ, S, S, R, R);
    GrassBake grass = generateGrassCPU(originX, originZ, S, lod);

    std::lock_guard<std::mutex> lock(g_pendingMutex);
    g_pendingUploads.push_back({coord, lod, std::move(cpu), std::move(grass)});
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
    slot->grass       = generateGrassCPU(originX, originZ, S, 0);
    g_chunks[packCoord(coord.x, coord.z)] = std::move(slot);
}

namespace chunks {

void Init() {
    loadTerrainMaterials();
    loadGrassMaterials();

    // Prewarm spawn area synchronously at LOD 0 so first frame has immediate geometry.
    const int PREWARM = 2;  // 5x5 = 25 chunks around spawn
    for (int dz = -PREWARM; dz <= PREWARM; ++dz) {
        for (int dx = -PREWARM; dx <= PREWARM; ++dx) {
            generateChunkBlocking({dx, dz});
        }
    }
}

void Update(Vector3 playerPos) {
    const float S = engine::math::WorldConfig::CHUNK_SIZE;
    const int   R = engine::math::WorldConfig::LOAD_RADIUS;

    int pcx = static_cast<int>(std::floor(playerPos.x / S));
    int pcz = static_cast<int>(std::floor(playerPos.z / S));

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

            bool needsGeneration = false;
            if (it == g_chunks.end()) {
                needsGeneration = true;
            } else if (it->second->lod != targetLOD) {
                needsGeneration = true;
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

    for (const auto& task : tasks) {
        uint64_t k = packCoord(task.coord.x, task.coord.z);
        g_inFlight.insert(k);
        ChunkCoord c = task.coord;
        int targetLOD = task.targetLOD;
        engine::jobs::GetGlobalPool().Submit([c, targetLOD] {
            generateChunkAsync(c, targetLOD);
        });
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

        // Chunk was unloaded while worker was building it  Edrop mesh
        if (wanted.find(k) == wanted.end()) continue;

        Model model = uploadMeshToGPU(up.mesh);

        auto it = g_chunks.find(k);
        if (it != g_chunks.end()) {
            // LOD level upgrade/downgrade: unload old GPU mesh and replace
            if (it->second->modelLoaded) UnloadModel(it->second->model);
            it->second->model       = model;
            it->second->modelLoaded = true;
            it->second->lod         = up.lod;
            it->second->aabbMin     = up.mesh.aabbMin;
            it->second->aabbMax     = up.mesh.aabbMax;
            it->second->grass       = std::move(up.grass);
        } else {
            // Brand new chunk slot
            auto slot = std::make_unique<ChunkSlot>();
            slot->coord       = up.coord;
            slot->model       = model;
            slot->modelLoaded = true;
            slot->lod         = up.lod;
            slot->aabbMin     = up.mesh.aabbMin;
            slot->aabbMax     = up.mesh.aabbMax;
            slot->grass       = std::move(up.grass);
            g_chunks[k] = std::move(slot);
        }
    }
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
    rlEnableBackfaceCulling();
}

static void pushGrassBandUniforms(Vector3 viewPos, float fadeStart, float fadeEnd, float meshH) {
    const float t = static_cast<float>(GetTime());
    Vector3 sun = Vector3Normalize(g_sunDir);
    if (g_grassLocTime >= 0) {
        SetShaderValue(g_grassShader, g_grassLocTime, &t, SHADER_UNIFORM_FLOAT);
    }
    if (g_grassLocViewPos >= 0) {
        SetShaderValue(g_grassShader, g_grassLocViewPos, &viewPos, SHADER_UNIFORM_VEC3);
    }
    if (g_grassLocFadeStart >= 0) {
        SetShaderValue(g_grassShader, g_grassLocFadeStart, &fadeStart, SHADER_UNIFORM_FLOAT);
    }
    if (g_grassLocFadeEnd >= 0) {
        SetShaderValue(g_grassShader, g_grassLocFadeEnd, &fadeEnd, SHADER_UNIFORM_FLOAT);
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
        g_grassDrawStats.bakedNear += slot->grass.near.size();
        g_grassDrawStats.bakedMid  += slot->grass.mid.size();
        g_grassDrawStats.bakedFar  += slot->grass.far.size();
    }

    if (!g_grassEnabled || !g_grassReady) return;
    if (g_grassClumpMesh == nullptr || g_grassClumpMesh->vaoId == 0) return;
    if (g_grassImpostorMesh == nullptr || g_grassImpostorMesh->vaoId == 0) return;

    clampGrassLodDistances();
    const float nearEnd = g_grassNearDist;
    const float midEnd = g_grassMidDist;
    const float farEnd = g_grassFarDist;
    const float nearEndSq = nearEnd * nearEnd;
    const float midEndSq = midEnd * midEnd;
    const float farEndSq = farEnd * farEnd;

    static std::vector<Matrix> nearBatch;
    static std::vector<Matrix> midBatch;
    static std::vector<Matrix> farBatch;
    nearBatch.clear();
    midBatch.clear();
    farBatch.clear();
    nearBatch.reserve(static_cast<size_t>(kGrassMaxNearDraw));
    midBatch.reserve(static_cast<size_t>(kGrassMaxMidDraw));
    farBatch.reserve(static_cast<size_t>(kGrassMaxFarDraw));

    const float S = engine::math::WorldConfig::CHUNK_SIZE;
    const float chunkCull = farEnd + S * 0.75f;
    const float chunkCullSq = chunkCull * chunkCull;
    constexpr float padY = 2.0f;

    for (auto& [key, slot] : g_chunks) {
        (void)key;
        if (slot->lod > kGrassChunkLodMax) continue;
        const GrassBake& g = slot->grass;
        if (g.near.empty() && g.mid.empty() && g.far.empty()) continue;

        const float cx = (static_cast<float>(slot->coord.x) + 0.5f) * S;
        const float cz = (static_cast<float>(slot->coord.z) + 0.5f) * S;
        const float cdx = cx - viewPos.x;
        const float cdz = cz - viewPos.z;
        if (cdx * cdx + cdz * cdz > chunkCullSq) continue;

        Vector3 mn = slot->aabbMin;
        Vector3 mx = slot->aabbMax;
        mx.y += padY;
        if (!aabbInFrustum(mn, mx)) continue;

        if (static_cast<int>(nearBatch.size()) < kGrassMaxNearDraw) {
            for (const Matrix& m : g.near) {
                if (static_cast<int>(nearBatch.size()) >= kGrassMaxNearDraw) break;
                const Vector3 o = grassOrigin(m);
                const float dx = o.x - viewPos.x;
                const float dz = o.z - viewPos.z;
                if (dx * dx + dz * dz < nearEndSq) nearBatch.push_back(m);
            }
        }

        if (static_cast<int>(midBatch.size()) < kGrassMaxMidDraw) {
            for (const Matrix& m : g.mid) {
                if (static_cast<int>(midBatch.size()) >= kGrassMaxMidDraw) break;
                const Vector3 o = grassOrigin(m);
                const float dx = o.x - viewPos.x;
                const float dz = o.z - viewPos.z;
                const float d2 = dx * dx + dz * dz;
                if (d2 >= nearEndSq && d2 < midEndSq) midBatch.push_back(m);
            }
        }

        if (static_cast<int>(farBatch.size()) < kGrassMaxFarDraw) {
            for (const Matrix& m : g.far) {
                if (static_cast<int>(farBatch.size()) >= kGrassMaxFarDraw) break;
                const Vector3 o = grassOrigin(m);
                const float dx = o.x - viewPos.x;
                const float dz = o.z - viewPos.z;
                const float d2 = dx * dx + dz * dz;
                if (d2 >= midEndSq && d2 < farEndSq) farBatch.push_back(m);
            }
        }
    }

    g_grassDrawStats.drawNear = nearBatch.size();
    g_grassDrawStats.drawMid  = midBatch.size();
    g_grassDrawStats.drawFar  = farBatch.size();
    const size_t clumpTris =
        (g_grassClumpMesh && g_grassClumpMesh->triangleCount > 0)
            ? static_cast<size_t>(g_grassClumpMesh->triangleCount) : 195u;
    const size_t impostorTris =
        (g_grassImpostorMesh && g_grassImpostorMesh->triangleCount > 0)
            ? static_cast<size_t>(g_grassImpostorMesh->triangleCount) : 4u;
    g_grassDrawStats.approxTris =
        g_grassDrawStats.drawNear * clumpTris +
        (g_grassDrawStats.drawMid + g_grassDrawStats.drawFar) * impostorTris;

    if (nearBatch.empty() && midBatch.empty() && farBatch.empty()) return;

    rlDisableBackfaceCulling();

    if (!nearBatch.empty()) {
        pushGrassBandUniforms(viewPos, farEnd * 2.0f, farEnd * 2.0f + 1.0f, g_grassClumpHeight);
        DrawMeshInstanced(*g_grassClumpMesh, g_grassClumpMaterial,
                          nearBatch.data(), static_cast<int>(nearBatch.size()));
    }

    if (!midBatch.empty()) {
        pushGrassBandUniforms(viewPos, midEnd * 0.92f, farEnd, g_grassImpostorHeight);
        DrawMeshInstanced(*g_grassImpostorMesh, g_grassImpostorMaterial,
                          midBatch.data(), static_cast<int>(midBatch.size()));
    }

    if (!farBatch.empty()) {
        pushGrassBandUniforms(viewPos, midEnd, farEnd, g_grassImpostorHeight);
        DrawMeshInstanced(*g_grassImpostorMesh, g_grassImpostorMaterial,
                          farBatch.data(), static_cast<int>(farBatch.size()));
    }

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
    unloadGrassMaterials();
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
        n += slot->grass.near.size() + slot->grass.mid.size() + slot->grass.far.size();
    }
    return n;
}

const GrassDrawStats& GetGrassDrawStats() { return g_grassDrawStats; }

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

void SetGrassNearDistance(float meters) {
    g_grassNearDist = meters;
    clampGrassLodDistances();
}
float GetGrassNearDistance() { return g_grassNearDist; }

void SetGrassMidDistance(float meters) {
    g_grassMidDist = meters;
    clampGrassLodDistances();
}
float GetGrassMidDistance() { return g_grassMidDist; }

void SetGrassFarDistance(float meters) {
    g_grassFarDist = meters;
    clampGrassLodDistances();
}
float GetGrassFarDistance() { return g_grassFarDist; }

void SetGrassDrawDistance(float meters) { SetGrassFarDistance(meters); }
float GetGrassDrawDistance() { return g_grassFarDist; }

void SetGrassNearDensity(float mul) { g_grassNearDensMul = std::clamp(mul, 0.05f, 3.0f); }
float GetGrassNearDensity() { return g_grassNearDensMul; }
void SetGrassMidDensity(float mul) { g_grassMidDensMul = std::clamp(mul, 0.05f, 2.0f); }
float GetGrassMidDensity() { return g_grassMidDensMul; }
void SetGrassFarDensity(float mul) { g_grassFarDensMul = std::clamp(mul, 0.02f, 1.0f); }
float GetGrassFarDensity() { return g_grassFarDensMul; }

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

}  // namespace chunks
}  // namespace engine::terrain
