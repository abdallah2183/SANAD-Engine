#version 450

// Push-constant test fragment shader — solid color driven entirely by push constants.
// This is the minimal shader that proves the push-constant path works end to end.

layout(push_constant) uniform PushConstants {
    vec4 color;
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = pc.color;
}
