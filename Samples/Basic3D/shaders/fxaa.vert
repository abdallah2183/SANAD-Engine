#version 450

// Fullscreen triangle for the tonemap pass. Same rule as lighting.vert: no Y
// flip — the HDR target was written with Vulkan's y-down rasterization, so
// sampling with uv = ndc*0.5+0.5 reads the exact pixel that produced it.

vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);

layout(location = 0) out vec2 out_uv;

void main() {
    vec2 pos = positions[gl_VertexIndex];
    gl_Position = vec4(pos, 0.0, 1.0);
    out_uv = pos * 0.5 + 0.5;
}
