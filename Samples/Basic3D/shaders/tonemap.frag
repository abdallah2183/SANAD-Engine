#version 450

// tonemap.frag — the final pass, and the whole §206 post stack after the bloom
// chain's own passes.
//
// Stage order, which is what rendering::apply_post_chain mirrors:
//
//   sharpen (unsharp mask on linear HDR)
//     -> bloom add (sum of the levels, scaled by intensity)
//     -> exposure
//     -> colour grade (contrast / temperature / tint / gamma, in HDR)
//     -> tonemap operator
//     -> gamma
//     -> saturation, vignette
//
// Every stage is guarded by its own enable value, and every neutral value is
// EXACTLY identity: with the whole stack off this shader computes
// `texture(hdr_input, in_uv).rgb * pc.exposure` and nothing else, which is the
// expression it computed before the stack existed. The golden pixels depend on
// that, so do not "tidy" a guard away — a stage that always runs costs an image
// that differs for every scene that never asked for it.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D hdr_input;
// Bloom levels 0..3, largest (half resolution) first. When bloom is off the
// renderer binds the HDR texture here instead and sets bloom_intensity to 0 —
// the guard below means these are then never read, so an "unused" binding can
// never leak a stale image into the frame.
layout(set = 0, binding = 1) uniform sampler2D bloom0;
layout(set = 0, binding = 2) uniform sampler2D bloom1;
layout(set = 0, binding = 3) uniform sampler2D bloom2;
layout(set = 0, binding = 4) uniform sampler2D bloom3;

// Depth of field needs two things this pass did not have: the depth target, and
// the inverse view-projection that turns a depth value into a view distance.
// Both are the SAME data the lighting pass already reads (bindings 3 and 4
// there) — this pass simply had no way to know how far away a pixel is.
//
// Only the first two members of the frame block are declared, which is legal
// because a std140 block may describe a PREFIX of the buffer. Declaring all
// forty floats again would be a second copy of FrameUniforms to keep in sync
// for two members this shader actually reads.
layout(set = 0, binding = 5) uniform FrameBlock {
    mat4 invViewProj;
    vec4 camPos_ambient;
    // The previous frame's view-projection, for the reprojection motion blur.
    // Third in the block because that is where it sits in FrameUniforms — a
    // std140 block may describe a PREFIX of the buffer, but not a scattered
    // subset of it.
    mat4 prevViewProj;
} frame;
layout(set = 0, binding = 6) uniform sampler2D gbuffer_depth;

// The colour-grading LUT, as a 2D STRIP: kLutSize slices side by side, each
// kLutSize texels square, so the texture is (kLutSize*kLutSize) x kLutSize.
// A strip rather than a 3D texture because the RHI's sampled views are 2D; the
// slice blend below costs one extra fetch and keeps the stage inside the
// texture format the rest of the engine already uploads.
//
// When no LUT is bound the renderer puts its 1x1 white texture here, which is
// harmless precisely because `lut_strength` at 0 means this is never sampled.
layout(set = 0, binding = 7) uniform sampler2D color_lut;

// The mode is carried as a float rather than an int because the renderer
// uploads the block as one float array. Values are compared by range, not
// equality, so a numeric fuzz from the float path cannot land between two
// modes.
//
//   0 Exponential — 1 - exp(-x). The shipped default and the *only* mode the
//     golden pixels were authored against; keep it at 0 so an unset block
//     reproduces the old frame bit for bit.
//   1 ACES       — Narkowicz's fitted filmic curve.
//   2 Reinhard   — x / (1 + x), the classic photographic operator.
//   3 Linear     — exposure only, for diagnostics and HDR capture.
//
// Must match Renderer3D's tonemap push block (16 floats, 64 bytes).
layout(push_constant) uniform PushConstants {
    float exposure;
    float vignette;         // 0 = off
    float saturation;       // 1 = neutral
    float tonemap_mode;
    float bloom_intensity;  // 0 = the bloom levels are not read at all
    float sharpen_amount;   // 0 = the unsharp mask is not evaluated
    float grade_enabled;    // > 0.5 = grade
    float grade_contrast;
    float grade_pivot;
    float grade_temperature;
    float grade_tint;
    float grade_gamma;
    float texel_x;          // 1 / output width, for the sharpen taps
    float texel_y;          // 1 / output height
    float sharpen_radius;   // tap-spacing multiplier
    // Lens effects (§206). Both are UV-space warps applied BEFORE the HDR
    // fetch, so both neutral values are exactly identity: at 0 the fetch is
    // `texture(hdr_input, in_uv)` and nothing about the frame changes.
    float lens_distortion;  // barrel (+) / pincushion (-) coefficient on r^2
    float lens_chroma;      // red/blue radial split, as a fraction of r
    // Depth of field. `dof_max_radius` at 0 is exactly "off" — the stage is
    // disabled by folding `enabled` into it at push time, the same way
    // `bloom_on` folds the bloom intensity.
    float dof_focus_distance; // world units from the camera that stay sharp
    float dof_focus_range;    // half-width of the sharp band, world units
    float dof_max_radius;     // blur radius in output texels at full confusion
    // Motion blur. `motion_intensity` at 0 is exactly "off" — the stage is
    // disabled by folding `enabled` into it at push time, like the others.
    float motion_intensity;   // multiplier on the reprojected velocity
    float motion_max_length;  // cap on the smear, in uv units
    // Colour-grading LUT strength, 0..1. 0 means the LUT is NOT READ, which is
    // what makes a scene with no LUT exactly the frame it was before.
    float lut_strength;
    float pad0;
} pc;

// The coordinate the HDR is fetched from for `channel` (0 = red, 1 = green,
// 2 = blue). Matches rendering::lens_sample_uv exactly.
//
// Green is never displaced, so a neutral grey pixel stays neutral and only a
// real colour edge fringes. Distortion is applied first and the chromatic
// split is then measured on the DISTORTED coordinate, so the two compose in
// the order a real lens imposes them.
vec2 lens_sample_uv(vec2 uv, int channel) {
    if (pc.lens_distortion != 0.0) {
        vec2 c = uv - 0.5;
        uv = 0.5 + c * (1.0 + pc.lens_distortion * dot(c, c));
    }
    if (pc.lens_chroma != 0.0 && channel != 1) {
        vec2 c = uv - 0.5;
        // Red outward, blue inward: the sign is what makes the fringe read as
        // a lens rather than as a colour-shift artefact.
        const float scale = (channel == 0) ? (1.0 + pc.lens_chroma) : (1.0 - pc.lens_chroma);
        uv = 0.5 + c * scale;
    }
    return uv;
}

vec3 lens_fetch() {
    if (pc.lens_distortion == 0.0 && pc.lens_chroma == 0.0) {
        return texture(hdr_input, in_uv).rgb;
    }
    if (pc.lens_chroma == 0.0) {
        // Distortion only: one fetch serves all three channels, because with no
        // chromatic split every channel reads the same coordinate.
        return texture(hdr_input, lens_sample_uv(in_uv, 1)).rgb;
    }
    return vec3(texture(hdr_input, lens_sample_uv(in_uv, 0)).r,
                texture(hdr_input, lens_sample_uv(in_uv, 1)).g,
                texture(hdr_input, lens_sample_uv(in_uv, 2)).b);
}

// --- Depth of field ---------------------------------------------------------

// Must match rendering::kDofTaps.
const int kDofTaps = 12;

// Circle of confusion for a surface `view_depth` away, in output texels: zero
// AT the focus distance, rising linearly to `dof_max_radius` at
// `focus_distance +/- dof_focus_range`, and clamped beyond that.
//
// A linear ramp rather than a thin-lens model, because the two things an author
// actually wants to set are "where is it sharp" and "how far does the blur
// take to reach full width" — a physical model would replace those with an
// aperture and a focal length that mean the same thing to nobody who is not a
// photographer.
//
// `dof_max_radius <= 0` is the OFF state: every CoC is 0, the gather returns
// its own centre, and the stage is exactly identity. That is what lets the
// renderer disable it by folding the flag into this one value.
float dof_coc(float view_depth) {
    if (pc.dof_max_radius <= 0.0 || pc.dof_focus_range <= 0.0) {
        return 0.0;
    }
    return clamp(abs(view_depth - pc.dof_focus_distance) / pc.dof_focus_range, 0.0, 1.0) *
           pc.dof_max_radius;
}

// Distance from the camera to the surface under `uv`, in world units. The sky
// (depth at the far plane) is reported as very far rather than exactly at the
// far plane, so a background that is not geometry still defocuses.
float dof_view_depth(vec2 uv) {
    const float d = texture(gbuffer_depth, uv).r;
    if (d >= 0.999999) {
        return 1.0e30;
    }
    const vec4 world = frame.invViewProj * vec4(uv * 2.0 - 1.0, d, 1.0);
    return distance(world.xyz / world.w, frame.camPos_ambient.xyz);
}

// A golden-angle disc of kDofTaps samples. The radius follows the CENTRE's
// confusion, and each tap is weighted by its OWN confusion clamped to the
// centre's: a tap that is in focus must not be dragged into the blur, which is
// what stops a sharp foreground from smearing onto a blurred background at
// every silhouette edge. A centre-only weighting produces exactly that.
//
// The gather reads the RAW HDR target — the image this pass just fetched — so
// what is blurred is what was read. The bloom chain's prefilter also reads the
// raw target, so the glow is generated from the un-defocused image; routing it
// through a defocused copy would need a second HDR target for a difference that
// is invisible once the glow is already a wide blur.
vec3 dof_blur(vec3 centre, vec2 uv, float centre_coc) {
    if (centre_coc <= 0.0) {
        return centre;
    }
    const vec2 texel = vec2(pc.texel_x, pc.texel_y);
    const float golden = 2.39996323; // the golden angle, in radians
    vec3 sum = vec3(0.0);
    float weight_sum = 0.0;
    for (int i = 0; i < kDofTaps; ++i) {
        const float fi = float(i) + 0.5;
        // sqrt() gives an even AREA density; without it the taps bunch at the
        // centre and the blur reads as a soft dot rather than a disc.
        const float radius = sqrt(fi / float(kDofTaps)) * centre_coc;
        const float angle = fi * golden;
        const vec2 tap_uv = uv + vec2(cos(angle), sin(angle)) * radius * texel;
        const float w = min(dof_coc(dof_view_depth(tap_uv)), centre_coc);
        sum += texture(hdr_input, tap_uv).rgb * w;
        weight_sum += w;
    }
    if (weight_sum <= 0.0) {
        return centre;
    }
    // Blend by how confused the CENTRE is: a sharp pixel keeps itself entirely
    // and a fully confused one takes the gather, so the transition across the
    // focus band is a ramp rather than a step.
    const float t = clamp(centre_coc / pc.dof_max_radius, 0.0, 1.0);
    return mix(centre, sum / weight_sum, t);
}

// --- Motion blur ------------------------------------------------------------

// Must match rendering::kMotionTaps.
const int kMotionTaps = 8;

// Where the surface under `uv` was on the PREVIOUS frame's screen, in uv.
//
// Reconstructed by taking the world position (from depth) and projecting it
// with the previous view-projection. That is the classic reprojection, and it
// is why this stage needs no velocity buffer and therefore no extra gbuffer
// attachment — the depth target the depth-of-field stage already reads carries
// everything required.
//
// It captures CAMERA motion only: a surface's own movement is not in the depth
// buffer, so a fast object under a still camera does not smear. Per-object
// motion needs a velocity attachment the deferred path does not write, which is
// a renderer change rather than a post stage.
vec2 motion_prev_uv(vec2 uv) {
    const float d = texture(gbuffer_depth, uv).r;
    if (d >= 0.999999) {
        return uv; // the sky is at infinity: no surface to reproject
    }
    const vec4 world = frame.invViewProj * vec4(uv * 2.0 - 1.0, d, 1.0);
    const vec4 prev = frame.prevViewProj * vec4(world.xyz / world.w, 1.0);
    if (abs(prev.w) < 1.0e-9) {
        return uv;
    }
    return (prev.xy / prev.w) * 0.5 + 0.5;
}

// A box average along the reprojected velocity. The tap count is fixed and the
// SPAN is capped, so a camera that teleports smears by at most
// `motion_max_length` instead of stretching one frame across the screen.
vec3 motion_blur(vec3 centre, vec2 uv) {
    if (pc.motion_intensity <= 0.0) {
        return centre;
    }
    const vec2 velocity = (uv - motion_prev_uv(uv)) * pc.motion_intensity;
    const float len = length(velocity);
    if (len < 1.0e-6) {
        return centre;
    }
    const float span = min(len, max(pc.motion_max_length, 0.0));
    const vec2 smear = velocity * (span / len);
    vec3 sum = vec3(0.0);
    for (int i = 0; i < kMotionTaps; ++i) {
        // Centred on the pixel: the shutter is open either side of it, so the
        // taps straddle `uv` rather than trailing behind it.
        const float t = (float(i) + 0.5) / float(kMotionTaps) - 0.5;
        sum += texture(hdr_input, uv + smear * t).rgb;
    }
    return sum / float(kMotionTaps);
}

// Unsharp mask on the linear HDR image. Matches rendering::unsharp_hdr.
vec3 sharpen_hdr(vec3 centre, vec2 uv) {
    vec2 t = vec2(pc.texel_x, pc.texel_y) * pc.sharpen_radius;
    vec3 blurred = (texture(hdr_input, uv + vec2(t.x, 0.0)).rgb +
                    texture(hdr_input, uv - vec2(t.x, 0.0)).rgb +
                    texture(hdr_input, uv + vec2(0.0, t.y)).rgb +
                    texture(hdr_input, uv - vec2(0.0, t.y)).rgb) * 0.25;
    return centre + (centre - blurred) * pc.sharpen_amount;
}

// Sum of the bloom levels. A bilinear fetch of a level half the size is the
// 2x2 tent filter an explicit upsample pass would apply, so summing the levels
// with their own samplers IS the upsample — which is why the chain has no
// upsample pass. The levels are averaged (1/N) so adding a level to the chain
// does not brighten the glow; `intensity` is the single control that does.
vec3 bloom_sum(vec2 uv) {
    vec3 s = texture(bloom0, uv).rgb;
    s += texture(bloom1, uv).rgb;
    s += texture(bloom2, uv).rgb;
    s += texture(bloom3, uv).rgb;
    return s * 0.25;
}

// Colour grade, applied in HDR before the tonemap operator (§206 puts grading
// before tonemapping, and the reason it matters: a contrast curve applied to an
// already-tonemapped image clips the highlights the operator had just rolled
// off). Matches rendering::apply_color_grade exactly, including the 0.25 gain.
vec3 color_grade(vec3 c) {
    const float kGain = 0.25;
    if (pc.grade_temperature != 0.0) {
        c.r *= 1.0 + kGain * pc.grade_temperature;
        c.b *= 1.0 - kGain * pc.grade_temperature;
    }
    if (pc.grade_tint != 0.0) {
        c.g *= 1.0 - kGain * pc.grade_tint;
    }
    if (pc.grade_contrast != 1.0) {
        c = (c - vec3(pc.grade_pivot)) * pc.grade_contrast + vec3(pc.grade_pivot);
    }
    if (pc.grade_gamma != 1.0 && pc.grade_gamma > 0.0) {
        // max(0, ...) because pow() of a negative base is undefined and a
        // contrast pivot below the black point can push a channel negative.
        c = pow(max(c, vec3(0.0)), vec3(1.0 / pc.grade_gamma));
    }
    return c;
}

// Must match rendering::kLutSize.
const int kLutSize = 16;

// The colour-grading LUT, applied in HDR next to the grade — §206 puts grading
// before the tonemap operator, and a LUT is a grade.
//
// The cube is stored as a strip, so blue picks the slice and red picks the
// column within it; the two slices either side of the blue value are fetched
// and blended, which is the trilinear lookup a 3D texture would do in hardware.
// The half-texel offsets are what keep the endpoints on the CENTRE of the first
// and last texels rather than on the texture's edge.
vec3 apply_lut(vec3 c) {
    if (pc.lut_strength <= 0.0) {
        return c;
    }
    const float n = float(kLutSize);
    const vec3 clamped = clamp(c, vec3(0.0), vec3(1.0));
    const float slice = clamped.b * (n - 1.0);
    const float s0 = floor(slice);
    const float s1 = min(s0 + 1.0, n - 1.0);
    const float f = slice - s0;
    const float row = (clamped.g * (n - 1.0) + 0.5) / n;
    const float col = clamped.r * (n - 1.0) + 0.5;
    const vec2 uv0 = vec2((s0 * n + col) / (n * n), row);
    const vec2 uv1 = vec2((s1 * n + col) / (n * n), row);
    const vec3 graded = mix(texture(color_lut, uv0).rgb, texture(color_lut, uv1).rgb, f);
    return mix(c, graded, clamp(pc.lut_strength, 0.0, 1.0));
}

void main() {
    // The lens stage comes first: it decides WHERE the image is read from, so
    // the sharpen taps and the bloom fetch must read at the same warped
    // coordinate — otherwise the glow would sit a few pixels off the surface
    // that cast it. `lens_uv` is exactly `in_uv` when both lens values are 0,
    // which is what keeps the stage free when it is not used.
    const vec2 lens_uv = lens_sample_uv(in_uv, 1); // green is never displaced
    vec3 hdr = lens_fetch();

    // Depth of field, first of the in-pass stages: it is a LENS effect, so it
    // happens before the sharpen, which is a sensor-side correction. The guard
    // is on the radius rather than a flag — `dof_max_radius` is 0 whenever the
    // stage is off, so an off stage costs one compare and nothing else.
    if (pc.dof_max_radius > 0.0) {
        hdr = dof_blur(hdr, lens_uv, dof_coc(dof_view_depth(lens_uv)));
    }

    // Motion blur, straight after depth of field: both are shutter/lens effects
    // that read the raw HDR target, and both are disabled by a zero value
    // rather than a flag.
    if (pc.motion_intensity > 0.0) {
        hdr = motion_blur(hdr, lens_uv);
    }

    if (pc.sharpen_amount > 0.0) {
        hdr = sharpen_hdr(hdr, lens_uv);
    }
    if (pc.bloom_intensity > 0.0) {
        hdr += bloom_sum(lens_uv) * pc.bloom_intensity;
    }

    // Exposure. With both guards above false this is the original first line,
    // `texture(...).rgb * pc.exposure`, exactly.
    hdr *= pc.exposure;

    if (pc.grade_enabled > 0.5) {
        hdr = color_grade(hdr);
    }
    // The LUT is a grade too, and it runs after the parametric one so an author
    // can shape the image first and then land it on a look.
    hdr = apply_lut(hdr);

    vec3 mapped;
    if (pc.tonemap_mode < 0.5) {
        // Exponential (default).
        mapped = vec3(1.0) - exp(-hdr);
    } else if (pc.tonemap_mode < 1.5) {
        // ACES (Narkowicz fitted). The constants are the published fit, not a
        // hand-tuned approximation — do not simplify them, the CPU mirror in
        // Renderer3D.cpp:tonemap() spells the same polynomial.
        const float a = 2.51;
        const float b = 0.03;
        const float c = 2.43;
        const float d = 0.59;
        const float e = 0.14;
        mapped = clamp((hdr * (a * hdr + b)) / (hdr * (c * hdr + d) + e), 0.0, 1.0);
    } else if (pc.tonemap_mode < 2.5) {
        // Reinhard. Never clamps: the curve approaches 1 asymptotically, which
        // is the whole point of the operator.
        mapped = hdr / (vec3(1.0) + hdr);
    } else {
        // Linear. Deliberately unclamped here — the UNorm target clamps on
        // write, and clamping in the shader would hide an over-bright scene
        // from a diagnostic capture.
        mapped = hdr;
    }

    // Gamma correction
    mapped = pow(mapped, vec3(1.0/2.2));
    // Post FX (must match rendering::apply_postfx exactly):
    // saturation around Rec.709 luma, then vignette darkening.
    float luma = dot(mapped, vec3(0.2126, 0.7152, 0.0722));
    mapped = mix(vec3(luma), mapped, pc.saturation);
    vec2 d = in_uv - 0.5;
    mapped *= 1.0 - pc.vignette * smoothstep(0.3, 0.9, length(d));
    out_color = vec4(mapped, 1.0);
}
