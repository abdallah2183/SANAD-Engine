#version 450

// Depth prepass: depth-only vertex shader. Renders the same world transform
// as the gbuffer pass so the prepass fills the depth buffer with exactly the
// geometry the gbuffer will test against.

layout(location = 0) in vec3 in_position;

layout(push_constant) uniform PushConstants {
    mat4 view_proj;
    mat4 model;
} pc;

void main() {
    gl_Position = pc.view_proj * pc.model * vec4(in_position, 1.0);
}
