#version 450

// NOTE: byte-twin of ssao.vert (the blur pass shares the fullscreen
// triangle). nf_compile_shaders compiles <base>.vert + <base>.frag pairs,
// so the shared vertex stage needs a name per target. Keep the two in sync.


// Fullscreen triangle for the SSAO passes (raw + blur share it).
//
// Same orientation contract as lighting.vert: no Y flip, uv = ndc*0.5+0.5
// reads the texel this pixel covers.

vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);

layout(location = 0) out vec2 out_uv;

void main() {
    vec2 pos = positions[gl_VertexIndex];
    gl_Position = vec4(pos, 0.0, 1.0);
    out_uv = pos * 0.5 + 0.5;
}
