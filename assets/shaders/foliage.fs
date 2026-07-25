#version 330

in vec2  fragTexCoord;
in float fragFade;
in vec3  fragOrigin;
in vec3  fragNormal;
in vec4  fragColor;

uniform vec3 sunDir;
uniform float sunIntensity;
uniform vec4 colDiffuse;
uniform sampler2D texture0;

out vec4 finalColor;

void main() {
    if (fragFade < 0.02) discard;

    vec4 tex = texture(texture0, fragTexCoord);
    float alpha = tex.a * fragColor.a;
    // Alpha cutout for leaves / ferns; bark stays opaque.
    if (alpha < 0.35) discard;

    float h = fract(sin(dot(fragOrigin.xz, vec2(12.9898, 78.233))) * 43758.5453);
    vec3 albedo = tex.rgb * fragColor.rgb;
    albedo *= mix(vec3(0.94, 0.96, 0.90), vec3(1.04, 1.02, 0.97), h);

    vec3 N = normalize(fragNormal);
    vec3 L = normalize(sunDir);
    float ndl = abs(dot(N, L));
    float lit = 0.42 + 0.58 * ndl * max(sunIntensity, 0.0);
    albedo *= lit;

    finalColor = vec4(albedo * colDiffuse.rgb, alpha * fragFade * colDiffuse.a);
}
