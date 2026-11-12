#version 450

// The depth prepass uses this same invariant position calculation.
invariant gl_Position;

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
    // Normals transform with the INVERSE TRANSPOSE of the model matrix.
    // mat3(model) is only correct for rigid/uniform-scale transforms; a
    // non-uniform scale shears it and makes every tangent-space normal lean
    // toward the stretched axis. For rigid/uniform-scale the two agree up to
    // a positive scalar, which the fragment shader's normalize() removes, so
    // existing assets are bit-for-bit unchanged. A singular model (zero scale)
    // falls back to the raw matrix instead of producing NaN normals.
    mat3 model3 = mat3(pc.model);
    mat3 normal_matrix = model3;
    if (abs(determinant(model3)) > 1e-8) {
        normal_matrix = transpose(inverse(model3));
    }
    out_normal = normal_matrix * in_normal;
    out_uv = in_uv0;
    out_uv1 = in_uv1;
    out_world_pos = world.xyz;
}
