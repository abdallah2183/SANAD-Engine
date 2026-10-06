#include <NF/Editor/Inspector.hpp>
#include <NF/Editor/Commands.hpp>
#include <NF/Assets/AssetId.hpp>
#include <NF/Rendering/ShadowCascades.hpp>
#include <NF/Runtime/Runtime.hpp>

#include <cmath>
#include <string>

namespace nf::editor {

namespace {

bool all_finite(std::initializer_list<float> vs) {
    for (float v : vs) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return true;
}

// Strict "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" hex check. The engine's
// UUID::from_string is intentionally lenient (skips stray characters), so an
// explicit shape check is required to reject garbage like "not-a-uuid".
bool strict_uuid_shape(const std::string& s) {
    if (s.size() != 36) {
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        const bool dash = (i == 8 || i == 13 || i == 18 || i == 23);
        const char c = s[i];
        if (dash) {
            if (c != '-') {
                return false;
            }
        } else {
            const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (!hex) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

TransformEdit read_transform(const ecs::World& world, ecs::Entity e) {
    TransformEdit out;
    const auto* t = world.get<scene::Transform>(e);
    if (t == nullptr) {
        return out;
    }
    out.px = t->local_x;
    out.py = t->local_y;
    out.pz = t->local_z;
    out.rx = t->rot_x;
    out.ry = t->rot_y;
    out.rz = t->rot_z;
    out.sx = t->scale_x;
    out.sy = t->scale_y;
    out.sz = t->scale_z;
    return out;
}

CameraEdit read_camera(const ecs::World& world, ecs::Entity e, bool& out_has) {
    CameraEdit out;
    const auto* c = world.get<runtime::CameraComponent>(e);
    out_has = (c != nullptr);
    if (c == nullptr) {
        return out;
    }
    out.active = c->is_active;
    out.fov_y = c->fov_y;
    out.near_plane = c->near_plane;
    out.far_plane = c->far_plane;
    return out;
}

LightEdit read_light(const ecs::World& world, ecs::Entity e, bool& out_has) {
    LightEdit out;
    const auto* l = world.get<runtime::DirectionalLight>(e);
    out_has = (l != nullptr);
    if (l == nullptr) {
        return out;
    }
    out.dir_x = l->dir_x;
    out.dir_y = l->dir_y;
    out.dir_z = l->dir_z;
    out.color_r = l->color_r;
    out.color_g = l->color_g;
    out.color_b = l->color_b;
    out.intensity = l->intensity;
    out.cast_shadows = l->cast_shadows;
    out.shadow_strength = l->shadow_strength;
    out.shadow_bias = l->shadow_bias;
    out.shadow_cascades = static_cast<int>(l->shadow_cascades);
    out.shadow_distance = l->shadow_distance;
    return out;
}

std::unique_ptr<ICommand> make_transform_command(ecs::World& world, ecs::Entity e,
                                                 const TransformEdit& edit, std::string& out_err) {
    if (!e.valid() || !world.is_alive(e)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    auto* t = world.get<scene::Transform>(e);
    if (t == nullptr) {
        out_err = "Entity has no Transform";
        return nullptr;
    }
    if (!all_finite({edit.px, edit.py, edit.pz, edit.rx, edit.ry, edit.rz, edit.sx, edit.sy,
                     edit.sz})) {
        out_err = "Transform values must be finite numbers";
        return nullptr;
    }
    if (edit.sx == 0.0f || edit.sy == 0.0f || edit.sz == 0.0f) {
        out_err = "Scale components must be non-zero";
        return nullptr;
    }
    // Clamp absurd magnitudes instead of corrupting the scene.
    auto clamp_pos = [](float v) { return (v > 100000.0f) ? 100000.0f : ((v < -100000.0f) ? -100000.0f : v); };
    scene::Transform after = *t;
    after.local_x = clamp_pos(edit.px);
    after.local_y = clamp_pos(edit.py);
    after.local_z = clamp_pos(edit.pz);
    after.rot_x = edit.rx;
    after.rot_y = edit.ry;
    after.rot_z = edit.rz;
    after.scale_x = edit.sx;
    after.scale_y = edit.sy;
    after.scale_z = edit.sz;
    return std::make_unique<SetTransformCommand>(e, *t, after);
}

std::unique_ptr<ICommand> make_camera_command(ecs::World& world, ecs::Entity e,
                                              const CameraEdit& edit, std::string& out_err) {
    if (!e.valid() || !world.is_alive(e)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    if (!all_finite({edit.fov_y, edit.near_plane, edit.far_plane})) {
        out_err = "Camera values must be finite numbers";
        return nullptr;
    }
    if (edit.fov_y < 1.0f || edit.fov_y > 179.0f) {
        out_err = "FOV must be within [1, 179] degrees";
        return nullptr;
    }
    if (edit.near_plane <= 0.0f) {
        out_err = "Near plane must be positive";
        return nullptr;
    }
    if (edit.far_plane <= edit.near_plane) {
        out_err = "Far plane must be greater than near plane";
        return nullptr;
    }
    const auto* cur = world.get<runtime::CameraComponent>(e);
    const bool had = (cur != nullptr);
    const runtime::CameraComponent before = had ? *cur : runtime::CameraComponent{};
    runtime::CameraComponent after = before;
    after.is_active = edit.active;
    after.fov_y = edit.fov_y;
    after.near_plane = edit.near_plane;
    after.far_plane = edit.far_plane;
    return std::make_unique<SetCameraCommand>(e, had, before, after);
}

std::unique_ptr<ICommand> make_light_command(ecs::World& world, ecs::Entity e,
                                             const LightEdit& edit, std::string& out_err) {
    if (!e.valid() || !world.is_alive(e)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    if (!all_finite({edit.dir_x, edit.dir_y, edit.dir_z, edit.color_r, edit.color_g, edit.color_b,
                      edit.intensity, edit.shadow_strength, edit.shadow_bias})) {
        out_err = "Light values must be finite numbers";
        return nullptr;
    }
    const float len2 = edit.dir_x * edit.dir_x + edit.dir_y * edit.dir_y + edit.dir_z * edit.dir_z;
    if (len2 < 1e-12f) {
        out_err = "Light direction must be non-zero";
        return nullptr;
    }
    if (edit.intensity < 0.0f) {
        out_err = "Light intensity must be non-negative";
        return nullptr;
    }
    if (edit.color_r < 0.0f || edit.color_r > 1.0f || edit.color_g < 0.0f || edit.color_g > 1.0f ||
        edit.color_b < 0.0f || edit.color_b > 1.0f) {
        out_err = "Light color channels must be within [0, 1]";
        return nullptr;
    }
    if (edit.shadow_strength < 0.0f || edit.shadow_strength > 1.0f) {
        out_err = "Shadow strength must be within [0, 1]";
        return nullptr;
    }
    if (edit.shadow_bias < 0.0f || edit.shadow_bias > 0.01f) {
        out_err = "Shadow bias must be within [0, 0.01]";
        return nullptr;
    }
    // The cascade count indexes a fixed 2x2 atlas, so anything outside
    // [1, kMaxShadowCascades] has nowhere to render. Rejected here rather than
    // passed on for the renderer to clamp: a count the artist cannot achieve
    // should say so, not quietly render as some other number.
    if (edit.shadow_cascades < 1 ||
        edit.shadow_cascades > static_cast<int>(rendering::kMaxShadowCascades)) {
        out_err = "Shadow cascades must be within [1, " +
                  std::to_string(rendering::kMaxShadowCascades) + "]";
        return nullptr;
    }
    // 0 is the documented "cast to the camera's far plane" sentinel, so only
    // negative distances are invalid.
    if (edit.shadow_distance < 0.0f) {
        out_err = "Shadow distance must be non-negative";
        return nullptr;
    }
    const auto* cur = world.get<runtime::DirectionalLight>(e);
    const bool had = (cur != nullptr);
    const runtime::DirectionalLight before = had ? *cur : runtime::DirectionalLight{};
    runtime::DirectionalLight after = before;
    after.dir_x = edit.dir_x;
    after.dir_y = edit.dir_y;
    after.dir_z = edit.dir_z;
    after.color_r = edit.color_r;
    after.color_g = edit.color_g;
    after.color_b = edit.color_b;
    after.intensity = edit.intensity;
    after.cast_shadows = edit.cast_shadows;
    after.shadow_strength = edit.shadow_strength;
    after.shadow_bias = edit.shadow_bias;
    after.shadow_cascades = static_cast<u32>(edit.shadow_cascades);
    after.shadow_distance = edit.shadow_distance;
    return std::make_unique<SetLightCommand>(e, had, before, after);
}

SkyEdit read_sky(const ecs::World& world, ecs::Entity e, bool& out_has) {
    SkyEdit out;
    const auto* s = world.get<runtime::SkyComponent>(e);
    out_has = (s != nullptr);
    if (s == nullptr) {
        return out;
    }
    out.zenith[0] = s->zenith_r;
    out.zenith[1] = s->zenith_g;
    out.zenith[2] = s->zenith_b;
    out.horizon[0] = s->horizon_r;
    out.horizon[1] = s->horizon_g;
    out.horizon[2] = s->horizon_b;
    out.ground[0] = s->ground_r;
    out.ground[1] = s->ground_g;
    out.ground[2] = s->ground_b;
    out.clear[0] = s->clear_r;
    out.clear[1] = s->clear_g;
    out.clear[2] = s->clear_b;
    out.sun_disk = s->sun_disk;
    out.sun_glow = s->sun_glow;
    out.enabled = s->enabled;
    return out;
}

std::unique_ptr<ICommand> make_sky_command(ecs::World& world, ecs::Entity e,
                                           const SkyEdit& edit, std::string& out_err) {
    if (!e.valid() || !world.is_alive(e)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    if (!all_finite({edit.zenith[0], edit.zenith[1], edit.zenith[2], edit.horizon[0],
                      edit.horizon[1], edit.horizon[2], edit.ground[0], edit.ground[1],
                      edit.ground[2], edit.clear[0], edit.clear[1], edit.clear[2],
                      edit.sun_disk, edit.sun_glow})) {
        out_err = "Sky values must be finite numbers";
        return nullptr;
    }
    for (float c : {edit.zenith[0], edit.zenith[1], edit.zenith[2], edit.horizon[0],
                    edit.horizon[1], edit.horizon[2], edit.ground[0], edit.ground[1],
                    edit.ground[2], edit.clear[0], edit.clear[1], edit.clear[2]}) {
        if (c < 0.0f || c > 4.0f) {
            out_err = "Sky colors must be within [0, 4] (HDR headroom allowed)";
            return nullptr;
        }
    }
    if (edit.sun_disk < 0.0f || edit.sun_disk > 8.0f) {
        out_err = "Sun disk multiplier must be within [0, 8]";
        return nullptr;
    }
    if (edit.sun_glow < 0.0f || edit.sun_glow > 8.0f) {
        out_err = "Sun glow multiplier must be within [0, 8]";
        return nullptr;
    }
    const auto* cur = world.get<runtime::SkyComponent>(e);
    const bool had = (cur != nullptr);
    const runtime::SkyComponent before = had ? *cur : runtime::SkyComponent{};
    runtime::SkyComponent after = before;
    after.zenith_r = edit.zenith[0];
    after.zenith_g = edit.zenith[1];
    after.zenith_b = edit.zenith[2];
    after.horizon_r = edit.horizon[0];
    after.horizon_g = edit.horizon[1];
    after.horizon_b = edit.horizon[2];
    after.ground_r = edit.ground[0];
    after.ground_g = edit.ground[1];
    after.ground_b = edit.ground[2];
    after.clear_r = edit.clear[0];
    after.clear_g = edit.clear[1];
    after.clear_b = edit.clear[2];
    after.sun_disk = edit.sun_disk;
    after.sun_glow = edit.sun_glow;
    after.enabled = edit.enabled;
    return std::make_unique<SetSkyCommand>(e, had, before, after);
}

TimeOfDayEdit read_time_of_day(const ecs::World& world, ecs::Entity e, bool& out_has) {
    TimeOfDayEdit out;
    const auto* t = world.get<runtime::TimeOfDayComponent>(e);
    out_has = (t != nullptr);
    if (t == nullptr) {
        return out;
    }
    out.time_hours = t->time_hours;
    out.day_length_seconds = t->day_length_seconds;
    out.enabled = t->enabled;
    out.drive_light = t->drive_light;
    return out;
}

std::unique_ptr<ICommand> make_time_of_day_command(ecs::World& world, ecs::Entity e,
                                                   const TimeOfDayEdit& edit,
                                                   std::string& out_err) {
    if (!e.valid() || !world.is_alive(e)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    if (!all_finite({edit.time_hours, edit.day_length_seconds})) {
        out_err = "Day/night values must be finite numbers";
        return nullptr;
    }
    // A negative day length is normalised to 0 (frozen) rather than rejected,
    // matching the scene loader: it is an obvious way to write "stop the clock",
    // and rejecting the edit would be the larger surprise. A non-finite value is
    // refused above, because that means the widget produced something broken.
    float day_length = edit.day_length_seconds;
    if (day_length < 0.0f) {
        day_length = 0.0f;
    }
    // Wrapped, not clamped, and for the same reason the loader wraps: 25.0 means
    // 1am. A clamp would silently turn an author's "one hour later" into 24:00.
    float hours = std::fmod(edit.time_hours, 24.0f);
    if (hours < 0.0f) {
        hours += 24.0f;
    }

    const auto* cur = world.get<runtime::TimeOfDayComponent>(e);
    const bool had = (cur != nullptr);
    const runtime::TimeOfDayComponent before = had ? *cur : runtime::TimeOfDayComponent{};
    runtime::TimeOfDayComponent after = before;
    after.time_hours = hours;
    after.day_length_seconds = day_length;
    after.enabled = edit.enabled;
    after.drive_light = edit.drive_light;
    return std::make_unique<SetTimeOfDayCommand>(e, had, before, after);
}

runtime::PostProcessComponent read_post_process(const ecs::World& world, ecs::Entity e,
                                                bool& out_has) {
    const auto* pp = world.get<runtime::PostProcessComponent>(e);
    out_has = (pp != nullptr);
    return pp != nullptr ? *pp : runtime::PostProcessComponent{};
}

std::unique_ptr<ICommand> make_post_process_command(ecs::World& world, ecs::Entity e,
                                                    const runtime::PostProcessComponent& edit,
                                                    std::string& out_err) {
    if (!e.valid() || !world.is_alive(e)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    if (!all_finite({edit.bloom_threshold, edit.bloom_knee, edit.bloom_intensity,
                     edit.bloom_radius, edit.grade_contrast, edit.grade_pivot,
                     edit.grade_temperature, edit.grade_tint, edit.grade_gamma,
                     edit.sharpen_amount, edit.sharpen_radius, edit.saturation,
                     edit.vignette})) {
        out_err = "Post-processing values must be finite numbers";
        return nullptr;
    }
    // The ranges match the scene loader's, deliberately: a value the editor
    // accepts and the loader then refuses would make the same scene load
    // differently depending on how it was authored, which is the worst of both.
    // They are wide where the renderer tolerates an extreme (threshold 0 means
    // "everything blooms", knee 0 is a hard cut) and tight only where the value
    // would be nonsense.
    const auto in_range = [&out_err](const char* name, float v, float lo, float hi) {
        if (v < lo || v > hi) {
            out_err = std::string(name) + " must be within [" + std::to_string(lo) + ", " +
                      std::to_string(hi) + "]";
            return false;
        }
        return true;
    };
    if (!in_range("bloom threshold", edit.bloom_threshold, 0.0f, 1000.0f)) return nullptr;
    if (!in_range("bloom knee", edit.bloom_knee, 0.0f, 1000.0f)) return nullptr;
    if (!in_range("bloom intensity", edit.bloom_intensity, 0.0f, 100.0f)) return nullptr;
    // A radius at or below zero collapses all thirteen taps onto one texel.
    if (!in_range("bloom radius", edit.bloom_radius, 0.01f, 16.0f)) return nullptr;
    if (!in_range("grade contrast", edit.grade_contrast, 0.0f, 16.0f)) return nullptr;
    if (!in_range("grade pivot", edit.grade_pivot, 0.0f, 1000.0f)) return nullptr;
    if (!in_range("grade temperature", edit.grade_temperature, -1.0f, 1.0f)) return nullptr;
    if (!in_range("grade tint", edit.grade_tint, -1.0f, 1.0f)) return nullptr;
    if (!in_range("grade gamma", edit.grade_gamma, 0.05f, 8.0f)) return nullptr;
    if (!in_range("sharpen amount", edit.sharpen_amount, 0.0f, 8.0f)) return nullptr;
    if (!in_range("sharpen radius", edit.sharpen_radius, 0.01f, 16.0f)) return nullptr;
    if (!in_range("saturation", edit.saturation, 0.0f, 8.0f)) return nullptr;
    if (!in_range("vignette", edit.vignette, 0.0f, 1.0f)) return nullptr;
    // Lens effects. Symmetric on distortion because barrel and pincushion are
    // both legitimate, and bounded at +/-0.5 because past that the warp folds
    // the corners over themselves. The bounds match the scene loader's.
    if (!in_range("lens distortion", edit.lens_distortion, -0.5f, 0.5f)) return nullptr;
    if (!in_range("lens chromatic aberration", edit.lens_chromatic_aberration, 0.0f, 0.1f)) {
        return nullptr;
    }
    // Depth of field. A focus distance is unbounded above on purpose (focusing
    // on the sky is "nothing is out of focus"), and the range has a real floor
    // because the ramp divides by it. The bounds match the scene loader's.
    if (!in_range("dof focus distance", edit.dof_focus_distance, 0.0f, 1.0e30f)) return nullptr;
    if (!in_range("dof focus range", edit.dof_focus_range, 0.01f, 1.0e6f)) return nullptr;
    if (!in_range("dof radius", edit.dof_max_radius, 0.0f, 64.0f)) return nullptr;
    // Motion blur. A negative intensity is not "blur the other way" and a zero
    // cap would mean the same thing as a zero intensity, so both have floors.
    if (!in_range("motion intensity", edit.motion_intensity, 0.0f, 16.0f)) return nullptr;
    if (!in_range("motion max length", edit.motion_max_length, 0.001f, 1.0f)) return nullptr;
    // Exposure is opt-in (0 = the renderer's own), and the tonemap operator is
    // a closed set whose -1 means "not authored". Bounds match the scene
    // loader's, so the editor and the loader agree on what a scene may say.
    if (!in_range("exposure", edit.exposure, 0.0f, 1000.0f)) return nullptr;
    if (edit.tonemap < -1 || edit.tonemap > 3) {
        out_err = "tonemap must be -1 (not set) or a known operator";
        return nullptr;
    }
    // The LUT strength is a blend weight, so it is bounded by definition. The
    // path itself is not validated here: the runtime resolves it through the
    // VFS and warns when it is unusable, which is the only place that knows
    // whether the file exists.
    if (!in_range("lut strength", edit.lut_strength, 0.0f, 1.0f)) return nullptr;

    const auto* cur = world.get<runtime::PostProcessComponent>(e);
    const bool had = (cur != nullptr);
    const runtime::PostProcessComponent before = had ? *cur : runtime::PostProcessComponent{};
    runtime::PostProcessComponent after = edit;
    return std::make_unique<SetPostProcessCommand>(e, had, before, after);
}

std::unique_ptr<ICommand> make_reverb_zone_command(ecs::World& world, ecs::Entity e,
                                                   const audio::ReverbZoneComponent& edit,
                                                   std::string& out_err) {
    if (!e.valid() || !world.is_alive(e)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    if (!all_finite({edit.radius, edit.inner_radius, edit.wet_gain, edit.decay_seconds,
                     edit.pre_delay_seconds, edit.echo_spacing_seconds})) {
        out_err = "Reverb zone values must be finite numbers";
        return nullptr;
    }
    // The bounds mirror the scene loader's, deliberately: a value the editor
    // accepts and the loader then refuses would make the same scene load
    // differently depending on how it was authored.
    //
    // A radius at or below zero is REFUSED rather than clamped: the loader
    // drops such a line with a warning, so clamping here would turn a mistake
    // into a silent, differently-authored scene.
    if (edit.radius <= 0.0f) {
        out_err = "Reverb zone radius must be greater than zero";
        return nullptr;
    }
    if (edit.inner_radius < 0.0f || edit.inner_radius > edit.radius) {
        out_err = "Reverb zone inner radius must be within [0, radius]";
        return nullptr;
    }
    if (edit.wet_gain < 0.0f || edit.wet_gain > 1.0f) {
        out_err = "Reverb zone wet gain must be within [0, 1]";
        return nullptr;
    }
    if (edit.decay_seconds < 0.0f || edit.pre_delay_seconds < 0.0f ||
        edit.echo_spacing_seconds < 0.0f) {
        out_err = "Reverb zone times must not be negative";
        return nullptr;
    }

    const auto* cur = world.get<audio::ReverbZoneComponent>(e);
    const bool had = (cur != nullptr);
    const audio::ReverbZoneComponent before = had ? *cur : audio::ReverbZoneComponent{};
    return std::make_unique<SetReverbZoneCommand>(e, had, before, edit);
}

std::unique_ptr<ICommand> make_music_command(ecs::World& world, ecs::Entity e,
                                             const audio::MusicComponent& after,
                                             std::string& out_err) {
    if (!e.valid() || !world.is_alive(e)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    if (after.buffer_name.empty()) {
        out_err = "Music needs a buffer path";
        return nullptr;
    }
    if (!all_finite({after.volume, after.fade_in_seconds}) || after.volume < 0.0f ||
        after.volume > 1.0f || after.fade_in_seconds < 0.0f) {
        out_err = "Music volume must be within [0, 1] and the fade must not be negative";
        return nullptr;
    }
    // `owned_buffer` is taken as given: the caller resolved it (EditorApp, which
    // owns the VFS) or carried it over from the live component. Clearing it here
    // would silence a track that is already decoded.
    const auto* cur = world.get<audio::MusicComponent>(e);
    const bool had = (cur != nullptr);
    const audio::MusicComponent before = had ? *cur : audio::MusicComponent{};
    return std::make_unique<SetMusicCommand>(e, had, before, after);
}

std::unique_ptr<ICommand> make_ambience_command(ecs::World& world, ecs::Entity e,
                                                const audio::AmbienceComponent& after,
                                                std::string& out_err) {
    if (!e.valid() || !world.is_alive(e)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    if (after.buffer_name.empty()) {
        out_err = "Ambience needs a buffer path";
        return nullptr;
    }
    if (!all_finite({after.fade_in_seconds}) || after.fade_in_seconds < 0.0f) {
        out_err = "Ambience fade must not be negative";
        return nullptr;
    }
    const auto* cur = world.get<audio::AmbienceComponent>(e);
    const bool had = (cur != nullptr);
    const audio::AmbienceComponent before = had ? *cur : audio::AmbienceComponent{};
    return std::make_unique<SetAmbienceCommand>(e, had, before, after);
}

std::unique_ptr<ICommand> make_mesh_command(ecs::World& world, ecs::Entity e,
                                            const std::string& asset_id_text,
                                            const std::string& material, std::string& out_err) {
    if (!e.valid() || !world.is_alive(e)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    if (!strict_uuid_shape(asset_id_text)) {
        out_err = "Invalid AssetId '" + asset_id_text + "' (expected xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)";
        return nullptr;
    }
    const assets::AssetId id = assets::AssetId::from_string(asset_id_text);
    if (!id.valid()) {
        out_err = "Invalid AssetId '" + asset_id_text + "'";
        return nullptr;
    }
    const auto* cur = world.get<runtime::MeshComponent>(e);
    const bool had = (cur != nullptr);
    const runtime::MeshComponent before = had ? *cur : runtime::MeshComponent{};
    runtime::MeshComponent after;
    after.mesh_id = id;
    after.material = material;
    return std::make_unique<SetMeshCommand>(e, had, before, after);
}

std::unique_ptr<ICommand> make_rename_command(ecs::World& world, ecs::Entity e,
                                              const std::string& new_name, std::string& out_err) {
    if (!e.valid() || !world.is_alive(e)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    if (new_name.size() > 128) {
        out_err = "Name is too long (max 128 characters)";
        return nullptr;
    }
    for (char c : new_name) {
        if (c == '\n' || c == '\r') {
            out_err = "Name must not contain newlines";
            return nullptr;
        }
    }
    auto cmd = std::make_unique<RenameCommand>(e, new_name);
    if (!cmd->capture_old(world)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    return cmd;
}

std::unique_ptr<ICommand> make_material_assignment_command(ecs::World& world, ecs::Entity e,
                                                           const std::string& after_path,
                                                           std::string& out_err) {
    if (!e.valid() || !world.is_alive(e)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    const auto* mc = world.get<runtime::MeshComponent>(e);
    if (mc == nullptr) {
        out_err = "Entity has no MeshComponent to assign a material to";
        return nullptr;
    }
    if (after_path.size() > 256) {
        out_err = "Material path is too long (max 256 characters)";
        return nullptr;
    }
    return std::make_unique<SetMaterialAssignmentCommand>(e, mc->material, after_path);
}

rendering::PBRMaterialParams read_material_params(runtime::Runtime& runtime, const std::string& path) {
    rendering::PBRMaterialParams params{};
    // Best effort: unknown paths resolve to defaults, never garbage.
    runtime.material_params(path, params);
    return params;
}

std::unique_ptr<ICommand> make_material_params_command(runtime::Runtime& runtime,
                                                       const std::string& path,
                                                       const MaterialEdit& edit, std::string& out_err) {
    if (!all_finite({edit.base_color[0], edit.base_color[1], edit.base_color[2], edit.base_color[3],
                     edit.metallic, edit.roughness, edit.ao, edit.emission[0], edit.emission[1],
                     edit.emission[2], edit.emission_strength})) {
        out_err = "Material values must be finite numbers";
        return nullptr;
    }
    for (int i = 0; i < 3; ++i) {
        if (edit.base_color[i] < 0.0f || edit.base_color[i] > 1.0f) {
            out_err = "Base color channels must be within [0, 1]";
            return nullptr;
        }
    }
    if (edit.metallic < 0.0f || edit.metallic > 1.0f) {
        out_err = "Metallic must be within [0, 1]";
        return nullptr;
    }
    if (edit.roughness < 0.0f || edit.roughness > 1.0f) {
        out_err = "Roughness must be within [0, 1]";
        return nullptr;
    }
    if (edit.ao < 0.0f || edit.ao > 1.0f) {
        out_err = "AO must be within [0, 1]";
        return nullptr;
    }
    for (int i = 0; i < 3; ++i) {
        if (edit.emission[i] < 0.0f || edit.emission[i] > 1.0f) {
            out_err = "Emission channels must be within [0, 1]";
            return nullptr;
        }
    }
    if (edit.emission_strength < 0.0f || edit.emission_strength > 8.0f) {
        out_err = "Emission strength must be within [0, 8]";
        return nullptr;
    }
    rendering::PBRMaterialParams before{};
    runtime.material_params(path, before);
    rendering::PBRMaterialParams after = before;
    for (int i = 0; i < 4; ++i) {
        after.base_color[i] = edit.base_color[i];
    }
    after.metallic = edit.metallic;
    after.roughness = edit.roughness;
    after.ao = edit.ao;
    for (int i = 0; i < 3; ++i) {
        after.emission[i] = edit.emission[i];
    }
    after.emission_strength = edit.emission_strength;
    // NOTE: use_base_color_texture is owned by the albedo binding, never by
    // scalar edits — copying `before` preserves it.
    return std::make_unique<SetMaterialParamsCommand>(&runtime, path, before, after);
}

std::unique_ptr<ICommand> make_material_albedo_command(runtime::Runtime& runtime,
                                                       const std::string& material_path,
                                                       const std::string& before_tex,
                                                       const std::string& after_tex) {
    return std::make_unique<SetMaterialAlbedoCommand>(&runtime, material_path, before_tex, after_tex);
}

std::unique_ptr<ICommand> make_material_mip_command(runtime::Runtime& runtime,
                                                    const std::string& material_path,
                                                    rhi::MipMapMode after_mode,
                                                    std::string& out_err) {
    const int idx = static_cast<int>(after_mode);
    if (idx < 0 || idx > 2) {
        out_err = "Invalid mip filter mode";
        return nullptr;
    }
    const rhi::MipMapMode before = runtime.material_mip_mode(material_path);
    return std::make_unique<SetMaterialMipModeCommand>(&runtime, material_path, before, after_mode);
}

} // namespace nf::editor
