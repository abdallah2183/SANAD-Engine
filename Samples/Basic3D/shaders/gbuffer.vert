#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv0;
// The terrain layer splat: (slot, blend). Optional — a mesh that never
// classifies its surface leaves this at (0, 0).
layout(location = 3) in vec2 in_uv1;

layout(push_constant) uniform PushConstants {
    mat4 view_proj;
    mat4 model;
} pc;

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec2 out_uv;
layout(location = 2) out vec2 out_uv1;
// World position for derivative-based tangent frames (normal mapping without
// vertex tangents — see perturb_mapped_normal in brdf.glsl). Interpolating a
// position is exact (affine in the vertex attributes).
layout(location = 3) out vec3 out_world_pos;

void main() {
    vec4 world = pc.model * vec4(in_position, 1.0);
    gl_Position = pc.view_proj * world;
    // Transform normal by model (assume uniform scale, so use mat3(model))
    out_normal = mat3(pc.model) * in_normal;
    out_uv = in_uv0;
    out_uv1 = in_uv1;
    out_world_pos = world.xyz;
}
