#include "engine/terrain/chunk_manager.hpp"
#include "engine/math/noise.hpp"
#include "engine/jobs/thread_pool.hpp"
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
    std::unique_ptr<unsigned char[]>  colors;     // rgba*vertexCount
    std::unique_ptr<unsigned short[]> indices;    // 3*triangleCount
    Vector3 aabbMin = {0, 0, 0};
    Vector3 aabbMax = {0, 0, 0};
};

static Color heightToColor(float y) {
    if (y < -1.0f) return {60, 90, 60, 255};       // low green (marsh)
    if (y <  5.0f) return {90, 130, 70, 255};      // plains green
    if (y < 20.0f) return {110, 100, 80, 255};     // hills brown
    if (y < 40.0f) return {130, 120, 110, 255};    // mountain rock
    return {220, 220, 230, 255};                    // snowcap
}

// Generate a heightmap mesh covering [origin, origin + size] in world space.
static MeshCPU generateMeshCPU(float originX, float originZ, float sizeX, float sizeZ,
                               int resX, int resZ) {
    MeshCPU m;
    m.vertexCount   = resX * resZ;
    m.triangleCount = (resX - 1) * (resZ - 1) * 2;

    m.vertices = std::make_unique<float[]>(m.vertexCount * 3);
    m.normals  = std::make_unique<float[]>(m.vertexCount * 3);
    m.colors   = std::make_unique<unsigned char[]>(m.vertexCount * 4);
    m.indices  = std::make_unique<unsigned short[]>(m.triangleCount * 3);

    float minY =  1e9f, maxY = -1e9f;

    // Vertices + colors
    for (int z = 0; z < resZ; ++z) {
        for (int x = 0; x < resX; ++x) {
            float wx = originX + static_cast<float>(x) / static_cast<float>(resX - 1) * sizeX;
            float wz = originZ + static_cast<float>(z) / static_cast<float>(resZ - 1) * sizeZ;
            float wy = engine::math::WorldHeight(wx, wz);
            if (wy < minY) minY = wy;
            if (wy > maxY) maxY = wy;

            int i = z * resX + x;
            m.vertices[i * 3 + 0] = wx;
            m.vertices[i * 3 + 1] = wy;
            m.vertices[i * 3 + 2] = wz;

            Color c = heightToColor(wy);
            m.colors[i * 4 + 0] = c.r;
            m.colors[i * 4 + 1] = c.g;
            m.colors[i * 4 + 2] = c.b;
            m.colors[i * 4 + 3] = 255;
        }
    }

    // --- MODIFIED: Indices with proper CCW winding order when viewed from above ---
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

    // Normals via central differences on the height field.
    float stepX = sizeX / static_cast<float>(resX - 1);
    float stepZ = sizeZ / static_cast<float>(resZ - 1);
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

    m.aabbMin = {originX,         minY, originZ};
    m.aabbMax = {originX + sizeX, maxY, originZ + sizeZ};
    return m;
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
    size_t tSize = cpu.vertexCount * 2 * sizeof(float);  // texcoords (u,v per vertex)

    mesh.vertices  = static_cast<float*>(MemAlloc(static_cast<unsigned int>(vSize)));
    mesh.normals   = static_cast<float*>(MemAlloc(static_cast<unsigned int>(vSize)));
    mesh.colors    = static_cast<unsigned char*>(MemAlloc(static_cast<unsigned int>(cSize)));
    mesh.indices   = static_cast<unsigned short*>(MemAlloc(static_cast<unsigned int>(iSize)));
    mesh.texcoords = static_cast<float*>(MemAlloc(static_cast<unsigned int>(tSize)));

    std::memcpy(mesh.vertices, cpu.vertices.get(), vSize);
    std::memcpy(mesh.normals,  cpu.normals.get(),  vSize);
    std::memcpy(mesh.colors,   cpu.colors.get(),   cSize);
    std::memcpy(mesh.indices,  cpu.indices.get(),  iSize);

    // Fill texcoords with zeros — Raylib default shader needs valid UVs
    // to sample the white default texture and multiply with vertex colors.
    std::memset(mesh.texcoords, 0, tSize);

    UploadMesh(&mesh, false);
    return LoadModelFromMesh(mesh);
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
};

static std::unordered_map<uint64_t, std::unique_ptr<ChunkSlot>> g_chunks;

// Cross-thread queue of finished CPU meshes waiting for GPU upload.
struct PendingUpload {
    ChunkCoord coord;
    int        lod;
    MeshCPU    mesh;
};
static std::mutex                 g_pendingMutex;
static std::vector<PendingUpload> g_pendingUploads;

// Chunk coords currently in-flight on the thread pool. Prevents double-enqueue.
static std::unordered_set<uint64_t> g_inFlight;

// GPU upload budget per frame (smooth streaming across many chunks).
static constexpr int GPU_UPLOAD_BUDGET_PER_FRAME = 8;

// Worker task: build CPU mesh at given LOD resolution, push to pending queue.
static void generateChunkAsync(ChunkCoord coord, int lod) {
    const float S = engine::math::WorldConfig::CHUNK_SIZE;
    const int   R = engine::math::GetResolutionForLOD(lod);
    float originX = coord.x * S;
    float originZ = coord.z * S;

    MeshCPU cpu = generateMeshCPU(originX, originZ, S, S, R, R);

    std::lock_guard<std::mutex> lock(g_pendingMutex);
    g_pendingUploads.push_back({coord, lod, std::move(cpu)});
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
    g_chunks[packCoord(coord.x, coord.z)] = std::move(slot);
}

namespace chunks {

void Init() {
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
            int targetLOD = engine::math::GetLODForDistance(dist);

            bool needsGeneration = false;
            if (g_chunks.find(k) == g_chunks.end()) {
                needsGeneration = true;
            } else if (g_chunks[k]->lod != targetLOD) {
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

        // Chunk was unloaded while worker was building it — drop mesh
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
        } else {
            // Brand new chunk slot
            auto slot = std::make_unique<ChunkSlot>();
            slot->coord       = up.coord;
            slot->model       = model;
            slot->modelLoaded = true;
            slot->lod         = up.lod;
            slot->aabbMin     = up.mesh.aabbMin;
            slot->aabbMax     = up.mesh.aabbMax;
            g_chunks[k] = std::move(slot);
        }
    }
}

void Draw() {
    // Debug: log once to confirm Draw() is being reached
    static bool logged = false;
    if (!logged) {
        TraceLog(LOG_INFO, "TERRAIN: chunks::Draw() called, %zu chunks loaded", g_chunks.size());
        logged = true;
    }

    rlDisableBackfaceCulling();
    for (auto& [key, slot] : g_chunks) {
        if (slot->modelLoaded) {
            DrawModel(slot->model, {0.0f, 0.0f, 0.0f}, 1.0f, WHITE);
        }
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
}

size_t LoadedChunkCount() { return g_chunks.size(); }

size_t PendingUploadCount() {
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    return g_pendingUploads.size();
}

}  // namespace chunks
}  // namespace engine::terrain
