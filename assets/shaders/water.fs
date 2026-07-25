#version 330

in vec3 fragPosition;
in vec3 fragNormal;
in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0; // albedo (subtle tint / scroll detail — always on unit 0)
uniform sampler2D texture2; // normal (optional detail; may be unbound after batch flush)
uniform sampler2D envMap;   // equirectangular HDRI for reflections

uniform vec4  colDiffuse;
uniform vec3  camPos;
uniform vec3  sunDir;
uniform float uTime;
uniform float uvScale;
uniform float opacity;
uniform float brightness;
uniform float exposure;
uniform vec3  ambientCube[6];
uniform float iblStrength;

uniform float lakeMode;      // 0 = river, 1 = lake
uniform vec2  lakeCenter;
uniform vec2  lakeRadii;
uniform float lakeAngle;
uniform float lakeMaxDepth;
uniform float lakeWarpAmp;
uniform float lakeWarpFreq;
uniform float lakePhase;

out vec4 finalColor;

const float PI = 3.14159265359;

float saturate(float x) { return clamp(x, 0.0, 1.0); }

vec2 dirToEquirect(vec3 d) {
    d = normalize(d);
    float u = atan(d.z, d.x) * (0.5 / PI) + 0.5;
    float v = asin(clamp(d.y, -1.0, 1.0)) / PI + 0.5;
    return vec2(u, 1.0 - v);
}

vec3 sampleEnv(vec3 rd) {
    vec3 hdr = texture(envMap, dirToEquirect(rd)).rgb * max(exposure, 0.01);
    return hdr / (hdr + vec3(1.0));
}

vec3 sampleIrradiance(vec3 n) {
    vec3 n2 = n * n;
    return n2.x * (n.x >= 0.0 ? ambientCube[0] : ambientCube[1])
         + n2.y * (n.y >= 0.0 ? ambientCube[2] : ambientCube[3])
         + n2.z * (n.z >= 0.0 ? ambientCube[4] : ambientCube[5]);
}

// Procedural ripples in world XZ — continuous across the whole disc.
vec3 procRipple(vec2 p, float t) {
    float dx =
        cos(p.x * 0.19 + t * 1.05) * 0.14 +
        cos(p.x * 0.47 - t * 0.82 + p.y * 0.21) * 0.07 +
        cos((p.x + p.y) * 0.09 + t * 0.4) * 0.04;
    float dz =
        cos(p.y * 0.17 + t * 0.95) * 0.14 +
        cos(p.y * 0.43 + t * 0.70 + p.x * 0.18) * 0.07 +
        cos((p.x - p.y) * 0.11 + t * 0.35) * 0.04;
    return normalize(vec3(-dx, 1.0, -dz));
}

vec3 texRipple(vec2 uv) {
    vec3 t = texture(texture2, uv).xyz * 2.0 - 1.0;
    float mag = length(t.xy);
    if (mag < 0.04) return vec3(0.0, 1.0, 0.0);
    return normalize(vec3(t.x * 0.55, 1.0, t.y * 0.55));
}

float lakeCoverage(vec2 worldXZ) {
    vec2 d = worldXZ - lakeCenter;
    float c = cos(lakeAngle);
    float s = sin(lakeAngle);
    vec2 local = vec2(d.x * c + d.y * s, -d.x * s + d.y * c);
    vec2 n = local / max(lakeRadii, vec2(1.0));
    float r = length(n);
    if (r < 1.0e-5) return 0.0;

    float ang = atan(n.y, n.x);
    float warp = 1.0
        + lakeWarpAmp * sin(ang * lakeWarpFreq + lakePhase)
        + lakeWarpAmp * 0.55 * sin(ang * (lakeWarpFreq * 1.7) + lakePhase * 1.3)
        + lakeWarpAmp * 0.25 * sin(local.x * 0.031 + lakePhase) * cos(local.y * 0.027);
    warp = clamp(warp, 0.55, 1.55);
    return r / warp;
}

void main() {
    float cov = 0.0;
    float shoreBand = 0.0;
    float rimFade = 1.0;

    if (fragColor.a < 0.008) discard;

    float depth01 = saturate(fragColor.r);
    if (lakeMode > 0.5) {
        depth01 = saturate(depth01 * mix(0.90, 1.12, saturate(lakeMaxDepth / 16.0)));
        cov = lakeCoverage(fragPosition.xz);
        float disc = length(fragPosition.xz - lakeCenter) / 300.0;
        if (disc > 1.06) discard;
        float shallow = 1.0 - smoothstep(0.05, 0.42, depth01);
        shoreBand = max(
            smoothstep(0.55, 0.88, cov) * (1.0 - smoothstep(0.95, 1.25, cov)),
            shallow * 0.75);
        rimFade = 1.0 - smoothstep(0.88, 1.04, disc);
    }

    vec2 uv1;
    vec2 uv2;
    if (lakeMode > 0.5) {
        uv1 = fragPosition.xz * uvScale + vec2(uTime * 0.10, uTime * 0.06);
        uv2 = fragPosition.xz * uvScale * 1.55 + vec2(uTime * -0.07, uTime * 0.11);
    } else {
        float flow = uTime * 0.55;
        uv1 = vec2(fragTexCoord.x * 2.2, fragTexCoord.y * 3.5 + flow);
        uv2 = vec2(fragTexCoord.x * 3.1 + 0.4, fragTexCoord.y * 2.2 - flow * 0.65);
    }

    vec3 Ngeo = normalize(fragNormal);
    vec3 Nproc = procRipple(fragPosition.xz * 0.65, uTime);
    vec3 Ntex = normalize(texRipple(uv1) + texRipple(uv2));
    float texW = (lakeMode > 0.5) ? 0.28 : 0.40;
    vec3 N = normalize(mix(mix(Ngeo, Nproc, 0.70), Ntex, texW));

    vec3 V = normalize(camPos - fragPosition);
    vec3 L = normalize(sunDir);
    vec3 H = normalize(L + V);
    vec3 R = reflect(-V, N);

    float ndotl = saturate(dot(N, L));
    float sunTerm = ndotl * 0.22 + 0.35;
    float spec  = pow(saturate(dot(N, H)), 64.0);
    float fres  = pow(1.0 - saturate(dot(N, V)), 2.4);

    vec3 alb1 = texture(texture0, uv1).rgb;
    vec3 alb2 = texture(texture0, uv2).rgb;
    float ripple = (alb1.r + alb2.g + alb1.b) * 0.33;
    float flowAmt = 0.86 + ripple * 0.22;

    vec3 shallow = vec3(0.22, 0.55, 0.58);
    vec3 mid     = vec3(0.07, 0.28, 0.42);
    vec3 deep    = vec3(0.015, 0.08, 0.20);
    vec3 body = mix(shallow, mid, saturate(depth01 * 1.35));
    body = mix(body, deep, saturate((depth01 - 0.32) * 1.7));
    body = mix(body, body * mix(vec3(0.85), alb1, 0.55), 0.18);

    vec3 irr = sampleIrradiance(N);
    body *= flowAmt * mix(1.06, 0.84, depth01) * (sunTerm + irr * iblStrength * 0.55);

    // HDRI environment reflection (the real specular IBL win on water)
    vec3 env = sampleEnv(R);
    // Slightly blur rough water by mixing reflection with irradiance
    env = mix(env, irr, 0.18);
    body = mix(body, env, fres * mix(0.28, 0.62, saturate(depth01 + 0.15)));
    body += vec3(0.75, 0.85, 0.95) * spec * mix(0.40, 0.12, depth01);
    body += env * fres * 0.10;

    if (lakeMode > 0.5) {
        vec3 foam = vec3(0.70, 0.82, 0.86);
        body = mix(body, mix(body * vec3(1.10, 1.15, 1.12), foam, 0.45), shoreBand * 0.45);
    } else {
        float edge = 1.0 - saturate(fragColor.a);
        body = mix(body, vec3(0.55, 0.70, 0.76), edge * 0.35);
    }

    vec3 color = body * brightness * colDiffuse.rgb;

    float alpha = mix(opacity * 0.42, min(opacity + 0.10, 0.88), depth01);
    alpha = mix(alpha, min(alpha + 0.12, 0.90), fres);
    alpha *= colDiffuse.a * mix(0.45, 1.0, saturate(fragColor.a * 1.15));
    alpha = max(alpha, mix(0.0, opacity * 0.55, depth01 * depth01));

    if (lakeMode > 0.5) {
        alpha = mix(alpha, min(alpha + 0.05, 0.86), shoreBand * 0.25);
        alpha *= mix(0.72, 1.0, saturate(depth01 * 1.15 + 0.22));
        alpha *= rimFade;
        if (alpha < 0.015) discard;
    }

    finalColor = vec4(color, clamp(alpha, 0.0, 0.92));
}
