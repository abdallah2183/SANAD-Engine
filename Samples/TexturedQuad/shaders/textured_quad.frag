#version 450

// NOVAForge Engine — Textured quad sample fragment shader
//
// Samples the bound texture at the interpolated UV. This is the first shader
// that reads a resource through a descriptor set, so it is the real test of
// the RHI's binding abstraction.

layout(location = 0) in vec2 frag_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D albedo_texture;

void main() {
    out_color = texture(albedo_texture, frag_uv);
}
