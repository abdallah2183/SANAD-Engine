#version 450

// Editor ImGui overlay vertex shader. Matches ImDrawVert exactly:
// pos (vec2, offset 0), uv (vec2, offset 8), col (u8x4 normalized, offset 16).
// ImGui screen space is y-down with origin top-left; Vulkan NDC is y-down
// too, so scale/translate maps pixels straight across (same convention as
// the official ImGui Vulkan backend).

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_col;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_col;

layout(push_constant) uniform PushConstants {
    vec2 scale;
    vec2 translate;
} pc;

void main() {
    out_uv = in_uv;
    out_col = in_col;
    gl_Position = vec4(in_pos * pc.scale + pc.translate, 0.0, 1.0);
}
