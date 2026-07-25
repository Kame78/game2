#version 330

in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;

uniform mat4 mvp;
uniform mat4 matModel;
uniform float uTime;
uniform float lakeMode;
uniform vec2  lakeCenter;

out vec3 fragPosition;
out vec3 fragNormal;
out vec2 fragTexCoord;
out vec4 fragColor;

void main() {
    // Immediate-mode lake/river verts are already in world space; mvp is view-proj.
    vec3 pos = vertexPosition;

    if (lakeMode > 0.5) {
        vec2 p = pos.xz - lakeCenter;
        float w =
            sin(p.x * 0.071 + uTime * 1.15) * cos(p.y * 0.063 + uTime * 0.92) * 0.11 +
            sin(p.x * 0.145 - uTime * 1.55 + p.y * 0.09) * 0.055 +
            sin((p.x + p.y) * 0.038 + uTime * 0.55) * 0.04;
        w *= mix(0.25, 1.0, clamp(vertexColor.r * 1.4, 0.0, 1.0));
        pos.y += w;
    }

    vec4 worldPos = matModel * vec4(pos, 1.0);
    fragPosition = worldPos.xyz;
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;

    // Continuous procedural wave normal (XZ plane). Do not rely on texture2 —
    // raylib batch flushes clear extra sampler units mid-draw and caused a
    // hard shading seam across large lake meshes.
    vec3 n = mat3(matModel) * vertexNormal;
    if (lakeMode > 0.5) {
        vec2 p = pos.xz - lakeCenter;
        float a = p.x * 0.071 + uTime * 1.15;
        float b = p.y * 0.063 + uTime * 0.92;
        float c = p.x * 0.145 - uTime * 1.55 + p.y * 0.09;
        float dWx =
            cos(a) * 0.071 * cos(b) * 0.11 +
            cos(c) * 0.145 * 0.055 +
            cos((p.x + p.y) * 0.038 + uTime * 0.55) * 0.038 * 0.04;
        float dWz =
            sin(a) * (-sin(b) * 0.063) * 0.11 +
            cos(c) * 0.09 * 0.055 +
            cos((p.x + p.y) * 0.038 + uTime * 0.55) * 0.038 * 0.04;
        n = normalize(vec3(-dWx, 1.0, -dWz));
    }
    fragNormal = normalize(n);

    gl_Position = mvp * vec4(pos, 1.0);
}
