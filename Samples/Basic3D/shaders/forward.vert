#version 450

// Forward (transparency) vertex shader.
//
// The deferred path cannot draw a transparent surface: the gbuffer holds one
// surface per pixel, and blending into it would blend the raw material
// parameters of two different surfaces as if they were one. So a transparent
// object is drawn in a separate pass AFTER the deferred lighting pass, over the
// lit HDR image.
//
// The transform math is identical to gbuffer.vert (same push constants, same
// vertex layout) — deliberately, so a surface lands on the same pixel in both
// paths. The only addition is the world position, which the deferred pass
// reconstructs from depth but a forward pass has to interpolate. Interpolating
// a position is exact (it is affine in the vertex attributes), so the shadow
// projectors see the same point either way.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv0;

layout(push_constant) uniform PushConstants {
    mat4 view_proj;
    mat4 model;
} pc;

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec2 out_uv;
layout(location = 2) out vec3 out_world_pos;

void main() {
    vec4 world = pc.model * vec4(in_position, 1.0);
    gl_Position = pc.view_proj * world;
    // Transform normal by model (assume uniform scale, so use mat3(model))
    out_normal = mat3(pc.model) * in_normal;
    out_uv = in_uv0;
    out_world_pos = world.xyz;
}
