#version 450

// CliffStory — 2D sprite fragment shader.
//
// Straight-alpha modulate, matching the blend state declared on the render
// pass (src = SrcAlpha, dst = OneMinusSrcAlpha, op = Add).
//
// The pass carries no depth attachment on purpose. scene2d::SpriteBatcher sorts
// by (page, depth) and the sample pushes sprites strictly back-to-front, so the
// painter's algorithm *is* the depth test. That buys correct alpha ordering for
// free — the ivy leaves overlapping the rock, the lantern glow over the cliff,
// the mist bands between the parallax layers — without a depth buffer, a
// depth-prepass, or a sorted second pass.

// The one descriptor every page shares: each page gets its own
// MaterialInstance (its own descriptor set) but all of them reference this same
// pipeline, which is the whole point of the Material/instance split in
// NF/Rendering/Material.hpp.
layout(set = 0, binding = 0) uniform sampler2D albedo_texture;

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in vec4 frag_color;
layout(location = 0) out vec4 out_color;

void main() {
    vec4 texel = texture(albedo_texture, frag_uv);
    // Multiplicative on both colour and alpha. Tinting therefore also fades a
    // sprite: the mist bands and the lantern glow reuse this exact path instead
    // of needing a second blended pipeline.
    out_color = vec4(texel.rgb * frag_color.rgb, texel.a * frag_color.a);
}