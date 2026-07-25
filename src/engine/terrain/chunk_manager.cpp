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
// Instanced grass (Poly Haven grass_medium clump, LOD0 chunks only)
// ---------------------------------------------------------------------------
static bool     g_grassEnabled = true;
static float    g_grassDensity = 1.0f;
static float    g_grassMaxSlope = 0.32f;
static float    g_grassDrawDistance = 220.0f;
static constexpr float kGrassWaterGateMax = 0.12f;
static constexpr int   kGrassMaxPerChunk = 1400; // denser plains coverage cap
static constexpr int   kGrassMaxLod = 0; // only near LOD0 rings
// Source mesh is ~0.323 m tall, bottom-origin. Scale ≈4 → ~1.3 m clumps.
static constexpr float kGrassMeshHeight = 0.323f;
static constexpr float kGrassBaseScale = 3.9f;
static constexpr float kGrassSink = 0.02f; // bury 2 cm so slopes don't show gaps

static Shader   g_grassShader = {};
static Material g_grassMaterial = {};
static Model    g_grassModel = {};
static Mesh*    g_grassMesh = nullptr; // owned by g_grassModel
static Texture  g_grassTexture = {};
static bool     g_grassOwnsTexture = false;
static bool     g_grassReady = false;
static int      g_grassLocTime = -1;
static int      g_grassLocViewPos = -1;
static int      g_grassLocFadeStart = -1;
static int      g_grassLocFadeEnd = -1;
static int      g_grassLocSunDir = -1;
static int      g_grassLocSunIntensity = -1;
static int      g_grassLocMeshHeight = -1;

static void loadGrassMaterials() {
    if (g_grassReady) return;

    std::string vs = makeAssetPath("assets/shaders/grass.vs");
    std::string fs = makeAssetPath("assets/shaders/grass.fs");
    g_grassShader = LoadShader(vs.c_str(), fs.c_str());
    if (g_grassShader.id == 0) {
        TraceLog(LOG_WARNING, "GRASS: shader failed to load");
        return;
    }

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

    std::string modelPath = makeAssetPath("assets/models/grass/grass_medium.obj");
    g_grassModel = LoadModel(modelPath.c_str());
    if (g_grassModel.meshCount <= 0 || g_grassModel.meshes == nullptr) {
        TraceLog(LOG_WARNING, "GRASS: failed to load model %s", modelPath.c_str());
        UnloadShader(g_grassShader);
        g_grassShader = {};
        return;
    }
    g_grassMesh = &g_grassModel.meshes[0];

    std::string texPath = makeAssetPath("assets/models/grass/grass_albedo.png");
    Image img = LoadImage(texPath.c_str());
    if (img.data != nullptr) {
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        g_grassTexture = LoadTextureFromImage(img);
        UnloadImage(img);
        GenTextureMipmaps(&g_grassTexture);
        SetTextureFilter(g_grassTexture, TEXTURE_FILTER_BILINEAR);
        g_grassOwnsTexture = true;
    } else {
        TraceLog(LOG_WARNING, "GRASS: albedo missing (%s), using white", texPath.c_str());
        Image fallback = GenImageColor(4, 4, Color{180, 200, 90, 255});
        g_grassTexture = LoadTextureFromImage(fallback);
        UnloadImage(fallback);
        g_grassOwnsTexture = true;
    }

    g_grassMaterial = LoadMaterialDefault();
    g_grassMaterial.shader = g_grassShader;
    g_grassMaterial.maps[MATERIAL_MAP_ALBEDO].texture = g_grassTexture;
    g_grassMaterial.maps[MATERIAL_MAP_ALBEDO].color = WHITE;
    g_grassReady = true;
    TraceLog(LOG_INFO,
             "GRASS: loaded %s (%d verts, %d tris) + albedo; denser plains instancing",
             modelPath.c_str(),
             g_grassMesh->vertexCount,
             g_grassMesh->triangleCount);
}

static void unloadGrassMaterials() {
    if (!g_grassReady && g_grassModel.meshCount == 0 && g_grassShader.id == 0) return;

    // Detach owned resources before UnloadMaterial (it would free shader/texture).
    if (g_grassMaterial.maps) {
        g_grassMaterial.maps[MATERIAL_MAP_ALBEDO].texture = {};
        g_grassMaterial.shader.id = rlGetShaderIdDefault();
        UnloadMaterial(g_grassMaterial);
    }
    g_grassMaterial = {};

    if (g_grassModel.meshCount > 0) {
        // Model may own a default material; clear our shared texture refs first.
        for (int i = 0; i < g_grassModel.materialCount; ++i) {
            g_grassModel.materials[i].maps[MATERIAL_MAP_ALBEDO].texture = {};
            g_grassModel.materials[i].shader.id = rlGetShaderIdDefault();
        }
        UnloadModel(g_grassModel);
    }
    g_grassModel = {};
    g_grassMesh = nullptr;

    if (g_grassOwnsTexture && g_grassTexture.id > 0) UnloadTexture(g_grassTexture);
    g_grassTexture = {};
    g_grassOwnsTexture = false;

    if (g_grassShader.id > 0) UnloadShader(g_grassShader);
    g_grassShader = {};
    g_grassReady = false;
}

static Matrix makeGrassTransform(float x, float y, float z, float yaw, float scaleY, float scaleXZ) {
    Matrix S = MatrixScale(scaleXZ, scaleY, scaleXZ);
    Matrix R = MatrixRotateY(yaw);
    Matrix T = MatrixTranslate(x, y, z);
    return MatrixMultiply(MatrixMultiply(T, R), S);
}

// Deterministic grass instances for one chunk. Safe on worker threads.
// Approx at density=1 on Plains: ~1.35 m spacing → up to kGrassMaxPerChunk (1400)
// instances / 128 m chunk. ~9–16 LOD0 chunks in draw range → ~12k–22k instances
// × ~195 tris ≈ 2.3–4.3 M tris peak after frustum cull (typically lower).
static std::vector<Matrix> generateGrassCPU(float originX, float originZ, float size, int lod) {
    std::vector<Matrix> out;
    if (lod > kGrassMaxLod) return out;
    const float density = std::clamp(g_grassDensity, 0.0f, 2.0f);
    if (density < 0.01f) return out;

    // Dense plains carpet: ~1.35 m cell at density 1 on a 128 m chunk.
    const float spacing = 1.35f / std::sqrt(density);
    const int cells = std::max(1, static_cast<int>(std::floor(size / spacing)));
    out.reserve(static_cast<size_t>(std::min(cells * cells, kGrassMaxPerChunk)));

    const uint64_t seed = engine::math::GetWorldConfig().seed ^ 0x67A55ULL;
    const float maxSlope = g_grassMaxSlope;

    for (int iz = 0; iz < cells && static_cast<int>(out.size()) < kGrassMaxPerChunk; ++iz) {
        for (int ix = 0; ix < cells && static_cast<int>(out.size()) < kGrassMaxPerChunk; ++ix) {
            const int32_t gx = static_cast<int32_t>(std::floor(originX)) + ix * 17 + iz * 3;
            const int32_t gz = static_cast<int32_t>(std::floor(originZ)) + iz * 17 + ix * 5;
            uint64_t h0 = engine::math::hash2D(seed, gx, gz);
            float jx = engine::math::randFloat01(h0);
            float jz = engine::math::randFloat01(engine::math::splitmix64(h0));
            float wx = originX + (static_cast<float>(ix) + jx) * (size / static_cast<float>(cells));
            float wz = originZ + (static_cast<float>(iz) + jz) * (size / static_cast<float>(cells));

            const float gate = engine::math::WaterGate(wx, wz);
            if (gate > kGrassWaterGateMax) continue;

            const auto biome = engine::math::PrimaryRegion(wx, wz);
            float accept = 0.0f;
            switch (biome) {
                case engine::math::WorldRegion::Plains:   accept = 1.00f; break;
                case engine::math::WorldRegion::Wetlands: accept = 0.35f; break;
                case engine::math::WorldRegion::Hills:    accept = 0.10f; break;
                default: continue; // Mountains / Water
            }
            uint64_t h1 = engine::math::splitmix64(h0 ^ 0x9E3779B97F4A7C15ULL);
            if (engine::math::randFloat01(h1) > accept) continue;

            if (engine::math::TerrainSlope(wx, wz) > maxSlope) continue;

            // Match rendered chunk mesh height; bottom-origin mesh + slight sink.
            const float groundY = engine::math::WorldHeight(wx, wz);
            const float waterY = engine::math::LocalWaterLevel(wx, wz);
            if (groundY < waterY + 0.2f) continue;
            const float wy = groundY - kGrassSink;

            uint64_t h2 = engine::math::splitmix64(h1);
            float yaw = engine::math::randFloat01(h2) * 6.2831853f;
            float scaleMul = 0.85f + engine::math::randFloat01(engine::math::splitmix64(h2)) * 0.40f;
            if (biome == engine::math::WorldRegion::Wetlands) scaleMul *= 1.10f;
            float scaleY = kGrassBaseScale * scaleMul;
            float scaleXZ = kGrassBaseScale * (0.90f +
                engine::math::randFloat01(engine::math::splitmix64(h2 ^ 0xA5ULL)) * 0.25f);

            out.push_back(makeGrassTransform(wx, wy, wz, yaw, scaleY, scaleXZ));
        }
    }
    return out;
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
    std::vector<Matrix> grassTransforms;
};

static std::unordered_map<uint64_t, std::unique_ptr<ChunkSlot>> g_chunks;

// Cross-thread queue of finished CPU meshes waiting for GPU upload.
struct PendingUpload {
    ChunkCoord coord;
    int        lod;
    MeshCPU    mesh;
    std::vector<Matrix> grass;
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
    std::vector<Matrix> grass = generateGrassCPU(originX, originZ, S, lod);

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
    slot->grassTransforms = generateGrassCPU(originX, originZ, S, 0);
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
            it->second->grassTransforms = std::move(up.grass);
        } else {
            // Brand new chunk slot
            auto slot = std::make_unique<ChunkSlot>();
            slot->coord       = up.coord;
            slot->model       = model;
            slot->modelLoaded = true;
            slot->lod         = up.lod;
            slot->aabbMin     = up.mesh.aabbMin;
            slot->aabbMax     = up.mesh.aabbMax;
            slot->grassTransforms = std::move(up.grass);
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

void DrawGrass(Vector3 viewPos) {
    if (!g_grassEnabled || !g_grassReady) return;
    if (g_grassMesh == nullptr || g_grassMesh->vaoId == 0) return;

    static std::vector<Matrix> batch;
    batch.clear();

    const float S = engine::math::WorldConfig::CHUNK_SIZE;
    const float drawDist = std::max(40.0f, g_grassDrawDistance);
    const float drawDistSq = drawDist * drawDist;
    // Pad AABB for scaled clumps (~1.3 m tall).
    constexpr float padY = 2.0f;

    for (auto& [key, slot] : g_chunks) {
        (void)key;
        if (slot->lod > kGrassMaxLod) continue;
        if (slot->grassTransforms.empty()) continue;

        const float cx = (static_cast<float>(slot->coord.x) + 0.5f) * S;
        const float cz = (static_cast<float>(slot->coord.z) + 0.5f) * S;
        const float dx = cx - viewPos.x;
        const float dz = cz - viewPos.z;
        if (dx * dx + dz * dz > drawDistSq) continue;

        Vector3 mn = slot->aabbMin;
        Vector3 mx = slot->aabbMax;
        mx.y += padY;
        if (!aabbInFrustum(mn, mx)) continue;

        batch.insert(batch.end(), slot->grassTransforms.begin(), slot->grassTransforms.end());
    }

    if (batch.empty()) return;

    const float t = static_cast<float>(GetTime());
    const float fadeStart = drawDist * 0.72f;
    const float fadeEnd = drawDist;
    Vector3 sun = Vector3Normalize(g_sunDir);
    const float meshH = kGrassMeshHeight;

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

    rlDisableBackfaceCulling();
    // Alpha-tested cutout (discard in FS) — no blend sort needed.
    DrawMeshInstanced(*g_grassMesh, g_grassMaterial, batch.data(), static_cast<int>(batch.size()));
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
        n += slot->grassTransforms.size();
    }
    return n;
}

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

void SetGrassDrawDistance(float meters) { g_grassDrawDistance = std::clamp(meters, 40.0f, 800.0f); }
float GetGrassDrawDistance() { return g_grassDrawDistance; }

}  // namespace chunks
}  // namespace engine::terrain
