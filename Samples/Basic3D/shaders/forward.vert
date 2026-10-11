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
// Location 3: where this fragment lands on the TARGET, in the fullscreen
// passes' own uv space (ndc*0.5+0.5, v=0 at NDC -1). Needed because the SSAO
// texture is a screen-space buffer: the surface's own uv (location 1) says
// where this fragment is on the MESH, which is a different thing entirely and
// was being sampled as if it were the other — so a small pane in the middle of
// the screen read the occlusion of a completely different part of the frame.
layout(location = 3) out vec2 out_screen_uv;

void main() {
    vec4 world = pc.model * vec4(in_position, 1.0);
    vec4 clip = pc.view_proj * world;
    gl_Position = clip;
    // Transform normal by model (assume uniform scale, so use mat3(model))
    out_normal = mat3(pc.model) * in_normal;
    out_uv = in_uv0;
    out_world_pos = world.xyz;
    out_screen_uv = (clip.xy / clip.w) * 0.5 + 0.5;
}
