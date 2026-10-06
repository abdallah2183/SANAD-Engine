#define _CRT_SECURE_NO_WARNINGS
#include <NF/Runtime/RuntimeSceneLoader.hpp>
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
static std::string field_value(const std::string& line, const char* key) {
    const size_t p = line.find(key);
    if (p == std::string::npos) return {};
    const size_t start = p + std::strlen(key);
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
    if (!std::getline(iss, line) || line.rfind("# NOVAForge Scene",0)!=0) { result.error = "Invalid scene header"; return result; }
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
                if (id_pos != std::string::npos) {
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
    out << "# NOVAForge Scene v1\n";
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
                << " motion_length=" << pp->motion_max_length;
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
            out << "\n";
        }
        const auto* c = scene_obj.world().get<CameraComponent>(e);
        if (c) {
            out << "  Camera: fov=" << c->fov_y << " aspect=" << c->aspect << " near=" << c->near_plane << " far=" << c->far_plane << " active=" << (c->is_active ? "true" : "false") << "\n";
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
    if (const auto* c = src.get<CameraComponent>(se)) {
        dst.add<CameraComponent>(de, *c);
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
