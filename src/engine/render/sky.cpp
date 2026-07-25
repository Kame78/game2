#include "engine/render/sky.hpp"
#include "rlgl.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace engine::render::sky {

namespace {

constexpr float kPi = 3.14159265358979323846f;

Shader  g_shader      = {};
Texture g_hdri        = {};
Model   g_cube        = {};
int     g_locExposure = -1;
bool    g_ready       = false;
bool    g_modelOk     = false;

// Evening-road PureSky is relatively bright; start slightly under 1.0.
float   g_exposure    = 0.85f;
Color   g_hazeColor   = {180, 200, 220, 255};
Vector3 g_hazeLinear  = {0.71f, 0.78f, 0.86f};
float   g_hazeTintStrength = 1.0f;
Vector3 g_ambientCube[6] = {};

std::string makeAssetPath(const char* relative) {
    return std::string(GetApplicationDirectory()) + relative;
}

float luminance(float r, float g, float b) {
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

Vector3 tonemapReinhard(Vector3 c, float exposure) {
    c.x *= exposure;
    c.y *= exposure;
    c.z *= exposure;
    c.x = c.x / (1.0f + c.x);
    c.y = c.y / (1.0f + c.y);
    c.z = c.z / (1.0f + c.z);
    auto toSrgb = [](float x) {
        x = std::clamp(x, 0.0f, 1.0f);
        return std::pow(x, 1.0f / 2.2f);
    };
    return {toSrgb(c.x), toSrgb(c.y), toSrgb(c.z)};
}

struct HdriView {
    const Image* img = nullptr;
    int channels = 3;
    bool isF32 = true;
};

bool makeHdriView(const Image& img, HdriView& out) {
    out.img = &img;
    out.isF32 =
        img.format == PIXELFORMAT_UNCOMPRESSED_R32G32B32 ||
        img.format == PIXELFORMAT_UNCOMPRESSED_R32G32B32A32;
    const bool isU8 =
        img.format == PIXELFORMAT_UNCOMPRESSED_R8G8B8 ||
        img.format == PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    if (!out.isF32 && !isU8) return false;
    out.channels = (img.format == PIXELFORMAT_UNCOMPRESSED_R32G32B32 ||
                    img.format == PIXELFORMAT_UNCOMPRESSED_R8G8B8)
                       ? 3
                       : 4;
    return img.data != nullptr && img.width > 0 && img.height > 0;
}

Vector3 readPixel(const HdriView& v, int x, int y) {
    x = ((x % v.img->width) + v.img->width) % v.img->width;
    y = std::clamp(y, 0, v.img->height - 1);
    const size_t idx = (static_cast<size_t>(y) * v.img->width + x) * v.channels;
    if (v.isF32) {
        const float* px = static_cast<const float*>(v.img->data);
        return {px[idx], px[idx + 1], px[idx + 2]};
    }
    const unsigned char* px = static_cast<const unsigned char*>(v.img->data);
    return {px[idx] / 255.0f, px[idx + 1] / 255.0f, px[idx + 2] / 255.0f};
}

// Matches sky.fs / water env sampling (Y-up, V flipped so row 0 = +Y).
Vector3 directionFromUv(float u, float vImg) {
    const float lon = (u - 0.5f) * (2.0f * kPi);
    const float lat = (0.5f - vImg) * kPi;
    const float cl = std::cos(lat);
    return {cl * std::cos(lon), std::sin(lat), cl * std::sin(lon)};
}

void estimateHazeAndAmbient(const Image& img) {
    HdriView view{};
    if (!makeHdriView(img, view)) return;

    const int stepX = std::max(1, img.width / 128);
    const int stepY = std::max(1, img.height / 64);

    double hazeR = 0.0, hazeG = 0.0, hazeB = 0.0, hazeW = 0.0;
    double cubeR[6] = {}, cubeG[6] = {}, cubeB[6] = {}, cubeW[6] = {};

    const Vector3 faces[6] = {
        {1, 0, 0}, {-1, 0, 0},
        {0, 1, 0}, {0, -1, 0},
        {0, 0, 1}, {0, 0, -1},
    };

    const float dLon = (2.0f * kPi) * static_cast<float>(stepX) / static_cast<float>(img.width);
    const float dLat = kPi * static_cast<float>(stepY) / static_cast<float>(img.height);

    for (int y = 0; y < img.height; y += stepY) {
        const float vImg = (static_cast<float>(y) + 0.5f) / static_cast<float>(img.height);
        const float lat = (0.5f - vImg) * kPi;
        const float cosLat = std::cos(lat);
        const float solid = std::max(cosLat, 0.0f) * dLat * dLon;
        if (solid <= 1e-8f) continue;

        for (int x = 0; x < img.width; x += stepX) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(img.width);
            const Vector3 dir = directionFromUv(u, vImg);
            const Vector3 c = readPixel(view, x, y);

            if (dir.y > 0.05f) {
                const double w = static_cast<double>(dir.y) * solid;
                hazeR += c.x * w;
                hazeG += c.y * w;
                hazeB += c.z * w;
                hazeW += w;
            }

            for (int f = 0; f < 6; ++f) {
                const float nd = dir.x * faces[f].x + dir.y * faces[f].y + dir.z * faces[f].z;
                if (nd <= 0.0f) continue;
                const double w = static_cast<double>(nd) * solid;
                cubeR[f] += c.x * w;
                cubeG[f] += c.y * w;
                cubeB[f] += c.z * w;
                cubeW[f] += w;
            }
        }
    }

    if (hazeW > 1e-6) {
        Vector3 avg = {
            static_cast<float>(hazeR / hazeW),
            static_cast<float>(hazeG / hazeW),
            static_cast<float>(hazeB / hazeW),
        };
        g_hazeLinear = tonemapReinhard(avg, g_exposure);
        g_hazeColor = {
            static_cast<unsigned char>(g_hazeLinear.x * 255.0f + 0.5f),
            static_cast<unsigned char>(g_hazeLinear.y * 255.0f + 0.5f),
            static_cast<unsigned char>(g_hazeLinear.z * 255.0f + 0.5f),
            255,
        };
        TraceLog(LOG_INFO, "SKY: haze RGB (%d,%d,%d) L=%.3f",
                 g_hazeColor.r, g_hazeColor.g, g_hazeColor.b,
                 luminance(g_hazeLinear.x, g_hazeLinear.y, g_hazeLinear.z));
    }

    // Cosine-hemisphere irradiance ≁E(sum L * cos * dOmega)  Ealready weighted by cos.
    for (int f = 0; f < 6; ++f) {
        if (cubeW[f] < 1e-8) {
            g_ambientCube[f] = {0.15f, 0.15f, 0.18f};
            continue;
        }
        Vector3 irr = {
            static_cast<float>(cubeR[f] / cubeW[f]),
            static_cast<float>(cubeG[f] / cubeW[f]),
            static_cast<float>(cubeB[f] / cubeW[f]),
        };
        irr.x *= g_exposure;
        irr.y *= g_exposure;
        irr.z *= g_exposure;
        // Soft clamp so terrain doesn't blow out before albedo multiply.
        auto soft = [](float x) { return x / (1.0f + x * 0.35f); };
        g_ambientCube[f] = {soft(irr.x), soft(irr.y), soft(irr.z)};
    }

    TraceLog(LOG_INFO,
             "SKY: ambientCube +Y=(%.2f,%.2f,%.2f) +X=(%.2f,%.2f,%.2f)",
             g_ambientCube[2].x, g_ambientCube[2].y, g_ambientCube[2].z,
             g_ambientCube[0].x, g_ambientCube[0].y, g_ambientCube[0].z);
}

}  // namespace

void Init() {
    if (g_ready) return;

    const std::string vs = makeAssetPath("assets/shaders/sky.vs");
    const std::string fs = makeAssetPath("assets/shaders/sky.fs");
    const std::string hdriPath = makeAssetPath("assets/textures/sky/evening_road_puresky.hdr");

    g_shader = LoadShader(vs.c_str(), fs.c_str());
    if (g_shader.id == 0) {
        TraceLog(LOG_WARNING, "SKY: shader failed to load");
        return;
    }
    g_locExposure = GetShaderLocation(g_shader, "exposure");

    Image img = LoadImage(hdriPath.c_str());
    if (img.data == nullptr) {
        TraceLog(LOG_WARNING, "SKY: failed to load HDRI %s (is SUPPORT_FILEFORMAT_HDR enabled?)",
                 hdriPath.c_str());
        UnloadShader(g_shader);
        g_shader = {};
        return;
    }

    TraceLog(LOG_INFO, "SKY: loaded %s (%dx%d fmt=%d)",
             hdriPath.c_str(), img.width, img.height, img.format);
    estimateHazeAndAmbient(img);

    g_hdri = LoadTextureFromImage(img);
    UnloadImage(img);
    if (g_hdri.id == 0) {
        TraceLog(LOG_WARNING, "SKY: GPU upload failed");
        UnloadShader(g_shader);
        g_shader = {};
        return;
    }

    GenTextureMipmaps(&g_hdri);
    // Trilinear if mipmaps exist; otherwise bilinear is fine for equirect HDRI.
    SetTextureFilter(g_hdri, g_hdri.mipmaps > 1
                                  ? TEXTURE_FILTER_TRILINEAR
                                  : TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(g_hdri, TEXTURE_WRAP_REPEAT);

    Mesh cube = GenMeshCube(2.0f, 2.0f, 2.0f);
    g_cube = LoadModelFromMesh(cube);
    g_modelOk = (g_cube.meshCount > 0 && g_cube.materialCount > 0);
    if (g_modelOk) {
        g_cube.materials[0].shader = g_shader;
        g_cube.materials[0].maps[MATERIAL_MAP_ALBEDO].texture = g_hdri;
        g_cube.materials[0].maps[MATERIAL_MAP_ALBEDO].color = WHITE;
    }

    g_ready = true;
    TraceLog(LOG_INFO, "SKY: HDRI + IBL ready (exposure=%.2f)", g_exposure);
}

void Draw(const Camera3D& cam) {
    if (!g_ready || !g_modelOk) return;

    if (g_locExposure >= 0) {
        SetShaderValue(g_shader, g_locExposure, &g_exposure, SHADER_UNIFORM_FLOAT);
    }

    rlDisableBackfaceCulling();
    rlDisableDepthMask();

    const float scale = 50.0f;
    DrawModelEx(g_cube, cam.position, Vector3{0.0f, 1.0f, 0.0f}, 0.0f,
                Vector3{scale, scale, scale}, WHITE);

    rlEnableDepthMask();
    rlEnableBackfaceCulling();

    // Clear leftover HDRI binds so terrain splat samplers cannot see it.
    for (int slot = 0; slot < 8; ++slot) {
        rlActiveTextureSlot(slot);
        rlDisableTexture();
    }
    rlActiveTextureSlot(0);
}

void Shutdown() {
    if (!g_ready && g_shader.id == 0 && g_hdri.id == 0) return;

    if (g_modelOk) {
        if (g_cube.materialCount > 0) {
            g_cube.materials[0].maps[MATERIAL_MAP_ALBEDO].texture = {};
            g_cube.materials[0].shader = {};
        }
        UnloadModel(g_cube);
        g_cube = {};
        g_modelOk = false;
    }
    if (g_hdri.id > 0) UnloadTexture(g_hdri);
    if (g_shader.id > 0) UnloadShader(g_shader);
    g_hdri = {};
    g_shader = {};
    g_locExposure = -1;
    g_ready = false;
}

float GetExposure() { return g_exposure; }

void SetExposure(float exposure) {
    g_exposure = std::clamp(exposure, 0.01f, 8.0f);
}

Color GetHazeColor() { return g_hazeColor; }

Vector3 GetHazeColorLinear() {
    return {
        g_hazeLinear.x * g_hazeTintStrength,
        g_hazeLinear.y * g_hazeTintStrength,
        g_hazeLinear.z * g_hazeTintStrength,
    };
}

void SetHazeTintStrength(float strength) {
    g_hazeTintStrength = std::clamp(strength, 0.0f, 2.0f);
}

float GetHazeTintStrength() { return g_hazeTintStrength; }

Texture GetEnvTexture() { return g_hdri; }

const Vector3* GetAmbientCube() { return g_ambientCube; }

Vector3 EvaluateIrradiance(Vector3 normal) {
    const float len = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (len > 1e-6f) {
        normal.x /= len;
        normal.y /= len;
        normal.z /= len;
    }
    const float x2 = normal.x * normal.x;
    const float y2 = normal.y * normal.y;
    const float z2 = normal.z * normal.z;
    const Vector3& px = g_ambientCube[0];
    const Vector3& nx = g_ambientCube[1];
    const Vector3& py = g_ambientCube[2];
    const Vector3& ny = g_ambientCube[3];
    const Vector3& pz = g_ambientCube[4];
    const Vector3& nz = g_ambientCube[5];
    Vector3 r = {
        x2 * (normal.x >= 0.0f ? px.x : nx.x) +
            y2 * (normal.y >= 0.0f ? py.x : ny.x) +
            z2 * (normal.z >= 0.0f ? pz.x : nz.x),
        x2 * (normal.x >= 0.0f ? px.y : nx.y) +
            y2 * (normal.y >= 0.0f ? py.y : ny.y) +
            z2 * (normal.z >= 0.0f ? pz.y : nz.y),
        x2 * (normal.x >= 0.0f ? px.z : nx.z) +
            y2 * (normal.y >= 0.0f ? py.z : ny.z) +
            z2 * (normal.z >= 0.0f ? pz.z : nz.z),
    };
    return r;
}

bool IsReady() { return g_ready; }

}  // namespace engine::render::sky
