#version 450

// Pick fragment shader — writes the entity id as a colour, one byte per
// channel. The pass runs with depth testing and depth writes enabled, so the
// nearest surface at each pixel wins and the result is "what the user sees",
// not "what a ray happened to hit".
//
// Encoding: rgb holds the low 24 bits of (id + 1), and a is forced to 1.0 as an
// occupied marker. Reserving 0 for the clear value means a cleared pixel
// (a == 0) is unambiguously "no object here" rather than "entity 0", which is a
// valid entity. 24 bits caps the scene at 16.7M simultaneously pickable
// objects, far beyond anything this renderer will hold.
//
// The push-constant block must be declared identically in both stages; the
// vertex stage uses view_proj/model, this one uses pick_id.

layout(location = 0) out vec4 out_id;

layout(push_constant) uniform PushConstants {
    mat4 view_proj;
    mat4 model;
    vec4 pick_id;
} pc;

void main() {
    out_id = vec4(pc.pick_id.rgb, 1.0);
}
