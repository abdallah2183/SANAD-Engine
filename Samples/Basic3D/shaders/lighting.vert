#version 450

// Fullscreen triangle for the deferred lighting pass.
//
// No Y flip here: Vulkan's NDC y already points down, so NDC (nx, ny) lands
// on the framebuffer pixel the gbuffer pass wrote — sampling with
// uv = ndc*0.5+0.5 reads exactly that pixel. Flipping (as an earlier version
// did) reads the image mirrored vertically, which symmetric scenes hide.

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
