#include "game/enemy_model.hpp"
#include "raylib.h"
#include "rlgl.h"
#include "raymath.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>

namespace game::enemy_model {

namespace {
    Model            g_model      = {};
    ModelAnimation*  g_anims      = nullptr;
    int              g_animCount  = 0;
    int              g_clipIndex[static_cast<int>(AnimClip::Count)];
    bool             g_ready      = false;
    float            g_scale      = 1.0f;
    float            g_meshHeight = 2.0f;
    bool             g_loggedInvalid = false;

    // Must match raylib rmodels.c GLTF_ANIMDELAY (17 ms ≈ 60 poses/sec).
    constexpr float kAnimSampleFps = 1000.0f / 17.0f;
    // Player eye height is 2.0m (player_system EYE_HEIGHT). Mesh AABB is taller than
    // the visible body (~20–30% slack), so target > 2.0 to read as player-sized.
    constexpr float kTargetHeight  = 2.55f;

    // Root motion: Quaternius Walk loops in place (hip sways, net travel ≈ 0).
    // Synthetic stride = meters of forward travel per full clip cycle at rate 1.0.
    // Tuned ~0.85m * (2.55/2.0) so one Walk cycle ≈ one visual stride at player scale.
    constexpr float kSyntheticWalkStride = 1.10f;
    constexpr float kSyntheticRunStride  = 1.45f;
    constexpr float kRealRootNetThreshold = 0.25f; // model-space net XZ over cycle

    int         g_rootBone      = -1;
    char        g_rootBoneName[32] = "Hips";
    bool        g_syntheticRoot = true;
    float       g_stridePerFrame[static_cast<int>(AnimClip::Count)] = {};

    std::string makeAssetPath(const char* relative) {
        return std::string(GetApplicationDirectory()) + relative;
    }

    bool nameContains(const char* name, const char* needle) {
        if (!name || !needle) return false;
        return std::strstr(name, needle) != nullptr;
    }

    void mapClipsByName() {
        for (int i = 0; i < static_cast<int>(AnimClip::Count); ++i) {
            g_clipIndex[i] = -1;
        }

        for (int i = 0; i < g_animCount; ++i) {
            const char* n = g_anims[i].name;
            if (nameContains(n, "Idle") && g_clipIndex[static_cast<int>(AnimClip::Idle)] < 0) {
                g_clipIndex[static_cast<int>(AnimClip::Idle)] = i;
            } else if (nameContains(n, "Walk") && g_clipIndex[static_cast<int>(AnimClip::Walk)] < 0) {
                g_clipIndex[static_cast<int>(AnimClip::Walk)] = i;
            } else if (nameContains(n, "Run") && g_clipIndex[static_cast<int>(AnimClip::Run)] < 0) {
                g_clipIndex[static_cast<int>(AnimClip::Run)] = i;
            } else if (nameContains(n, "Bite") &&
                       g_clipIndex[static_cast<int>(AnimClip::Attack)] < 0) {
                g_clipIndex[static_cast<int>(AnimClip::Attack)] = i;
            } else if (nameContains(n, "Crawl") && g_clipIndex[static_cast<int>(AnimClip::Crawl)] < 0) {
                g_clipIndex[static_cast<int>(AnimClip::Crawl)] = i;
            }
        }

        if (g_clipIndex[static_cast<int>(AnimClip::Attack)] < 0) {
            for (int i = 0; i < g_animCount; ++i) {
                if (nameContains(g_anims[i].name, "Attack")) {
                    g_clipIndex[static_cast<int>(AnimClip::Attack)] = i;
                    break;
                }
            }
        }

        int fallback = g_clipIndex[static_cast<int>(AnimClip::Idle)];
        if (fallback < 0 && g_animCount > 0) fallback = 0;
        for (int i = 0; i < static_cast<int>(AnimClip::Count); ++i) {
            if (g_clipIndex[i] < 0) g_clipIndex[i] = fallback;
        }

        for (int i = 0; i < g_animCount; ++i) {
            TraceLog(LOG_INFO, "ENEMY: anim[%d] name='%s' frames=%d bones=%d",
                     i, g_anims[i].name, g_anims[i].frameCount, g_anims[i].boneCount);
        }
        TraceLog(LOG_INFO,
                 "ENEMY: clips Idle=%d Walk=%d Run=%d Attack=%d Crawl=%d sampleFps=%.2f",
                 g_clipIndex[static_cast<int>(AnimClip::Idle)],
                 g_clipIndex[static_cast<int>(AnimClip::Walk)],
                 g_clipIndex[static_cast<int>(AnimClip::Run)],
                 g_clipIndex[static_cast<int>(AnimClip::Attack)],
                 g_clipIndex[static_cast<int>(AnimClip::Crawl)],
                 kAnimSampleFps);
    }

    void ensureAlbedoTexture(Model& model) {
        if (model.materialCount <= 0) return;

        for (int mi = 0; mi < model.materialCount; ++mi) {
            // Default shader has no boneMatrices — keep CPU skinning path only.
            if (model.materials[mi].shader.locs) {
                model.materials[mi].shader.locs[SHADER_LOC_BONE_MATRICES] = -1;
            }

            Texture& albedo = model.materials[mi].maps[MATERIAL_MAP_ALBEDO].texture;
            if (albedo.id > 0) {
                // Detailed body albedo (not the old 32x32 palette) — bilinear.
                SetTextureFilter(albedo, TEXTURE_FILTER_BILINEAR);
                continue;
            }

            std::string texPath = makeAssetPath("assets/models/enemies/zombie/ZombieTexture.png");
            Texture2D tex = LoadTexture(texPath.c_str());
            if (tex.id == 0) {
                TraceLog(LOG_WARNING, "ENEMY: missing zombie texture %s", texPath.c_str());
                continue;
            }
            SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
            albedo = tex;
            model.materials[mi].maps[MATERIAL_MAP_ALBEDO].color = WHITE;
        }
    }

    // LoadModel() uploads with GL_STATIC_DRAW. Recreate position/normal VBOs as
    // DYNAMIC so UpdateModelAnimation uploads are reliable, and refresh VAO links.
    void makeMeshesDynamicForAnimation(Model& model) {
        for (int i = 0; i < model.meshCount; ++i) {
            Mesh& mesh = model.meshes[i];
            if (!mesh.animVertices || !mesh.vboId || mesh.vertexCount <= 0) continue;

            constexpr int kPos = 0;
            constexpr int kNrm = 2;
            const int bytes = mesh.vertexCount * 3 * (int)sizeof(float);

            const unsigned int oldPos = mesh.vboId[kPos];
            const unsigned int oldNrm = mesh.vboId[kNrm];
            mesh.vboId[kPos] = rlLoadVertexBuffer(mesh.animVertices, bytes, true);
            if (oldPos) rlUnloadVertexBuffer(oldPos);

            if (mesh.animNormals) {
                mesh.vboId[kNrm] = rlLoadVertexBuffer(mesh.animNormals, bytes, true);
                if (oldNrm) rlUnloadVertexBuffer(oldNrm);
            }

            if (mesh.vaoId != 0) {
                rlEnableVertexArray(mesh.vaoId);
                rlEnableVertexBuffer(mesh.vboId[kPos]);
                rlSetVertexAttribute(kPos, 3, RL_FLOAT, 0, 0, 0);
                rlEnableVertexAttribute(kPos);
                if (mesh.animNormals && mesh.vboId[kNrm]) {
                    rlEnableVertexBuffer(mesh.vboId[kNrm]);
                    rlSetVertexAttribute(kNrm, 3, RL_FLOAT, 0, 0, 0);
                    rlEnableVertexAttribute(kNrm);
                }
                rlDisableVertexArray();
            }
        }
        TraceLog(LOG_INFO, "ENEMY: skinned mesh VBOs recreated as DYNAMIC_DRAW");
    }

    int findRootBoneIndex(const ModelAnimation& anim) {
        for (int i = 0; i < anim.boneCount; ++i) {
            const char* n = anim.bones[i].name;
            if (!n) continue;
            if (std::strcmp(n, "Hips") == 0 || std::strcmp(n, "Hip") == 0 ||
                std::strcmp(n, "Root") == 0 || std::strcmp(n, "root") == 0 ||
                nameContains(n, "Hips") || nameContains(n, "mixamorig_Hips")) {
                return i;
            }
        }
        // Prefer a bone with parent -1 (armature root).
        for (int i = 0; i < anim.boneCount; ++i) {
            if (anim.bones[i].parent < 0) return i;
        }
        return anim.boneCount > 0 ? 0 : -1;
    }

    void analyzeRootMotion() {
        g_rootBone = -1;
        g_syntheticRoot = true;
        for (int i = 0; i < static_cast<int>(AnimClip::Count); ++i) {
            g_stridePerFrame[i] = 0.0f;
        }

        const int walkIdx = g_clipIndex[static_cast<int>(AnimClip::Walk)];
        if (walkIdx < 0 || !g_anims) {
            TraceLog(LOG_WARNING, "ENEMY: root-motion analysis skipped (no Walk)");
            return;
        }

        ModelAnimation& walk = g_anims[walkIdx];
        g_rootBone = findRootBoneIndex(walk);
        if (g_rootBone < 0 || walk.frameCount < 2 || !walk.framePoses) {
            TraceLog(LOG_WARNING, "ENEMY: no root bone for root-motion");
            return;
        }
        std::strncpy(g_rootBoneName, walk.bones[g_rootBone].name, sizeof(g_rootBoneName) - 1);
        g_rootBoneName[sizeof(g_rootBoneName) - 1] = '\0';

        Vector3 p0 = walk.framePoses[0][g_rootBone].translation;
        Vector3 pLast = walk.framePoses[walk.frameCount - 1][g_rootBone].translation;
        float netX = pLast.x - p0.x;
        float netZ = pLast.z - p0.z;
        float netXZ = sqrtf(netX * netX + netZ * netZ);

        float maxDev = 0.0f;
        for (int f = 0; f < walk.frameCount; ++f) {
            Vector3 p = walk.framePoses[f][g_rootBone].translation;
            float dx = p.x - p0.x;
            float dz = p.z - p0.z;
            float d = sqrtf(dx * dx + dz * dz);
            if (d > maxDev) maxDev = d;
        }

        // In-place loops return near start; hip sway still produces maxDev > 0.
        g_syntheticRoot = (netXZ < kRealRootNetThreshold);

        const int runIdx = g_clipIndex[static_cast<int>(AnimClip::Run)];
        auto setStride = [&](AnimClip clip, float strideMeters) {
            int idx = g_clipIndex[static_cast<int>(clip)];
            if (idx < 0 || g_anims[idx].frameCount <= 0) return;
            g_stridePerFrame[static_cast<int>(clip)] =
                strideMeters / (float)g_anims[idx].frameCount;
        };
        setStride(AnimClip::Walk, kSyntheticWalkStride);
        setStride(AnimClip::Run, kSyntheticRunStride);

        TraceLog(LOG_INFO,
                 "ENEMY: root bone='%s' idx=%d Walk netXZ=%.3f maxDev=%.3f mode=%s "
                 "walkStride=%.2fm/cycle runStride=%.2fm/cycle scale=%.4f",
                 g_rootBoneName, g_rootBone, netXZ, maxDev,
                 g_syntheticRoot ? "SYNTHETIC" : "REAL",
                 kSyntheticWalkStride, kSyntheticRunStride, g_scale);

        // Append to diag file if present.
        std::string diagPath = makeAssetPath("enemy_anim_diag.txt");
        FILE* f = std::fopen(diagPath.c_str(), "a");
        if (f) {
            std::fprintf(f,
                         "rootBone='%s' idx=%d netXZ=%.4f maxDev=%.4f synthetic=%d "
                         "walkStride=%.3f runStride=%.3f\n",
                         g_rootBoneName, g_rootBone, netXZ, maxDev,
                         g_syntheticRoot ? 1 : 0,
                         kSyntheticWalkStride, kSyntheticRunStride);
            std::fclose(f);
        }
    }

    AnimClip clipForAnimIndex(int animIndex) {
        for (int c = 0; c < static_cast<int>(AnimClip::Count); ++c) {
            if (g_clipIndex[c] == animIndex) return static_cast<AnimClip>(c);
        }
        return AnimClip::Idle;
    }

    void writeDiagFile(const Model& model, const char* path) {
        std::string diagPath = makeAssetPath("enemy_anim_diag.txt");
        FILE* f = std::fopen(diagPath.c_str(), "w");
        if (!f) return;
        std::fprintf(f, "path=%s\n", path);
        std::fprintf(f, "ready modelBones=%d meshes=%d anims=%d sampleFps=%.3f\n",
                     model.boneCount, model.meshCount, g_animCount, kAnimSampleFps);
        for (int mi = 0; mi < model.meshCount; ++mi) {
            const Mesh& mesh = model.meshes[mi];
            std::fprintf(f,
                         "mesh[%d] verts=%d boneCount=%d weights=%d ids=%d animVerts=%d matrices=%d\n",
                         mi, mesh.vertexCount, mesh.boneCount,
                         mesh.boneWeights ? 1 : 0, mesh.boneIds ? 1 : 0,
                         mesh.animVertices ? 1 : 0, mesh.boneMatrices ? 1 : 0);
        }
        for (int i = 0; i < g_animCount; ++i) {
            std::fprintf(f, "anim[%d] name='%s' frames=%d bones=%d valid=%d\n",
                         i, g_anims[i].name, g_anims[i].frameCount, g_anims[i].boneCount,
                         IsModelAnimationValid(model, g_anims[i]) ? 1 : 0);
        }
        std::fprintf(f, "clips Idle=%d Walk=%d Run=%d Attack=%d Crawl=%d\n",
                     g_clipIndex[static_cast<int>(AnimClip::Idle)],
                     g_clipIndex[static_cast<int>(AnimClip::Walk)],
                     g_clipIndex[static_cast<int>(AnimClip::Run)],
                     g_clipIndex[static_cast<int>(AnimClip::Attack)],
                     g_clipIndex[static_cast<int>(AnimClip::Crawl)]);
        std::fclose(f);
        TraceLog(LOG_INFO, "ENEMY: wrote %s", diagPath.c_str());
    }
}

void Init() {
    if (g_ready) return;

    for (int i = 0; i < static_cast<int>(AnimClip::Count); ++i) {
        g_clipIndex[i] = -1;
    }

    std::string path = makeAssetPath("assets/models/enemies/zombie/Zombie.glb");
    Model model = LoadModel(path.c_str());
    if (model.meshCount <= 0 || model.meshes == nullptr) {
        TraceLog(LOG_WARNING, "ENEMY: failed to load zombie glb %s, trying OBJ", path.c_str());
        if (model.meshCount > 0) UnloadModel(model);
        path = makeAssetPath("assets/models/enemies/zombie/Zombie.obj");
        model = LoadModel(path.c_str());
    }

    if (model.meshCount <= 0 || model.meshes == nullptr) {
        TraceLog(LOG_WARNING, "ENEMY: failed to load zombie %s", path.c_str());
        if (model.meshCount > 0) UnloadModel(model);
        g_model = {};
        g_ready = false;
        return;
    }

    ensureAlbedoTexture(model);
    makeMeshesDynamicForAnimation(model);

    TraceLog(LOG_INFO, "ENEMY: model bones=%d meshes=%d materials=%d",
             model.boneCount, model.meshCount, model.materialCount);
    for (int mi = 0; mi < model.meshCount; ++mi) {
        const Mesh& mesh = model.meshes[mi];
        TraceLog(LOG_INFO,
                 "ENEMY: mesh[%d] verts=%d boneCount=%d boneWeights=%s boneIds=%s "
                 "animVerts=%s boneMatrices=%s",
                 mi, mesh.vertexCount, mesh.boneCount,
                 mesh.boneWeights ? "yes" : "NO",
                 mesh.boneIds ? "yes" : "NO",
                 mesh.animVertices ? "yes" : "NO",
                 mesh.boneMatrices ? "yes" : "NO");
    }

    g_animCount = 0;
    g_anims = LoadModelAnimations(path.c_str(), &g_animCount);
    if (g_anims && g_animCount > 0) {
        mapClipsByName();
        for (int i = 0; i < g_animCount; ++i) {
            const bool ok = IsModelAnimationValid(model, g_anims[i]);
            TraceLog(ok ? LOG_INFO : LOG_WARNING,
                     "ENEMY: anim[%d] '%s' frames=%d bones=%d valid=%s",
                     i, g_anims[i].name, g_anims[i].frameCount, g_anims[i].boneCount,
                     ok ? "yes" : "NO");
        }
        int idleIdx = g_clipIndex[static_cast<int>(AnimClip::Idle)];
        if (idleIdx >= 0) {
            UpdateModelAnimation(model, g_anims[idleIdx], 0);
        }
    } else {
        TraceLog(LOG_WARNING, "ENEMY: no animations in %s (static mesh only)", path.c_str());
    }

    BoundingBox bb = GetModelBoundingBox(model);
    g_meshHeight = bb.max.y - bb.min.y;
    if (g_meshHeight < 0.01f) g_meshHeight = 1.0f;
    g_scale = kTargetHeight / g_meshHeight;

    g_model = model;
    g_ready = true;
    writeDiagFile(g_model, path.c_str());
    analyzeRootMotion(); // after diag create; appends root-motion summary
    TraceLog(LOG_INFO,
             "ENEMY: zombie ready path=%s meshH=%.3f scale=%.4f feetY=%.3f anims=%d",
             path.c_str(), g_meshHeight, g_scale, bb.min.y, g_animCount);
}

void Shutdown() {
    if (g_anims) {
        UnloadModelAnimations(g_anims, g_animCount);
        g_anims = nullptr;
        g_animCount = 0;
    }
    if (g_model.meshCount > 0) {
        UnloadModel(g_model);
    }
    g_model = {};
    g_ready = false;
    g_scale = 1.0f;
    g_meshHeight = 2.0f;
    g_loggedInvalid = false;
    g_rootBone = -1;
    g_syntheticRoot = true;
    for (int i = 0; i < static_cast<int>(AnimClip::Count); ++i) {
        g_clipIndex[i] = -1;
        g_stridePerFrame[i] = 0.0f;
    }
}

bool IsReady() { return g_ready; }

const Model& GetModel() { return g_model; }

float GetUniformScale() { return g_scale; }

float GetMeshHeight() { return g_meshHeight; }

float GetTargetHeight() { return kTargetHeight; }

int GetAnimIndex(AnimClip clip) {
    int c = static_cast<int>(clip);
    if (c < 0 || c >= static_cast<int>(AnimClip::Count)) return -1;
    return g_clipIndex[c];
}

int GetAnimFrameCount(int animIndex) {
    if (!g_anims || animIndex < 0 || animIndex >= g_animCount) return 0;
    return g_anims[animIndex].frameCount;
}

float GetAnimSampleFps() { return kAnimSampleFps; }

void ApplyAnimation(int animIndex, int frame) {
    if (!g_ready || !g_anims || animIndex < 0 || animIndex >= g_animCount) return;
    ModelAnimation& anim = g_anims[animIndex];
    if (anim.frameCount <= 0) return;
    if (frame < 0) frame = 0;
    if (frame >= anim.frameCount) frame = frame % anim.frameCount;

    if (!IsModelAnimationValid(g_model, anim)) {
        if (!g_loggedInvalid) {
            TraceLog(LOG_WARNING,
                     "ENEMY: IsModelAnimationValid failed modelBones=%d animBones=%d (still applying)",
                     g_model.boneCount, anim.boneCount);
            g_loggedInvalid = true;
        }
    }

    // CPU skinning + VBO upload (default shader has no GPU boneMatrices).
    UpdateModelAnimation(g_model, anim, frame);
}

bool UsesSyntheticRootMotion() { return g_syntheticRoot; }

int GetRootBoneIndex() { return g_rootBone; }

const char* GetRootBoneName() { return g_rootBoneName; }

Vector2 GetRootMotionDeltaXZ(int animIndex, int fromFrame, int toFrame) {
    Vector2 out = {0.0f, 0.0f};
    if (!g_ready || !g_anims || animIndex < 0 || animIndex >= g_animCount) return out;

    ModelAnimation& anim = g_anims[animIndex];
    if (anim.frameCount <= 0 || !anim.framePoses) return out;
    if (fromFrame < 0 || toFrame < 0) return out;

    fromFrame %= anim.frameCount;
    toFrame %= anim.frameCount;
    if (fromFrame == toFrame) return out;

    AnimClip clip = clipForAnimIndex(animIndex);
    const bool loco = (clip == AnimClip::Walk || clip == AnimClip::Run);

    if (g_syntheticRoot || g_rootBone < 0) {
        if (!loco) return out;
        // Continuous forward step; include wrap (from last → 0).
        float step = g_stridePerFrame[static_cast<int>(clip)];
        out.y = step; // model +Z forward stored in Vector2.y
        return out;
    }

    // Real root translation: zero on wrap to avoid teleport back to cycle start.
    if (toFrame < fromFrame) return out;

    Vector3 a = anim.framePoses[fromFrame][g_rootBone].translation;
    Vector3 b = anim.framePoses[toFrame][g_rootBone].translation;
    out.x = (b.x - a.x) * g_scale;
    out.y = (b.z - a.z) * g_scale; // model Z → Vector2.y
    return out;
}

bool AdvanceAnimation(int animIndex, float dt, float playbackRate,
                      int& animFrame, float& animTimer, Vector2& outMotionXZ) {
    outMotionXZ = {0.0f, 0.0f};
    if (!g_ready || !g_anims || animIndex < 0 || animIndex >= g_animCount) return false;

    const int frameCount = g_anims[animIndex].frameCount;
    if (frameCount <= 0) return false;

    float rate = playbackRate;
    if (rate < 0.01f) rate = 1.0f;

    bool wrapped = false;
    animTimer += dt * kAnimSampleFps * rate;
    while (animTimer >= 1.0f) {
        animTimer -= 1.0f;
        const int from = animFrame;
        animFrame += 1;
        if (animFrame >= frameCount) {
            animFrame = 0;
            wrapped = true;
        }
        Vector2 step = GetRootMotionDeltaXZ(animIndex, from, animFrame);
        outMotionXZ.x += step.x;
        outMotionXZ.y += step.y;
    }
    return wrapped;
}

}  // namespace game::enemy_model
