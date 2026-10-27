#define _CRT_SECURE_NO_WARNINGS
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Rendering/DynamicResolution.hpp>
#include <NF/Rendering/RenderScale.hpp>
#include <NF/Runtime/CharacterComponent.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Assets/AssetId.hpp>
#include <NF/Audio/AudioDecoder.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/PrefabLink.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Animation/Components.hpp>
#include <NF/Audio/Components.hpp>
#include <NF/Gameplay/Components.hpp>
#include <NF/Gameplay/GameplayState.hpp>
#include <NF/Scripting/ScriptEngine.hpp>
#include <NF/Vfx/Components.hpp>
#include <cmath>
#include <NF/Core/Logger.hpp>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <cstring>
#include <unordered_map>

namespace nf::runtime {

static std::string trim(const std::string& s) {
    size_t a=0; while(a<s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b=s.size(); while(b>a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a,b-a);
}

// Value of a `key=value` token on a component line, up to the next space.
// Returns "" when the key is absent. `key` must include the '='.
//
// The key must start a token: `render_scale=` must not match the tail of
// `min_render_scale=`. A plain substring search did exactly that once Phase 38
// put both keys on the same line, and the renderer received the floor as its
// ceiling. Requiring the preceding byte to be start-of-line or whitespace is
// the format's actual token rule.
static std::string field_value(const std::string& line, const char* key) {
    const size_t key_len = std::strlen(key);
    size_t p = line.find(key);
    while (p != std::string::npos && p != 0 &&
           !std::isspace(static_cast<unsigned char>(line[p - 1]))) {
        p = line.find(key, p + 1);
    }
    if (p == std::string::npos) return {};
    const size_t start = p + key_len;
    const size_t end = line.find(' ', start);
    return trim(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
}

// As above, parsed as a float. Returns false (leaving `out` untouched) when the
// key is absent or the value is not a number, so callers keep their defaults
// instead of silently reading a zero.
static bool field_float(const std::string& line, const char* key, f32& out) {
    const std::string v = field_value(line, key);
    if (v.empty()) return false;
    try {
        size_t used = 0;
        const f32 parsed = std::stof(v, &used);
        if (used != v.size()) return false;
        out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

// Parses `key(x,y,z)` into the outs. Returns false (leaving the outs
// untouched) when the key is absent or malformed, so callers keep their
// defaults instead of silently reading a zero — same contract as field_float.
static bool field_vec3(const std::string& line, const char* key, f32& x, f32& y, f32& z) {
    const size_t p = line.find(key);
    if (p == std::string::npos) {
        return false;
    }
    f32 vx = 0.0f, vy = 0.0f, vz = 0.0f;
    const std::string fmt = std::string(key) + "(%f,%f,%f)";
    if (sscanf(line.c_str() + p, fmt.c_str(), &vx, &vy, &vz) != 3) {
        return false;
    }
    x = vx;
    y = vy;
    z = vz;
    return true;
}

static bool finite3(f32 x, f32 y, f32 z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

// --- Local light lines (Phase 26) --------------------------------------------
//
// The `type=Point` and `type=Spot` branches of a Light line follow the contract
// the rest of the format already keeps: every key optional, an absent key keeps
// the component's default, and a key that is present but impossible rejects the
// whole line with a warning instead of writing half-trusted numbers into a light
// the author will judge by looking at the screen.
//
// The bounds are not a taste. They are what the renderer, its shadow projector
// and the GPU light block can represent — so a line that passes them cannot ask
// for something the frame quietly ignores and the author spends an afternoon
// chasing. `out_bad` names the key that failed so the warning can say which
// number, not just which line.

/// `color(r,g,b)` on a light line. Non-negative finite channels only: the
/// lighting pass multiplies these by intensity, so a negative channel would
/// subtract light from the scene, which no value on the line asks for.
/// 100 is well past any usable lamp colour and short of an inf written by hand.
static bool light_color(const std::string& line, f32& r, f32& g, f32& b, std::string& out_bad) {
    f32 cr = r, cg = g, cb = b;
    if (!field_vec3(line, "color", cr, cg, cb)) {
        return true; // absent: the defaults stand
    }
    if (!finite3(cr, cg, cb) || cr < 0.0f || cr > 100.0f || cg < 0.0f || cg > 100.0f ||
        cb < 0.0f || cb > 100.0f) {
        out_bad = "color";
        return false;
    }
    r = cr;
    g = cg;
    b = cb;
    return true;
}

/// One optional float key constrained to `[lo, hi]`, defaulting to whatever `out`
/// already holds.
///
/// Named for what it does rather than for its first caller: it began life
/// serving the point/spot light lines, and the post-process line needs exactly
/// the same contract (absent key keeps the default, a present-but-invalid one
/// names itself so the warning can say which).
static bool optional_float_in_range(const std::string& line, const char* key, f32 lo, f32 hi, f32& out,
                        std::string& out_bad) {
    f32 v = out;
    if (!field_float(line, key, v)) {
        return true; // absent, or not a number: not this function's call to make
    }
    if (!std::isfinite(v) || v < lo || v > hi) {
        out_bad = key;
        return false;
    }
    out = v;
    return true;
}

/// The u32 twin of the helper above, for the handful of integer keys the scene
/// format carries (max particles, cloth iterations, navmesh polygon cap). Same
/// contract: absent keeps the default, present-but-out-of-range names itself.
static bool optional_int_in_range(const std::string& line, const char* key, long long lo, long long hi,
                                  u32& out, std::string& out_bad) {
    const std::string v = field_value(line, key);
    if (v.empty()) {
        return true; // absent: not this function's call to make
    }
    long long parsed = 0;
    try {
        size_t used = 0;
        parsed = std::stoll(v, &used);
        if (used != v.size()) {
            out_bad = key;
            return false;
        }
    } catch (...) {
        out_bad = key;
        return false;
    }
    if (parsed < lo || parsed > hi) {
        out_bad = key;
        return false;
    }
    out = static_cast<u32>(parsed);
    return true;
}

/// The `PostProcess:` line (design §206). Every key is optional and an absent
/// one keeps the component default, so a scene can carry just `bloom=true` and
/// inherit everything else.
///
/// Ranges are deliberately loose where the renderer tolerates the extreme:
/// a bloom threshold of 0 is "everything blooms" and a knee of 0 is a hard cut,
/// both legitimate authoring choices. They are bounded where a value would be
/// nonsense — a negative threshold, a negative intensity, a zero or negative
/// radius (which would collapse every tap onto one texel), a negative contrast
/// or gamma (which would invert the image).
/// The `tonemap=` name for a `rendering::TonemapMode` value. The inverse of the
/// parse table above; an unknown value writes "exponential" so a hand-edited
/// file with a nonsense number round-trips to something loadable.
static const char* post_process_tonemap_name(int mode) {
    switch (mode) {
        case 1: return "aces";
        case 2: return "reinhard";
        case 3: return "linear";
        default: return "exponential";
    }
}

static bool parse_post_process(const std::string& line, PostProcessComponent& out,
                               std::string& out_bad) {
    out.bloom_enabled = line.find("bloom=true") != std::string::npos;
    if (!optional_float_in_range(line, "bloom_threshold=", 0.0f, 1000.0f, out.bloom_threshold, out_bad)) return false;
    if (!optional_float_in_range(line, "bloom_knee=", 0.0f, 1000.0f, out.bloom_knee, out_bad)) return false;
    if (!optional_float_in_range(line, "bloom_intensity=", 0.0f, 100.0f, out.bloom_intensity, out_bad)) return false;
    // A radius at or below zero collapses all thirteen taps onto the centre
    // texel, i.e. a blur that blurs nothing.
    if (!optional_float_in_range(line, "bloom_radius=", 0.01f, 16.0f, out.bloom_radius, out_bad)) return false;

    out.grade_enabled = line.find("grade=true") != std::string::npos;
    if (!optional_float_in_range(line, "grade_contrast=", 0.0f, 16.0f, out.grade_contrast, out_bad)) return false;
    if (!optional_float_in_range(line, "grade_pivot=", 0.0f, 1000.0f, out.grade_pivot, out_bad)) return false;
    if (!optional_float_in_range(line, "grade_temperature=", -1.0f, 1.0f, out.grade_temperature, out_bad)) return false;
    if (!optional_float_in_range(line, "grade_tint=", -1.0f, 1.0f, out.grade_tint, out_bad)) return false;
    if (!optional_float_in_range(line, "grade_gamma=", 0.05f, 8.0f, out.grade_gamma, out_bad)) return false;

    out.sharpen_enabled = line.find("sharpen=true") != std::string::npos;
    if (!optional_float_in_range(line, "sharpen_amount=", 0.0f, 8.0f, out.sharpen_amount, out_bad)) return false;
    if (!optional_float_in_range(line, "sharpen_radius=", 0.01f, 16.0f, out.sharpen_radius, out_bad)) return false;

    if (!optional_float_in_range(line, "saturation=", 0.0f, 8.0f, out.saturation, out_bad)) return false;
    if (!optional_float_in_range(line, "vignette=", 0.0f, 1.0f, out.vignette, out_bad)) return false;

    out.lens_enabled = line.find("lens=true") != std::string::npos;
    // Distortion is a signed coefficient and both signs are legitimate (barrel
    // and pincushion), so the range is symmetric. Past roughly +/-0.5 the warp
    // folds the corners over themselves, which is a stylization rather than a
    // lens; the bound refuses the fold rather than letting a typo produce one.
    if (!optional_float_in_range(line, "lens_distortion=", -0.5f, 0.5f, out.lens_distortion, out_bad)) return false;
    if (!optional_float_in_range(line, "lens_chroma=", 0.0f, 0.1f, out.lens_chromatic_aberration, out_bad)) return false;

    out.dof_enabled = line.find("dof=true") != std::string::npos;
    // A focus distance is unbounded above on purpose: focusing on the far plane
    // (the sky) is a legitimate way to say "nothing is out of focus".
    if (!optional_float_in_range(line, "dof_focus=", 0.0f, 1.0e30f, out.dof_focus_distance, out_bad)) return false;
    // A zero range would divide by zero in the ramp, so the floor is a real
    // bound rather than a stylistic one.
    if (!optional_float_in_range(line, "dof_range=", 0.01f, 1.0e6f, out.dof_focus_range, out_bad)) return false;
    if (!optional_float_in_range(line, "dof_radius=", 0.0f, 64.0f, out.dof_max_radius, out_bad)) return false;

    out.motion_enabled = line.find("motion=true") != std::string::npos;
    // A non-positive intensity is "off", not "blur in the other direction", so
    // the floor is zero and the value is the stage's own switch.
    if (!optional_float_in_range(line, "motion_intensity=", 0.0f, 16.0f, out.motion_intensity, out_bad)) return false;
    // A zero cap would mean "never smear", which is what a zero intensity
    // already says; the floor keeps the two from meaning the same thing.
    if (!optional_float_in_range(line, "motion_length=", 0.001f, 1.0f, out.motion_max_length, out_bad)) return false;

    // Anti-aliasing. `fxaa=true` is the switch; the thresholds keep their
    // defaults when absent, so a one-key line is a complete line. The bounds
    // are the ones the renderer clamps to, so a value the runtime would have
    // had to silently re-clamp is refused here instead.
    out.fxaa_enabled = line.find("fxaa=true") != std::string::npos;
    if (!optional_float_in_range(line, "fxaa_threshold=", 0.01f, 1.0f, out.fxaa_threshold, out_bad)) return false;
    if (!optional_float_in_range(line, "fxaa_threshold_min=", 0.0f, 1.0f, out.fxaa_threshold_min, out_bad)) return false;

    // Internal render scale (Phase 37). The bounds are the renderer's own, so
    // a value accepted here is never silently re-clamped by the renderer, and
    // an out-of-range value is refused by name instead of producing a frame at
    // a resolution the author did not ask for.
    if (!optional_float_in_range(line, "render_scale=", rendering::kMinRenderScale,
                                 rendering::kMaxRenderScale, out.render_scale, out_bad)) {
        return false;
    }

    // Dynamic resolution (Phase 38). `render_scale` is the ceiling, so the
    // floor must not sit above it; an inverted pair is refused by name rather
    // than silently swapped, because the file would then mean something other
    // than what its author wrote. The target frame rate uses the same bounds
    // the editor slider uses.
    out.dynamic_resolution = line.find("dynamic_resolution=true") != std::string::npos;
    // The window policy is an alternative decision source for the SAME scale:
    // it reads the same ceiling/floor/target, so it needs no keys of its own
    // beyond the switch.
    out.frame_policy = line.find("frame_policy=true") != std::string::npos;
    if (!optional_float_in_range(line, "target_fps=", rendering::kMinTargetFps,
                                 rendering::kMaxTargetFps, out.target_fps, out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "min_render_scale=", rendering::kMinRenderScale,
                                 rendering::kMaxRenderScale, out.min_render_scale, out_bad)) {
        return false;
    }
    if (out.min_render_scale > out.render_scale) {
        out_bad = "min_render_scale=";
        return false;
    }
    // Phase 40: the governor's load clock. `cpu` is the default and is never
    // written because its absence already means CPU; `gpu` opts into the real
    // timestamp sample (with an automatic CPU fallback when none is fresh).
    // A closed set: a typo must fail by name, not silently run on the CPU.
    {
        const std::string timing = field_value(line, "dynamic_resolution_timing=");
        if (!timing.empty()) {
            if (timing == "cpu") {
                out.dynamic_resolution_timing = kDynamicResolutionTimingCpu;
            } else if (timing == "gpu") {
                out.dynamic_resolution_timing = kDynamicResolutionTimingGpu;
            } else {
                out_bad = "dynamic_resolution_timing=";
                return false;
            }
        }
    }

    // Exposure and the tonemap operator, both OPT-IN. The sentinels (0 and -1)
    // mean "the scene says nothing", so an absent key leaves the renderer's own
    // setting alone — which is what the acceptance run's golden pixels need.
    if (!optional_float_in_range(line, "exposure=", 0.0f, 1000.0f, out.exposure, out_bad)) return false;
    {
        const std::string name = field_value(line, "tonemap=");
        if (!name.empty()) {
            if (name == "exponential") {
                out.tonemap = 0;
            } else if (name == "aces") {
                out.tonemap = 1;
            } else if (name == "reinhard") {
                out.tonemap = 2;
            } else if (name == "linear") {
                out.tonemap = 3;
            } else {
                // A closed set, so an unknown name is refused rather than
                // defaulted: silently picking Exponential would make a typo
                // look like a working setting.
                out_bad = "tonemap";
                return false;
            }
        }
    }
    // The colour LUT: a logical image path, resolved by the runtime. A strength
    // with no path does nothing, which the writer keeps out of the file.
    out.lut_path = field_value(line, "lut=");
    if (!optional_float_in_range(line, "lut_strength=", 0.0f, 1.0f, out.lut_strength, out_bad)) return false;

    // Distance fog. `fog=true` is the switch; the colour and band keep their
    // defaults when absent, so a one-key line is a complete line (the same
    // contract every other stage on this line follows).
    out.fog_enabled = line.find("fog=true") != std::string::npos;
    {
        f32 fr = out.fog_r, fg = out.fog_g, fb = out.fog_b;
        if (field_vec3(line, "fog_color", fr, fg, fb)) {
            if (!finite3(fr, fg, fb) || fr < 0.0f || fr > 100.0f || fg < 0.0f || fg > 100.0f ||
                fb < 0.0f || fb > 100.0f) {
                out_bad = "fog_color=";
                return false;
            }
            out.fog_r = fr;
            out.fog_g = fg;
            out.fog_b = fb;
        }
    }
    if (!optional_float_in_range(line, "fog_start=", 0.0f, 1.0e6f, out.fog_start, out_bad)) return false;
    // A zero end would make the fade band degenerate; the renderer clamps the
    // two into order at extraction, but refuse the value that cannot describe a
    // band at all.
    if (!optional_float_in_range(line, "fog_end=", 0.01f, 1.0e6f, out.fog_end, out_bad)) return false;
    return true;
}

// `Feedback:` (Phase 56). Same contract as the post-process line: every key is
// optional, an absent key keeps the component default, and a present value the
// director would have had to silently re-clamp is refused by name. The bounds
// are the director's own clamps, so a line that passes here means exactly what
// it says.
static bool parse_feedback(const std::string& line, FeedbackComponent& out,
                           std::string& out_bad) {
    if (!optional_float_in_range(line, "flash_time=", 0.002f, 10.0f, out.flash_seconds, out_bad)) return false;
    if (!optional_float_in_range(line, "flash_size=", 0.001f, 10.0f, out.flash_size, out_bad)) return false;
    if (!optional_float_in_range(line, "tracer_time=", 0.002f, 10.0f, out.tracer_seconds, out_bad)) return false;
    if (!optional_float_in_range(line, "tracer_length=", 0.01f, 1000.0f, out.tracer_length, out_bad)) return false;
    if (!optional_float_in_range(line, "tracer_width=", 0.0005f, 1.0f, out.tracer_width, out_bad)) return false;
    if (!optional_float_in_range(line, "impact_time=", 0.01f, 600.0f, out.impact_seconds, out_bad)) return false;
    if (!optional_float_in_range(line, "impact_size=", 0.001f, 10.0f, out.impact_size, out_bad)) return false;
    if (!optional_float_in_range(line, "shot_volume=", 0.0f, 4.0f, out.shot_volume, out_bad)) return false;
    if (!optional_float_in_range(line, "impact_volume=", 0.0f, 4.0f, out.impact_volume, out_bad)) return false;
    // Phase 59. `impact_volume_explosive=` cannot shadow `impact_volume=`:
    // field_value requires the byte before a key to be whitespace, and the
    // longer key's prefix is `impact_volume_`, not `impact_volume=`.
    if (!optional_float_in_range(line, "blast_time=", 0.002f, 10.0f, out.blast_flash_seconds, out_bad)) return false;
    if (!optional_float_in_range(line, "blast_size=", 0.001f, 10.0f, out.blast_flash_size, out_bad)) return false;
    if (!optional_float_in_range(line, "impact_volume_explosive=", 0.0f, 4.0f,
                                 out.impact_volume_explosive, out_bad)) return false;
    if (!optional_float_in_range(line, "impact_volume_fire=", 0.0f, 4.0f,
                                 out.impact_volume_fire, out_bad)) return false;
    if (!optional_float_in_range(line, "light_intensity=", 0.0f, 1000.0f, out.flash_light_intensity, out_bad)) return false;
    if (!optional_float_in_range(line, "light_radius=", 0.01f, 1000.0f, out.flash_light_radius, out_bad)) return false;
    if (!optional_float_in_range(line, "light_r=", 0.0f, 1.0f, out.flash_light_r, out_bad)) return false;
    if (!optional_float_in_range(line, "light_g=", 0.0f, 1.0f, out.flash_light_g, out_bad)) return false;
    if (!optional_float_in_range(line, "light_b=", 0.0f, 1.0f, out.flash_light_b, out_bad)) return false;
    if (!optional_int_in_range(line, "max_flashes=", 1, 4096, out.max_flashes, out_bad)) return false;
    if (!optional_int_in_range(line, "max_tracers=", 1, 4096, out.max_tracers, out_bad)) return false;
    if (!optional_int_in_range(line, "max_impacts=", 1, 4096, out.max_impacts, out_bad)) return false;
    if (!optional_int_in_range(line, "max_sounds=", 1, 4096, out.max_sounds, out_bad)) return false;
    if (!optional_int_in_range(line, "max_lights=", 1, 4096, out.max_lights, out_bad)) return false;
    out.audio = line.find("audio=false") == std::string::npos;
    out.enabled = line.find("enabled=false") == std::string::npos;
    out.flash_light = line.find("light=false") == std::string::npos;
    return true;
}

// `Perception:` (Phase 60). Same contract as the feedback line above: every key
// optional, an absent key keeps the component default, and a present value the
// perception module would have had to re-clamp is refused by name. The bounds
// are the module's own working ranges — a sight range of zero is legal (a blind
// NPC is authored, not a bug), a negative one is not.
static bool parse_perception(const std::string& line, PerceptionComponent& out,
                             std::string& out_bad) {
    if (!optional_float_in_range(line, "sight=", 0.0f, 10000.0f, out.sight_range, out_bad)) return false;
    if (!optional_float_in_range(line, "fov=", 0.0f, 180.0f, out.sight_half_fov_deg, out_bad)) return false;
    if (!optional_float_in_range(line, "gain=", 0.0f, 1000.0f, out.sight_gain, out_bad)) return false;
    if (!optional_float_in_range(line, "loss=", 0.0f, 1000.0f, out.sight_loss, out_bad)) return false;
    if (!optional_float_in_range(line, "linger=", 0.0f, 3600.0f, out.sight_linger, out_bad)) return false;
    if (!optional_float_in_range(line, "hearing=", 0.0f, 10000.0f, out.hearing_range, out_bad)) return false;
    if (!optional_float_in_range(line, "touch=", 0.0f, 1000.0f, out.touch_range, out_bad)) return false;
    if (!optional_float_in_range(line, "noise=", 0.0f, 1.0f, out.noise, out_bad)) return false;
    if (!optional_float_in_range(line, "visible=", 0.0f, 1.0f, out.visibility, out_bad)) return false;
    if (!optional_float_in_range(line, "radius=", 0.0f, 100.0f, out.radius, out_bad)) return false;
    if (!optional_float_in_range(line, "eye=", 0.0f, 100.0f, out.eye_height, out_bad)) return false;
    out.pain = line.find("pain=false") == std::string::npos;
    out.governs_combat = line.find("combat=false") == std::string::npos;
    out.enabled = line.find("enabled=false") == std::string::npos;
    return true;
}

// `Brain:` and `BrainAction:` (Phase 61). The brain line declares the decision
// maker, and each action line is one consideration appended to it. The contract
// is the same as every other optional line: an absent key keeps the default, and
// a present value the module would have had to re-clamp is refused by name.
//
// `name=` is a single token, like `Faction: team=`: it is what Lua and the
// editor read back, and a name with a space in it would be read as two fields.
static bool parse_brain(const std::string& line, BrainComponent& out, std::string& out_bad) {
    if (!optional_float_in_range(line, "rate=", 0.0f, 60.0f, out.rate, out_bad)) return false;
    out.enabled = line.find("enabled=false") == std::string::npos;
    return true;
}

/// The four curve presets, which the explicit fields then override. Named
/// because "shaped like a step at the breakpoint" is what an author is thinking,
/// and `slope=10 exponent=1 input_offset=0.5` is what they would have to type.
static void apply_curve_preset(std::string_view preset, BrainAction& out) {
    if (preset == "linear") {
        out.slope = 1.0f;
        out.exponent = 1.0f;
        out.input_offset = 0.0f;
        out.output_offset = 0.0f;
    } else if (preset == "quadratic") {
        out.slope = 1.0f;
        out.exponent = 2.0f;
        out.input_offset = 0.0f;
        out.output_offset = 0.0f;
    } else if (preset == "inverse") {
        out.slope = -1.0f;
        out.exponent = 1.0f;
        out.input_offset = 0.0f;
        out.output_offset = 1.0f;
    } else if (preset == "threshold") {
        out.slope = 10.0f;
        out.exponent = 1.0f;
        out.input_offset = 0.5f;
        out.output_offset = 0.0f;
    }
}

static bool parse_brain_action(const std::string& line, BrainAction& out, std::string& out_bad) {
    const std::string name = field_value(line, "name=");
    if (name.empty()) {
        out_bad = "name";
        return false;
    }
    out.name = name;

    const std::string input = field_value(line, "input=");
    if (input.empty()) {
        out_bad = "input";
        return false;
    }
    if (!brain_input_from_name(input, out.input)) {
        out_bad = "input"; // the name is what the warning prints
        return false;
    }

    // The preset first, then the explicit fields, so a line may say
    // `curve=inverse slope=-2` and mean it.
    const std::string preset = field_value(line, "curve=");
    if (!preset.empty()) {
        if (preset != "linear" && preset != "quadratic" && preset != "inverse" &&
            preset != "threshold") {
            out_bad = "curve";
            return false;
        }
        apply_curve_preset(preset, out);
    }

    if (!optional_float_in_range(line, "slope=", -1000.0f, 1000.0f, out.slope, out_bad)) return false;
    if (!optional_float_in_range(line, "exponent=", 0.0f, 16.0f, out.exponent, out_bad)) return false;
    if (!optional_float_in_range(line, "input_offset=", -1000000.0f, 1000000.0f, out.input_offset, out_bad)) return false;
    if (!optional_float_in_range(line, "output_offset=", -1000.0f, 1000.0f, out.output_offset, out_bad)) return false;
    if (!optional_float_in_range(line, "weight=", 0.0f, 1000.0f, out.weight, out_bad)) return false;
    out.hold_fire = line.find("hold_fire=true") != std::string::npos;
    return true;
}

// `States:`, `State:` and `Transition:` (Phase 62). The machine's declaration,
// its states and its guarded edges, each on its own line so a patrol reads like
// the shape it is. Same contract as the brain lines: an absent key keeps the
// default, and a value that cannot be read is refused by name.
static bool parse_states(const std::string& line, StateMachineComponent& out,
                         std::string& out_bad) {
    if (!optional_float_in_range(line, "rate=", 0.0f, 60.0f, out.rate, out_bad)) return false;
    out.initial = field_value(line, "initial=");
    out.enabled = line.find("enabled=false") == std::string::npos;
    return true;
}

static bool parse_state(const std::string& line, StateSpec& out, std::string& out_bad) {
    const std::string name = field_value(line, "name=");
    if (name.empty()) {
        out_bad = "name";
        return false;
    }
    out.name = name;
    out.hold_fire = line.find("hold_fire=true") != std::string::npos;
    return true;
}

static bool parse_transition(const std::string& line, TransitionSpec& out, std::string& out_bad) {
    out.from = field_value(line, "from=");
    if (out.from.empty()) {
        out_bad = "from";
        return false;
    }
    out.to = field_value(line, "to=");
    if (out.to.empty()) {
        out_bad = "to";
        return false;
    }
    const std::string when = field_value(line, "when=");
    if (when.empty() || !brain_input_from_name(when, out.when)) {
        out_bad = "when";
        return false;
    }
    if (!optional_float_in_range(line, "at=", -1000000.0f, 1000000.0f, out.at, out_bad)) {
        return false;
    }
    out.below = line.find("below=true") != std::string::npos;
    return true;
}

/// `key=x,y,z`. The comma form, like the camera's `pivot=`, because a link is a
/// pair of positions and `from(x,y,z) to(x,y,z)` would nest parentheses twice on
/// one line.
static bool field_vec3_commas(const std::string& line, const char* key, f32& x, f32& y, f32& z) {
    const std::string v = field_value(line, key);
    if (v.empty()) {
        return false;
    }
    f32 vx = 0.0f;
    f32 vy = 0.0f;
    f32 vz = 0.0f;
    if (sscanf(v.c_str(), "%f,%f,%f", &vx, &vy, &vz) != 3) {
        return false;
    }
    if (!finite3(vx, vy, vz)) {
        return false;
    }
    x = vx;
    y = vy;
    z = vz;
    return true;
}

// `Terrain:` (Phase 63). The ground the navigation mesh walks on, built from a
// heightmap image when one is authored and from deterministic value noise
// otherwise. Same contract as every other optional line.
static bool parse_terrain(const std::string& line, TerrainComponent& out, std::string& out_bad) {
    if (!optional_int_in_range(line, "res=", 2, 1024, out.resolution, out_bad)) return false;
    if (!optional_int_in_range(line, "octaves=", 0, 12, out.octaves, out_bad)) return false;
    if (!optional_int_in_range(line, "seed=", 0, 0x7FFFFFFF, out.seed, out_bad)) return false;
    if (!optional_float_in_range(line, "area=", 0.1f, 100000.0f, out.area, out_bad)) return false;
    if (!optional_float_in_range(line, "height=", 0.0f, 100000.0f, out.height_scale, out_bad)) return false;
    if (!optional_float_in_range(line, "freq=", 0.0001f, 10.0f, out.noise_frequency, out_bad)) return false;
    out.heightmap = field_value(line, "heightmap=");
    out.material = field_value(line, "material=");
    out.enabled = line.find("enabled=false") == std::string::npos;
    return true;
}

// `NavLink:` (Phase 63): one authored off-mesh connection on the navigation
// volume's entity. Both endpoints are required; an endpoint that is not on the
// baked mesh is dropped by the module and counted, which the validator can see.
static bool parse_nav_link(const std::string& line, NavLinkSpec& out, std::string& out_bad) {
    if (!field_vec3_commas(line, "from=", out.start_x, out.start_y, out.start_z)) {
        out_bad = "from";
        return false;
    }
    if (!field_vec3_commas(line, "to=", out.end_x, out.end_y, out.end_z)) {
        out_bad = "to";
        return false;
    }
    out.bidirectional = line.find("one_way=true") == std::string::npos;
    return true;
}

static bool parse_point_light(const std::string& line, PointLightComponent& out,
                              std::string& out_bad) {
    if (!light_color(line, out.color_r, out.color_g, out.color_b, out_bad)) return false;
    if (!optional_float_in_range(line, "intensity=", 0.0f, 1000.0f, out.intensity, out_bad)) return false;
    // A radius of zero is a light that illuminates nothing at any brightness;
    // refusing it turns "why is my torch invisible" into a line in the log.
    if (!optional_float_in_range(line, "radius=", 0.01f, 10000.0f, out.radius, out_bad)) return false;
    if (!optional_float_in_range(line, "shadow_strength=", 0.0f, 1.0f, out.shadow_strength, out_bad)) return false;
    if (!optional_float_in_range(line, "shadow_bias=", 0.0f, 0.01f, out.shadow_bias, out_bad)) return false;
    if (!optional_float_in_range(line, "shadow_distance=", 0.0f, 10000.0f, out.shadow_distance, out_bad)) return false;
    out.cast_shadows = line.find("shadows=true") != std::string::npos;
    out.enabled = line.find("enabled=false") == std::string::npos;
    return true;
}

static bool parse_spot_light(const std::string& line, SpotLightComponent& out,
                             std::string& out_bad) {
    f32 dx = out.dir_x, dy = out.dir_y, dz = out.dir_z;
    if (field_vec3(line, "dir", dx, dy, dz)) {
        if (!finite3(dx, dy, dz)) {
            out_bad = "dir";
            return false;
        }
        // Stored normalised. The shader and the shadow projector both treat
        // `direction` as a unit vector, so an unnormalised one on the line would
        // not tilt the beam, only scale the dot products the cone test reads —
        // a lamp whose cone is the wrong width for no visible reason.
        const f32 len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len < 1e-6f) {
            out_bad = "dir"; // a zero vector has no direction to keep
            return false;
        }
        out.dir_x = dx / len;
        out.dir_y = dy / len;
        out.dir_z = dz / len;
    }
    if (!light_color(line, out.color_r, out.color_g, out.color_b, out_bad)) return false;
    if (!optional_float_in_range(line, "intensity=", 0.0f, 1000.0f, out.intensity, out_bad)) return false;
    if (!optional_float_in_range(line, "range=", 0.01f, 10000.0f, out.range, out_bad)) return false;
    // Half a turn is the widest cone one perspective projector can fit; beyond
    // it the shadow map's faces stop covering the light's own reach.
    if (!optional_float_in_range(line, "inner_rad=", 0.0f, 1.5707963f, out.inner_angle_rad, out_bad)) return false;
    if (!optional_float_in_range(line, "outer_rad=", 0.0f, 1.5707963f, out.outer_angle_rad, out_bad)) return false;
    if (out.inner_angle_rad > out.outer_angle_rad) {
        // The falloff window would run backwards: the shader's smoothstep between
        // the two angles is a division by (outer - inner).
        out_bad = "inner_rad/outer_rad";
        return false;
    }
    if (!optional_float_in_range(line, "shadow_strength=", 0.0f, 1.0f, out.shadow_strength, out_bad)) return false;
    if (!optional_float_in_range(line, "shadow_bias=", 0.0f, 0.01f, out.shadow_bias, out_bad)) return false;
    if (!optional_float_in_range(line, "shadow_distance=", 0.0f, 10000.0f, out.shadow_distance, out_bad)) return false;
    out.cast_shadows = line.find("shadows=true") != std::string::npos;
    out.enabled = line.find("enabled=false") == std::string::npos;
    return true;
}

// --- Navigation lines (Phase 28) ---------------------------------------------
//
// Same contract as every other component line: every key optional, an absent
// key keeps the component default, a present-but-impossible one rejects the
// whole line and names itself in the warning. The bounds are the ones
// `ai::NavMesh::build` and the agent stepper can actually use — a zero cell
// size divides by zero in the voxeliser, a zero footprint bakes nothing, and a
// negative speed would walk an agent backwards through its own path.

static bool parse_navmesh(const std::string& line, NavMeshComponent& out, std::string& out_bad) {
    f32 ax = out.area_x, ay = out.area_y, az = out.area_z;
    if (field_vec3(line, "area", ax, ay, az)) {
        if (!finite3(ax, ay, az) || ax <= 0.0f || ay <= 0.0f || az <= 0.0f || ax > 100000.0f ||
            ay > 100000.0f || az > 100000.0f) {
            out_bad = "area";
            return false;
        }
        out.area_x = ax;
        out.area_y = ay;
        out.area_z = az;
    }
    if (!optional_float_in_range(line, "cell=", 0.01f, 100.0f, out.cell_size, out_bad)) return false;
    if (!optional_float_in_range(line, "cell_height=", 0.01f, 100.0f, out.cell_height, out_bad)) return false;
    if (!optional_float_in_range(line, "slope=", 0.0f, 90.0f, out.walkable_slope_deg, out_bad)) return false;
    if (!optional_float_in_range(line, "climb=", 0.0f, 100.0f, out.walkable_climb, out_bad)) return false;
    if (!optional_float_in_range(line, "headroom=", 0.01f, 1000.0f, out.walkable_height, out_bad)) return false;
    if (!optional_float_in_range(line, "min_area=", 0.0f, 1000000.0f, out.min_region_area, out_bad)) return false;
    if (!optional_float_in_range(line, "agent_radius=", 0.0f, 100.0f, out.agent_radius, out_bad)) return false;
    if (!optional_float_in_range(line, "jump_distance=", 0.0f, 1000.0f, out.jump_distance, out_bad)) return false;
    if (!optional_float_in_range(line, "jump_height=", 0.0f, 1000.0f, out.jump_height, out_bad)) return false;
    // A polygon with two vertices is a line: the mesh's cover produces
    // rectangles, so three is the smallest meaningful cap and sixty-four is far
    // past any the convex merge can reach anyway.
    if (!optional_int_in_range(line, "max_verts=", 3, 64, out.max_verts_per_poly, out_bad)) return false;
    out.enabled = line.find("enabled=false") == std::string::npos;
    return true;
}

static bool parse_nav_agent(const std::string& line, NavAgentComponent& out, std::string& out_bad) {
    f32 gx = out.goal_x, gy = out.goal_y, gz = out.goal_z;
    if (field_vec3(line, "goal", gx, gy, gz)) {
        if (!finite3(gx, gy, gz)) {
            out_bad = "goal";
            return false;
        }
        out.goal_x = gx;
        out.goal_y = gy;
        out.goal_z = gz;
    }
    if (!optional_float_in_range(line, "speed=", 0.0f, 1000.0f, out.speed, out_bad)) return false;
    // The arrival radius must be positive: zero makes "arrived" unreachable on a
    // rounded waypoint, so an agent would orbit its goal forever.
    if (!optional_float_in_range(line, "arrive=", 0.001f, 100.0f, out.arrive_radius, out_bad)) return false;
    out.enabled = line.find("enabled=false") == std::string::npos;
    return true;
}

// --- Patrol routes (Phase 64) -----------------------------------------------
//
// `NavRoute:` declares the route's policy and `NavPoint:` appends one
// destination. Same optional-key contract as every line above: absent keeps
// the default, present-but-impossible rejects that line by name. The route and
// its points may be interleaved; the loader merges them on the entity.

static bool parse_nav_route(const std::string& line, NavRouteComponent& out,
                            std::string& out_bad) {
    if (!optional_float_in_range(line, "wait=", 0.0f, 3600.0f, out.wait, out_bad)) return false;
    out.loop = line.find("loop=false") == std::string::npos;
    out.ping_pong = line.find("ping_pong=true") != std::string::npos;
    out.enabled = line.find("enabled=false") == std::string::npos;
    // Phase 65: an absent key keeps `off`, so a Phase 64 route parses exactly
    // as it did. A present-but-unknown policy rejects the line rather than
    // silently walking a patrol the author meant to gate.
    const std::string hold = field_value(line, "hold=");
    if (!hold.empty() && !nav_route_hold_from_name(hold, out.hold)) {
        out_bad = "hold";
        return false;
    }
    if (!optional_float_in_range(line, "resume=", 0.0f, 3600.0f, out.resume, out_bad)) return false;
    // Phase 66: how long to search the last known position after the hold ends.
    // Absent keeps zero, so a Phase 65 route parses exactly as it did.
    if (!optional_float_in_range(line, "investigate=", 0.0f, 3600.0f, out.investigate, out_bad)) return false;
    // Phase 67: a squad token, parsed like `Faction: team=`. A route with no
    // token stays solo, and the loader refuses an over-long token by name
    // rather than silently truncating a name two routes would stop matching.
    out.squad = field_value(line, "squad=");
    if (out.squad.size() > 64u) {
        out_bad = "squad=";
        return false;
    }
    return true;
}

static bool parse_nav_point(const std::string& line, NavPointSpec& out, std::string& out_bad) {
    if (!field_vec3_commas(line, "pos=", out.x, out.y, out.z)) {
        out_bad = "pos";
        return false;
    }
    // -1 is the component's sentinel for "use the route's wait", so a point
    // only carries a wait when the author wrote one. Zero is meaningful: it
    // overrides a non-zero route wait with "do not pause here".
    f32 wait = out.wait;
    if (!optional_float_in_range(line, "wait=", 0.0f, 3600.0f, wait, out_bad)) return false;
    out.wait = wait;
    return true;
}

// --- Weapons and hit points (Phase 44) --------------------------------------
//
// The two lines follow the contract the rest of the format keeps: every key is
// optional, an absent key keeps the component's default, and a key that is
// present but impossible rejects the WHOLE line with a warning naming the key.
// Half a weapon is worse than no weapon: a scene whose magazine was accepted
// and whose cadence was not would fire at the engine's default rate and look
// like the author's number was ignored.
//
// The bounds are what the runtime can honour, not taste. 3600 rpm is the
// fastest cadence the clock represents (kMaxRoundsPerMinute); a spread past 90
// degrees is a weapon that shoots behind itself, which no scene means.

static bool parse_weapon(const std::string& line, WeaponProfile& out, std::string& out_bad) {
    const std::string mode = field_value(line, "mode=");
    if (!mode.empty() && !fire_mode_from_name(mode, out.mode)) {
        out_bad = "mode";
        return false;
    }
    const std::string delivery = field_value(line, "delivery=");
    if (!delivery.empty() && !weapon_delivery_from_name(delivery, out.delivery)) {
        out_bad = "delivery";
        return false;
    }
    const std::string damage_type = field_value(line, "damage_type=");
    if (!damage_type.empty() && !damage_type_from_name(damage_type, out.damage_type)) {
        out_bad = "damage_type";
        return false;
    }
    const std::string status = field_value(line, "status=");
    if (!status.empty() && !status_kind_from_name(status, out.status)) {
        out_bad = "status";
        return false;
    }
    if (!optional_float_in_range(line, "status_dps=", 0.0f, 1000000.0f,
                                 out.status_damage_per_second, out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "status_time=", 0.0f, 3600.0f, out.status_seconds,
                                 out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "status_tick=", 0.01f, 60.0f, out.status_tick_seconds,
                                 out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "rpm=", 1.0f, kMaxRoundsPerMinute,
                                 out.rounds_per_minute, out_bad)) {
        return false;
    }
    if (!optional_int_in_range(line, "magazine=", 1, 10000, out.magazine, out_bad)) return false;
    if (!optional_int_in_range(line, "reserve=", 0, 100000, out.reserve, out_bad)) return false;
    if (!optional_float_in_range(line, "reload=", 0.0f, 60.0f, out.reload_seconds, out_bad)) {
        return false;
    }
    if (!optional_int_in_range(line, "burst=", 1, 100, out.burst_count, out_bad)) return false;
    if (!optional_float_in_range(line, "burst_gap=", 0.0f, 10.0f, out.burst_gap_seconds,
                                 out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "damage=", 0.0f, 100000.0f, out.damage, out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "range=", 0.001f, 100000.0f, out.range, out_bad)) {
        return false;
    }
    // Projectile delivery (Phase 46). The keys are accepted on a hitscan line
    // too, but mean nothing there and are not written back; a scene that flips
    // delivery later therefore does not need to retype them.
    if (!optional_float_in_range(line, "speed=", 0.001f, 100000.0f, out.projectile_speed,
                                 out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "gravity=", 0.0f, 1000.0f, out.projectile_gravity,
                                 out_bad)) {
        return false;
    }
    if (!optional_int_in_range(line, "pellets=", 1, 64, out.pellets, out_bad)) return false;
    // Radial blast (Phase 51). All three keys are optional and zero by default,
    // so a weapon line written before this phase keeps its exact behaviour.
    if (!optional_float_in_range(line, "blast_radius=", 0.0f, 1000.0f, out.blast_radius,
                                 out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "blast_damage=", 0.0f, 1000000.0f, out.blast_damage,
                                 out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "blast_min=", 0.0f, 1.0f, out.blast_min_scale, out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "spread=", 0.0f, 90.0f, out.spread_deg, out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "bloom=", 0.0f, 90.0f, out.spread_bloom_deg, out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "spread_max=", 0.001f, 90.0f, out.spread_max_deg,
                                 out_bad)) {
        return false;
    }
    // The ceiling must be at least the floor, or the cone the weapon reports
    // and the cone it may reach disagree from the first shot.
    if (out.spread_max_deg < out.spread_deg) {
        out_bad = "spread_max";
        return false;
    }
    if (!optional_float_in_range(line, "recover=", 0.0f, 1000.0f, out.spread_recover_deg_per_s,
                                 out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "recoil=", 0.0f, 45.0f, out.recoil_pitch, out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "recoil_yaw=", 0.0f, 45.0f, out.recoil_yaw, out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "recoil_growth=", 0.0f, 10.0f, out.recoil_growth,
                                 out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "falloff_start=", 0.0f, 100000.0f, out.falloff_start,
                                 out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "falloff_end=", 0.0f, 100000.0f, out.falloff_end,
                                 out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "falloff_min=", 0.0f, 1.0f, out.falloff_min_scale,
                                 out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "pierce=", 0.0f, 1.0f, out.armor_penetration, out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "shake=", 0.0f, 1.0f, out.shake, out_bad)) return false;
    if (!optional_int_in_range(line, "seed=", 0, 4294967295LL, out.seed, out_bad)) return false;
    out.auto_reload = line.find("auto_reload=false") == std::string::npos;
    out.enabled = line.find("enabled=false") == std::string::npos;
    return true;
}

static bool parse_health(const std::string& line, HealthComponent& out, std::string& out_bad) {
    const f32 authored_max = out.max_health;
    if (!optional_float_in_range(line, "max=", 0.001f, 1000000.0f, out.max_health, out_bad)) {
        return false;
    }
    // Current hit points default to the MAXIMUM, not to the component's
    // default of 100: a scene that writes `max=250` and nothing else means a
    // healthy 250, not a wounded 100.
    if (out.health == authored_max) {
        out.health = out.max_health;
    }
    if (!optional_float_in_range(line, "health=", 0.0f, 1000000.0f, out.health, out_bad)) {
        return false;
    }
    if (out.health > out.max_health) {
        out.health = out.max_health;
    }
    if (!optional_float_in_range(line, "regen=", 0.0f, 100000.0f, out.regen_per_second, out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "regen_delay=", 0.0f, 3600.0f, out.regen_delay, out_bad)) {
        return false;
    }
    // Armor (Phase 50). `armor=` is the CURRENT pool and `armor_max=` the
    // denominator; an authored current without a maximum means a full vest, so
    // `armor=50` is the obvious thing an author expects. A current above its
    // maximum is clamped for the same reason health is.
    const bool has_armor = line.find("armor=") != std::string::npos;
    const bool has_armor_max = line.find("armor_max=") != std::string::npos;
    if (!optional_float_in_range(line, "armor=", 0.0f, 1000000.0f, out.armor, out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "armor_max=", 0.0f, 1000000.0f, out.max_armor, out_bad)) {
        return false;
    }
    if (has_armor && !has_armor_max) {
        out.max_armor = out.armor;
    } else if (has_armor_max && !has_armor) {
        // A scene that names only the ceiling means a full pool, the same way
        // `Health: max=250` without `health=` starts at 250.
        out.armor = out.max_armor;
    }
    if (out.max_armor <= 0.0f) {
        // An armor pool with no denominator is not a pool: a scene that wrote
        // only a current value gets its current value as the maximum above,
        // and a scene that wrote neither stays at zero (armor disabled).
        out.armor = 0.0f;
    } else if (out.armor > out.max_armor) {
        out.armor = out.max_armor;
    }
    if (!optional_float_in_range(line, "absorb=", 0.0f, 1.0f, out.armor_absorb, out_bad)) {
        return false;
    }
    out.hit_zones = line.find("zones=true") != std::string::npos;
    if (!optional_float_in_range(line, "head=", 0.001f, 100.0f, out.headshot_multiplier,
                                 out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "limb=", 0.0f, 100.0f, out.limb_multiplier, out_bad)) {
        return false;
    }
    // Per-type resistance (Phase 52). All three are optional and zero by
    // default, so a pre-Phase-52 health line keeps its exact damage.
    if (!optional_float_in_range(line, "resist_ballistic=", 0.0f, 1.0f,
                                 out.ballistic_resistance, out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "resist_explosive=", 0.0f, 1.0f,
                                 out.explosive_resistance, out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "resist_fire=", 0.0f, 1.0f, out.fire_resistance,
                                 out_bad)) {
        return false;
    }
    out.invulnerable = line.find("invulnerable=true") != std::string::npos;
    out.enabled = line.find("enabled=false") != std::string::npos;
    return true;
}

// One status entry: `burn=4,3,0.5` = dps, seconds, tick. A missing tick uses
// half a second. The whole entry is refused on a malformed list, so a scene
// cannot get a burn with no duration.
static bool parse_status_entry(const std::string& line, const char* key, StatusKind kind,
                               StatusEffectSet& out, std::string& out_bad) {
    std::string value = field_value(line, key);
    if (value.empty()) {
        return true;
    }
    for (char& c : value) {
        if (c == ',') {
            c = ' ';
        }
    }
    std::istringstream in(value);
    std::vector<f32> numbers;
    f32 number = 0.0f;
    while (in >> number) {
        numbers.push_back(number);
    }
    if (in.fail() && !in.eof()) {
        out_bad = key;
        return false;
    }
    if (numbers.size() < 2u || numbers.size() > 3u) {
        out_bad = key;
        return false;
    }
    const f32 tick = numbers.size() == 3u ? numbers[2] : 0.5f;
    const StatusEffect effect = make_status(kind, numbers[0], numbers[1], tick);
    if (!status_valid(effect) || !status_add(out, effect)) {
        out_bad = key;
        return false;
    }
    return true;
}

// `Status:` (Phase 54). The three effects are authored independently; a bad
// one rejects the whole line, exactly like a bad Health key.
static bool parse_status(const std::string& line, StatusEffectComponent& out,
                         std::string& out_bad) {
    if (!parse_status_entry(line, "burn=", StatusKind::Burning, out.effects, out_bad)) {
        return false;
    }
    if (!parse_status_entry(line, "bleed=", StatusKind::Bleeding, out.effects, out_bad)) {
        return false;
    }
    if (!parse_status_entry(line, "corrode=", StatusKind::Corroding, out.effects, out_bad)) {
        return false;
    }
    out.enabled = line.find("enabled=false") == std::string::npos;
    return true;
}

// A combat agent (Phase 45). Every key is optional except the target, which is
// free text and therefore written LAST — the same rule `follow=` follows, and
// for the same reason: a name with a space would otherwise be read as two
// fields.
static bool parse_combat_agent(const std::string& line, CombatAgentComponent& out,
                               std::string& out_bad) {
    if (!optional_float_in_range(line, "eye=", 0.0f, 10.0f, out.eye_height, out_bad)) return false;
    if (!optional_float_in_range(line, "aim=", 0.0f, 10.0f, out.target_height, out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "reaction=", 0.0f, 10.0f, out.config.reaction_seconds,
                                 out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "turn=", 0.0f, 3600.0f, out.config.aim_speed_deg_per_s,
                                 out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "cone=", 0.0f, 90.0f, out.config.fire_cone_deg,
                                 out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "range=", 0.1f, 10000.0f, out.config.max_range,
                                 out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "burst=", 0.0f, 60.0f, out.config.burst_on_seconds,
                                 out_bad)) {
        return false;
    }
    if (!optional_float_in_range(line, "rest=", 0.0f, 60.0f, out.config.burst_off_seconds,
                                 out_bad)) {
        return false;
    }
    out.target = field_value(line, "target=");
    out.auto_target = line.find("auto_target=true") != std::string::npos;
    out.retaliate = line.find("retaliate=true") != std::string::npos;
    out.enabled = line.find("enabled=false") == std::string::npos;
    // An agent with no target is legal (it stands there), but an agent whose
    // cone is empty can never fire, which is a scene bug worth naming.
    if (out.config.fire_cone_deg <= 0.0f) {
        out_bad = "cone";
        return false;
    }
    return true;
}

static bool parse_faction(const std::string& line, FactionComponent& out,
                          std::string& out_bad) {
    // `team=` is a token, not free text: a name with a space would make the
    // saved line ambiguous. It is written first and read with field_value,
    // which already stops at whitespace.
    out.team = field_value(line, "team=");
    if (out.team.size() > 64u) {
        out_bad = "team";
        return false;
    }
    out.friendly_fire = line.find("friendly_fire=true") != std::string::npos;
    out.enabled = line.find("enabled=false") == std::string::npos;
    return true;
}

static bool parse_vehicle(const std::string& line, physics::VehicleComponent& out,
                          std::string& out_bad) {
    // A drivable car: configuration only. throttle/steer/brake/handbrake are
    // live inputs and the JoltVehicle pointer is session state — none of the
    // five is parsed here, same rule as Character's wish_dir/jump.
    physics::JoltVehicleConfig& cfg = out.config;
    f32 cx = cfg.chassis_half_extents.x, cy = cfg.chassis_half_extents.y, cz = cfg.chassis_half_extents.z;
    if (field_vec3(line, "chassis", cx, cy, cz)) {
        if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(cz) ||
            cx <= 0.0f || cx > 10.0f || cy <= 0.0f || cy > 10.0f || cz <= 0.0f || cz > 10.0f) {
            out_bad = "chassis";
            return false;
        }
        cfg.chassis_half_extents = Vec3(cx, cy, cz);
    }
    if (!optional_float_in_range(line, "mass=", 1.0f, 100000.0f, cfg.chassis_mass, out_bad)) return false;
    if (!optional_float_in_range(line, "wheel_radius=", 0.05f, 2.0f, cfg.wheel_radius, out_bad)) return false;
    if (!optional_float_in_range(line, "wheel_width=", 0.05f, 2.0f, cfg.wheel_width, out_bad)) return false;
    if (!optional_float_in_range(line, "track=", 0.1f, 10.0f, cfg.track_half_width, out_bad)) return false;
    if (!optional_float_in_range(line, "wheelbase=", 0.1f, 20.0f, cfg.wheelbase_half_length, out_bad)) return false;
    // Attachment height is chassis-local and usually negative (wheels hang
    // below the body), so the range admits both signs.
    if (!optional_float_in_range(line, "wheel_y=", -5.0f, 5.0f, cfg.wheel_y, out_bad)) return false;
    if (!optional_float_in_range(line, "steer_deg=", 0.0f, 60.0f, cfg.max_steer_deg, out_bad)) return false;
    if (!optional_float_in_range(line, "torque=", 0.0f, 100000.0f, cfg.engine_max_torque, out_bad)) return false;
    if (!optional_float_in_range(line, "susp_min=", 0.01f, 5.0f, cfg.suspension_min_length, out_bad)) return false;
    if (!optional_float_in_range(line, "susp_max=", 0.01f, 5.0f, cfg.suspension_max_length, out_bad)) return false;
    // A minimum travel past the maximum is a spring that can never extend:
    // refuse the pair rather than simulate it.
    if (cfg.suspension_min_length > cfg.suspension_max_length) {
        out_bad = "susp_max";
        return false;
    }
    if (!optional_float_in_range(line, "susp_freq=", 0.1f, 20.0f, cfg.suspension_frequency_hz, out_bad)) return false;
    if (!optional_float_in_range(line, "susp_damping=", 0.0f, 5.0f, cfg.suspension_damping, out_bad)) return false;
    if (!optional_float_in_range(line, "tire_peak=", 0.0f, 5.0f, cfg.tire_peak_friction, out_bad)) return false;
    if (!optional_float_in_range(line, "tire_limit=", 0.0f, 5.0f, cfg.tire_limit_friction, out_bad)) return false;
    if (!optional_float_in_range(line, "wheel_inertia=", 0.01f, 100.0f, cfg.wheel_inertia, out_bad)) return false;
    if (!optional_float_in_range(line, "angular_damping=", 0.0f, 5.0f, cfg.angular_damping, out_bad)) return false;
    if (!optional_float_in_range(line, "brake_torque=", 0.0f, 50000.0f, cfg.max_brake_torque, out_bad)) return false;
    if (!optional_float_in_range(line, "handbrake_torque=", 0.0f, 100000.0f, cfg.max_handbrake_torque, out_bad)) return false;
    out.enabled = line.find("enabled=false") == std::string::npos;
    return true;
}

// Axis back to the single-letter form the loader reads. Only the three
// cardinal axes are writable; anything else collapses to Y, which matches the
// loader's own default and keeps save/load a fixed point.
static const char* axis_name(const Vec3& axis) {
    if (axis.x > 0.5f) return "x";
    if (axis.z > 0.5f) return "z";
    return "y";
}

// --- Module property encoding ----------------------------------------------
//
// A component line is a sequence of space-separated `key=value` tokens, and
// `field_value` stops at the first space. Module state is user data, so the
// values are escaped rather than assumed clean — without that, `label=hello
// world` would silently truncate to `hello` on the round trip.
//
// The encoding itself lives in NF/Gameplay/GameplayState.hpp, shared with the
// save system. A second copy here would be free to drift, and the failure mode
// of a drifted escape is silent data corruption rather than an error.

SceneLoadResult load_scene_from_physical(const std::filesystem::path& physical_path) {
    SceneLoadResult result;
    std::ifstream in(physical_path, std::ios::binary);
    if (!in) { result.error = "Failed to open scene file: " + physical_path.string(); return result; }
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return load_scene_from_text(content);
}

SceneLoadResult load_scene_from_text(std::string_view text) {
    SceneLoadResult result;
    std::string content(text);
    std::istringstream iss(content);
    std::string line;
    if (!std::getline(iss, line) || line.rfind("# SANAD Scene",0)!=0) { result.error = "Invalid scene header"; return result; }
    if (!std::getline(iss, line)) { result.error="Missing version"; return result; }
    {
        auto pos = line.find(":");
        if (pos==std::string::npos) { result.error="Invalid version line"; return result; }
        std::string vstr = trim(line.substr(pos+1));
        int version = 0;
        try { version = std::stoi(vstr); } catch(...) { result.error="Invalid version number"; return result; }
        if (version != 1) { result.error = "Unsupported scene version: " + vstr + " (expected 1)"; return result; }
    }
    std::string scene_name;
    if (!std::getline(iss, line)) { result.error="Missing name"; return result; }
    {
        auto pos = line.find(":");
        if (pos!=std::string::npos) scene_name = trim(line.substr(pos+1));
    }
    if (!std::getline(iss, line)) { result.error="Missing entity_count"; return result; }
    size_t entity_count=0;
    {
        auto pos = line.find(":");
        if (pos==std::string::npos) { result.error="Invalid entity_count"; return result; }
        try { entity_count = static_cast<size_t>(std::stoul(trim(line.substr(pos+1)))); } catch(...) { result.error="Invalid entity_count number"; return result; }
    }
    auto scene = std::make_unique<scene::Scene>(scene_name);
    scene->metadata().version = "1";
    for (size_t i=0;i<entity_count;++i){
        if (!std::getline(iss, line) || trim(line) != "---") { result.error="Expected '---' separator"; return result; }
        if (!std::getline(iss, line) || line.rfind("entity:",0)!=0) { result.error="Expected entity line"; return result; }
        std::string rest = trim(line.substr(7));
        size_t colon = rest.find(":");
        if (colon==std::string::npos) { result.error="Invalid entity line"; return result; }
        ecs::Entity e = scene->world().create_entity();
        std::streampos next_pos;
        while (true) {
            next_pos = iss.tellg();
            if (!std::getline(iss, line)) break;
            std::string trimmed = trim(line);
            if (trimmed.empty()) continue;
            if (trimmed=="---" || trimmed.rfind("entity:",0)==0) { iss.seekg(next_pos); break; }
            if (line.rfind("  Name:",0)==0) {
                std::string nm = trim(line.substr(7));
                if (!nm.empty()) scene->world().add<scene::NameComponent>(e, scene::NameComponent{nm});
            } else if (line.rfind("  Prefab:",0)==0) {
                size_t pp = line.find("path=");
                if (pp != std::string::npos) {
                    std::string p = trim(line.substr(pp + 5));
                    if (!p.empty()) {
                        scene->world().add<scene::PrefabLinkComponent>(
                            e, scene::PrefabLinkComponent{p});
                    }
                }
            } else if (line.rfind("  Transform:",0)==0) {
                float lx=0,ly=0,lz=0, wx=0,wy=0,wz=0;
                int pid=-1, pgen=0;
                // Parent is located by substring: the line may carry optional
                // rot()/scale() segments between world() and parent().
                sscanf(line.c_str(), "  Transform: local(%f,%f,%f) world(%f,%f,%f)", &lx,&ly,&lz,&wx,&wy,&wz);
                size_t ppos = line.find("parent(");
                if (ppos != std::string::npos) {
                    sscanf(line.c_str()+ppos, "parent(%d:%d)", &pid, &pgen);
                }
                auto& trans = scene->world().add<scene::Transform>(e);
                trans.local_x=lx; trans.local_y=ly; trans.local_z=lz;
                trans.world_x=wx; trans.world_y=wy; trans.world_z=wz;
                size_t rpos = line.find("rot(");
                if (rpos != std::string::npos) {
                    float rx=0,ry=0,rz=0;
                    if (sscanf(line.c_str()+rpos, "rot(%f,%f,%f)", &rx,&ry,&rz)==3) {
                        trans.rot_x=rx; trans.rot_y=ry; trans.rot_z=rz;
                    }
                }
                size_t spos = line.find("scale(");
                if (spos != std::string::npos) {
                    float sx=1,sy=1,sz=1;
                    if (sscanf(line.c_str()+spos, "scale(%f,%f,%f)", &sx,&sy,&sz)==3) {
                        if (sx != 0.0f && sy != 0.0f && sz != 0.0f) {
                            trans.scale_x=sx; trans.scale_y=sy; trans.scale_z=sz;
                        }
                    }
                }
                if (pid>=0 && pid != static_cast<int>(0xFFFFFFFF)) {
                    trans.parent = ecs::Entity{static_cast<u32>(pid), static_cast<u32>(pgen)};
                    if (!scene->world().is_alive(trans.parent)) {
                        result.warnings.push_back("Parent entity " + std::to_string(pid) + " not found for entity " + std::to_string(e.id));
                        trans.parent = ecs::kInvalidEntity;
                    }
                }
            } else if (line.rfind("  Mesh:",0)==0) {
                size_t id_pos = line.find("asset_id=");
                size_t mat_pos0 = line.find("material=");
                if (id_pos == std::string::npos && mat_pos0 != std::string::npos) {
                    // A material without a mesh. A rigged character gets its
                    // geometry from the rig, and the material is a separate
                    // thing: refusing the line would leave the character with
                    // the engine's grey fallback.
                    MeshComponent comp;
                    comp.material = trim(line.substr(mat_pos0 + 9));
                    scene->world().add<MeshComponent>(e, comp);
                } else if (id_pos != std::string::npos) {
                    size_t start = id_pos+9;
                    size_t end = line.find(" ", start);
                    std::string id_str = trim(line.substr(start, end-start));
                    auto uuid = UUID::from_string(id_str);
                    if (!uuid.is_valid()) {
                        result.warnings.push_back("Invalid AssetId for Mesh: " + id_str);
                        result.missing_assets.push_back(id_str);
                    } else {
                        MeshComponent comp;
                        comp.mesh_id = assets::AssetId(uuid);
                        size_t mat_pos = line.find("material=");
                        if (mat_pos != std::string::npos) comp.material = trim(line.substr(mat_pos+9));
                        scene->world().add<MeshComponent>(e, comp);
                    }
                }
            } else if (line.rfind("  Decal:",0)==0) {
                // Projected decal (Phase 36). Placement and orientation come
                // from the entity Transform, so the line carries only the
                // projection extents, the material and the blend trim. Every
                // key is optional; a present-but-impossible value rejects the
                // line so the author sees a warning instead of an invisible or
                // NaN-sized decal.
                DecalComponent decal;
                decal.material = field_value(line, "material=");
                bool ok = true;
                std::string decal_bad;
                f32 sx = decal.size_x, sy = decal.size_y, sz = decal.size_z;
                if (field_vec3(line, "size", sx, sy, sz)) {
                    ok = finite3(sx, sy, sz) &&
                         sx >= 0.001f && sx <= 1000.0f &&
                         sy >= 0.001f && sy <= 1000.0f &&
                         sz >= 0.001f && sz <= 1000.0f;
                    // Name the key. The warning quotes `decal_bad`, and an
                    // empty name reads as "invalid ''" - the author would have
                    // to guess which value was refused.
                    if (!ok) {
                        decal_bad = "size";
                    }
                    decal.size_x = sx;
                    decal.size_y = sy;
                    decal.size_z = sz;
                }
                if (ok) {
                    ok = optional_float_in_range(line, "opacity=", 0.0f, 1.0f, decal.opacity,
                                                 decal_bad);
                }
                decal.enabled = line.find("enabled=false") == std::string::npos;
                if (!ok) {
                    result.warnings.push_back("Decal line with invalid '" + decal_bad +
                                              "' was ignored: " + line);
                } else {
                    scene->world().add<DecalComponent>(e, std::move(decal));
                }
            } else if (line.rfind("  Light:",0)==0) {
                // `type=` decides which light the numbers describe. Three shapes
                // share one line prefix because the format's readers are humans,
                // and a human writing a lighting block wants the sun, the torch
                // and the searchlight in one place — splitting them into
                // `PointLight:`/`SpotLight:` lines would add two keywords for no
                // gain and lose the chance to reject a Light line that asks to be
                // two things at once.
                //
                // The Directional branch below is deliberately untouched, key for
                // key: scenes written three phases ago must come back with the
                // numbers they went in with, and a Light line with no `type=` is
                // the sun, exactly as it was before the word meant anything.
                // An unrecognised type is refused rather than quietly promoted to
                // a directional light — a scene whose `type=Area` loads as a
                // bright outdoor sun is wrong in a way the author will blame on
                // the renderer.
                const std::string light_type = field_value(line, "type=");
                if (light_type == "Point") {
                    PointLightComponent pl;
                    std::string bad;
                    if (parse_point_light(line, pl, bad)) {
                        scene->world().add<PointLightComponent>(e, pl);
                    } else {
                        result.warnings.push_back("Light line with invalid " + bad +
                                                  " was ignored: " + line);
                    }
                } else if (light_type == "Spot") {
                    SpotLightComponent sl;
                    std::string bad;
                    if (parse_spot_light(line, sl, bad)) {
                        scene->world().add<SpotLightComponent>(e, sl);
                    } else {
                        result.warnings.push_back("Light line with invalid " + bad +
                                                  " was ignored: " + line);
                    }
                } else if (!light_type.empty() && light_type != "Directional") {
                    result.warnings.push_back("Light line with unknown type was ignored: " + line);
                } else {
                    DirectionalLight light;
                    size_t dir_pos = line.find("dir(");
                    if (dir_pos != std::string::npos) sscanf(line.c_str()+dir_pos, "dir(%f,%f,%f)", &light.dir_x, &light.dir_y, &light.dir_z);
                    size_t col_pos = line.find("color(");
                    if (col_pos != std::string::npos) sscanf(line.c_str()+col_pos, "color(%f,%f,%f)", &light.color_r, &light.color_g, &light.color_b);
                    size_t int_pos = line.find("intensity=");
                    if (int_pos != std::string::npos) sscanf(line.c_str()+int_pos, "intensity=%f", &light.intensity);
                    // Absent key keeps the default (true): old scene files load lit as before.
                    if (line.find("shadows=false") != std::string::npos) light.cast_shadows = false;
                    size_t ss_pos = line.find("shadow_strength=");
                    if (ss_pos != std::string::npos) sscanf(line.c_str()+ss_pos, "shadow_strength=%f", &light.shadow_strength);
                    size_t sb_pos = line.find("shadow_bias=");
                    if (sb_pos != std::string::npos) sscanf(line.c_str()+sb_pos, "shadow_bias=%f", &light.shadow_bias);
                    size_t sc_pos = line.find("shadow_cascades=");
                    if (sc_pos != std::string::npos) {
                        // Scanned into an unsigned rather than a u32 directly: %u's
                        // argument type is fixed by the C standard, and u32 is only
                        // incidentally unsigned on this platform.
                        unsigned cascades = light.shadow_cascades;
                        sscanf(line.c_str()+sc_pos, "shadow_cascades=%u", &cascades);
                        light.shadow_cascades = cascades;
                    }
                    size_t sd_pos = line.find("shadow_distance=");
                    if (sd_pos != std::string::npos) sscanf(line.c_str()+sd_pos, "shadow_distance=%f", &light.shadow_distance);
                    scene->world().add<DirectionalLight>(e, light);
                }
            } else if (line.rfind("  Sky:",0)==0) {
                SkyComponent sky;
                size_t zp = line.find("zenith(");
                if (zp != std::string::npos) sscanf(line.c_str()+zp, "zenith(%f,%f,%f)", &sky.zenith_r, &sky.zenith_g, &sky.zenith_b);
                size_t hp = line.find("horizon(");
                if (hp != std::string::npos) sscanf(line.c_str()+hp, "horizon(%f,%f,%f)", &sky.horizon_r, &sky.horizon_g, &sky.horizon_b);
                size_t gp = line.find("ground(");
                if (gp != std::string::npos) sscanf(line.c_str()+gp, "ground(%f,%f,%f)", &sky.ground_r, &sky.ground_g, &sky.ground_b);
                size_t cp = line.find("clear(");
                if (cp != std::string::npos) sscanf(line.c_str()+cp, "clear(%f,%f,%f)", &sky.clear_r, &sky.clear_g, &sky.clear_b);
                size_t dp = line.find("sun_disk=");
                if (dp != std::string::npos) sscanf(line.c_str()+dp, "sun_disk=%f", &sky.sun_disk);
                size_t glp = line.find("sun_glow=");
                if (glp != std::string::npos) sscanf(line.c_str()+glp, "sun_glow=%f", &sky.sun_glow);
                // Absent key keeps the default (true): old scene files keep the sky.
                if (line.find("enabled=false") != std::string::npos) sky.enabled = false;
                scene->world().add<SkyComponent>(e, sky);
            } else if (line.rfind("  TimeOfDay:",0)==0) {
                // Day/night clock. Every key is optional and an absent one keeps
                // the component default, so a scene can carry just `hours=` for a
                // frozen golden hour and nothing else.
                TimeOfDayComponent tod;
                if (field_float(line, "hours=", tod.time_hours) &&
                    std::isfinite(tod.time_hours)) {
                    // Wrap rather than reject: 25.0 is a natural way to write 1am,
                    // and -6.0 is how someone writes "6pm yesterday". Refusing the
                    // whole scene over either would be the bigger surprise. A
                    // non-finite value is a different matter — it means the file is
                    // broken, and silently keeping the default would hide it.
                    tod.time_hours = std::fmod(tod.time_hours, 24.0f);
                    if (tod.time_hours < 0.0f) tod.time_hours += 24.0f;
                } else if (line.find("hours=") != std::string::npos) {
                    result.warnings.push_back("TimeOfDay line with non-finite hours was ignored: " + line);
                }
                // A non-positive or non-finite day length means "frozen", which
                // TimeOfDay::advance already defines as a no-op. Normalising here
                // keeps an absurd value (1e30) from becoming a division by itself
                // somewhere downstream.
                if (field_float(line, "day_length=", tod.day_length_seconds) &&
                    std::isfinite(tod.day_length_seconds)) {
                    if (tod.day_length_seconds < 0.0f) tod.day_length_seconds = 0.0f;
                } else if (line.find("day_length=") != std::string::npos) {
                    result.warnings.push_back("TimeOfDay line with non-finite day_length was ignored: " + line);
                }
                tod.enabled = line.find("enabled=false") == std::string::npos;
                tod.drive_light = line.find("drive_light=false") == std::string::npos;
                scene->world().add<TimeOfDayComponent>(e, tod);
            } else if (line.rfind("  PostProcess:",0)==0) {
                PostProcessComponent pp;
                std::string bad;
                if (parse_post_process(line, pp, bad)) {
                    scene->world().add<PostProcessComponent>(e, pp);
                } else {
                    result.warnings.push_back("PostProcess line with invalid " + bad +
                                              " was ignored: " + line);
                }
            } else if (line.rfind("  Feedback:",0)==0) {
                // Combat feedback (Phase 56). Absent means "draw nothing new";
                // the line opts the scene in. A bad value rejects the line by
                // name, the same rule `Decal:` and `PostProcess:` follow.
                FeedbackComponent fb;
                std::string bad;
                if (parse_feedback(line, fb, bad)) {
                    scene->world().add<FeedbackComponent>(e, fb);
                } else {
                    result.warnings.push_back("Feedback line with invalid " + bad +
                                              " was ignored: " + line);
                }
            } else if (line.rfind("  Perception:",0)==0) {
                // Perception (Phase 60). Absent means "this entity has no
                // senses", so every scene written before this line existed keeps
                // exactly the combat behaviour it had. A bad value rejects the
                // line by name.
                PerceptionComponent perc;
                std::string bad;
                if (parse_perception(line, perc, bad)) {
                    scene->world().add<PerceptionComponent>(e, perc);
                } else {
                    result.warnings.push_back("Perception line with invalid " + bad +
                                              " was ignored: " + line);
                }
            } else if (line.rfind("  Brain:",0)==0) {
                // The decision maker (Phase 61). If actions were already added by
                // `BrainAction:` lines above this one, the declared rate and
                // enabled flag update in place: the line order must not silently
                // drop a list of considerations.
                BrainComponent brain;
                std::string bad;
                if (parse_brain(line, brain, bad)) {
                    if (auto* existing = scene->world().get<BrainComponent>(e)) {
                        existing->rate = brain.rate;
                        existing->enabled = brain.enabled;
                    } else {
                        scene->world().add<BrainComponent>(e, std::move(brain));
                    }
                } else {
                    result.warnings.push_back("Brain line with invalid " + bad +
                                              " was ignored: " + line);
                }
            } else if (line.rfind("  BrainAction:",0)==0) {
                BrainAction action;
                std::string bad;
                if (parse_brain_action(line, action, bad)) {
                    auto* brain = scene->world().get<BrainComponent>(e);
                    if (brain == nullptr) {
                        // A consideration without a brain: the line IS the brain's
                        // action list, so it creates one with the defaults rather
                        // than dropping the author's work on line order.
                        scene->world().add<BrainComponent>(e, BrainComponent{});
                        brain = scene->world().get<BrainComponent>(e);
                    }
                    if (brain != nullptr) {
                        brain->actions.push_back(std::move(action));
                    }
                } else {
                    result.warnings.push_back("BrainAction line with invalid " + bad +
                                              " was ignored: " + line);
                }
            } else if (line.rfind("  States:",0)==0) {
                // The machine (Phase 62). Like the brain line, it updates in
                // place when its states were already declared above it: the line
                // order must not drop a list of states.
                StateMachineComponent machine;
                std::string bad;
                if (parse_states(line, machine, bad)) {
                    if (auto* existing = scene->world().get<StateMachineComponent>(e)) {
                        existing->rate = machine.rate;
                        existing->enabled = machine.enabled;
                        if (!machine.initial.empty()) {
                            existing->initial = machine.initial;
                        }
                    } else {
                        scene->world().add<StateMachineComponent>(e, std::move(machine));
                    }
                } else {
                    result.warnings.push_back("States line with invalid " + bad +
                                              " was ignored: " + line);
                }
            } else if (line.rfind("  State:",0)==0) {
                StateSpec state;
                std::string bad;
                if (parse_state(line, state, bad)) {
                    auto* machine = scene->world().get<StateMachineComponent>(e);
                    if (machine == nullptr) {
                        scene->world().add<StateMachineComponent>(e, StateMachineComponent{});
                        machine = scene->world().get<StateMachineComponent>(e);
                    }
                    if (machine != nullptr) {
                        machine->states.push_back(std::move(state));
                    }
                } else {
                    result.warnings.push_back("State line with invalid " + bad +
                                              " was ignored: " + line);
                }
            } else if (line.rfind("  Transition:",0)==0) {
                TransitionSpec transition;
                std::string bad;
                if (parse_transition(line, transition, bad)) {
                    auto* machine = scene->world().get<StateMachineComponent>(e);
                    if (machine == nullptr) {
                        scene->world().add<StateMachineComponent>(e, StateMachineComponent{});
                        machine = scene->world().get<StateMachineComponent>(e);
                    }
                    if (machine != nullptr) {
                        machine->transitions.push_back(std::move(transition));
                    }
                } else {
                    result.warnings.push_back("Transition line with invalid " + bad +
                                              " was ignored: " + line);
                }
            } else if (line.rfind("  Camera:",0)==0) {
                CameraComponent cam;
                size_t fov_pos = line.find("fov=");
                if (fov_pos != std::string::npos) sscanf(line.c_str()+fov_pos, "fov=%f", &cam.fov_y);
                size_t asp_pos = line.find("aspect=");
                if (asp_pos != std::string::npos) sscanf(line.c_str()+asp_pos, "aspect=%f", &cam.aspect);
                size_t near_pos = line.find("near=");
                if (near_pos != std::string::npos) sscanf(line.c_str()+near_pos, "near=%f", &cam.near_plane);
                size_t far_pos = line.find("far=");
                if (far_pos != std::string::npos) sscanf(line.c_str()+far_pos, "far=%f", &cam.far_plane);
                cam.is_active = line.find("active=true") != std::string::npos;
                // The follow camera. Every field is optional and a missing one
                // leaves the default in place, so a scene written before the
                // follow camera existed loads exactly as it did: a camera that
                // looks at the origin.
                size_t follow_pos = line.find("follow=");
                if (follow_pos != std::string::npos) {
                    // The name runs to the next space, so a name with no
                    // spaces is the whole value.
                    const std::string follow_value = line.substr(follow_pos + 7);
                    const size_t end = follow_value.find(' ');
                    cam.follow_target = (end == std::string::npos) ? follow_value
                                                                  : follow_value.substr(0, end);
                }
                auto scan_vec3 = [&](const char* prefix, float& x, float& y, float& z) {
                    const size_t pos = line.find(prefix);
                    if (pos == std::string::npos) return;
                    sscanf(line.c_str() + pos, "%*[^=]=%f,%f,%f", &x, &y, &z);
                };
                scan_vec3("pivot=", cam.pivot_x, cam.pivot_y, cam.pivot_z);
                // A key is only found when it stands on its own: `distance=`
                // must not match inside `ads_distance=`, which would silently
                // read the sights distance as the base one. The old plain
                // find() worked while every key was unique as a PREFIX; the
                // ADS keys broke that assumption, so the lookup states it.
                auto find_key = [&line](const char* key) -> size_t {
                    size_t pos = line.find(key);
                    while (pos != std::string::npos) {
                        const bool attached =
                            pos > 0 && (std::isalnum(static_cast<unsigned char>(line[pos - 1])) != 0 ||
                                        line[pos - 1] == '_');
                        if (!attached) {
                            return pos;
                        }
                        pos = line.find(key, pos + 1);
                    }
                    return std::string::npos;
                };
                size_t dist_pos = find_key("distance=");
                if (dist_pos != std::string::npos) sscanf(line.c_str()+dist_pos, "distance=%f", &cam.distance);
                size_t yaw_pos = line.find("yaw=");
                if (yaw_pos != std::string::npos) sscanf(line.c_str()+yaw_pos, "yaw=%f", &cam.yaw);
                size_t pitch_pos = line.find("pitch=");
                if (pitch_pos != std::string::npos) sscanf(line.c_str()+pitch_pos, "pitch=%f", &cam.pitch);
                size_t smooth_pos = line.find("smoothing=");
                if (smooth_pos != std::string::npos) sscanf(line.c_str()+smooth_pos, "smoothing=%f", &cam.smoothing);
                size_t radius_pos = line.find("radius=");
                if (radius_pos != std::string::npos) sscanf(line.c_str()+radius_pos, "radius=%f", &cam.collision_radius);
                cam.avoid_collision = line.find("avoid=0") == std::string::npos;
                // Aiming the camera. Every key is optional and a missing one
                // keeps the default, so a scene written before the camera
                // could be turned loads exactly as it did.
                if (line.find("mouse=0") != std::string::npos) cam.mouse_look = false;
                size_t look_sens_pos = line.find("looksens=");
                if (look_sens_pos != std::string::npos) sscanf(line.c_str()+look_sens_pos, "looksens=%f", &cam.look_sensitivity);
                size_t stick_sens_pos = line.find("sticksens=");
                if (stick_sens_pos != std::string::npos) sscanf(line.c_str()+stick_sens_pos, "sticksens=%f", &cam.stick_sensitivity);
                size_t pitch_min_pos = line.find("pitchmin=");
                if (pitch_min_pos != std::string::npos) sscanf(line.c_str()+pitch_min_pos, "pitchmin=%f", &cam.pitch_min);
                size_t pitch_max_pos = line.find("pitchmax=");
                if (pitch_max_pos != std::string::npos) sscanf(line.c_str()+pitch_max_pos, "pitchmax=%f", &cam.pitch_max);
                if (line.find("invert_y=1") != std::string::npos) cam.invert_look_y = true;
                size_t zoom_min_pos = line.find("zoommin=");
                if (zoom_min_pos != std::string::npos) sscanf(line.c_str()+zoom_min_pos, "zoommin=%f", &cam.zoom_min);
                size_t zoom_max_pos = line.find("zoommax=");
                if (zoom_max_pos != std::string::npos) sscanf(line.c_str()+zoom_max_pos, "zoommax=%f", &cam.zoom_max);
                size_t zoom_step_pos = line.find("zoomstep=");
                if (zoom_step_pos != std::string::npos) sscanf(line.c_str()+zoom_step_pos, "zoomstep=%f", &cam.zoom_step);
                size_t align_pos = line.find("align=");
                if (align_pos != std::string::npos) sscanf(line.c_str()+align_pos, "align=%f", &cam.align_speed);
                // Aiming down sights and shake trim. Same contract as every
                // other key on this line: absent means the default, so a
                // scene authored before they existed loads unchanged.
                if (line.find("ads=1") != std::string::npos) cam.ads_enabled = true;
                size_t ads_dist_pos = find_key("ads_distance=");
                if (ads_dist_pos != std::string::npos) sscanf(line.c_str()+ads_dist_pos, "ads_distance=%f", &cam.ads_distance);
                size_t ads_sens_pos = find_key("ads_sens=");
                if (ads_sens_pos != std::string::npos) sscanf(line.c_str()+ads_sens_pos, "ads_sens=%f", &cam.ads_sensitivity);
                size_t ads_speed_pos = find_key("ads_speed=");
                if (ads_speed_pos != std::string::npos) sscanf(line.c_str()+ads_speed_pos, "ads_speed=%f", &cam.ads_speed);
                size_t shake_pos = find_key("shake=");
                if (shake_pos != std::string::npos) sscanf(line.c_str()+shake_pos, "shake=%f", &cam.shake_strength);
                size_t shake_decay_pos = find_key("shake_decay=");
                if (shake_decay_pos != std::string::npos) sscanf(line.c_str()+shake_decay_pos, "shake_decay=%f", &cam.shake_decay);
                size_t recoil_pos = find_key("recoil=");
                if (recoil_pos != std::string::npos) sscanf(line.c_str()+recoil_pos, "recoil=%f", &cam.recoil_recover);
                size_t recoil_max_pos = find_key("recoil_max=");
                if (recoil_max_pos != std::string::npos) sscanf(line.c_str()+recoil_max_pos, "recoil_max=%f", &cam.recoil_max);
                scene->world().add<CameraComponent>(e, cam);
            } else if (line.rfind("  RigidBody:",0)==0) {
                physics::RigidBodyComponent rb;
                if (line.find("type=Static") != std::string::npos) {
                    rb.type = physics::BodyType::Static;
                } else if (line.find("type=Kinematic") != std::string::npos) {
                    rb.type = physics::BodyType::Kinematic;
                }
                // Parsed by substring rather than one big sscanf: the fields are
                // all optional and a missing one must leave the default in place,
                // not fail the whole line.  We search for the key prefix (e.g.
                // "mass=") and then sscanf the value from that offset — the format
                // string contains the prefix so sscanf matches it literally.
                auto scan = [&](const char* prefix, f32& out) {
                    const size_t p = line.find(prefix);
                    if (p != std::string::npos) {
                        sscanf(line.c_str() + p + std::strlen(prefix), "%f", &out);
                    }
                };
                scan("mass=", rb.mass);
                scan("friction=", rb.friction);
                scan("restitution=", rb.restitution);
                scan("linear_damping=", rb.linear_damping);
                scan("angular_damping=", rb.angular_damping);
                rb.allow_sleep = line.find("allow_sleep=false") == std::string::npos;
                scene->world().add<physics::RigidBodyComponent>(e, rb);
            } else if (line.rfind("  Collider:",0)==0) {
                physics::ColliderComponent col;
                if (line.find("shape=Box") != std::string::npos) {
                    float hx=0.5f, hy=0.5f, hz=0.5f;
                    const size_t hp = line.find("half(");
                    if (hp != std::string::npos) {
                        sscanf(line.c_str()+hp, "half(%f,%f,%f)", &hx,&hy,&hz);
                    }
                    col.shape = physics::Shape::make_box(Vec3(hx,hy,hz));
                } else if (line.find("shape=Plane") != std::string::npos) {
                    float nx=0.0f, ny=1.0f, nz=0.0f;
                    const size_t np = line.find("normal(");
                    if (np != std::string::npos) {
                        sscanf(line.c_str()+np, "normal(%f,%f,%f)", &nx,&ny,&nz);
                    }
                    col.shape = physics::Shape::make_plane(Vec3(nx,ny,nz));
                } else { // Sphere is the default shape
                    float r = 0.5f;
                    const size_t rp = line.find("radius=");
                    if (rp != std::string::npos) {
                        sscanf(line.c_str()+rp, "radius=%f", &r);
                    }
                    col.shape = physics::Shape::make_sphere(r);
                }
                scene->world().add<physics::ColliderComponent>(e, col);
            } else if (line.rfind("  Destructible:",0)==0) {
                // Phase 19. The fracture asset is cooked from this spec at
                // load, so every field here is a build knob rather than a
                // reference to something on disk — there is no fracture-asset
                // import pipeline, exactly as there is no animation or audio
                // one. Missing keys keep the defaults, the same as every other
                // component line.
                DestructibleComponent d;
                unsigned chunks = d.chunks;
                unsigned seed = d.seed;
                size_t cp = line.find("chunks=");
                if (cp != std::string::npos) sscanf(line.c_str()+cp, "chunks=%u", &chunks);
                size_t sp = line.find("seed=");
                if (sp != std::string::npos) sscanf(line.c_str()+sp, "seed=%u", &seed);
                d.chunks = chunks;
                d.seed = seed;
                (void)field_float(line, "strength=", d.strength);
                (void)field_float(line, "damage_threshold=", d.damage_threshold);
                (void)field_float(line, "blast_radius=", d.blast_radius);
                d.enabled = line.find("enabled=false") == std::string::npos;
                scene->world().add<DestructibleComponent>(e, d);
            } else if (line.rfind("  CharacterRig:",0)==0) {
                // The RIG: a glTF that carries the mesh, the skin and the
                // clips. The file is the only field -- everything else (the
                // skeleton, the clips, the locomotion) comes from it.
                //
                // Its own line, not "Character:": that one is the physics
                // character controller, and a scene may carry both (the
                // controller moves, the rig draws).
                CharacterComponent rigged;
                rigged.source = field_value(line, "file=");
                scene->world().add<CharacterComponent>(e, rigged);
            } else if (line.rfind("  Animation:",0)==0) {
                animation::AnimationComponent anim;

                const std::string clip_name = field_value(line, "clip=");
                if (!clip_name.empty()) {
                    anim.player.set_clip(clip_name);
                }

                // `procedural=` names a clip the engine builds itself. Without
                // it a clip name is only a label: the import pipeline is an
                // explicit Phase 9 non-goal (§7), so there is nothing on disk to
                // load and the entity would sample the rest pose forever.
                const std::string procedural = field_value(line, "procedural=");
                if (!procedural.empty() && !clip_name.empty()) {
                    animation::ProceduralClipSpec spec;
                    spec.kind = (procedural == "bob") ? animation::ProceduralClipSpec::Kind::Bob
                                                      : animation::ProceduralClipSpec::Kind::Spin;
                    const std::string axis = field_value(line, "axis=");
                    if (axis == "x") {
                        spec.axis = {1.0f, 0.0f, 0.0f};
                    } else if (axis == "z") {
                        spec.axis = {0.0f, 0.0f, 1.0f};
                    } else {
                        spec.axis = {0.0f, 1.0f, 0.0f};
                    }
                    (void)field_float(line, "turns=", spec.turns);
                    (void)field_float(line, "amplitude=", spec.amplitude);
                    (void)field_float(line, "duration=", spec.duration);

                    // No skeleton import pipeline either, so a procedural clip
                    // gets the minimal single-bone rig. Logged rather than
                    // silent: a real rig would give a different result, and a
                    // substitution the user cannot see is the kind of gap this
                    // project keeps having to re-find.
                    if (anim.skeleton.bones.empty()) {
                        anim.skeleton = animation::make_default_skeleton();
                        NF_LOG_INFO(LogCategory::Core,
                                    "Runtime: entity {} has no skeleton data; using a single-bone root rig for procedural clip '{}'",
                                    e.id, clip_name);
                    }

                    anim.clips[clip_name] =
                        animation::make_procedural_clip(clip_name, anim.skeleton, spec);
                    anim.has_procedural = true;
                    anim.procedural = spec;
                    anim.procedural_clip_name = clip_name;
                } else if (!clip_name.empty()) {
                    result.warnings.push_back(
                        "Animation clip '" + clip_name +
                        "' has no data (no procedural= spec, and there is no animation import pipeline yet); the entity will not move");
                }

                (void)field_float(line, "speed=", anim.speed);
                anim.player.set_speed(anim.speed);

                const std::string loop = field_value(line, "loop=");
                if (loop == "none") {
                    anim.player.set_loop_mode(animation::LoopMode::None);
                } else if (loop == "pingpong") {
                    anim.player.set_loop_mode(animation::LoopMode::PingPong);
                } else {
                    anim.player.set_loop_mode(animation::LoopMode::Loop);
                }

                anim.paused = line.find("paused=true") != std::string::npos;
                anim.use_state_machine = line.find("state_machine=true") != std::string::npos;
                // An AnimationComponent in a scene plays unless it says it is
                // paused. AnimationPlayer::update() returns the current time
                // unchanged unless the state is Playing, so without this a
                // loaded scene would sit at t = 0 forever and the entity would
                // look static even with a valid clip.
                if (!anim.paused) {
                    anim.player.play();
                }
                scene->world().add<animation::AnimationComponent>(e, std::move(anim));
            } else if (line.rfind("  Audio:",0)==0) {
                audio::AudioComponent aud;
                aud.buffer_name = field_value(line, "buffer=");

                // Same reasoning as the animation clip above: `tone=` is the
                // quick procedural path. A `buffer=` logical path (e.g.
                // content://Audio/shoot.wav) resolves through the VFS import
                // pipeline after load (see resolve_scene_audio below).
                f32 tone_hz = 0.0f;
                if (field_float(line, "tone=", tone_hz) && tone_hz > 0.0f) {
                    f32 tone_seconds = 0.5f;
                    (void)field_float(line, "tone_duration=", tone_seconds);
                    aud.owned_buffer =
                        audio::make_tone_buffer(tone_hz, tone_seconds, audio::kDefaultSampleRate, 1);
                    aud.tone_hz = tone_hz;
                    aud.tone_duration = tone_seconds;
                }

                (void)field_float(line, "volume=", aud.volume);
                (void)field_float(line, "pitch=", aud.pitch);
                aud.looping = line.find("looping=true") != std::string::npos;
                aud.spatial = line.find("spatial=true") != std::string::npos;
                aud.autoplay = line.find("autoplay=true") != std::string::npos;
                // Routing: which Settings slider owns this source, and whether
                // walls muffle it. An unknown bus name is reported rather than
                // silently defaulting, so a typo cannot route a whole level's
                // dialogue into the world mix.
                {
                    audio::BusId bus_id = aud.bus;
                    if (audio::bus_id_from_name(field_value(line, "bus="), bus_id)) {
                        aud.bus = bus_id;
                    } else if (line.find("bus=") != std::string::npos) {
                        result.warnings.push_back("Audio line with unknown bus was ignored: " + line);
                    }
                }
                aud.occluded = line.find("occluded=false") == std::string::npos;
                // Attenuation range: inspector-editable data the writer below
                // always persists, so a tuned falloff survives a save/load.
                // Absent or non-positive keys keep the defaults (old files).
                f32 min_dist = aud.spatial_settings.min_distance;
                if (field_float(line, "min_distance=", min_dist) && std::isfinite(min_dist) &&
                    min_dist > 0.0f) {
                    aud.spatial_settings.min_distance = min_dist;
                }
                f32 max_dist = aud.spatial_settings.max_distance;
                if (field_float(line, "max_distance=", max_dist) && std::isfinite(max_dist) &&
                    max_dist > 0.0f) {
                    aud.spatial_settings.max_distance = max_dist;
                }
                scene->world().add<audio::AudioComponent>(e, std::move(aud));
            } else if (line.rfind("  ReverbZone:",0)==0) {
                // A cave, a tunnel, a hall. The zone's position comes from the
                // entity's transform at adopt time, not from this line, so the
                // author places the echo by placing the object.
                audio::ReverbZoneComponent rz;
                (void)field_float(line, "radius=", rz.radius);
                (void)field_float(line, "inner=", rz.inner_radius);
                (void)field_float(line, "wet=", rz.wet_gain);
                (void)field_float(line, "decay=", rz.decay_seconds);
                (void)field_float(line, "predelay=", rz.pre_delay_seconds);
                (void)field_float(line, "spacing=", rz.echo_spacing_seconds);
                rz.enabled = line.find("enabled=false") == std::string::npos;
                if (!(rz.radius > 0.0f)) {
                    // A zone that reaches nothing is a mistake, not a setting.
                    // Kept out of the world so it cannot be mistaken for an
                    // author's "no reverb here".
                    result.warnings.push_back(
                        "ReverbZone with a non-positive radius was ignored: " + line);
                } else {
                    // `inner <= radius` is the ReverbZone contract; an inner
                    // radius past the outer one would make the falloff invert.
                    rz.inner_radius = std::min(std::max(rz.inner_radius, 0.0f), rz.radius);
                    rz.wet_gain = std::min(std::max(rz.wet_gain, 0.0f), 1.0f);
                    rz.decay_seconds = std::max(rz.decay_seconds, 0.0f);
                    rz.pre_delay_seconds = std::max(rz.pre_delay_seconds, 0.0f);
                    rz.echo_spacing_seconds = std::max(rz.echo_spacing_seconds, 0.0f);
                    scene->world().add<audio::ReverbZoneComponent>(e, rz);
                }
            } else if (line.rfind("  Music:",0)==0) {
                audio::MusicComponent mus;
                mus.buffer_name = field_value(line, "buffer=");
                (void)field_float(line, "volume=", mus.volume);
                (void)field_float(line, "fade=", mus.fade_in_seconds);
                mus.enabled = line.find("enabled=false") == std::string::npos;
                scene->world().add<audio::MusicComponent>(e, std::move(mus));
            } else if (line.rfind("  Ambience:",0)==0) {
                audio::AmbienceComponent amb;
                amb.buffer_name = field_value(line, "buffer=");
                (void)field_float(line, "fade=", amb.fade_in_seconds);
                amb.enabled = line.find("enabled=false") == std::string::npos;
                scene->world().add<audio::AmbienceComponent>(e, std::move(amb));
            } else if (line.rfind("  Module:",0)==0) {
                gameplay::GameplayModuleComponent comp;
                comp.module_name = field_value(line, "name=");
                comp.enabled = line.find("enabled=false") == std::string::npos;

                // The state is whatever the module's class declared as
                // SerializeField; the loader does not need to know the type.
                const std::string encoded = field_value(line, "props=");
                if (!encoded.empty()) {
                    gameplay::decode_properties(encoded, comp.properties);
                }

                if (comp.module_name.empty()) {
                    // A nameless module cannot be matched to a factory, so it is
                    // reported rather than stored as an orphan component.
                    result.warnings.push_back("Module line without a name was ignored: " + line);
                } else {
                    scene->world().add<gameplay::GameplayModuleComponent>(e, std::move(comp));
                }
            } else if (line.rfind("  Script:",0)==0) {
                // File-backed Lua only: the frame steps ScriptSystem (Lua), not
                // the C# host, so any other lang is rejected loudly rather than
                // stored as an entity that looks scripted but never ticks.
                // Paths carry no spaces (field_value stops at the first one),
                // the same constraint Module props documents.
                const std::string lang = field_value(line, "lang=");
                const std::string path = field_value(line, "path=");
                if (!lang.empty() && lang != "lua") {
                    result.warnings.push_back("Script line with unsupported lang was ignored: " + line);
                } else if (path.empty()) {
                    result.warnings.push_back("Script line without a path was ignored: " + line);
                } else if (path.find(' ') != std::string::npos) {
                    result.warnings.push_back("Script path with spaces was ignored: " + line);
                } else {
                    scripting::ScriptComponent comp;
                    comp.path = path;
                    comp.enabled = line.find("enabled=false") == std::string::npos;
                    scene->world().add<scripting::ScriptComponent>(e, std::move(comp));
                }
            } else if (line.rfind("  Particles:",0)==0) {
                // One emitter per entity; the Runtime builds the live system
                // on adopt. Every field is optional (defaults stand in), but
                // a non-finite or out-of-range value rejects the line: the
                // emitter would otherwise simulate something the artist never
                // authored.
                vfx::ParticleComponent comp;
                bool ok = true;
                f32 rate = comp.config.rate;
                if (field_float(line, "rate=", rate)) {
                    ok = std::isfinite(rate) && rate >= 0.0f && rate <= 100000.0f;
                    comp.config.rate = rate;
                }
                f32 lifetime = comp.config.lifetime;
                if (ok && field_float(line, "lifetime=", lifetime)) {
                    ok = std::isfinite(lifetime) && lifetime > 0.0f && lifetime <= 60.0f;
                    comp.config.lifetime = lifetime;
                }
                f32 spread = comp.config.lifetime_spread;
                if (ok && field_float(line, "lifetime_spread=", spread)) {
                    ok = std::isfinite(spread) && spread >= 0.0f && spread <= 1.0f;
                    comp.config.lifetime_spread = spread;
                }
                f32 vx = 0.0f, vy = 0.0f, vz = 0.0f;
                if (ok && field_vec3(line, "velocity", vx, vy, vz)) {
                    ok = finite3(vx, vy, vz);
                    comp.config.velocity = Vec3(vx, vy, vz);
                }
                if (ok && field_vec3(line, "vel_spread", vx, vy, vz)) {
                    ok = finite3(vx, vy, vz);
                    comp.config.velocity_spread = Vec3(vx, vy, vz);
                }
                if (ok && field_vec3(line, "gravity", vx, vy, vz)) {
                    ok = finite3(vx, vy, vz);
                    comp.config.gravity = Vec3(vx, vy, vz);
                }
                f32 drag = comp.config.drag;
                if (ok && field_float(line, "drag=", drag)) {
                    ok = std::isfinite(drag) && drag >= 0.0f && drag <= 10.0f;
                    comp.config.drag = drag;
                }
                f32 start_size = comp.config.start_size;
                if (ok && field_float(line, "start_size=", start_size)) {
                    ok = std::isfinite(start_size) && start_size >= 0.0f;
                    comp.config.start_size = start_size;
                }
                f32 end_size = comp.config.end_size;
                if (ok && field_float(line, "end_size=", end_size)) {
                    ok = std::isfinite(end_size) && end_size >= 0.0f;
                    comp.config.end_size = end_size;
                }
                if (ok && field_vec3(line, "start_color", vx, vy, vz)) {
                    ok = finite3(vx, vy, vz);
                    comp.config.start_color = Vec3(vx, vy, vz);
                }
                if (ok && field_vec3(line, "end_color", vx, vy, vz)) {
                    ok = finite3(vx, vy, vz);
                    comp.config.end_color = Vec3(vx, vy, vz);
                }
                {
                    const std::string maxp = field_value(line, "max_particles=");
                    if (ok && !maxp.empty()) {
                        try {
                            const int parsed = std::stoi(maxp);
                            ok = parsed >= 1 && parsed <= 1048576;
                            comp.config.max_particles = static_cast<u32>(parsed);
                        } catch (...) {
                            ok = false;
                        }
                    }
                }
                comp.enabled = line.find("enabled=false") == std::string::npos;
                if (!ok) {
                    result.warnings.push_back("Particles line with invalid values was ignored: " + line);
                } else {
                    scene->world().add<vfx::ParticleComponent>(e, std::move(comp));
                }
            } else if (line.rfind("  Cloth:",0)==0) {
                // Same contract as Particles above: optional fields, finite
                // checks, reject-with-warning rather than a silently different
                // sheet. The sheet's world origin comes from the entity
                // Transform at build time, so no origin is stored here.
                physics::ClothComponent comp;
                bool ok = true;
                {
                    const std::string rx = field_value(line, "res_x=");
                    if (!rx.empty()) {
                        try {
                            const int parsed = std::stoi(rx);
                            ok = parsed >= 2 && parsed <= 128;
                            comp.config.res_x = parsed;
                        } catch (...) {
                            ok = false;
                        }
                    }
                }
                {
                    const std::string rz = field_value(line, "res_z=");
                    if (ok && !rz.empty()) {
                        try {
                            const int parsed = std::stoi(rz);
                            ok = parsed >= 2 && parsed <= 128;
                            comp.config.res_z = parsed;
                        } catch (...) {
                            ok = false;
                        }
                    }
                }
                f32 spacing = comp.config.spacing;
                if (ok && field_float(line, "spacing=", spacing)) {
                    ok = std::isfinite(spacing) && spacing > 0.0f && spacing <= 10.0f;
                    comp.config.spacing = spacing;
                }
                f32 mass = comp.config.mass;
                if (ok && field_float(line, "mass=", mass)) {
                    ok = std::isfinite(mass) && mass > 0.0f && mass <= 1000.0f;
                    comp.config.mass = mass;
                }
                f32 damping = comp.config.damping;
                if (ok && field_float(line, "damping=", damping)) {
                    ok = std::isfinite(damping) && damping >= 0.0f && damping < 1.0f;
                    comp.config.damping = damping;
                }
                f32 stiffness = comp.config.stiffness;
                if (ok && field_float(line, "stiffness=", stiffness)) {
                    ok = std::isfinite(stiffness) && stiffness >= 0.0f && stiffness <= 1.0f;
                    comp.config.stiffness = stiffness;
                }
                {
                    const std::string it = field_value(line, "iterations=");
                    if (ok && !it.empty()) {
                        try {
                            const int parsed = std::stoi(it);
                            ok = parsed >= 1 && parsed <= 32;
                            comp.config.iterations = parsed;
                        } catch (...) {
                            ok = false;
                        }
                    }
                }
                {
                    const std::string ss = field_value(line, "substeps=");
                    if (ok && !ss.empty()) {
                        try {
                            const int parsed = std::stoi(ss);
                            ok = parsed >= 1 && parsed <= 8;
                            comp.config.substeps = parsed;
                        } catch (...) {
                            ok = false;
                        }
                    }
                }
                f32 gx = 0.0f, gy = 0.0f, gz = 0.0f;
                if (ok && field_vec3(line, "gravity", gx, gy, gz)) {
                    ok = finite3(gx, gy, gz);
                    comp.config.gravity = Vec3(gx, gy, gz);
                }
                comp.enabled = line.find("enabled=false") == std::string::npos;
                if (!ok) {
                    result.warnings.push_back("Cloth line with invalid values was ignored: " + line);
                } else {
                    scene->world().add<physics::ClothComponent>(e, std::move(comp));
                }
            } else if (line.rfind("  Character:",0)==0) {
                // Player-style mover: config only. wish_dir/jump are live
                // inputs and grounded is solver state — none of the three is
                // scene data, so none is parsed here.
                physics::CharacterComponent comp;
                bool ok = true;
                f32 radius = comp.config.radius;
                if (field_float(line, "radius=", radius)) {
                    ok = std::isfinite(radius) && radius > 0.0f && radius <= 5.0f;
                    comp.config.radius = radius;
                }
                f32 max_speed = comp.config.max_speed;
                if (ok && field_float(line, "max_speed=", max_speed)) {
                    ok = std::isfinite(max_speed) && max_speed >= 0.0f && max_speed <= 100.0f;
                    comp.config.max_speed = max_speed;
                }
                f32 accel = comp.config.acceleration;
                if (ok && field_float(line, "acceleration=", accel)) {
                    ok = std::isfinite(accel) && accel >= 0.0f && accel <= 1000.0f;
                    comp.config.acceleration = accel;
                }
                f32 air = comp.config.air_control;
                if (ok && field_float(line, "air_control=", air)) {
                    ok = std::isfinite(air) && air >= 0.0f && air <= 1.0f;
                    comp.config.air_control = air;
                }
                f32 jump_speed = comp.config.jump_speed;
                if (ok && field_float(line, "jump_speed=", jump_speed)) {
                    ok = std::isfinite(jump_speed) && jump_speed >= 0.0f && jump_speed <= 50.0f;
                    comp.config.jump_speed = jump_speed;
                }
                f32 slope = comp.config.slope_limit_deg;
                if (ok && field_float(line, "slope_limit=", slope)) {
                    ok = std::isfinite(slope) && slope >= 0.0f && slope <= 90.0f;
                    comp.config.slope_limit_deg = slope;
                }
                f32 mass = comp.config.mass;
                if (ok && field_float(line, "mass=", mass)) {
                    ok = std::isfinite(mass) && mass > 0.0f && mass <= 10000.0f;
                    comp.config.mass = mass;
                }
                f32 friction = comp.config.friction;
                if (ok && field_float(line, "friction=", friction)) {
                    ok = std::isfinite(friction) && friction >= 0.0f && friction <= 10.0f;
                    comp.config.friction = friction;
                }
                comp.enabled = line.find("enabled=false") == std::string::npos;
                if (!ok) {
                    result.warnings.push_back("Character line with invalid values was ignored: " + line);
                } else {
                    scene->world().add<physics::CharacterComponent>(e, std::move(comp));
                }
            } else if (line.rfind("  NavMesh:",0)==0) {
                // The navigation volume: placement comes from the entity
                // Transform, so the line carries only the extents and the bake
                // tuning. Same optional-keys contract as the lines above.
                NavMeshComponent comp;
                std::string bad;
                if (!parse_navmesh(line, comp, bad)) {
                    result.warnings.push_back("NavMesh line with invalid '" + bad +
                                              "' was ignored: " + line);
                } else {
                    scene->world().add<NavMeshComponent>(e, std::move(comp));
                }
            } else if (line.rfind("  NavLink:",0)==0) {
                // One authored off-mesh link on the navigation volume's entity
                // (Phase 63). Like `BrainAction:`, the line appends to the
                // component its neighbours declare, creating it when the author
                // wrote the links first.
                NavLinkSpec link;
                std::string bad;
                if (!parse_nav_link(line, link, bad)) {
                    result.warnings.push_back("NavLink line with invalid '" + bad +
                                              "' was ignored: " + line);
                } else {
                    auto* navmesh = scene->world().get<NavMeshComponent>(e);
                    if (navmesh == nullptr) {
                        scene->world().add<NavMeshComponent>(e, NavMeshComponent{});
                        navmesh = scene->world().get<NavMeshComponent>(e);
                    }
                    if (navmesh != nullptr) {
                        navmesh->links.push_back(link);
                    }
                }
            } else if (line.rfind("  Terrain:",0)==0) {
                // The ground the navmesh walks on (Phase 63). Absent means the
                // flat floor the bake has always used, so a scene that never
                // heard of terrain keeps the mesh it had.
                TerrainComponent terrain;
                std::string bad;
                if (!parse_terrain(line, terrain, bad)) {
                    result.warnings.push_back("Terrain line with invalid '" + bad +
                                              "' was ignored: " + line);
                } else {
                    scene->world().add<TerrainComponent>(e, std::move(terrain));
                }
            } else if (line.rfind("  NavAgent:",0)==0) {
                // A walker: config plus the authored goal. The live path is a
                // runtime artefact and is never parsed or written.
                NavAgentComponent comp;
                std::string bad;
                if (!parse_nav_agent(line, comp, bad)) {
                    result.warnings.push_back("NavAgent line with invalid '" + bad +
                                              "' was ignored: " + line);
                } else {
                    scene->world().add<NavAgentComponent>(e, std::move(comp));
                }
            } else if (line.rfind("  NavRoute:",0)==0) {
                // The patrol policy (Phase 64). Points may be written before or
                // after this line, so the parse MERGES the policy into whatever
                // the entity already carries instead of replacing it.
                NavRouteComponent route;
                std::string bad;
                if (!parse_nav_route(line, route, bad)) {
                    result.warnings.push_back("NavRoute line with invalid '" + bad +
                                              "' was ignored: " + line);
                } else {
                    auto* existing = scene->world().get<NavRouteComponent>(e);
                    if (existing == nullptr) {
                        scene->world().add<NavRouteComponent>(e, std::move(route));
                    } else {
                        existing->wait = route.wait;
                        existing->loop = route.loop;
                        existing->ping_pong = route.ping_pong;
                        existing->enabled = route.enabled;
                        existing->hold = route.hold;
                        existing->resume = route.resume;
                        existing->investigate = route.investigate;
                        existing->squad = route.squad;
                    }
                }
            } else if (line.rfind("  NavPoint:",0)==0) {
                // One destination on the patrol route (Phase 64). Like
                // `NavLink:`, the line appends to the component its neighbours
                // declare and creates it when the author wrote the points first.
                NavPointSpec point;
                std::string bad;
                if (!parse_nav_point(line, point, bad)) {
                    result.warnings.push_back("NavPoint line with invalid '" + bad +
                                              "' was ignored: " + line);
                } else {
                    auto* route = scene->world().get<NavRouteComponent>(e);
                    if (route == nullptr) {
                        scene->world().add<NavRouteComponent>(e, NavRouteComponent{});
                        route = scene->world().get<NavRouteComponent>(e);
                    }
                    if (route != nullptr) {
                        route->points.push_back(point);
                    }
                }
            } else if (line.rfind("  Weapon:",0)==0) {
                // A gun (Phase 44): the authored profile only. The live state
                // (rounds left, cooldown, burst queue) is session data and is
                // built from the profile, so a loaded scene always starts with
                // the magazine the file describes.
                WeaponProfile profile;
                std::string bad;
                if (!parse_weapon(line, profile, bad) || !weapon_valid(profile)) {
                    result.warnings.push_back("Weapon line with invalid '" +
                                              (bad.empty() ? std::string("profile") : bad) +
                                              "' was ignored: " + line);
                } else {
                    scene->world().add<WeaponComponent>(e, WeaponComponent{profile});
                }
            } else if (line.rfind("  Health:",0)==0) {
                HealthComponent health;
                std::string bad;
                if (!parse_health(line, health, bad) || !health_valid(health)) {
                    result.warnings.push_back("Health line with invalid '" +
                                              (bad.empty() ? std::string("profile") : bad) +
                                              "' was ignored: " + line);
                } else {
                    scene->world().add<HealthComponent>(e, health);
                }
            } else if (line.rfind("  Status:",0)==0) {
                // Active effects can be authored for an environmental hazard
                // (a burning room) or restored from a save. Source is session
                // data and is left invalid on load.
                StatusEffectComponent status;
                std::string bad;
                if (!parse_status(line, status, bad)) {
                    result.warnings.push_back("Status line with invalid '" + bad +
                                              "' was ignored: " + line);
                } else if (status.effects.count > 0u) {
                    scene->world().add<StatusEffectComponent>(e, status);
                }
            } else if (line.rfind("  CombatAgent:",0)==0) {
                // An NPC that shoots (Phase 45). Configuration only; the aim
                // between frames is session state and is never parsed.
                CombatAgentComponent agent;
                std::string bad;
                if (!parse_combat_agent(line, agent, bad)) {
                    result.warnings.push_back("CombatAgent line with invalid '" + bad +
                                              "' was ignored: " + line);
                } else {
                    scene->world().add<CombatAgentComponent>(e, std::move(agent));
                }
            } else if (line.rfind("  Faction:",0)==0) {
                // Affiliation (Phase 46). No component means neutral; the
                // component exists so a team can be authored without inventing
                // a field on Health or Weapon.
                FactionComponent faction;
                std::string bad;
                if (!parse_faction(line, faction, bad)) {
                    result.warnings.push_back("Faction line with invalid '" + bad +
                                              "' was ignored: " + line);
                } else {
                    scene->world().add<FactionComponent>(e, std::move(faction));
                }
            } else if (line.rfind("  Vehicle:",0)==0) {
                // A drivable car: configuration only. The four drive inputs
                // are live state (same rule as Character's wish_dir/jump) and
                // the JoltVehicle pointer is session state — none is parsed.
                physics::VehicleComponent comp;
                std::string bad;
                if (!parse_vehicle(line, comp, bad)) {
                    result.warnings.push_back("Vehicle line with invalid '" + bad +
                                              "' was ignored: " + line);
                } else {
                    scene->world().add<physics::VehicleComponent>(e, std::move(comp));
                }
            } else {
                result.warnings.push_back("Unknown component line: " + line);
            }
        }
    }
    {
        auto all = scene->world().query<scene::Transform>();
        for (ecs::Entity e : all) {
            std::set<u32> visited;
            ecs::Entity cur = e;
            while (cur.valid()) {
                if (visited.count(cur.id)) {
                    result.error = "Cyclic hierarchy detected at entity " + std::to_string(e.id);
                    return result;
                }
                visited.insert(cur.id);
                auto* t = scene->world().get<scene::Transform>(cur);
                if (!t || !t->parent.valid()) break;
                cur = t->parent;
                if (!scene->world().is_alive(cur)) break;
            }
        }
    }
    scene::propagate_transforms(scene->world());
    result.scene = std::move(scene);
    result.success = true;
    return result;
}

SceneLoadResult load_scene_from_vfs(assets::VirtualFileSystem& vfs, const std::string& logical_path) {
    auto r = vfs.resolve(logical_path);
    if (!r.ok) {
        SceneLoadResult res;
        res.error = r.error;
        return res;
    }
    SceneLoadResult res = load_scene_from_physical(r.value);
    if (res.success && res.scene) {
        resolve_scene_audio(vfs, *res.scene, res.warnings);
    }
    return res;
}

// Binds AudioComponent::buffer_name through the VFS import pipeline:
// reads the file and decodes it (WAV/OGG/MP3/FLAC) into owned_buffer at the
// mix rate, so the source is audible without any procedural tone. Unresolvable
// names keep the old loud warning instead of silent failure.
//
// `what` names the component in the warning ("Audio", "Music", "Ambience") and
// is the only thing that differs between the three callers, so they cannot
// drift in how a missing or undecodable file is reported. "Audio" reproduces
// the original message byte for byte.
static bool decode_scene_audio(assets::VirtualFileSystem& vfs, const std::string& logical_path,
                               const char* what, audio::AudioBuffer& out,
                               std::vector<std::string>& warnings) {
    if (logical_path.empty() || !out.samples.empty()) {
        return false;
    }
    auto bytes = vfs.read_bytes(logical_path);
    if (!bytes.ok) {
        warnings.push_back(std::string(what) + " buffer '" + logical_path +
                           "' not found in VFS; the source will be silent");
        return false;
    }
    audio::DecodeOptions options;
    options.target_sample_rate = audio::kDefaultSampleRate;
    audio::DecodeResult decoded =
        audio::decode_audio_memory(bytes.value.data(), bytes.value.size(), options);
    if (!decoded.ok) {
        warnings.push_back(std::string(what) + " buffer '" + logical_path +
                           "' could not be decoded (" + decoded.error +
                           "); the source will be silent");
        return false;
    }
    out = std::move(decoded.buffer);
    return true;
}

void resolve_scene_audio(assets::VirtualFileSystem& vfs, scene::Scene& scene_obj,
                         std::vector<std::string>& warnings) {
    for (ecs::Entity e : scene_obj.world().all_entities()) {
        if (auto* aud = scene_obj.world().get<audio::AudioComponent>(e)) {
            // A `tone=` spec already generated its samples; the asset path is
            // the other route and only one of them can win.
            if (aud->buffer == nullptr && aud->owned_buffer.samples.empty()) {
                (void)decode_scene_audio(vfs, aud->buffer_name, "Audio", aud->owned_buffer,
                                         warnings);
            }
        }
        if (auto* mus = scene_obj.world().get<audio::MusicComponent>(e)) {
            (void)decode_scene_audio(vfs, mus->buffer_name, "Music", mus->owned_buffer, warnings);
        }
        if (auto* amb = scene_obj.world().get<audio::AmbienceComponent>(e)) {
            (void)decode_scene_audio(vfs, amb->buffer_name, "Ambience", amb->owned_buffer,
                                     warnings);
        }
    }
}

std::string serialize_scene_to_text(const scene::Scene& scene_obj) {
    std::ostringstream out;
    // f32 keeps ~7 significant decimal digits, but a default-formatted stream
    // writes 6. A transform saved and loaded through this format therefore
    // drifts a little on every cycle — the %.6g trap — and a save that was
    // supposed to restore a position restores a nearby one. Nine digits is the
    // shortest width that round-trips every f32, so the stream is set once and
    // every float below inherits it.
    out << std::setprecision(9);
    out << "# SANAD Scene v1\n";
    out << "version: 1\n";
    out << "name: " << scene_obj.name() << "\n";
    auto entities = scene_obj.world().all_entities();
    out << "entity_count: " << entities.size() << "\n";
    for (ecs::Entity e : entities) {
        out << "---\n";
        out << "entity: " << e.id << ":" << e.generation << "\n";
        const auto* n = scene_obj.world().get<scene::NameComponent>(e);
        if (n && !n->name.empty()) {
            out << "  Name: " << n->name << "\n";
        }
        const auto* pl = scene_obj.world().get<scene::PrefabLinkComponent>(e);
        if (pl && !pl->prefab_path.empty()) {
            out << "  Prefab: path=" << pl->prefab_path << "\n";
        }
        const auto* t = scene_obj.world().get<scene::Transform>(e);
        if (t) {
            out << "  Transform: local(" << t->local_x << "," << t->local_y << "," << t->local_z << ") world(" << t->world_x << "," << t->world_y << "," << t->world_z << ") rot(" << t->rot_x << "," << t->rot_y << "," << t->rot_z << ") scale(" << t->scale_x << "," << t->scale_y << "," << t->scale_z << ") parent(" << t->parent.id << ":" << t->parent.generation << ")\n";
        }
        const auto* m = scene_obj.world().get<MeshComponent>(e);
        if (m) {
            out << "  Mesh: asset_id=" << m->mesh_id.to_string() << " material=" << m->material << "\n";
        }
        // Projected decal (Phase 36). Written after Mesh, where a reader
        // already expects per-entity surface data. The material key is omitted
        // when empty so an untinted decal does not claim a path it never named,
        // and `enabled=false` is the only non-default boolean form.
        const auto* dc = scene_obj.world().get<DecalComponent>(e);
        if (dc) {
            out << "  Decal:";
            if (!dc->material.empty()) {
                out << " material=" << dc->material;
            }
            out << " size(" << dc->size_x << "," << dc->size_y << "," << dc->size_z << ")"
                << " opacity=" << dc->opacity;
            if (!dc->enabled) {
                out << " enabled=false";
            }
            out << "\n";
        }
        const auto* l = scene_obj.world().get<DirectionalLight>(e);
        if (l) {
            out << "  Light: type=Directional dir(" << l->dir_x << "," << l->dir_y << "," << l->dir_z << ") color(" << l->color_r << "," << l->color_g << "," << l->color_b << ") intensity=" << l->intensity;
            if (!l->cast_shadows) out << " shadows=false";
            // Non-default shadow tuning is explicit so old files (which omit
            // both keys) keep rendering exactly as before.
            if (l->shadow_strength != 1.0f) out << " shadow_strength=" << l->shadow_strength;
            if (l->shadow_bias != 0.0005f) out << " shadow_bias=" << l->shadow_bias;
            // Written only when the scene departs from the renderer's default, so
            // a scene authored before cascades existed round-trips to the exact
            // same bytes it was loaded from.
            if (l->shadow_cascades != 4u) out << " shadow_cascades=" << l->shadow_cascades;
            if (l->shadow_distance != 0.0f) out << " shadow_distance=" << l->shadow_distance;
            out << "\n";
        }
        const auto* point = scene_obj.world().get<PointLightComponent>(e);
        if (point) {
            out << "  Light: type=Point color(" << point->color_r << "," << point->color_g << ","
                << point->color_b << ") intensity=" << point->intensity << " radius=" << point->radius;
            if (point->cast_shadows) out << " shadows=true";
            if (point->shadow_strength != 1.0f) out << " shadow_strength=" << point->shadow_strength;
            if (point->shadow_bias != 0.0005f) out << " shadow_bias=" << point->shadow_bias;
            if (point->shadow_distance != 0.0f) out << " shadow_distance=" << point->shadow_distance;
            if (!point->enabled) out << " enabled=false";
            out << "\n";
        }
        const auto* slt = scene_obj.world().get<SpotLightComponent>(e);
        if (slt) {
            out << "  Light: type=Spot dir(" << slt->dir_x << "," << slt->dir_y << "," << slt->dir_z
                << ") color(" << slt->color_r << "," << slt->color_g << "," << slt->color_b
                << ") intensity=" << slt->intensity << " range=" << slt->range
                << " inner_rad=" << slt->inner_angle_rad << " outer_rad=" << slt->outer_angle_rad;
            if (slt->cast_shadows) out << " shadows=true";
            if (slt->shadow_strength != 1.0f) out << " shadow_strength=" << slt->shadow_strength;
            if (slt->shadow_bias != 0.0005f) out << " shadow_bias=" << slt->shadow_bias;
            if (slt->shadow_distance != 0.0f) out << " shadow_distance=" << slt->shadow_distance;
            if (!slt->enabled) out << " enabled=false";
            out << "\n";
        }
        const auto* sky = scene_obj.world().get<SkyComponent>(e);
        if (sky) {
            out << "  Sky: zenith(" << sky->zenith_r << "," << sky->zenith_g << "," << sky->zenith_b << ")"
                << " horizon(" << sky->horizon_r << "," << sky->horizon_g << "," << sky->horizon_b << ")"
                << " ground(" << sky->ground_r << "," << sky->ground_g << "," << sky->ground_b << ")"
                << " clear(" << sky->clear_r << "," << sky->clear_g << "," << sky->clear_b << ")"
                << " sun_disk=" << sky->sun_disk << " sun_glow=" << sky->sun_glow
                << " enabled=" << (sky->enabled ? "true" : "false") << "\n";
            // The clock is written only when the scene carries one, so a scene
            // that has never heard of TimeOfDay round-trips to the exact bytes it
            // was loaded from.
            const auto* tod = scene_obj.world().get<TimeOfDayComponent>(e);
            if (tod != nullptr) {
                out << "  TimeOfDay: hours=" << tod->time_hours
                    << " day_length=" << tod->day_length_seconds
                    << " enabled=" << (tod->enabled ? "true" : "false")
                    << " drive_light=" << (tod->drive_light ? "true" : "false") << "\n";
            }
        }
        // The post stack, written only when the entity carries one — same
        // contract as the clock above, so a scene that has never heard of
        // PostProcess round-trips to the bytes it was loaded from. Written
        // outside the sky block because a scene may want a glow without a
        // procedural sky.
        const auto* pp = scene_obj.world().get<PostProcessComponent>(e);
        if (pp) {
            out << "  PostProcess: bloom=" << (pp->bloom_enabled ? "true" : "false")
                << " bloom_threshold=" << pp->bloom_threshold
                << " bloom_knee=" << pp->bloom_knee
                << " bloom_intensity=" << pp->bloom_intensity
                << " bloom_radius=" << pp->bloom_radius
                << " grade=" << (pp->grade_enabled ? "true" : "false")
                << " grade_contrast=" << pp->grade_contrast
                << " grade_pivot=" << pp->grade_pivot
                << " grade_temperature=" << pp->grade_temperature
                << " grade_tint=" << pp->grade_tint
                << " grade_gamma=" << pp->grade_gamma
                << " sharpen=" << (pp->sharpen_enabled ? "true" : "false")
                << " sharpen_amount=" << pp->sharpen_amount
                << " sharpen_radius=" << pp->sharpen_radius
                << " saturation=" << pp->saturation
                << " vignette=" << pp->vignette
                << " lens=" << (pp->lens_enabled ? "true" : "false")
                << " lens_distortion=" << pp->lens_distortion
                << " lens_chroma=" << pp->lens_chromatic_aberration
                << " dof=" << (pp->dof_enabled ? "true" : "false")
                << " dof_focus=" << pp->dof_focus_distance
                << " dof_range=" << pp->dof_focus_range
                << " dof_radius=" << pp->dof_max_radius
                << " motion=" << (pp->motion_enabled ? "true" : "false")
                << " motion_intensity=" << pp->motion_intensity
                << " motion_length=" << pp->motion_max_length
                << " fxaa=" << (pp->fxaa_enabled ? "true" : "false")
                << " fxaa_threshold=" << pp->fxaa_threshold
                << " fxaa_threshold_min=" << pp->fxaa_threshold_min;
            // 1.0 is the identity, so only a scene that actually supersamples
            // (or renders below native) writes the key. A round trip of an
            // old scene stays byte-identical.
            if (pp->render_scale != 1.0f) {
                out << " render_scale=" << pp->render_scale;
            }
            // The governor's tuning is authored state only while the governor
            // is on. Writing target_fps/min_render_scale for a scene that never
            // enables it would add two keys to every existing level on save.
            if (pp->dynamic_resolution) {
                out << " dynamic_resolution=true"
                    << " target_fps=" << pp->target_fps
                    << " min_render_scale=" << pp->min_render_scale;
                // CPU is the absent/default state; only the opt-in GPU clock
                // adds a key, so a Phase 38/39 scene round-trips byte-identically.
                if (pp->dynamic_resolution_timing == kDynamicResolutionTimingGpu) {
                    out << " dynamic_resolution_timing=gpu";
                }
            }
            // The window policy shares the governor's ceiling/floor/target, so
            // it is a single switch and only written when a scene names it.
            if (pp->frame_policy) {
                out << " frame_policy=true";
            }
            // Written ONLY when authored. These two have sentinels, so an
            // unconditional write would turn every scene into one that names an
            // exposure and an operator it never chose — and the acceptance
            // scene's golden pixels are pinned to the renderer's defaults.
            if (pp->exposure > 0.0f) {
                out << " exposure=" << pp->exposure;
            }
            if (pp->tonemap >= 0) {
                out << " tonemap=" << post_process_tonemap_name(pp->tonemap);
            }
            // The strength is written only alongside a path: on its own it is a
            // claim that changes nothing, and a reader would be right to wonder
            // what LUT it referred to.
            if (!pp->lut_path.empty()) {
                out << " lut=" << pp->lut_path;
                if (pp->lut_strength > 0.0f) {
                    out << " lut_strength=" << pp->lut_strength;
                }
            }
            out << " fog=" << (pp->fog_enabled ? "true" : "false")
                << " fog_color(" << pp->fog_r << "," << pp->fog_g << "," << pp->fog_b << ")"
                << " fog_start=" << pp->fog_start << " fog_end=" << pp->fog_end;
            out << "\n";
        }
        // Combat feedback (Phase 56). Only an entity that carries the component
        // writes the line, so every scene authored before 0.6.0 round-trips to
        // the same bytes. It is new, so writing every key makes a hand-written
        // one-key line save back as the complete state the reader resolved.
        const auto* fb = scene_obj.world().get<FeedbackComponent>(e);
        if (fb) {
            out << "  Feedback: flash_time=" << fb->flash_seconds
                << " flash_size=" << fb->flash_size
                << " tracer_time=" << fb->tracer_seconds
                << " tracer_length=" << fb->tracer_length
                << " tracer_width=" << fb->tracer_width
                << " impact_time=" << fb->impact_seconds
                << " impact_size=" << fb->impact_size
                << " shot_volume=" << fb->shot_volume
                << " impact_volume=" << fb->impact_volume
                // Phase 59 additions. Written like every key above: a scene that
                // carries the component saves back as the complete state the
                // reader resolved, so a one-key hand-written line round-trips
                // into something a human can still read and edit.
                << " blast_time=" << fb->blast_flash_seconds
                << " blast_size=" << fb->blast_flash_size
                << " impact_volume_explosive=" << fb->impact_volume_explosive
                << " impact_volume_fire=" << fb->impact_volume_fire
                << " light_intensity=" << fb->flash_light_intensity
                << " light_radius=" << fb->flash_light_radius
                << " light_r=" << fb->flash_light_r
                << " light_g=" << fb->flash_light_g
                << " light_b=" << fb->flash_light_b
                << " max_flashes=" << fb->max_flashes
                << " max_tracers=" << fb->max_tracers
                << " max_impacts=" << fb->max_impacts
                << " max_sounds=" << fb->max_sounds
                << " max_lights=" << fb->max_lights;
            if (!fb->audio) {
                out << " audio=false";
            }
            if (!fb->flash_light) {
                out << " light=false";
            }
            if (!fb->enabled) {
                out << " enabled=false";
            }
            out << "\n";
        }
        const auto* rigged = scene_obj.world().get<CharacterComponent>(e);
        if (rigged) {
            // Only the source is written: the skeleton, the clips and the
            // skin data all come from the file, and writing a copy of them
            // would be a second source of truth that can disagree with it.
            out << "  CharacterRig: file=" << rigged->source << "\n";
        }
        const auto* c = scene_obj.world().get<CameraComponent>(e);
        if (c) {
            out << "  Camera: fov=" << c->fov_y << " aspect=" << c->aspect << " near=" << c->near_plane << " far=" << c->far_plane << " active=" << (c->is_active ? "true" : "false");
            // The follow camera. The name is written LAST and runs to the end
            // of the line: it is the only free-form value on the line, and a
            // name with a space in it would otherwise be read as two fields.
            out << " pivot=" << c->pivot_x << "," << c->pivot_y << "," << c->pivot_z
                << " distance=" << c->distance << " yaw=" << c->yaw << " pitch=" << c->pitch
                << " smoothing=" << c->smoothing << " radius=" << c->collision_radius
                << " avoid=" << (c->avoid_collision ? "1" : "0");
            // Aiming. Only what differs from the default is written, so a
            // scene that never touched these keys round-trips unchanged —
            // the same rule the shadow tuning follows.
            if (!c->mouse_look) {
                out << " mouse=0";
            }
            if (c->look_sensitivity != 0.12f) {
                out << " looksens=" << c->look_sensitivity;
            }
            if (c->stick_sensitivity != 180.0f) {
                out << " sticksens=" << c->stick_sensitivity;
            }
            if (c->pitch_min != -80.0f) {
                out << " pitchmin=" << c->pitch_min;
            }
            if (c->pitch_max != 60.0f) {
                out << " pitchmax=" << c->pitch_max;
            }
            if (c->invert_look_y) {
                out << " invert_y=1";
            }
            if (c->zoom_min != 1.5f) {
                out << " zoommin=" << c->zoom_min;
            }
            if (c->zoom_max != 12.0f) {
                out << " zoommax=" << c->zoom_max;
            }
            if (c->zoom_step != 0.6f) {
                out << " zoomstep=" << c->zoom_step;
            }
            if (c->align_speed != 0.0f) {
                out << " align=" << c->align_speed;
            }
            // Aiming down sights and shake trim, written only when authored —
            // the same rule as the aiming keys above, so an untouched scene
            // round-trips byte for byte.
            if (c->ads_enabled) {
                out << " ads=1 ads_distance=" << c->ads_distance
                    << " ads_sens=" << c->ads_sensitivity << " ads_speed=" << c->ads_speed;
            }
            if (c->shake_strength != 1.0f) {
                out << " shake=" << c->shake_strength;
            }
            if (c->shake_decay != 1.4f) {
                out << " shake_decay=" << c->shake_decay;
            }
            if (c->recoil_recover != 8.0f) {
                out << " recoil=" << c->recoil_recover;
            }
            if (c->recoil_max != 12.0f) {
                out << " recoil_max=" << c->recoil_max;
            }
            if (!c->follow_target.empty()) {
                out << " follow=" << c->follow_target;
            }
            out << "\n";
        }
        // The gun (Phase 44). Written in full, like Sky and Decal: the line is
        // new, so there is no older file whose bytes must be preserved, and a
        // reader can see every number a weapon has without opening the code.
        // The burst pair is the exception — it only means anything in burst
        // mode, and writing it for a rifle would suggest it did.
        const auto* wc = scene_obj.world().get<WeaponComponent>(e);
        if (wc) {
            const WeaponProfile& w = wc->profile;
            out << "  Weapon: mode=" << fire_mode_name(w.mode)
                << " rpm=" << w.rounds_per_minute << " magazine=" << w.magazine
                << " reserve=" << w.reserve << " reload=" << w.reload_seconds
                << " damage=" << w.damage << " range=" << w.range
                << " pellets=" << w.pellets << " spread=" << w.spread_deg
                << " bloom=" << w.spread_bloom_deg << " spread_max=" << w.spread_max_deg
                << " recover=" << w.spread_recover_deg_per_s
                << " recoil=" << w.recoil_pitch << " recoil_yaw=" << w.recoil_yaw
                << " recoil_growth=" << w.recoil_growth
                << " falloff_start=" << w.falloff_start << " falloff_end=" << w.falloff_end
                << " falloff_min=" << w.falloff_min_scale;
            // Phase 50 is opt-in: an old line that never authored penetration
            // round-trips without a new key.
            if (w.damage_type != DamageType::Ballistic) {
                out << " damage_type=" << damage_type_name(w.damage_type);
            }
            if (w.armor_penetration > 0.0f) {
                out << " pierce=" << w.armor_penetration;
            }
            // A blast is opt-in too. Write all three values together so the
            // line cannot describe a radius with no damage or vice versa.
            if (w.blast_radius > 0.0f || w.blast_damage > 0.0f) {
                out << " blast_radius=" << w.blast_radius
                    << " blast_damage=" << w.blast_damage
                    << " blast_min=" << w.blast_min_scale;
            }
            if (w.status != StatusKind::None) {
                out << " status=" << status_kind_name(w.status)
                    << " status_dps=" << w.status_damage_per_second
                    << " status_time=" << w.status_seconds
                    << " status_tick=" << w.status_tick_seconds;
            }
            out << " shake=" << w.shake << " seed=" << w.seed;
            // Hitscan is the historical default, so it writes no key. A
            // projectile writes its delivery and the two numbers that define
            // its flight; keeping them off a hitscan line is what lets an
            // older file round-trip byte-for-byte.
            if (w.delivery == WeaponDelivery::Projectile) {
                out << " delivery=projectile speed=" << w.projectile_speed
                    << " gravity=" << w.projectile_gravity;
            }
            if (w.mode == FireMode::Burst) {
                out << " burst=" << w.burst_count << " burst_gap=" << w.burst_gap_seconds;
            }
            if (!w.auto_reload) {
                out << " auto_reload=false";
            }
            if (!w.enabled) {
                out << " enabled=false";
            }
            out << "\n";
        }
        // Hit points. `health` is written as the CURRENT value: a scene saved
        // mid-fight saves the wound, which is what "the scene says where this
        // entity is" means everywhere else in this format.
        const auto* hc = scene_obj.world().get<HealthComponent>(e);
        if (hc) {
            out << "  Health: max=" << hc->max_health << " health=" << hc->health;
            if (hc->regen_per_second != 0.0f) {
                out << " regen=" << hc->regen_per_second;
            }
            if (hc->regen_delay != 0.0f) {
                out << " regen_delay=" << hc->regen_delay;
            }
            // Armor and zones are opt-in. The pair is written only when an
            // armor pool exists, so a pre-Phase-50 scene stays byte-identical.
            if (hc->max_armor > 0.0f || hc->armor > 0.0f) {
                out << " armor=" << hc->armor << " armor_max=" << hc->max_armor
                    << " absorb=" << hc->armor_absorb;
            }
            if (hc->hit_zones) {
                out << " zones=true head=" << hc->headshot_multiplier
                    << " limb=" << hc->limb_multiplier;
            }
            // Resistance is written only when authored, one key per type, so a
            // scene that has none keeps its old line byte for byte.
            if (hc->ballistic_resistance > 0.0f) {
                out << " resist_ballistic=" << hc->ballistic_resistance;
            }
            if (hc->explosive_resistance > 0.0f) {
                out << " resist_explosive=" << hc->explosive_resistance;
            }
            if (hc->fire_resistance > 0.0f) {
                out << " resist_fire=" << hc->fire_resistance;
            }
            if (hc->invulnerable) {
                out << " invulnerable=true";
            }
            if (!hc->enabled) {
                out << " enabled=false";
            }
            out << "\n";
        }
        // Active status effects (Phase 54). The remaining time is saved, like
        // health, so a scene saved mid-burn resumes with the fire still on it.
        // The source entity is session data and is not written.
        const auto* status = scene_obj.world().get<StatusEffectComponent>(e);
        if (status != nullptr && status->effects.count > 0u) {
            out << "  Status:";
            for (u32 i = 0; i < status->effects.count; ++i) {
                const StatusEffect& effect = status->effects.effects[i];
                out << " " << status_kind_name(effect.kind) << "="
                    << effect.damage_per_second << "," << effect.remaining_seconds
                    << "," << effect.tick_seconds;
            }
            if (!status->enabled) {
                out << " enabled=false";
            }
            out << "\n";
        }
        // An NPC that fights (Phase 45). `target=` is free text and therefore
        // last, the same rule the camera's `follow=` follows.
        const auto* ca = scene_obj.world().get<CombatAgentComponent>(e);
        if (ca) {
            out << "  CombatAgent: eye=" << ca->eye_height << " aim=" << ca->target_height
                << " reaction=" << ca->config.reaction_seconds
                << " turn=" << ca->config.aim_speed_deg_per_s
                << " cone=" << ca->config.fire_cone_deg << " range=" << ca->config.max_range
                << " burst=" << ca->config.burst_on_seconds
                << " rest=" << ca->config.burst_off_seconds;
            if (!ca->enabled) {
                out << " enabled=false";
            }
            if (ca->auto_target) {
                out << " auto_target=true";
            }
            if (ca->retaliate) {
                out << " retaliate=true";
            }
            if (!ca->target.empty()) {
                out << " target=" << ca->target;
            }
            out << "\n";
        }
        // Affiliation (Phase 46). An empty team writes no line: the component
        // is semantically identical to its absence, so preserving bytes for a
        // neutral entity matters more than recording an empty component.
        const auto* fc = scene_obj.world().get<FactionComponent>(e);
        if (fc && !fc->team.empty()) {
            out << "  Faction: team=" << fc->team;
            if (fc->friendly_fire) {
                out << " friendly_fire=true";
            }
            if (!fc->enabled) {
                out << " enabled=false";
            }
            out << "\n";
        }
        // Perception (Phase 60). Written like the feedback line: numbers always,
        // booleans only when they differ from the default, so a hand-written
        // one-key line saves back as the complete state the reader resolved
        // without growing keys nobody set.
        const auto* pc = scene_obj.world().get<PerceptionComponent>(e);
        if (pc) {
            out << "  Perception: sight=" << pc->sight_range
                << " fov=" << pc->sight_half_fov_deg
                << " gain=" << pc->sight_gain
                << " loss=" << pc->sight_loss
                << " linger=" << pc->sight_linger
                << " hearing=" << pc->hearing_range
                << " touch=" << pc->touch_range
                << " noise=" << pc->noise
                << " visible=" << pc->visibility
                << " radius=" << pc->radius
                << " eye=" << pc->eye_height;
            if (!pc->pain) {
                out << " pain=false";
            }
            if (!pc->governs_combat) {
                out << " combat=false";
            }
            if (!pc->enabled) {
                out << " enabled=false";
            }
            out << "\n";
        }
        // The decision layer (Phase 61). The brain line carries the clock, and
        // every action is its own line so the list reads like the considerations
        // it is; the order is what breaks a tie, so it is written in order.
        const auto* brain = scene_obj.world().get<BrainComponent>(e);
        if (brain) {
            out << "  Brain: rate=" << brain->rate;
            if (!brain->enabled) {
                out << " enabled=false";
            }
            out << "\n";
            for (const BrainAction& action : brain->actions) {
                out << "  BrainAction: name=" << action.name
                    << " input=" << brain_input_name(action.input)
                    << " slope=" << action.slope
                    << " exponent=" << action.exponent
                    << " input_offset=" << action.input_offset
                    << " output_offset=" << action.output_offset
                    << " weight=" << action.weight;
                if (action.hold_fire) {
                    out << " hold_fire=true";
                }
                out << "\n";
            }
        }
        // The state machine (Phase 62): the declaration, then one line per state
        // and one per guarded edge, in the authored order that decides ties.
        const auto* machine = scene_obj.world().get<StateMachineComponent>(e);
        if (machine) {
            out << "  States: rate=" << machine->rate;
            if (!machine->initial.empty()) {
                out << " initial=" << machine->initial;
            }
            if (!machine->enabled) {
                out << " enabled=false";
            }
            out << "\n";
            for (const StateSpec& state : machine->states) {
                out << "  State: name=" << state.name;
                if (state.hold_fire) {
                    out << " hold_fire=true";
                }
                out << "\n";
            }
            for (const TransitionSpec& transition : machine->transitions) {
                out << "  Transition: from=" << transition.from << " to=" << transition.to
                    << " when=" << brain_input_name(transition.when)
                    << " at=" << transition.at;
                if (transition.below) {
                    out << " below=true";
                }
                out << "\n";
            }
        }
        // Physics. The runtime body handle is deliberately NOT written: it is a
        // per-session identity (generation-checked), and a stale one from a
        // previous run is meaningless — reloading rebuilds it from the scene.
        const auto* rb = scene_obj.world().get<physics::RigidBodyComponent>(e);
        if (rb) {
            const char* type = (rb->type == physics::BodyType::Static) ? "Static"
                             : (rb->type == physics::BodyType::Kinematic) ? "Kinematic"
                                                                          : "Dynamic";
            out << "  RigidBody: type=" << type
                << " mass=" << rb->mass
                << " friction=" << rb->friction
                << " restitution=" << rb->restitution
                << " linear_damping=" << rb->linear_damping
                << " angular_damping=" << rb->angular_damping
                << " allow_sleep=" << (rb->allow_sleep ? "true" : "false") << "\n";
        }
        const auto* col = scene_obj.world().get<physics::ColliderComponent>(e);
        if (col) {
            if (col->shape.type == physics::ShapeType::Box) {
                out << "  Collider: shape=Box half("
                    << col->shape.box.half_extents.x << ","
                    << col->shape.box.half_extents.y << ","
                    << col->shape.box.half_extents.z << ")\n";
            } else if (col->shape.type == physics::ShapeType::Plane) {
                out << "  Collider: shape=Plane normal("
                    << col->shape.plane.normal.x << ","
                    << col->shape.plane.normal.y << ","
                    << col->shape.plane.normal.z << ")\n";
            } else {
                out << "  Collider: shape=Sphere radius=" << col->shape.sphere.radius << "\n";
            }
        }
        const auto* dst = scene_obj.world().get<DestructibleComponent>(e);
        if (dst) {
            // Every field is written, even at its default: the whole component
            // is a build spec for an asset that is never stored, so a round trip
            // must reproduce the exact same fracture rather than a default one.
            out << "  Destructible: chunks=" << dst->chunks
                << " seed=" << dst->seed
                << " strength=" << dst->strength
                << " damage_threshold=" << dst->damage_threshold
                << " blast_radius=" << dst->blast_radius
                << " enabled=" << (dst->enabled ? "true" : "false") << "\n";
        }
        const auto* anim = scene_obj.world().get<animation::AnimationComponent>(e);
        if (anim) {
            const char* loop = (anim->player.loop_mode() == animation::LoopMode::None) ? "none"
                             : (anim->player.loop_mode() == animation::LoopMode::PingPong) ? "pingpong"
                                                                                          : "loop";
            out << "  Animation: clip=" << anim->player.clip_name()
                << " speed=" << anim->speed
                << " loop=" << loop
                << " paused=" << (anim->paused ? "true" : "false")
                << " state_machine=" << (anim->use_state_machine ? "true" : "false");
            // The clip itself is generated at load time, never stored, so the
            // spec is the only thing that can rebuild it. Writing only the clip
            // name would reload the scene with an empty clip and the entity
            // would silently stop moving.
            if (anim->has_procedural) {
                out << " procedural="
                    << (anim->procedural.kind == animation::ProceduralClipSpec::Kind::Bob ? "bob"
                                                                                         : "spin")
                    << " axis=" << axis_name(anim->procedural.axis)
                    << " turns=" << anim->procedural.turns
                    << " amplitude=" << anim->procedural.amplitude
                    << " duration=" << anim->procedural.duration;
            }
            out << "\n";
        }
        const auto* aud = scene_obj.world().get<audio::AudioComponent>(e);
        if (aud) {
            out << "  Audio: buffer=" << aud->buffer_name
                << " volume=" << aud->volume
                << " pitch=" << aud->pitch
                << " looping=" << (aud->looping ? "true" : "false")
                << " spatial=" << (aud->spatial ? "true" : "false")
                << " autoplay=" << (aud->autoplay ? "true" : "false")
                << " min_distance=" << aud->spatial_settings.min_distance
                << " max_distance=" << aud->spatial_settings.max_distance
                << " bus=" << audio::bus_name(aud->bus)
                << " occluded=" << (aud->occluded ? "true" : "false");
            // Same as the animation spec above: the samples are generated, so
            // the spec is what makes them survive a save/load.
            if (aud->tone_hz > 0.0f) {
                out << " tone=" << aud->tone_hz << " tone_duration=" << aud->tone_duration;
            }
            out << "\n";
        }
        // The audio environment lines are written ONLY when their component
        // exists, so a scene that never heard of them round-trips byte for byte
        // — the same rule the Sky, TimeOfDay and PostProcess lines follow.
        const auto* rz = scene_obj.world().get<audio::ReverbZoneComponent>(e);
        if (rz) {
            out << "  ReverbZone: radius=" << rz->radius
                << " inner=" << rz->inner_radius
                << " wet=" << rz->wet_gain
                << " decay=" << rz->decay_seconds
                << " predelay=" << rz->pre_delay_seconds
                << " spacing=" << rz->echo_spacing_seconds
                << " enabled=" << (rz->enabled ? "true" : "false")
                << "\n";
        }
        const auto* mus = scene_obj.world().get<audio::MusicComponent>(e);
        if (mus) {
            out << "  Music: buffer=" << mus->buffer_name
                << " volume=" << mus->volume
                << " fade=" << mus->fade_in_seconds
                << " enabled=" << (mus->enabled ? "true" : "false")
                << "\n";
        }
        const auto* amb = scene_obj.world().get<audio::AmbienceComponent>(e);
        if (amb) {
            out << "  Ambience: buffer=" << amb->buffer_name
                << " fade=" << amb->fade_in_seconds
                << " enabled=" << (amb->enabled ? "true" : "false")
                << "\n";
        }
        const auto* mod = scene_obj.world().get<gameplay::GameplayModuleComponent>(e);
        if (mod && !mod->module_name.empty()) {
            out << "  Module: name=" << mod->module_name
                << " enabled=" << (mod->enabled ? "true" : "false")
                << " props=" << gameplay::encode_properties(mod->properties)
                << "\n";
        }
        // File-backed scripts only: an inline-only source (tests, live edits
        // never written to a file) has no path to persist, and storing Lua
        // inline would need an escaping scheme this line format does not have.
        // Such components are runtime-only by design and are skipped here.
        const auto* script = scene_obj.world().get<scripting::ScriptComponent>(e);
        if (script && !script->path.empty()) {
            out << "  Script: lang=lua path=" << script->path
                << " enabled=" << (script->enabled ? "true" : "false") << "\n";
        }
        // Particles: the live array is never stored (like fracture assets),
        // so every config field is written even at its default — a round
        // trip must reproduce the exact same emission.
        const auto* particles = scene_obj.world().get<vfx::ParticleComponent>(e);
        if (particles) {
            const vfx::EmitterConfig& cfg = particles->config;
            out << "  Particles: rate=" << cfg.rate
                << " lifetime=" << cfg.lifetime
                << " lifetime_spread=" << cfg.lifetime_spread
                << " velocity(" << cfg.velocity.x << "," << cfg.velocity.y << "," << cfg.velocity.z << ")"
                << " vel_spread(" << cfg.velocity_spread.x << "," << cfg.velocity_spread.y << ","
                << cfg.velocity_spread.z << ")"
                << " gravity(" << cfg.gravity.x << "," << cfg.gravity.y << "," << cfg.gravity.z << ")"
                << " drag=" << cfg.drag
                << " start_size=" << cfg.start_size
                << " end_size=" << cfg.end_size
                << " start_color(" << cfg.start_color.x << "," << cfg.start_color.y << ","
                << cfg.start_color.z << ")"
                << " end_color(" << cfg.end_color.x << "," << cfg.end_color.y << ","
                << cfg.end_color.z << ")"
                << " max_particles=" << cfg.max_particles
                << " enabled=" << (particles->enabled ? "true" : "false") << "\n";
        }
        // Cloth: material spec only — the sheet's world origin comes from
        // the entity Transform at build time and is never stored here.
        const auto* cloth = scene_obj.world().get<physics::ClothComponent>(e);
        if (cloth) {
            const physics::ClothConfig& cfg = cloth->config;
            out << "  Cloth: res_x=" << cfg.res_x
                << " res_z=" << cfg.res_z
                << " spacing=" << cfg.spacing
                << " mass=" << cfg.mass
                << " damping=" << cfg.damping
                << " stiffness=" << cfg.stiffness
                << " iterations=" << cfg.iterations
                << " substeps=" << cfg.substeps
                << " gravity(" << cfg.gravity.x << "," << cfg.gravity.y << "," << cfg.gravity.z << ")"
                << " enabled=" << (cloth->enabled ? "true" : "false") << "\n";
        }
        // Character: config only — wish_dir/jump are live inputs and grounded
        // is solver state, so none of the three is scene data.
        const auto* character = scene_obj.world().get<physics::CharacterComponent>(e);
        if (character) {
            const physics::CharacterConfig& cfg = character->config;
            out << "  Character: radius=" << cfg.radius
                << " max_speed=" << cfg.max_speed
                << " acceleration=" << cfg.acceleration
                << " air_control=" << cfg.air_control
                << " jump_speed=" << cfg.jump_speed
                << " slope_limit=" << cfg.slope_limit_deg
                << " mass=" << cfg.mass
                << " friction=" << cfg.friction
                << " enabled=" << (character->enabled ? "true" : "false") << "\n";
        }
        // Navigation volume and its walkers. Written only when the entity
        // carries the component, so a scene that never heard of navigation
        // round-trips byte for byte — the same rule Sky/PostProcess follow.
        const auto* nm = scene_obj.world().get<NavMeshComponent>(e);
        if (nm) {
            out << "  NavMesh: area(" << nm->area_x << "," << nm->area_y << "," << nm->area_z << ")"
                << " cell=" << nm->cell_size
                << " cell_height=" << nm->cell_height
                << " slope=" << nm->walkable_slope_deg
                << " climb=" << nm->walkable_climb
                << " headroom=" << nm->walkable_height
                << " min_area=" << nm->min_region_area
                << " agent_radius=" << nm->agent_radius
                << " jump_distance=" << nm->jump_distance
                << " jump_height=" << nm->jump_height
                << " max_verts=" << nm->max_verts_per_poly
                << " enabled=" << (nm->enabled ? "true" : "false") << "\n";
            // The authored off-mesh links (Phase 63), one line each, in order —
            // the order is the order A* considers them.
            if (nm != nullptr) {
                for (const NavLinkSpec& link : nm->links) {
                    out << "  NavLink: from=" << link.start_x << "," << link.start_y << ","
                        << link.start_z << " to=" << link.end_x << "," << link.end_y << ","
                        << link.end_z;
                    if (!link.bidirectional) {
                        out << " one_way=true";
                    }
                    out << "\n";
                }
            }
        }
        // The terrain (Phase 63): every field is written, because the component
        // exists only when the author asked for it.
        const auto* terrain = scene_obj.world().get<TerrainComponent>(e);
        if (terrain) {
            out << "  Terrain: res=" << terrain->resolution
                << " area=" << terrain->area
                << " height=" << terrain->height_scale
                << " freq=" << terrain->noise_frequency
                << " octaves=" << terrain->octaves
                << " seed=" << terrain->seed;
            if (!terrain->heightmap.empty()) {
                out << " heightmap=" << terrain->heightmap;
            }
            if (!terrain->material.empty()) {
                out << " material=" << terrain->material;
            }
            if (!terrain->enabled) {
                out << " enabled=false";
            }
            out << "\n";
        }
        const auto* agent = scene_obj.world().get<NavAgentComponent>(e);
        if (agent) {
            out << "  NavAgent: speed=" << agent->speed
                << " goal(" << agent->goal_x << "," << agent->goal_y << "," << agent->goal_z << ")"
                << " arrive=" << agent->arrive_radius
                << " enabled=" << (agent->enabled ? "true" : "false") << "\n";
        }
        // The patrol route (Phase 64): policy on one line, one destination per
        // line after it, so the order an author reads is the order the agent
        // walks. A scene without the component writes nothing, which is what
        // keeps its file byte for byte what it was before this phase.
        const auto* route = scene_obj.world().get<NavRouteComponent>(e);
        if (route) {
            out << "  NavRoute: wait=" << route->wait
                << " loop=" << (route->loop ? "true" : "false")
                << " ping_pong=" << (route->ping_pong ? "true" : "false")
                << " enabled=" << (route->enabled ? "true" : "false");
            // Phase 65 keys are written only when authored: a route that never
            // opted in keeps the exact Phase 64 line, byte for byte.
            if (route->hold != NavRouteHoldPolicy::Off) {
                out << " hold=" << nav_route_hold_name(route->hold);
            }
            if (route->resume > 0.0f) {
                out << " resume=" << route->resume;
            }
            // Phase 66: same rule — only a route that opted in writes the key.
            if (route->investigate > 0.0f) {
                out << " investigate=" << route->investigate;
            }
            // Phase 67: an empty squad is the solo route, so it writes nothing.
            if (!route->squad.empty()) {
                out << " squad=" << route->squad;
            }
            out << "\n";
            for (const NavPointSpec& nav_point : route->points) {
                out << "  NavPoint: pos=" << nav_point.x << "," << nav_point.y << "," << nav_point.z;
                // -1 is "use the route wait", and is what an absent key means.
                if (nav_point.wait >= 0.0f) {
                    out << " wait=" << nav_point.wait;
                }
                out << "\n";
            }
        }
        // Vehicle: config only — the four drive inputs are live gameplay state
        // and the JoltVehicle pointer is session state, so neither is scene
        // data (same rule as Character's wish_dir/jump/grounded).
        const auto* vehicle = scene_obj.world().get<physics::VehicleComponent>(e);
        if (vehicle) {
            const physics::JoltVehicleConfig& cfg = vehicle->config;
            out << "  Vehicle: chassis(" << cfg.chassis_half_extents.x << ","
                << cfg.chassis_half_extents.y << "," << cfg.chassis_half_extents.z << ")"
                << " mass=" << cfg.chassis_mass
                << " wheel_radius=" << cfg.wheel_radius
                << " wheel_width=" << cfg.wheel_width
                << " track=" << cfg.track_half_width
                << " wheelbase=" << cfg.wheelbase_half_length
                << " wheel_y=" << cfg.wheel_y
                << " steer_deg=" << cfg.max_steer_deg
                << " torque=" << cfg.engine_max_torque
                << " susp_min=" << cfg.suspension_min_length
                << " susp_max=" << cfg.suspension_max_length
                << " susp_freq=" << cfg.suspension_frequency_hz
                << " susp_damping=" << cfg.suspension_damping
                << " tire_peak=" << cfg.tire_peak_friction
                << " tire_limit=" << cfg.tire_limit_friction
                << " wheel_inertia=" << cfg.wheel_inertia
                << " angular_damping=" << cfg.angular_damping
                << " brake_torque=" << cfg.max_brake_torque
                << " handbrake_torque=" << cfg.max_handbrake_torque
                << " enabled=" << (vehicle->enabled ? "true" : "false") << "\n";
        }
    }
    return out.str();
}

bool save_scene_to_physical(const std::filesystem::path& physical_path, const scene::Scene& scene_obj, std::string& out_error) {
    const std::string text = serialize_scene_to_text(scene_obj);

    std::filesystem::path tmp = physical_path;
    tmp += ".tmp";
    std::error_code ec;
    std::filesystem::create_directories(tmp.parent_path(), ec);
    if (ec) { out_error = ec.message(); return false; }

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { out_error = "Failed to open file for writing: " + tmp.string(); return false; }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) { out_error = "Failed to write scene file"; std::filesystem::remove(tmp, ec); return false; }
    }

    std::filesystem::rename(tmp, physical_path, ec);
    if (ec) {
        std::filesystem::remove(physical_path, ec);
        std::filesystem::rename(tmp, physical_path, ec);
        if (ec) { out_error = ec.message(); std::filesystem::remove(tmp, ec); return false; }
    }
    return true;
}

bool save_scene_to_vfs(assets::VirtualFileSystem& vfs, const std::string& logical_path, const scene::Scene& scene_obj, std::string& out_error) {
    auto r = vfs.resolve(logical_path);
    if (!r.ok) { out_error = r.error; return false; }
    return save_scene_to_physical(r.value, scene_obj, out_error);
}

void copy_scene_entity(const ecs::World& src, ecs::Entity se, ecs::World& dst, ecs::Entity de) {
    if (const auto* t = src.get<scene::Transform>(se)) {
        dst.add<scene::Transform>(de, *t);
    }
    if (const auto* n = src.get<scene::NameComponent>(se)) {
        dst.add<scene::NameComponent>(de, *n);
    }
    if (const auto* p = src.get<scene::PrefabLinkComponent>(se)) {
        dst.add<scene::PrefabLinkComponent>(de, *p);
    }
    if (const auto* m = src.get<MeshComponent>(se)) {
        dst.add<MeshComponent>(de, *m);
    }
    if (const auto* dc = src.get<DecalComponent>(se)) {
        dst.add<DecalComponent>(de, *dc);
    }
    if (const auto* l = src.get<DirectionalLight>(se)) {
        dst.add<DirectionalLight>(de, *l);
    }
    if (const auto* pl = src.get<PointLightComponent>(se)) {
        dst.add<PointLightComponent>(de, *pl);
    }
    if (const auto* sl = src.get<SpotLightComponent>(se)) {
        dst.add<SpotLightComponent>(de, *sl);
    }
    if (const auto* s = src.get<SkyComponent>(se)) {
        dst.add<SkyComponent>(de, *s);
    }
    if (const auto* pp = src.get<PostProcessComponent>(se)) {
        dst.add<PostProcessComponent>(de, *pp);
    }
    if (const auto* fb = src.get<FeedbackComponent>(se)) {
        dst.add<FeedbackComponent>(de, *fb);
    }
    if (const auto* c = src.get<CameraComponent>(se)) {
        dst.add<CameraComponent>(de, *c);
    }
    if (const auto* wc = src.get<WeaponComponent>(se)) {
        // The profile is scene data; the state is rebuilt from it, so a copy
        // of a scene in the middle of a burst starts with a full magazine —
        // the same rule the physics body handle follows.
        dst.add<WeaponComponent>(de, WeaponComponent{wc->profile});
    }
    if (const auto* hc = src.get<HealthComponent>(se)) {
        HealthComponent fresh = *hc;
        // The quiet clock is session state: a copied entity has not been shot.
        fresh.since_damage = 0.0f;
        dst.add<HealthComponent>(de, fresh);
    }
    if (const auto* sc = src.get<StatusEffectComponent>(se)) {
        StatusEffectComponent fresh = *sc;
        // The source entity is a session handle; a copied effect is
        // environmental until a new hit names its shooter.
        for (u32 i = 0; i < fresh.effects.count; ++i) {
            fresh.effects.effects[i].source = ecs::kInvalidEntity;
        }
        dst.add<StatusEffectComponent>(de, fresh);
    }
    if (const auto* ca = src.get<CombatAgentComponent>(se)) {
        CombatAgentComponent fresh = *ca;
        // The aim between frames is session state: a copied agent starts
        // facing wherever its Transform faces, not where the original was
        // mid-turn.
        fresh.state = CombatAimState{};
        fresh.locked_target = ecs::kInvalidEntity;
        fresh.last_target_position = Vec3{0.0f, 0.0f, 0.0f};
        fresh.target_velocity = Vec3{0.0f, 0.0f, 0.0f};
        fresh.has_target_history = false;
        fresh.suppression = SuppressionState{};
        dst.add<CombatAgentComponent>(de, fresh);
    }
    if (const auto* fc = src.get<FactionComponent>(se)) {
        // Affiliation is entirely authored data: copying it copies the side.
        dst.add<FactionComponent>(de, *fc);
    }
    if (const auto* pc = src.get<PerceptionComponent>(se)) {
        // Senses are authored data, and the live meter lives in the Runtime, not
        // in the component — so a copy is a full copy.
        dst.add<PerceptionComponent>(de, *pc);
    }
    if (const auto* brain = src.get<BrainComponent>(se)) {
        // Considerations are authored data; the live decision lives in the
        // Runtime, so a copy is a full copy.
        dst.add<BrainComponent>(de, *brain);
    }
    if (const auto* machine = src.get<StateMachineComponent>(se)) {
        // States and edges are authored data on the same terms.
        dst.add<StateMachineComponent>(de, *machine);
    }
    if (const auto* rb = src.get<physics::RigidBodyComponent>(se)) {
        physics::RigidBodyComponent fresh = *rb;
        // A live body handle is per-session, never scene data: copying it
        // would alias two entities onto one body (or a dead one). The
        // runtime rebuilds bodies from the components after a merge/load.
        fresh.body = physics::BodyHandle{};
        dst.add<physics::RigidBodyComponent>(de, fresh);
    }
    if (const auto* col = src.get<physics::ColliderComponent>(se)) {
        dst.add<physics::ColliderComponent>(de, *col);
    }
    if (const auto* d = src.get<DestructibleComponent>(se)) {
        dst.add<DestructibleComponent>(de, *d);
    }
    if (const auto* a = src.get<animation::AnimationComponent>(se)) {
        dst.add<animation::AnimationComponent>(de, *a);
    }
    if (const auto* au = src.get<audio::AudioComponent>(se)) {
        dst.add<audio::AudioComponent>(de, *au);
    }
    if (const auto* rz = src.get<audio::ReverbZoneComponent>(se)) {
        dst.add<audio::ReverbZoneComponent>(de, *rz);
    }
    if (const auto* mus = src.get<audio::MusicComponent>(se)) {
        dst.add<audio::MusicComponent>(de, *mus);
    }
    if (const auto* amb = src.get<audio::AmbienceComponent>(se)) {
        dst.add<audio::AmbienceComponent>(de, *amb);
    }
    if (const auto* g = src.get<gameplay::GameplayModuleComponent>(se)) {
        dst.add<gameplay::GameplayModuleComponent>(de, *g);
    }
    if (const auto* s = src.get<scripting::ScriptComponent>(se)) {
        dst.add<scripting::ScriptComponent>(de, *s);
    }
    if (const auto* p = src.get<vfx::ParticleComponent>(se)) {
        dst.add<vfx::ParticleComponent>(de, *p);
    }
    if (const auto* c = src.get<physics::ClothComponent>(se)) {
        dst.add<physics::ClothComponent>(de, *c);
    }
    if (const auto* ch = src.get<physics::CharacterComponent>(se)) {
        // Live input/state (wish_dir/jump/grounded) is per-session, never
        // scene data: a merge that carried them would teleport intent.
        physics::CharacterComponent fresh = *ch;
        fresh.wish_dir = Vec3{0.0f, 0.0f, 0.0f};
        fresh.jump = false;
        fresh.grounded = false;
        dst.add<physics::CharacterComponent>(de, fresh);
    }
    if (const auto* nm = src.get<NavMeshComponent>(se)) {
        dst.add<NavMeshComponent>(de, *nm);
    }
    if (const auto* terrain = src.get<TerrainComponent>(se)) {
        // Pure scene data: the runtime rebuilds the mesh and the sampler from
        // the component on adopt, so a copy is a full copy.
        dst.add<TerrainComponent>(de, *terrain);
    }
    if (const auto* na = src.get<NavAgentComponent>(se)) {
        // The live path is per-session state the runtime keeps beside the
        // component, so there is nothing to scrub here — the component itself
        // is pure configuration plus the authored goal.
        dst.add<NavAgentComponent>(de, *na);
    }
    if (const auto* route = src.get<NavRouteComponent>(se)) {
        // Points are authored data; the live point index/direction/wait live
        // beside the component in the runtime, never inside it.
        dst.add<NavRouteComponent>(de, *route);
    }
    if (const auto* vc = src.get<physics::VehicleComponent>(se)) {
        // Live inputs (throttle/steer/brake/handbrake) are per-session, and
        // the JoltVehicle view points at another world's simulation: a merge
        // that carried either would drive a fresh car with a stale foot.
        physics::VehicleComponent fresh = *vc;
        fresh.throttle = 0.0f;
        fresh.steer = 0.0f;
        fresh.brake = 0.0f;
        fresh.handbrake = 0.0f;
        fresh.vehicle = nullptr;
        dst.add<physics::VehicleComponent>(de, fresh);
    }
}

namespace {

std::vector<ecs::Entity> collect_members(const ecs::World& world, ecs::Entity root) {
    std::vector<ecs::Entity> out;
    if (!root.valid() || !world.is_alive(root)) {
        return out;
    }
    // Iterative pre-order DFS (parent before children).
    std::vector<ecs::Entity> stack{root};
    while (!stack.empty()) {
        ecs::Entity cur = stack.back();
        stack.pop_back();
        if (!world.is_alive(cur)) {
            continue;
        }
        out.push_back(cur);
        auto kids = scene::get_children(world, cur);
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
            stack.push_back(*it);
        }
    }
    return out;
}

} // namespace

SceneMergeResult merge_scene_into_world(assets::VirtualFileSystem& vfs, const std::string& logical_path,
                                        ecs::World& dst_world) {
    SceneMergeResult out;
    SceneLoadResult loaded = load_scene_from_vfs(vfs, logical_path);
    if (!loaded.success || !loaded.scene) {
        out.error = loaded.error.empty() ? ("Cannot merge scene '" + logical_path + "'") : loaded.error;
        return out;
    }
    out.created = merge_loaded_scene_into_world(*loaded.scene, dst_world);
    out.warnings = loaded.warnings;
    for (const std::string& missing : loaded.missing_assets) {
        out.warnings.push_back("Missing asset: " + missing);
    }
    out.success = true;
    return out;
}

std::vector<ecs::Entity> merge_loaded_scene_into_world(scene::Scene& chunk, ecs::World& dst_world) {
    std::vector<ecs::Entity> created;
    const ecs::World& src = chunk.world();

    // Roots: entities with no live parent in the chunk (same rule as prefab
    // templates). Internal hierarchy is remapped below; chunk roots become
    // parentless members of the live world.
    std::vector<ecs::Entity> roots;
    for (ecs::Entity e : src.all_entities()) {
        const auto* t = src.get<scene::Transform>(e);
        if (t == nullptr || !t->parent.valid() || !src.is_alive(t->parent)) {
            roots.push_back(e);
        }
    }

    std::unordered_map<u32, ecs::Entity> remap;
    for (ecs::Entity root : roots) {
        const std::vector<ecs::Entity> members = collect_members(src, root);
        if (members.empty()) {
            continue;
        }
        ecs::Entity new_root;
        bool first = true;
        for (ecs::Entity se : members) {
            ecs::Entity de = dst_world.create_entity();
            remap[se.id] = de;
            copy_scene_entity(src, se, dst_world, de);
            // Chunk roots start parentless (their file parent, if any, did
            // not survive the liveness check above); the remap pass below
            // only rewires members below their own root.
            if (auto* dt = dst_world.get<scene::Transform>(de)) {
                dt->parent = ecs::kInvalidEntity;
                dt->dirty = true;
            }
            if (first) {
                new_root = de;
                first = false;
            }
        }
        for (ecs::Entity se : members) {
            if (se == root) {
                continue;
            }
            const auto* st = src.get<scene::Transform>(se);
            if (st == nullptr || !st->parent.valid()) {
                continue;
            }
            const auto it = remap.find(st->parent.id);
            if (it != remap.end()) {
                scene::set_parent(dst_world, remap[se.id], it->second);
            }
        }
        if (new_root.valid()) {
            created.push_back(new_root);
        }
    }

    scene::propagate_transforms(dst_world);
    return created;
}

} // namespace nf::runtime
