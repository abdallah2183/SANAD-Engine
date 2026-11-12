#version 450

invariant gl_Position;

// Depth prepass: depth-only vertex shader. Renders the same world transform
// as the gbuffer pass so the prepass fills the depth buffer with exactly the
// geometry the gbuffer will test against.

layout(location = 0) in vec3 in_position;

layout(push_constant) uniform PushConstants {
    mat4 view_proj;
    mat4 model;
} pc;

void main() {
    // Match gbuffer.vert's operation order as well as its transform. Rounding
    // differences can otherwise make the depth test reject the same surface.
    vec4 world = pc.model * vec4(in_position, 1.0);
    gl_Position = pc.view_proj * world;
}
