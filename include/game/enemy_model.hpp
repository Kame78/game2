#pragma once
#include "raylib.h"

namespace game::enemy_model {
    // Shared Quaternius zombie mesh + clips — load once at startup, draw for all enemies.
    enum class AnimClip {
        Idle = 0,
        Walk,
        Run,
        Attack,  // Bite
        Crawl,
        Count
    };

    void Init();
    void Shutdown();

    bool IsReady();
    const Model& GetModel();
    float GetUniformScale();   // AABB height → GetTargetHeight()
    float GetMeshHeight();     // unscaled AABB height (bind pose)
    float GetTargetHeight();   // world standing height (player-sized)

    // Resolve clip -> animation index (-1 if missing).
    int GetAnimIndex(AnimClip clip);
    int GetAnimFrameCount(int animIndex);

    // raylib samples glTF at ~60 poses/sec (GLTF_ANIMDELAY=17ms).
    float GetAnimSampleFps();

    // Apply pose for one enemy draw. Safe to call per-enemy on the shared Model
    // because each UpdateModelAnimation + Draw is sequential.
    void ApplyAnimation(int animIndex, int frame);

    // --- Root-motion / anim-driven locomotion ---
    // true = Walk hip translation is in-place; we use calibrated stride steps.
    bool UsesSyntheticRootMotion();
    int GetRootBoneIndex();
    const char* GetRootBoneName();

    // Model-local XZ delta for one frame step from→to (forward = +Z).
    // On wrap with real root motion: returns zero (avoid teleport).
    // Synthetic: always returns forward stride/frameCount (including wrap).
    Vector2 GetRootMotionDeltaXZ(int animIndex, int fromFrame, int toFrame);

    // Advance animation by dt; accumulate root-motion XZ in model space (+Z forward).
    // Returns true if a full cycle wrapped this tick (for one-shot clips).
    bool AdvanceAnimation(int animIndex, float dt, float playbackRate,
                          int& animFrame, float& animTimer, Vector2& outMotionXZ);
}
