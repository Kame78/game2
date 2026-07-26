#include "game/character_visual.hpp"
#include "rlgl.h"
#include "raymath.h"
#include <cmath>

namespace game {
namespace {

Color Shade(Color c, float mul) {
    auto ch = [](int v) -> unsigned char {
        if (v < 0) return 0;
        if (v > 255) return 255;
        return (unsigned char)v;
    };
    return Color{ch((int)(c.r * mul)), ch((int)(c.g * mul)), ch((int)(c.b * mul)), c.a};
}

Color Alpha(Color c, unsigned char a) {
    return Color{c.r, c.g, c.b, a};
}

void BeginChar(Vector3 center, float yawRad) {
    rlPushMatrix();
    rlTranslatef(center.x, center.y, center.z);
    rlRotatef(yawRad * RAD2DEG, 0.0f, 1.0f, 0.0f);
}

void EndChar() {
    rlPopMatrix();
}

void Box(Vector3 p, float w, float h, float d, Color c) {
    DrawCube(p, w, h, d, c);
}

void Sphere(Vector3 p, float r, Color c) {
    DrawSphere(p, r, c);
}

// Capsule-ish limb: cylinder between two points, sphere ends.
void Limb(Vector3 a, Vector3 b, float radius, Color c) {
    DrawCylinderEx(a, b, radius, radius, 8, c);
    DrawSphere(a, radius * 1.05f, c);
    DrawSphere(b, radius * 1.05f, c);
}

void DrawHumanoidCore(float H, float W, float D, Color skin, Color cloth, Color accent,
                      float anim, bool bulky, bool horns) {
    const float footY = -H * 0.5f;
    const float headR = H * (bulky ? 0.10f : 0.09f);
    const float torsoH = H * 0.28f;
    const float torsoW = W * (bulky ? 0.72f : 0.55f);
    const float torsoD = D * (bulky ? 0.55f : 0.42f);
    const float hipY = footY + H * 0.32f;
    const float chestY = hipY + torsoH * 0.55f;
    const float shoulderY = chestY + torsoH * 0.35f;
    const float headY = shoulderY + headR * 1.35f + H * 0.02f;

    float walk = sinf(anim * 6.0f);
    float walk2 = sinf(anim * 6.0f + 3.14159f);
    float armSwing = walk * H * 0.06f;
    float legSwing = walk * H * 0.08f;
    float legSwing2 = walk2 * H * 0.08f;
    float bob = fabsf(walk) * H * 0.01f;

    // Legs
    float legR = W * (bulky ? 0.14f : 0.11f);
    Vector3 hipL = {-torsoW * 0.22f, hipY, 0.0f};
    Vector3 hipR = { torsoW * 0.22f, hipY, 0.0f};
    Vector3 footL = {-torsoW * 0.22f, footY + H * 0.02f + bob, legSwing};
    Vector3 footR = { torsoW * 0.22f, footY + H * 0.02f + bob, legSwing2};
    Vector3 kneeL = {footL.x, (hipL.y + footL.y) * 0.5f, footL.z * 0.5f};
    Vector3 kneeR = {footR.x, (hipR.y + footR.y) * 0.5f, footR.z * 0.5f};
    Color pant = Shade(cloth, 0.75f);
    Limb(hipL, kneeL, legR, pant);
    Limb(kneeL, footL, legR * 0.9f, pant);
    Limb(hipR, kneeR, legR, pant);
    Limb(kneeR, footR, legR * 0.9f, pant);
    Box({footL.x, footY + H * 0.025f + bob, footL.z + H * 0.02f}, W * 0.18f, H * 0.04f, D * 0.28f, Shade(cloth, 0.45f));
    Box({footR.x, footY + H * 0.025f + bob, footR.z + H * 0.02f}, W * 0.18f, H * 0.04f, D * 0.28f, Shade(cloth, 0.45f));

    // Hips + torso
    Box({0, hipY, 0}, torsoW * 0.95f, H * 0.08f, torsoD * 0.95f, Shade(cloth, 0.85f));
    Box({0, chestY + bob, 0}, torsoW, torsoH, torsoD, cloth);
    if (bulky) {
        Box({-torsoW * 0.55f, shoulderY + bob, 0}, W * 0.28f, H * 0.08f, D * 0.35f, accent);
        Box({ torsoW * 0.55f, shoulderY + bob, 0}, W * 0.28f, H * 0.08f, D * 0.35f, accent);
    }

    // Arms
    float armR = W * (bulky ? 0.11f : 0.09f);
    Vector3 shL = {-torsoW * 0.55f, shoulderY + bob, 0};
    Vector3 shR = { torsoW * 0.55f, shoulderY + bob, 0};
    Vector3 handL = {shL.x - W * 0.05f, hipY + H * 0.05f + bob, armSwing};
    Vector3 handR = {shR.x + W * 0.05f, hipY + H * 0.05f + bob, -armSwing};
    Vector3 elbL = {(shL.x + handL.x) * 0.5f, (shL.y + handL.y) * 0.55f, handL.z * 0.5f};
    Vector3 elbR = {(shR.x + handR.x) * 0.5f, (shR.y + handR.y) * 0.55f, handR.z * 0.5f};
    Color sleeve = Shade(cloth, 0.9f);
    Limb(shL, elbL, armR, sleeve);
    Limb(elbL, handL, armR * 0.85f, skin);
    Limb(shR, elbR, armR, sleeve);
    Limb(elbR, handR, armR * 0.85f, skin);

    // Neck + head
    DrawCylinderEx({0, shoulderY + bob, 0}, {0, headY - headR * 0.7f + bob, 0},
                   headR * 0.35f, headR * 0.4f, 8, skin);
    Sphere({0, headY + bob, 0}, headR, skin);

    // Eyes
    float eyeZ = headR * 0.75f;
    float eyeY = headY + bob + headR * 0.05f;
    Sphere({-headR * 0.35f, eyeY, eyeZ}, headR * 0.18f, WHITE);
    Sphere({ headR * 0.35f, eyeY, eyeZ}, headR * 0.18f, WHITE);
    Sphere({-headR * 0.35f, eyeY, eyeZ + headR * 0.12f}, headR * 0.09f, Color{20, 20, 20, 255});
    Sphere({ headR * 0.35f, eyeY, eyeZ + headR * 0.12f}, headR * 0.09f, Color{20, 20, 20, 255});

    if (horns) {
        Color horn = Shade(accent, 0.7f);
        Limb({-headR * 0.45f, headY + headR * 0.5f + bob, 0},
             {-headR * 0.9f, headY + headR * 1.6f + bob, -headR * 0.2f}, headR * 0.18f, horn);
        Limb({ headR * 0.45f, headY + headR * 0.5f + bob, 0},
             { headR * 0.9f, headY + headR * 1.6f + bob, -headR * 0.2f}, headR * 0.18f, horn);
    }
}

void DrawWingPair(float shoulderY, float torsoW, float H, float W, Color wing, float flap, bool large) {
    float span = W * (large ? 2.8f : 1.8f);
    float chord = H * (large ? 0.55f : 0.38f);
    float thick = H * 0.04f;
    float lift = flap * H * 0.12f;
    // Left / right wing planes (slightly swept back)
    Box({-torsoW * 0.5f - span * 0.35f, shoulderY + lift, -chord * 0.15f},
        span * 0.7f, thick, chord, wing);
    Box({ torsoW * 0.5f + span * 0.35f, shoulderY + lift, -chord * 0.15f},
        span * 0.7f, thick, chord, wing);
    // Secondary feathers
    Color tip = Shade(wing, 1.15f);
    tip.a = wing.a;
    Box({-torsoW * 0.5f - span * 0.65f, shoulderY + lift * 0.6f - H * 0.05f, -chord * 0.05f},
        span * 0.4f, thick * 0.7f, chord * 0.7f, tip);
    Box({ torsoW * 0.5f + span * 0.65f, shoulderY + lift * 0.6f - H * 0.05f, -chord * 0.05f},
        span * 0.4f, thick * 0.7f, chord * 0.7f, tip);
}

void DrawHalo(float y, float radius, Color c) {
    DrawCircle3D({0, y, 0}, radius, {1, 0, 0}, 90.0f, c);
    DrawCircle3D({0, y + radius * 0.08f, 0}, radius * 0.85f, {1, 0, 0}, 90.0f, Shade(c, 1.2f));
}

void DrawGargoyle(float H, float W, float D, Color stone, float anim) {
    const float footY = -H * 0.5f;
    Color dark = Shade(stone, 0.65f);
    Color glow = Color{255, 90, 40, 255};
    float crouch = H * 0.04f;
    float flap = sinf(anim * 5.0f);

    // Hunched torso
    Box({0, footY + H * 0.38f - crouch, -H * 0.02f}, W * 0.7f, H * 0.32f, D * 0.55f, stone);
    Box({0, footY + H * 0.22f - crouch, 0}, W * 0.55f, H * 0.14f, D * 0.45f, dark);

    // Digigrade-ish legs
    Limb({-W * 0.18f, footY + H * 0.28f, 0}, {-W * 0.22f, footY + H * 0.12f, D * 0.15f}, W * 0.1f, dark);
    Limb({-W * 0.22f, footY + H * 0.12f, D * 0.15f}, {-W * 0.2f, footY + H * 0.03f, 0}, W * 0.09f, dark);
    Limb({ W * 0.18f, footY + H * 0.28f, 0}, { W * 0.22f, footY + H * 0.12f, D * 0.15f}, W * 0.1f, dark);
    Limb({ W * 0.22f, footY + H * 0.12f, D * 0.15f}, { W * 0.2f, footY + H * 0.03f, 0}, W * 0.09f, dark);
    Box({-W * 0.2f, footY + H * 0.02f, H * 0.02f}, W * 0.2f, H * 0.04f, D * 0.3f, Shade(stone, 0.5f));
    Box({ W * 0.2f, footY + H * 0.02f, H * 0.02f}, W * 0.2f, H * 0.04f, D * 0.3f, Shade(stone, 0.5f));

    // Arms + claws
    Limb({-W * 0.4f, footY + H * 0.48f, 0}, {-W * 0.55f, footY + H * 0.25f, D * 0.2f}, W * 0.09f, stone);
    Limb({ W * 0.4f, footY + H * 0.48f, 0}, { W * 0.55f, footY + H * 0.25f, D * 0.2f}, W * 0.09f, stone);
    for (int i = -1; i <= 1; i++) {
        Vector3 tipL = {-W * 0.62f + i * W * 0.04f, footY + H * 0.18f, D * 0.28f};
        Vector3 tipR = { W * 0.62f + i * W * 0.04f, footY + H * 0.18f, D * 0.28f};
        Limb({-W * 0.55f, footY + H * 0.25f, D * 0.2f}, tipL, W * 0.03f, dark);
        Limb({ W * 0.55f, footY + H * 0.25f, D * 0.2f}, tipR, W * 0.03f, dark);
    }

    // Blocky head + horns + glowing eyes
    float headY = footY + H * 0.58f - crouch;
    Box({0, headY, D * 0.05f}, W * 0.42f, H * 0.16f, D * 0.4f, stone);
    Limb({-W * 0.12f, headY + H * 0.08f, 0}, {-W * 0.28f, headY + H * 0.22f, -D * 0.05f}, W * 0.05f, dark);
    Limb({ W * 0.12f, headY + H * 0.08f, 0}, { W * 0.28f, headY + H * 0.22f, -D * 0.05f}, W * 0.05f, dark);
    Sphere({-W * 0.1f, headY + H * 0.02f, D * 0.22f}, W * 0.06f, glow);
    Sphere({ W * 0.1f, headY + H * 0.02f, D * 0.22f}, W * 0.06f, glow);

    // Stone wings
    Color wing = Shade(stone, 0.8f);
    wing.a = 230;
    DrawWingPair(footY + H * 0.5f - crouch, W * 0.5f, H, W, wing, flap, false);
}

void DrawAngel(float H, float W, float D, Color robe, float anim, bool arch) {
    Color skin = Color{255, 230, 200, 255};
    Color cloth = robe;
    Color accent = arch ? Color{255, 245, 180, 255} : Color{255, 220, 120, 255};
    Color wingCol = arch ? Color{255, 250, 230, 230} : Color{240, 235, 210, 210};

    DrawHumanoidCore(H, W, D, skin, cloth, accent, anim * 0.7f, arch, false);

    float flap = sinf(anim * (arch ? 3.5f : 4.5f));
    float shoulderY = -H * 0.5f + H * 0.72f;
    DrawWingPair(shoulderY, W * 0.55f, H, W * (arch ? 1.15f : 1.0f), wingCol, flap, arch);

    float headTop = -H * 0.5f + H * 0.95f;
    DrawHalo(headTop + H * 0.06f, W * (arch ? 0.45f : 0.32f), accent);

    if (arch) {
        // Soft glow aura + longer robe hem
        Sphere({0, -H * 0.05f, 0}, H * 0.35f, Alpha(accent, 35));
        Box({0, -H * 0.5f + H * 0.18f, 0}, W * 0.7f, H * 0.12f, D * 0.5f, Shade(cloth, 0.9f));
        // Light staff
        Limb({W * 0.45f, -H * 0.1f, D * 0.1f}, {W * 0.5f, H * 0.45f, D * 0.15f}, W * 0.04f, accent);
        Sphere({W * 0.5f, H * 0.48f, D * 0.15f}, W * 0.12f, Color{255, 255, 240, 255});
    }
}

void DrawReaper(float H, float W, float D, Color cloak, float anim) {
    const float footY = -H * 0.5f;
    Color bone = Color{210, 200, 185, 255};
    Color dark = Shade(cloak, 0.55f);
    Color voidC = Color{15, 5, 20, 255};
    Color eye = Color{180, 40, 255, 255};
    float sway = sinf(anim * 2.2f) * H * 0.02f;

    // Flowing cloak body
    Box({0, footY + H * 0.42f + sway, 0}, W * 0.75f, H * 0.55f, D * 0.45f, cloak);
    Box({0, footY + H * 0.18f, 0}, W * 0.9f, H * 0.22f, D * 0.55f, dark);
    // Hood
    Box({0, footY + H * 0.78f + sway, D * 0.02f}, W * 0.5f, H * 0.2f, D * 0.48f, cloak);
    Box({0, footY + H * 0.72f + sway, D * 0.12f}, W * 0.38f, H * 0.12f, D * 0.2f, voidC);
    // Skull face in hood
    Sphere({0, footY + H * 0.72f + sway, D * 0.18f}, W * 0.16f, bone);
    Sphere({-W * 0.07f, footY + H * 0.74f + sway, D * 0.28f}, W * 0.045f, eye);
    Sphere({ W * 0.07f, footY + H * 0.74f + sway, D * 0.28f}, W * 0.045f, eye);

    // Skeletal arms
    Limb({-W * 0.35f, footY + H * 0.55f, 0}, {-W * 0.55f, footY + H * 0.35f, D * 0.15f}, W * 0.05f, bone);
    Limb({-W * 0.55f, footY + H * 0.35f, D * 0.15f}, {-W * 0.5f, footY + H * 0.22f, D * 0.25f}, W * 0.045f, bone);
    Limb({ W * 0.2f, footY + H * 0.55f, 0}, {W * 0.35f, footY + H * 0.4f, D * 0.1f}, W * 0.05f, bone);

    // Scythe
    Vector3 poleB = {W * 0.35f, footY + H * 0.05f, D * 0.05f};
    Vector3 poleT = {W * 0.42f, footY + H * 0.95f, -D * 0.05f};
    Limb(poleB, poleT, W * 0.04f, Shade(cloak, 0.4f));
    // Curved blade (segmented)
    Vector3 bladeRoot = poleT;
    for (int i = 0; i < 5; i++) {
        float t0 = i / 5.0f;
        float t1 = (i + 1) / 5.0f;
        float a0 = t0 * 1.5f;
        float a1 = t1 * 1.5f;
        float br = H * 0.28f;
        Vector3 p0 = {bladeRoot.x - sinf(a0) * br, bladeRoot.y - (1.0f - cosf(a0)) * br * 0.35f, bladeRoot.z + cosf(a0) * br * 0.15f};
        Vector3 p1 = {bladeRoot.x - sinf(a1) * br, bladeRoot.y - (1.0f - cosf(a1)) * br * 0.35f, bladeRoot.z + cosf(a1) * br * 0.15f};
        Limb(p0, p1, W * 0.035f, Color{200, 200, 210, 255});
    }

    // Floating shadow / mist at feet
    Sphere({0, footY + H * 0.08f, 0}, W * 0.55f, Alpha(voidC, 90));
}

void DrawFamiliar(float H, float W, Color body, Color glow, float anim, bool sprite) {
    float bob = sinf(anim * 7.0f) * H * 0.15f;
    float flap = sinf(anim * 14.0f);
    Sphere({0, bob, 0}, H * 0.35f, body);
    Sphere({0, bob, 0}, H * 0.5f, Alpha(glow, 50));
    // Eyes
    Sphere({-H * 0.12f, bob + H * 0.05f, H * 0.28f}, H * 0.08f, WHITE);
    Sphere({ H * 0.12f, bob + H * 0.05f, H * 0.28f}, H * 0.08f, WHITE);
    Sphere({-H * 0.12f, bob + H * 0.05f, H * 0.34f}, H * 0.04f, Color{30, 20, 40, 255});
    Sphere({ H * 0.12f, bob + H * 0.05f, H * 0.34f}, H * 0.04f, Color{30, 20, 40, 255});
    // Tiny wings
    Color wing = sprite ? Color{255, 250, 200, 180} : Color{200, 140, 255, 180};
    Box({-W * 0.55f, bob + flap * H * 0.1f, 0}, W * 0.7f, H * 0.08f, H * 0.45f, wing);
    Box({ W * 0.55f, bob - flap * H * 0.1f, 0}, W * 0.7f, H * 0.08f, H * 0.45f, wing);
    if (sprite) {
        DrawHalo(bob + H * 0.45f, H * 0.35f, glow);
    }
}

} // namespace

void DrawCharacterVisual(CharacterVisual visual, Vector3 center,
                         float width, float height, float depth,
                         Color color, float yawRadians, float animTime) {
    if (visual == CharacterVisual::Box) {
        DrawCube(center, width, height, depth, color);
        DrawCubeWires(center, width, height, depth, BLACK);
        return;
    }

    float H = fmaxf(height, 0.2f);
    float W = fmaxf(width, H * 0.25f);
    float D = fmaxf(depth, H * 0.2f);

    BeginChar(center, yawRadians);

    switch (visual) {
    case CharacterVisual::Humanoid: {
        Color skin = Color{180, 120, 100, 255};
        Color cloth = color;
        DrawHumanoidCore(H, W, D, skin, cloth, Shade(cloth, 0.7f), animTime, false, false);
        break;
    }
    case CharacterVisual::Elite: {
        Color skin = Color{160, 90, 80, 255};
        DrawHumanoidCore(H, W, D, skin, color, Color{40, 40, 40, 255}, animTime, true, true);
        break;
    }
    case CharacterVisual::Gargoyle:
        DrawGargoyle(H, W, D, color, animTime);
        break;
    case CharacterVisual::BattleAngel:
        DrawAngel(H, W, D, color, animTime, false);
        break;
    case CharacterVisual::ArchAngel:
        DrawAngel(H, W, D, color, animTime, true);
        break;
    case CharacterVisual::Reaper:
        DrawReaper(H, W, D, color, animTime);
        break;
    case CharacterVisual::Pixie:
        DrawFamiliar(H, W, color, Color{200, 100, 255, 255}, animTime, false);
        break;
    case CharacterVisual::Sprite:
        DrawFamiliar(H, W, color, Color{255, 240, 160, 255}, animTime, true);
        break;
    default:
        DrawCube({0, 0, 0}, W, H, D, color);
        break;
    }

    EndChar();
}

} // namespace game
