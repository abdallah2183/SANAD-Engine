#version 450

// NOVAForge Engine — Textured quad sample vertex shader
//
// Real vertex buffer input (position + UV), unlike the triangle sample which
// generated vertices from gl_VertexIndex. This is the path every mesh will
// take from here on.

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec2 frag_uv;

void main() {
    gl_Position = vec4(in_position, 0.0, 1.0);
    frag_uv = in_uv;
}
