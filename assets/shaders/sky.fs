#version 330

in vec3 fragDir;

uniform sampler2D texture0;
uniform float exposure;

out vec4 finalColor;

const float PI = 3.14159265359;

vec2 dirToEquirect(vec3 d) {
    d = normalize(d);
    float u = atan(d.z, d.x) * (0.5 / PI) + 0.5;
    float v = asin(clamp(d.y, -1.0, 1.0)) / PI + 0.5;
    return vec2(u, 1.0 - v);
}

void main() {
    vec3 hdr = texture(texture0, dirToEquirect(fragDir)).rgb;
    hdr *= max(exposure, 0.01);

    // Reinhard tonemap keeps bright sunset lobes from blowing out.
    vec3 mapped = hdr / (hdr + vec3(1.0));
    mapped = pow(clamp(mapped, 0.0, 1.0), vec3(1.0 / 2.2));

    finalColor = vec4(mapped, 1.0);
}
