#version 330

in vec3 fragPosition;
in vec3 fragNormal;
in vec4 fragColor;

uniform sampler2D texGrass;
uniform sampler2D texForest;
uniform sampler2D texMud;
uniform sampler2D texDirt;
uniform sampler2D texDry;
uniform sampler2D texGravel;
uniform sampler2D texRock;
uniform sampler2D texSnow;

uniform float uvScale;
uniform vec3 sunDir;
uniform float sunIntensity;
uniform vec3 hazeColor;
uniform vec3 ambientCube[6];
uniform float iblStrength;
uniform float hazeStart;
uniform float hazeEnd;
uniform float hazeStrength;
uniform int debugMode;

// Up to 12 lakes for shoreline wetness (xyz = xz + surfaceY, w = boundR)
uniform int waterCount;
uniform vec4 waterBodies[12];

out vec4 finalColor;

float saturate(float x) { return clamp(x, 0.0, 1.0); }

float shoreWetness(vec3 pos) {
    float wet = 0.0;
    int n = waterCount;
    if (n > 12) n = 12;
    for (int i = 0; i < 12; ++i) {
        if (i >= n) break;
        vec4 b = waterBodies[i];
        float r = max(b.w, 1.0);
        vec2 d = pos.xz - b.xy;
        float dist = length(d);
        // Wide soft beach ring around each lake
        float ring = 1.0 - smoothstep(r * 0.55, r * 1.45, dist);
        float elev = b.z; // surfaceY
        // Strong when near/under water table, soft fade above it
        float elevWet = 1.0 - smoothstep(elev - 2.5, elev + 6.0, pos.y);
        float mudShelf = saturate((elev + 1.2 - pos.y) * 0.28);
        wet = max(wet, ring * max(elevWet, mudShelf));
    }
    return saturate(wet);
}

void main() {
    vec2 uv = fragPosition.xz * uvScale;

    vec3 n = normalize(fragNormal);
    float up = saturate(n.y);
    float steep = 1.0 - up;
    float h = fragPosition.y;
    float moist = saturate(fragColor.r);

    // Wide smoothsteps = soft biome borders (avoids chunky facet edges on coarse LODs)
    float wRock = smoothstep(0.35, 0.78, steep);
    float snowLine = 240.0 - moist * 50.0;
    float wSnow = (1.0 - wRock) * smoothstep(snowLine - 20.0, snowLine + 90.0, h) * smoothstep(0.25, 0.60, up);
    float wGravel = (1.0 - wRock) * (1.0 - wSnow) * smoothstep(120.0, 220.0, h);
    float wHigh = (1.0 - wRock) * (1.0 - wSnow) * (1.0 - wGravel);

    float low = saturate(1.0 - smoothstep(20.0, 90.0, h));
    float mid = saturate(smoothstep(20.0, 90.0, h) * (1.0 - smoothstep(90.0, 180.0, h)));

    float wMud    = low * smoothstep(0.45, 0.85, moist) * (1.0 - smoothstep(0.0, 14.0, h));
    float wForest = low * smoothstep(0.45, 0.85, moist) * smoothstep(0.0, 14.0, h);
    float wDry    = low * (1.0 - smoothstep(0.20, 0.55, moist));
    float wDirt   = mid * 0.55 + low * (1.0 - smoothstep(0.20, 0.55, moist)) * steep * 0.45;
    float wGrass  = low * (1.0 - smoothstep(0.45, 0.85, moist)) * smoothstep(0.20, 0.55, moist);
    wGrass += mid * 0.45;

    float lowMask = wHigh;
    wMud *= lowMask; wForest *= lowMask; wDry *= lowMask; wDirt *= lowMask; wGrass *= lowMask;

    float wFoothillRock = wHigh * smoothstep(40.0, 160.0, h) * 0.30;

    float layers[8];
    layers[0] = wGrass;
    layers[1] = wForest;
    layers[2] = wMud;
    layers[3] = wDirt + wDry * 0.5;
    layers[4] = wDry * 0.5;
    layers[5] = wGravel;
    layers[6] = wRock + wFoothillRock;
    layers[7] = wSnow;

    float sum = 0.0;
    for (int i = 0; i < 8; ++i) sum += layers[i];
    if (sum < 1e-4) {
        layers[0] = 1.0;
        sum = 1.0;
    }
    for (int i = 0; i < 8; ++i) layers[i] /= sum;

    // Dual UV scales reduce obvious tiling / big blotches from altitude
    vec2 uvNear = uv;
    vec2 uvFar  = fragPosition.xz * (uvScale * 0.27);
    float farMix = saturate((length(fragPosition.xz) - 200.0) / 900.0);

    vec3 albedoNear =
        texture(texGrass,  uvNear).rgb * layers[0] +
        texture(texForest, uvNear).rgb * layers[1] +
        texture(texMud,    uvNear).rgb * layers[2] +
        texture(texDirt,   uvNear).rgb * layers[3] +
        texture(texDry,    uvNear).rgb * layers[4] +
        texture(texGravel, uvNear).rgb * layers[5] +
        texture(texRock,   uvNear).rgb * layers[6] +
        texture(texSnow,   uvNear).rgb * layers[7];

    vec3 albedoFar =
        texture(texGrass,  uvFar).rgb * layers[0] +
        texture(texForest, uvFar).rgb * layers[1] +
        texture(texMud,    uvFar).rgb * layers[2] +
        texture(texDirt,   uvFar).rgb * layers[3] +
        texture(texDry,    uvFar).rgb * layers[4] +
        texture(texGravel, uvFar).rgb * layers[5] +
        texture(texRock,   uvFar).rgb * layers[6] +
        texture(texSnow,   uvFar).rgb * layers[7];

    vec3 albedo = mix(albedoNear, albedoFar, farMix * 0.65);

    // Darker, muddier shore blend into surrounding biomes
    float wet = shoreWetness(fragPosition);
    albedo = mix(albedo, albedo * vec3(0.48, 0.52, 0.50), wet * 0.55);
    albedo = mix(albedo, vec3(0.22, 0.26, 0.24), wet * wet * 0.35);
    // Pull wet shores toward mud/dirt tones
    vec3 mudTone = texture(texMud, uvNear).rgb * 0.65 + texture(texDirt, uvNear).rgb * 0.35;
    albedo = mix(albedo, mudTone, wet * 0.40);

    vec3 light = normalize(sunDir);
    float ndotl = saturate(dot(n, light));
    // Directional sun + HDRI diffuse irradiance (ambient cube)
    float sunTerm = (ndotl * 0.70 + 0.08) * max(sunIntensity, 0.0);
    vec3 n2 = n * n;
    vec3 irr =
        n2.x * (n.x >= 0.0 ? ambientCube[0] : ambientCube[1]) +
        n2.y * (n.y >= 0.0 ? ambientCube[2] : ambientCube[3]) +
        n2.z * (n.z >= 0.0 ? ambientCube[4] : ambientCube[5]);
    vec3 H = normalize(light + normalize(-fragPosition));
    float wetSpec = pow(saturate(dot(n, H)), 28.0) * wet * 0.28;
    vec3 lit = albedo * (sunTerm + irr * iblStrength) + vec3(0.40, 0.50, 0.55) * wetSpec;

    // Soft atmospheric haze — full blend well before far clip (8000), never a hard cut.
    float dist = length(fragPosition);
    float hazeRange = max(hazeEnd - hazeStart, 1.0);
    float haze = saturate((dist - hazeStart) / hazeRange);
    vec3 sky = hazeColor;
    lit = mix(lit, sky, haze * hazeStrength);

    // Editor debug overlays (vertex color: R=moist, G=biome id 0..4/255, B=waterGate)
    if (debugMode == 1) {
        int biome = int(fragColor.g * 255.0 + 0.5);
        vec3 bc = vec3(0.45, 0.75, 0.35); // Plains
        if (biome == 1) bc = vec3(0.70, 0.55, 0.25); // Hills
        if (biome == 2) bc = vec3(0.55, 0.55, 0.60); // Mountains
        if (biome == 3) bc = vec3(0.25, 0.55, 0.35); // Wetlands
        if (biome == 4) bc = vec3(0.20, 0.45, 0.85); // Water
        lit = mix(bc, sky, haze * hazeStrength * 0.5);
    } else if (debugMode == 2) {
        float slope = 1.0 - saturate(n.y);
        lit = mix(vec3(0.15, 0.55, 0.20), vec3(0.95, 0.25, 0.10), saturate(slope * 1.6));
        lit = mix(lit, sky, haze * hazeStrength * 0.35);
    } else if (debugMode == 3) {
        float band = fract(h / 40.0);
        lit = mix(vec3(0.20, 0.35, 0.55), vec3(0.95, 0.90, 0.55), band);
        lit = mix(lit, sky, haze * hazeStrength * 0.35);
    } else if (debugMode == 4) {
        float wg = fragColor.b;
        lit = mix(vec3(0.35, 0.40, 0.30), vec3(0.10, 0.55, 0.95), wg);
        lit = mix(lit, sky, haze * hazeStrength * 0.35);
    }

    finalColor = vec4(lit, 1.0);
}
