#version 330

// Instanced foliage (trees / undergrowth) — mvp is view*projection (DrawMeshInstanced).
// Soft fade out near draw distance. No wind (would bend trunks).
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
in mat4 instanceTransform;

uniform mat4 mvp;
uniform vec3  viewPos;
uniform float fadeInStart;
uniform float fadeInEnd;
uniform float fadeOutStart;
uniform float fadeOutEnd;

out vec2  fragTexCoord;
out float fragFade;
out vec3  fragOrigin;
out vec3  fragNormal;
out vec4  fragColor;

void main() {
    vec4 worldPos = instanceTransform * vec4(vertexPosition, 1.0);
    float dist = length(worldPos.xz - viewPos.xz);

    float fadeIn = 1.0;
    if (fadeInEnd > fadeInStart + 1e-4) {
        fadeIn = smoothstep(fadeInStart, fadeInEnd, dist);
    }
    float fadeOut = 1.0 - smoothstep(fadeOutStart, fadeOutEnd, dist);
    fragFade = fadeIn * fadeOut;

    fragTexCoord = vertexTexCoord;
    fragOrigin = instanceTransform[3].xyz;
    fragColor = vertexColor;
    mat3 nMat = mat3(instanceTransform);
    fragNormal = normalize(nMat * vertexNormal);

    gl_Position = mvp * worldPos;
}
