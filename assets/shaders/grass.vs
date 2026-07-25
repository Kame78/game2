#version 330

// Instanced grass clumps — mvp is view*projection (DrawMeshInstanced).
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
in mat4 instanceTransform;

uniform mat4 mvp;
uniform float uTime;
uniform vec3  viewPos;
uniform float fadeStart;
uniform float fadeEnd;
uniform float meshHeight;

out vec2  fragTexCoord;
out float fragFade;
out float fragTip;
out vec3  fragOrigin;
out vec3  fragNormal;

void main() {
    vec3 local = vertexPosition;
    float h = max(meshHeight, 0.05);
    float tip = clamp(local.y / h, 0.0, 1.0);

    vec3 origin = instanceTransform[3].xyz;
    float phase = origin.x * 0.37 + origin.z * 0.29;
    float wind =
        sin(uTime * 1.75 + phase) * 0.035 +
        sin(uTime * 2.35 + phase * 1.7) * 0.018;
    float bend = tip * tip;
    local.x += wind * bend;
    local.z += wind * 0.62 * bend;

    vec4 worldPos = instanceTransform * vec4(local, 1.0);
    float dist = length(worldPos.xz - viewPos.xz);
    fragFade = 1.0 - smoothstep(fadeStart, fadeEnd, dist);
    fragTexCoord = vertexTexCoord;
    fragTip = tip;
    fragOrigin = origin;
    // Approximate world normal (ignore non-uniform scale for wind-lit clumps).
    mat3 nMat = mat3(instanceTransform);
    fragNormal = normalize(nMat * vertexNormal);

    gl_Position = mvp * worldPos;
}
