#version 450

layout(location = 0) in vec3 inPosition;

layout(location = 0) out vec3 fragColor;

void main() {
    gl_Position = vec4(inPosition, 1.0);
    fragColor = vec3(0.5 * sin(gl_VertexIndex*1234.1254) + 0.5, 0.5, 0.5 * sin(gl_VertexIndex*943.0143 + 2.242) + 0.5);
}
