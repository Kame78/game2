#version 330

in vec3 vertexPosition;

uniform mat4 mvp;

// Model-space position on the unit cube ≈ view direction once the cube is
// centered on the camera (no rotation applied to the sky model).
out vec3 fragDir;

void main() {
    fragDir = vertexPosition;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
