#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv0;

layout(push_constant) uniform PushConstants {
    mat4 view_proj;
    mat4 model;
} pc;

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec2 out_uv;

void main() {
    gl_Position = pc.view_proj * pc.model * vec4(in_position, 1.0);
    // Transform normal by model (assume uniform scale, so use mat3(model))
    out_normal = mat3(pc.model) * in_normal;
    out_uv = in_uv0;
}
