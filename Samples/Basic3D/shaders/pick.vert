#version 450

// Pick vertex shader — the transform path is deliberately identical to
// gbuffer.vert so the id image lines up pixel-for-pixel with the rendered
// scene. If the two disagreed by even a pixel, a click would select whatever
// happened to be under the cursor in the id image rather than in the image the
// user is looking at.
//
// Only gl_Position matters here; the id itself is produced by the fragment
// stage, which reads the same push-constant block.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv0;

layout(push_constant) uniform PushConstants {
    mat4 view_proj;
    mat4 model;
    vec4 pick_id; // packed entity id; consumed by the fragment stage
} pc;

void main() {
    gl_Position = pc.view_proj * pc.model * vec4(in_position, 1.0);
}
