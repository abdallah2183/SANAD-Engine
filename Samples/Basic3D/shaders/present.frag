#version 450

// Present passthrough: copy a finished LDR target to the output, untouched.
//
// This is deliberately NOT tonemap.frag — that shader hardcodes
// `1 - exp(-hdr * exposure)` plus a gamma curve, which is correct for the
// renderer's linear HDR buffer and WRONG for an already-tonemapped LDR source
// (running it twice washes the image out). The editor presents its offscreen
// viewport through this path instead of re-rendering the whole scene into the
// swapchain just to fill the pixels behind the UI.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D src_input;

void main() {
    out_color = texture(src_input, in_uv);
}
