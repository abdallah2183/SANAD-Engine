#version 450

// Editor ImGui overlay fragment shader: vertex color times font/atlas texel.
// The bound texture is either the uploaded font atlas or the viewport target
// (both RGBA8), selected per draw command through the descriptor set.

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_col;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D tex_input;

void main() {
    out_color = in_col * texture(tex_input, in_uv);
}
