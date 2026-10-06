#version 450

// CliffStory — 2D sprite vertex shader.
//
// The input arrives already in normalised device coordinates.
// scene2d::SpriteBatcher bakes world -> screen *pixels* (its convention is
// origin top-left, y growing down, per Math2D.hpp), and the sample performs the
// one remaining step — the pixel -> NDC flip — on the CPU while it copies the
// baked batch into the upload buffer.
//
// Doing that conversion there rather than here is a deliberate choice. The
// alternative is a viewport-size uniform, which would mean either a push
// constant or a descriptor on a pipeline whose entire job is to multiply by
// constants. One multiply-add per vertex on the CPU is free at these sprite
// counts, and it keeps this shader identical for every caller: pixel-art
// sprites, the stone-cliff quads and the glyph atlas all go through it.

layout(location = 0) in vec2 in_position;   // NDC
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;     // straight (un-premultiplied) tint

layout(location = 0) out vec2 frag_uv;
layout(location = 1) out vec4 frag_color;

void main() {
    gl_Position = vec4(in_position, 0.0, 1.0);
    frag_uv = in_uv;
    frag_color = in_color;
}