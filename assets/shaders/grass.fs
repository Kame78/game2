#version 330

in vec2  fragTexCoord;
in float fragFade;
in float fragTip;
in vec3  fragOrigin;
in vec3  fragNormal;

uniform vec3 sunDir;
uniform float sunIntensity;
uniform vec4 colDiffuse;
uniform sampler2D texture0;

out vec4 finalColor;

void main() {
    if (fragFade < 0.02) discard;

    vec4 tex = texture(texture0, fragTexCoord);
    float alpha = tex.a;
    if (alpha < 0.35) discard;

    float h = fract(sin(dot(fragOrigin.xz, vec2(12.9898, 78.233))) * 43758.5453);
    vec3 albedo = tex.rgb;
    // Slight per-clump tint variation so carpets don't look stamped.
    albedo *= mix(vec3(0.92, 0.95, 0.88), vec3(1.05, 1.02, 0.95), h);
    albedo = mix(albedo * 0.78, albedo, clamp(fragTip * 1.15, 0.0, 1.0));

    vec3 N = normalize(fragNormal);
    // Two-sided lighting for thin cards.
    vec3 L = normalize(sunDir);
    float ndl = abs(dot(N, L));
    float lit = 0.38 + 0.62 * ndl * max(sunIntensity, 0.0);
    albedo *= lit;

    finalColor = vec4(albedo * colDiffuse.rgb, alpha * fragFade * colDiffuse.a);
}
