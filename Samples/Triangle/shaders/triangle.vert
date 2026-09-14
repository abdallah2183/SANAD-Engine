#version 450

// NOVAForge Engine — Triangle sample vertex shader
// Hardcoded triangle vertices; no vertex buffer needed for this first milestone.

vec2 positions[3] = vec2[](
    vec2( 0.0, -0.5),
    vec2( 0.5,  0.5),
    vec2(-0.5,  0.5)
);

vec3 colors[3] = vec3[](
    vec3(1.0, 0.0, 0.0),   // Red — top
    vec3(0.0, 1.0, 0.0),   // Green — bottom right
    vec3(0.0, 0.0, 1.0)    // Blue — bottom left
);

layout(location = 0) out vec3 frag_color;

void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    frag_color = colors[gl_VertexIndex];
}
