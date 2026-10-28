#include <NF/Runtime/SceneValidator.hpp>

#include <NF/Rendering/DynamicResolution.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Rendering/RenderScale.hpp>
#include <NF/Runtime/Factions.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Scripting/ScriptEngine.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <sstream>

namespace nf::runtime {

const char* validation_severity_name(ValidationSeverity severity) {
    switch (severity) {
    case ValidationSeverity::Info: return "info";
    case ValidationSeverity::Warning: return "warning";
    case ValidationSeverity::Error: return "error";
    }
    return "info";
}

void ValidationReport::add(ValidationSeverity severity, std::string entity, std::string message) {
    issues.push_back(ValidationIssue{severity, std::move(entity), std::move(message)});
    switch (severity) {
    case ValidationSeverity::Error: ++errors; break;
    case ValidationSeverity::Warning: ++warnings; break;
    case ValidationSeverity::Info: ++infos; break;
    }
}

/// A stable label for a report line: the entity's name, or its id when it has
/// none. An unnamed entity still has to be findable in the scene.
std::string entity_label(const ecs::World& world, ecs::Entity e) {
    if (const auto* n = world.get<scene::NameComponent>(e); n != nullptr && !n->name.empty()) {
        return n->name;
    }
    return "entity #" + std::to_string(e.id);
}

namespace {

/// A finite, in-range number. `false` means the value is unusable, and every
/// rule below turns that into a message naming the field rather than silently
/// skipping the entity — a validator that skips is a validator that lies.
bool sane(f32 v, f32 lo, f32 hi) {
    return std::isfinite(v) && v >= lo && v <= hi;
}

} // namespace

ValidationReport validate_scene(const scene::Scene& scene, std::string_view scene_path,
                                const AssetExists& asset_exists) {
    ValidationReport report;
    report.scene_path = std::string(scene_path);

    const ecs::World& world = scene.world();
    const std::vector<ecs::Entity> entities = world.all_entities();

    // --- names ---------------------------------------------------------
    // One name, one entity. Everything that refers to an entity by name —
    // `follow=`, `nf.find_entity`, a save slot — resolves to the FIRST match,
    // so a duplicate silently makes the other one unreachable.
    std::map<std::string, u32> name_counts;
    std::map<std::string, ecs::Entity> name_entities;
    for (const ecs::Entity e : entities) {
        if (const auto* n = world.get<scene::NameComponent>(e); n != nullptr && !n->name.empty()) {
            ++name_counts[n->name];
            // First wins, exactly like every name lookup in the engine.
            name_entities.emplace(n->name, e);
        }
    }
    for (const auto& [name, count] : name_counts) {
        if (count > 1) {
            report.add(ValidationSeverity::Error, name,
                       "name is used by " + std::to_string(count) +
                           " entities; a reference to it resolves to only one of them");
        }
    }

    // --- cameras -------------------------------------------------------
    bool has_active_camera = false;
    std::string followed_name;
    for (const ecs::Entity e : entities) {
        const auto* cam = world.get<CameraComponent>(e);
        if (cam == nullptr) {
            continue;
        }
        const std::string label = entity_label(world, e);
        if (cam->is_active) {
            has_active_camera = true;
            followed_name = cam->follow_target;
        }
        if (cam->is_active && !cam->follow_target.empty() &&
            name_counts.find(cam->follow_target) == name_counts.end()) {
            report.add(ValidationSeverity::Error, label,
                       "follows '" + cam->follow_target +
                           "', which is not the name of any entity — the camera will not move");
        }
        if (!sane(cam->fov_y, 10.0f, 170.0f)) {
            report.add(ValidationSeverity::Warning, label,
                       "fov " + std::to_string(cam->fov_y) + " is outside [10, 170]");
        }
        if (!sane(cam->distance, 0.05f, 1000.0f)) {
            report.add(ValidationSeverity::Warning, label,
                       "distance " + std::to_string(cam->distance) +
                           " is not positive; the camera would sit inside the pivot");
        }
        if (cam->pitch_min > cam->pitch_max) {
            report.add(ValidationSeverity::Warning, label,
                       "pitch_min is greater than pitch_max; the orbit clamp is inverted");
        }
        if (cam->ads_enabled && !sane(cam->ads_distance, 0.05f, 1000.0f)) {
            report.add(ValidationSeverity::Warning, label,
                       "ads_distance " + std::to_string(cam->ads_distance) + " is not positive");
        }
    }
    if (!has_active_camera) {
        report.add(ValidationSeverity::Warning, {},
                   "the scene has no active camera; the player will see the default view");
    }

    // --- physics -------------------------------------------------------
    // A dynamic body with no mass is a body Jolt will refuse or simulate as
    // something else; either way the scene does not do what it says.
    for (const ecs::Entity e : entities) {
        const auto* body = world.get<physics::RigidBodyComponent>(e);
        if (body == nullptr || body->type != physics::BodyType::Dynamic) {
            continue;
        }
        if (!std::isfinite(body->mass) || body->mass <= 0.0f) {
            report.add(ValidationSeverity::Error, entity_label(world, e),
                       "dynamic rigid body has mass " + std::to_string(body->mass) +
                           "; it must be positive");
        }
    }

    // --- post-process --------------------------------------------------
    for (const ecs::Entity e : entities) {
        const auto* pp = world.get<PostProcessComponent>(e);
        if (pp == nullptr) {
            continue;
        }
        const std::string label = entity_label(world, e);
        if (!sane(pp->render_scale, rendering::kMinRenderScale, rendering::kMaxRenderScale)) {
            report.add(ValidationSeverity::Error, label,
                       "render_scale " + std::to_string(pp->render_scale) + " is outside [" +
                           std::to_string(rendering::kMinRenderScale) + ", " +
                           std::to_string(rendering::kMaxRenderScale) + "]");
        }
        // The floor has its own bounds: 8.0 is not "too high relative to the
        // ceiling" when the ceiling is 9.0, but the renderer still rejects it.
        if (!sane(pp->min_render_scale, rendering::kMinRenderScale,
                  rendering::kMaxRenderScale)) {
            report.add(ValidationSeverity::Error, label,
                       "min_render_scale " + std::to_string(pp->min_render_scale) +
                           " is outside [" + std::to_string(rendering::kMinRenderScale) + ", " +
                           std::to_string(rendering::kMaxRenderScale) + "]");
        } else if (pp->min_render_scale > pp->render_scale) {
            report.add(ValidationSeverity::Error, label,
                       "min_render_scale is above render_scale; the governor could never hold "
                       "its ceiling");
        }
        if (!sane(pp->target_fps, rendering::kMinTargetFps, rendering::kMaxTargetFps)) {
            report.add(ValidationSeverity::Warning, label,
                       "target_fps " + std::to_string(pp->target_fps) + " is outside [" +
                           std::to_string(rendering::kMinTargetFps) + ", " +
                           std::to_string(rendering::kMaxTargetFps) + "]");
        }
        // A frame policy with no dynamic resolution is not an error: the
        // policy owns the scale on its own. It IS worth saying, because the
        // author probably meant to enable both.
        if (pp->frame_policy && !pp->dynamic_resolution) {
            report.add(ValidationSeverity::Info, label,
                       "frame_policy is on while dynamic_resolution is off; the policy will "
                       "still move the scale, which is probably not what the scene meant");
        }
    }

    // --- combat feedback (Phase 56) -------------------------------------
    // The loader refuses a bad line, so this is for the other two paths into a
    // scene (C++ and the editor) and for the values the editor can type after
    // load. A volume of zero is legal (that cue is silent); a non-positive or
    // non-finite duration is not, because the director would clamp it and the
    // author would be looking at a number that means something else.
    for (const ecs::Entity e : entities) {
        const auto* fb = world.get<FeedbackComponent>(e);
        if (fb == nullptr) {
            continue;
        }
        const std::string label = entity_label(world, e);
        auto check = [&report, &label](f32 value, f32 lo, f32 hi, const char* name) {
            if (!sane(value, lo, hi)) {
                report.add(ValidationSeverity::Warning, label,
                           std::string(name) + " " + std::to_string(value) + " is outside [" +
                               std::to_string(lo) + ", " + std::to_string(hi) +
                               "]; the director will clamp it");
            }
        };
        check(fb->flash_seconds, 0.002f, 10.0f, "flash_time");
        check(fb->flash_size, 0.001f, 10.0f, "flash_size");
        check(fb->tracer_seconds, 0.002f, 10.0f, "tracer_time");
        check(fb->tracer_length, 0.01f, 1000.0f, "tracer_length");
        check(fb->tracer_width, 0.0005f, 1.0f, "tracer_width");
        check(fb->impact_seconds, 0.01f, 600.0f, "impact_time");
        check(fb->impact_size, 0.001f, 10.0f, "impact_size");
        check(fb->shot_volume, 0.0f, 4.0f, "shot_volume");
        check(fb->impact_volume, 0.0f, 4.0f, "impact_volume");
        // Phase 59 additions: the blast flash, the per-channel gains and the
        // muzzle light. Caps are checked as floats because the report prints the
        // value; the range is the director's own 1..4096.
        check(fb->blast_flash_seconds, 0.002f, 10.0f, "blast_time");
        check(fb->blast_flash_size, 0.001f, 10.0f, "blast_size");
        check(fb->impact_volume_explosive, 0.0f, 4.0f, "impact_volume_explosive");
        check(fb->impact_volume_fire, 0.0f, 4.0f, "impact_volume_fire");
        check(fb->flash_light_intensity, 0.0f, 1000.0f, "light_intensity");
        check(fb->flash_light_radius, 0.01f, 1000.0f, "light_radius");
        check(fb->flash_light_r, 0.0f, 1.0f, "light_r");
        check(fb->flash_light_g, 0.0f, 1.0f, "light_g");
        check(fb->flash_light_b, 0.0f, 1.0f, "light_b");
        check(static_cast<f32>(fb->max_flashes), 1.0f, 4096.0f, "max_flashes");
        check(static_cast<f32>(fb->max_tracers), 1.0f, 4096.0f, "max_tracers");
        check(static_cast<f32>(fb->max_impacts), 1.0f, 4096.0f, "max_impacts");
        check(static_cast<f32>(fb->max_sounds), 1.0f, 4096.0f, "max_sounds");
        check(static_cast<f32>(fb->max_lights), 1.0f, 4096.0f, "max_lights");
    }

    // --- perception (Phase 60) -------------------------------------------
    // The loader refuses a bad line, so this is for the other two paths in (a
    // component built in C++, an inspector field typed past its slider) and for
    // the CROSS-FIELD mistake a key-by-key check cannot see: a brain that
    // governs combat while every sense is switched off can never fire, and the
    // only symptom would be an enemy that stands still.
    for (const ecs::Entity e : entities) {
        const auto* pc = world.get<PerceptionComponent>(e);
        if (pc == nullptr) {
            continue;
        }
        const std::string label = entity_label(world, e);
        auto check = [&report, &label](f32 value, f32 lo, f32 hi, const char* name) {
            if (!sane(value, lo, hi)) {
                report.add(ValidationSeverity::Warning, label,
                           std::string(name) + " " + std::to_string(value) + " is outside [" +
                               std::to_string(lo) + ", " + std::to_string(hi) +
                               "]; perception will clamp it");
            }
        };
        check(pc->sight_range, 0.0f, 10000.0f, "sight");
        check(pc->sight_half_fov_deg, 0.0f, 180.0f, "fov");
        check(pc->sight_gain, 0.0f, 1000.0f, "gain");
        check(pc->sight_loss, 0.0f, 1000.0f, "loss");
        check(pc->sight_linger, 0.0f, 3600.0f, "linger");
        check(pc->hearing_range, 0.0f, 10000.0f, "hearing");
        check(pc->touch_range, 0.0f, 1000.0f, "touch");
        check(pc->noise, 0.0f, 1.0f, "noise");
        check(pc->visibility, 0.0f, 1.0f, "visible");
        check(pc->radius, 0.0f, 100.0f, "radius");
        check(pc->eye_height, 0.0f, 100.0f, "eye");

        const bool governs = pc->enabled && pc->governs_combat &&
                             world.get<CombatAgentComponent>(e) != nullptr;
        const bool senses_anything = pc->sight_range > 0.0f || pc->hearing_range > 0.0f ||
                                     pc->touch_range > 0.0f || pc->pain;
        if (governs && !senses_anything) {
            // An error rather than a warning: the agent is wired to senses that
            // cannot report anything, so it will never fire. That is a level
            // that does not work, not one that looks a little off.
            report.add(ValidationSeverity::Error, label,
                       "CombatAgent with combat=true and every sense disabled: this agent "
                       "can never acquire a target");
        }
    }

    // --- squad radio (Phase 68) ------------------------------------------
    // The loader refuses a bad line, so this is for the other two paths in and
    // for the one mistake a single component cannot show: a radio with nothing
    // to send. A `Squad:` line on an entity with no enabled `Perception:` is
    // inert, and an empty token or zero range is a radio nobody can hear.
    for (const ecs::Entity e : entities) {
        const auto* squad = world.get<SquadComponent>(e);
        if (squad == nullptr) {
            continue;
        }
        const std::string label = entity_label(world, e);
        if (squad->token.empty()) {
            report.add(ValidationSeverity::Warning, label,
                       "Squad has no token: it shares nothing");
        } else {
            if (squad->token.size() > 64u) {
                report.add(ValidationSeverity::Warning, label,
                           "Squad token is longer than 64 characters: the loader will refuse "
                           "the line");
            }
            for (const char c : squad->token) {
                if (std::isspace(static_cast<unsigned char>(c)) != 0) {
                    report.add(ValidationSeverity::Warning, label,
                               "Squad token contains whitespace: the scene format reads one "
                               "token per key");
                    break;
                }
            }
        }
        if (!sane(squad->radio, 0.0f, 10000.0f)) {
            report.add(ValidationSeverity::Warning, label,
                       std::string("radio ") + std::to_string(squad->radio) +
                           " is outside [0, 10000]; the radio cannot reach that far");
        }
        if (!sane(squad->lost, 0.0f, 10000.0f)) {
            report.add(ValidationSeverity::Warning, label,
                       std::string("lost ") + std::to_string(squad->lost) +
                           " is outside [0, 10000]; no lost-contact callout can use that "
                           "window");
        }
        if (squad->enabled && squad->radio <= 0.0f) {
            report.add(ValidationSeverity::Warning, label,
                       "Squad radio=0 shares nothing: no contact is ever delivered");
        }
        if (squad->enabled) {
            const auto* perception = world.get<PerceptionComponent>(e);
            if (perception == nullptr || !perception->enabled) {
                report.add(ValidationSeverity::Warning, label,
                           "Squad has no enabled Perception line: this radio has no contacts "
                           "of its own to share");
            }
        }
    }

    // --- radio jammers (Phase 70) ----------------------------------------
    // A field with no reach or no strength is an authored component that does
    // nothing, which is worth naming rather than leaving as a silent typo.
    for (const ecs::Entity e : entities) {
        const auto* jammer = world.get<JammerComponent>(e);
        if (jammer == nullptr) {
            continue;
        }
        const std::string label = entity_label(world, e);
        if (!sane(jammer->radius, 0.0f, 10000.0f)) {
            report.add(ValidationSeverity::Warning, label,
                       std::string("Jammer radius ") + std::to_string(jammer->radius) +
                           " is outside [0, 10000]; it cannot cover that area");
        }
        if (!sane(jammer->strength, 0.0f, 1.0f)) {
            report.add(ValidationSeverity::Warning, label,
                       std::string("Jammer strength ") + std::to_string(jammer->strength) +
                           " is outside [0, 1]; it is clamped at runtime");
        }
        if (jammer->enabled && jammer->radius <= 0.0f) {
            report.add(ValidationSeverity::Warning, label,
                       "Jammer radius=0 jams nothing: no radio is inside its field");
        }
        if (jammer->enabled && jammer->strength <= 0.0f) {
            report.add(ValidationSeverity::Warning, label,
                       "Jammer strength=0 jams nothing: every radio keeps its full range");
        }
        if (world.get<scene::Transform>(e) == nullptr) {
            report.add(ValidationSeverity::Warning, label,
                       "Jammer has no Transform: its field has no position and jams nothing");
        }
    }

    // --- decision layer (Phase 61) ---------------------------------------
    // The loader refuses a bad line, so this is for the other two paths in and
    // for the mistakes only the WHOLE list can show: a brain with nothing to
    // consider, two considerations with the same name (the log and Lua would be
    // ambiguous), and a list where every weight is zero, which means the first
    // action always wins whatever the world looks like.
    for (const ecs::Entity e : entities) {
        const auto* brain = world.get<BrainComponent>(e);
        if (brain == nullptr) {
            continue;
        }
        const std::string label = entity_label(world, e);
        if (brain->enabled && brain->actions.empty()) {
            report.add(ValidationSeverity::Warning, label,
                       "Brain has no actions: this entity never decides anything");
        }
        if (!sane(brain->rate, 0.0f, 60.0f)) {
            report.add(ValidationSeverity::Warning, label,
                       std::string("rate ") + std::to_string(brain->rate) +
                           " is outside [0, 60]; the brain will clamp it");
        }
        bool all_zero_weight = !brain->actions.empty();
        for (usize i = 0; i < brain->actions.size(); ++i) {
            const BrainAction& action = brain->actions[i];
            if (action.weight > 0.0f) {
                all_zero_weight = false;
            }
            if (!sane(action.weight, 0.0f, 1000.0f) || !sane(action.slope, -1000.0f, 1000.0f) ||
                !sane(action.exponent, 0.0f, 16.0f) || !sane(action.input_offset, -1.0e6f, 1.0e6f) ||
                !sane(action.output_offset, -1000.0f, 1000.0f)) {
                report.add(ValidationSeverity::Warning, label,
                           "BrainAction '" + action.name +
                               "' has a non-finite or out-of-range number; the brain will clamp it");
            }
            for (usize j = i + 1; j < brain->actions.size(); ++j) {
                if (brain->actions[j].name == action.name) {
                    report.add(ValidationSeverity::Warning, label,
                               "two BrainActions are both named '" + action.name +
                                   "'; the decision log cannot tell them apart");
                }
            }
        }
        if (all_zero_weight) {
            report.add(ValidationSeverity::Warning, label,
                       "every BrainAction has weight 0: the first action always wins");
        }
    }

    // --- state machines (Phase 62) ---------------------------------------
    // The edges are cross-references, so this is where they are checked: the
    // loader parses one line at a time and cannot know whether the state a
    // transition names is declared further down the entity. A transition to
    // nowhere is an ERROR — the machine would drop the edge at bind time and the
    // NPC would never leave that state, which reads as "the AI is broken" rather
    // than as a typo.
    for (const ecs::Entity e : entities) {
        const auto* machine = world.get<StateMachineComponent>(e);
        if (machine == nullptr) {
            continue;
        }
        const std::string label = entity_label(world, e);
        if (machine->states.empty()) {
            if (machine->enabled) {
                report.add(ValidationSeverity::Warning, label,
                           "States has no states: this entity's machine does nothing");
            }
            continue;
        }
        if (!sane(machine->rate, 0.0f, 60.0f)) {
            report.add(ValidationSeverity::Warning, label,
                       std::string("rate ") + std::to_string(machine->rate) +
                           " is outside [0, 60]; the machine will clamp it");
        }

        const auto state_index = [&machine](const std::string& name) -> long long {
            for (usize i = 0; i < machine->states.size(); ++i) {
                if (machine->states[i].name == name) {
                    return static_cast<long long>(i);
                }
            }
            return -1;
        };

        if (!machine->initial.empty() && state_index(machine->initial) < 0) {
            report.add(ValidationSeverity::Error, label,
                       "States initial='" + machine->initial +
                           "' is not one of the declared states");
        }

        // Duplicate names make every edge that mentions them ambiguous.
        for (usize i = 0; i < machine->states.size(); ++i) {
            for (usize j = i + 1; j < machine->states.size(); ++j) {
                if (machine->states[j].name == machine->states[i].name) {
                    report.add(ValidationSeverity::Warning, label,
                               "two states are both named '" + machine->states[i].name + "'");
                }
            }
        }

        // Reachability: a state that is neither the start nor the destination of
        // an edge can never run. `incoming` counts edges, so the start is added
        // explicitly.
        std::vector<bool> reachable(machine->states.size(), false);
        const long long start = machine->initial.empty()
                                    ? 0
                                    : state_index(machine->initial);
        if (start >= 0 && static_cast<usize>(start) < reachable.size()) {
            reachable[static_cast<usize>(start)] = true;
        }
        for (const TransitionSpec& transition : machine->transitions) {
            const long long from = state_index(transition.from);
            const long long to = state_index(transition.to);
            if (from < 0) {
                report.add(ValidationSeverity::Error, label,
                           "Transition from='" + transition.from +
                               "' is not a declared state");
                continue;
            }
            if (to < 0) {
                report.add(ValidationSeverity::Error, label,
                           "Transition to='" + transition.to +
                               "' is not a declared state");
                continue;
            }
            reachable[static_cast<usize>(to)] = true;
        }
        for (usize i = 0; i < machine->states.size(); ++i) {
            if (!reachable[i]) {
                report.add(ValidationSeverity::Warning, label,
                           "state '" + machine->states[i].name +
                               "' has no incoming transition and is not the start: it can "
                               "never run");
            }
        }
    }

    // --- terrain and its links (Phase 63) --------------------------------
    //
    // The loader refuses a value outside its range, so these rules are for the
    // CROSS-FIELD mistakes: a field that multiplies nothing, a link the bake
    // will drop because one end is off the volume, a link that goes nowhere.
    // All of them are things an author cannot see by reading one line.
    for (const ecs::Entity e : entities) {
        const auto* terrain = world.get<TerrainComponent>(e);
        if (terrain == nullptr || !terrain->enabled) {
            continue;
        }
        const std::string label = entity_label(world, e);
        if (terrain->resolution < 2u) {
            report.add(ValidationSeverity::Error, label,
                       "terrain resolution " + std::to_string(terrain->resolution) +
                           " cannot form a grid (2 vertices per side is the minimum)");
        }
        if (!(terrain->area > 0.0f)) {
            report.add(ValidationSeverity::Error, label,
                       "terrain area must be positive; a zero extent is a line, not ground");
        }
        // height_scale multiplies the field, and the field is flat when it has
        // no octaves and no heightmap. The mesh still draws (a plane), which is
        // exactly why this is worth saying out loud.
        if (terrain->height_scale > 0.0f && terrain->octaves == 0u &&
            terrain->heightmap.empty()) {
            report.add(ValidationSeverity::Warning, label,
                       "terrain has height=" + std::to_string(terrain->height_scale) +
                           " but octaves=0 and no heightmap: the ground will be flat");
        }
    }

    // Links belong to a volume, so the pair is what gets checked. A link whose
    // endpoint falls outside the volume's own box is dropped by the bake (and
    // counted); saying so here turns a silent drop into a line in the report.
    for (const ecs::Entity e : entities) {
        const auto* navmesh = world.get<NavMeshComponent>(e);
        const auto* t = world.get<scene::Transform>(e);
        if (navmesh == nullptr || t == nullptr || !navmesh->enabled) {
            continue;
        }
        const std::string label = entity_label(world, e);
        for (usize i = 0; i < navmesh->links.size(); ++i) {
            const NavLinkSpec& link = navmesh->links[i];
            const std::string which = "link " + std::to_string(i + 1);
            const auto inside = [&](f32 x, f32 y, f32 z) {
                return x >= t->local_x && x <= t->local_x + navmesh->area_x &&
                       y >= t->local_y && y <= t->local_y + navmesh->area_y &&
                       z >= t->local_z && z <= t->local_z + navmesh->area_z;
            };
            if (!inside(link.start_x, link.start_y, link.start_z)) {
                report.add(ValidationSeverity::Warning, label,
                           which + " starts outside the volume: the bake will drop it");
            }
            if (!inside(link.end_x, link.end_y, link.end_z)) {
                report.add(ValidationSeverity::Warning, label,
                           which + " ends outside the volume: the bake will drop it");
            }
            const f32 dx = link.end_x - link.start_x;
            const f32 dy = link.end_y - link.start_y;
            const f32 dz = link.end_z - link.start_z;
            if (dx * dx + dy * dy + dz * dz < 1e-6f) {
                report.add(ValidationSeverity::Warning, label,
                           which + " starts and ends at the same point: it connects nothing");
            }
        }
    }

    // --- patrol routes (Phase 64) ---------------------------------------
    //
    // A route is only as good as the walker that follows it and the volume it
    // lives on, and neither fact is visible by reading one line. The runtime
    // degrades quietly (a one-point route falls back to the goal, a route with
    // no volume never binds), so the validator is where the author hears it.
    {
        const NavMeshComponent* first_volume = nullptr;
        const scene::Transform* first_volume_t = nullptr;
        for (const ecs::Entity e : entities) {
            const auto* nm = world.get<NavMeshComponent>(e);
            const auto* t = world.get<scene::Transform>(e);
            if (nm != nullptr && t != nullptr && nm->enabled) {
                first_volume = nm;
                first_volume_t = t;
                break;
            }
        }
        for (const ecs::Entity e : entities) {
            const auto* route = world.get<NavRouteComponent>(e);
            if (route == nullptr || !route->enabled) {
                continue;
            }
            const std::string label = entity_label(world, e);
            if (!world.has<NavAgentComponent>(e)) {
                report.add(ValidationSeverity::Warning, label,
                           "NavRoute without a NavAgent on the same entity: nothing will "
                           "walk this route");
            }
            if (route->points.size() < 2) {
                report.add(ValidationSeverity::Warning, label,
                           "NavRoute has " + std::to_string(route->points.size()) +
                               " point(s): a patrol needs at least two, so the agent will "
                               "keep walking to its goal");
            }
            if (route->loop && route->ping_pong) {
                report.add(ValidationSeverity::Warning, label,
                           "NavRoute sets loop=true and ping_pong=true: ping-pong wins and "
                           "the route never wraps");
            }
            // Phase 65: the perception gate reads the same entity's senses.
            // Without a sensor the route walks exactly as it did before, which
            // is a silent no-op the author should hear about.
            if (route->hold != NavRouteHoldPolicy::Off) {
                const auto* perception = world.get<PerceptionComponent>(e);
                if (perception == nullptr || !perception->enabled) {
                    report.add(ValidationSeverity::Warning, label,
                               "NavRoute uses a hold= policy but the entity has no enabled "
                               "Perception line: the patrol can never be held");
                }
            } else if (route->resume > 0.0f) {
                report.add(ValidationSeverity::Warning, label,
                           "NavRoute sets resume= without hold=: the delay has no effect "
                           "on the patrol");
            }
            // Phase 66: the investigation replaces a hold's release. Without a
            // gate there is never a last known position to search.
            if (route->investigate > 0.0f && route->hold == NavRouteHoldPolicy::Off) {
                report.add(ValidationSeverity::Warning, label,
                           "NavRoute sets investigate= without hold=: there is no hold "
                           "to release, so the search never runs");
            }
            // Phase 67: a squad shares a search; a route that never searches
            // has nothing to share with its mates.
            if (!route->squad.empty() && route->investigate <= 0.0f) {
                report.add(ValidationSeverity::Warning, label,
                           "NavRoute sets squad= without investigate=: the squad has no "
                           "search to share");
            }
            if (first_volume == nullptr || first_volume_t == nullptr) {
                report.add(ValidationSeverity::Warning, label,
                           "NavRoute exists but the scene has no enabled navigation volume: "
                           "no mesh is baked and the agent never moves");
                continue;
            }
            for (usize i = 0; i < route->points.size(); ++i) {
                const NavPointSpec& point = route->points[i];
                const bool inside =
                    point.x >= first_volume_t->local_x &&
                    point.x <= first_volume_t->local_x + first_volume->area_x &&
                    point.y >= first_volume_t->local_y &&
                    point.y <= first_volume_t->local_y + first_volume->area_y &&
                    point.z >= first_volume_t->local_z &&
                    point.z <= first_volume_t->local_z + first_volume->area_z;
                if (!inside) {
                    report.add(ValidationSeverity::Warning, label,
                               "route point " + std::to_string(i + 1) +
                                   " is outside the navigation volume: the agent cannot "
                                   "path there");
                }
            }
        }
    }

    // --- combat (Phase 44) ---------------------------------------------
    //
    // A `Weapon:` line the loader refused never reaches here, so these rules
    // are for the other two paths into a scene: a component built in C++ or
    // through the API, and the CROSS-FIELD mistakes a key-by-key check cannot
    // see (a falloff curve that runs backwards, hit points above the ceiling).
    //
    // The "no camera follows this" note is an INFO and not a warning on
    // purpose: an NPC's rifle is a legitimate weapon on an entity no camera
    // follows. It is worth saying because the common cause is a typo in
    // `follow=`, which makes the player's gun silently never fire.
    for (const ecs::Entity e : entities) {
        const std::string label = entity_label(world, e);
        if (const auto* w = world.get<WeaponComponent>(e); w != nullptr) {
            if (!weapon_valid(w->profile)) {
                report.add(ValidationSeverity::Error, label,
                           "weapon profile cannot fire (magazine, pellets, cadence or range is "
                           "zero, or the spread ceiling is below its floor)");
            }
            if (w->profile.falloff_end > 0.0f &&
                w->profile.falloff_end <= w->profile.falloff_start) {
                report.add(ValidationSeverity::Warning, label,
                           "falloff_end is not past falloff_start; the damage curve is flat");
            }
            if (w->state.in_magazine > w->profile.magazine) {
                report.add(ValidationSeverity::Warning, label,
                           "the magazine holds more rounds than its capacity; it will be "
                           "clamped on the next reload");
            }
            if (w->profile.blast_radius > 0.0f && w->profile.blast_damage <= 0.0f) {
                report.add(ValidationSeverity::Warning, label,
                           "blast_radius is set but blast_damage is zero; the blast is inert");
            }
            if (w->profile.blast_damage > 0.0f && w->profile.blast_radius <= 0.0f) {
                report.add(ValidationSeverity::Warning, label,
                           "blast_damage is set but blast_radius is zero; the blast is inert");
            }
            if (w->profile.enabled && (followed_name.empty() || followed_name != label)) {
                report.add(ValidationSeverity::Info, label,
                           "no active camera follows this entity, so the trigger will never "
                           "reach this weapon (a script can still fire it)");
            }
        }
        if (const auto* h = world.get<HealthComponent>(e); h != nullptr) {
            if (!health_valid(*h)) {
                report.add(ValidationSeverity::Error, label,
                           "max_health is not a positive number, or health/armor has an "
                           "invalid pool, fraction or zone multiplier; the entity cannot "
                           "take a finite hit");
            }
            if (h->max_health > 0.0f && h->health > h->max_health) {
                report.add(ValidationSeverity::Warning, label,
                           "health is above max_health; it is clamped when the scene loads");
            }
            if (h->max_armor > 0.0f && h->armor > h->max_armor) {
                report.add(ValidationSeverity::Warning, label,
                           "armor is above armor_max; it is clamped when the scene loads");
            }
            if (h->hit_zones && world.get<physics::ColliderComponent>(e) == nullptr) {
                report.add(ValidationSeverity::Info, label,
                           "hit zones are enabled but the entity has no collider; every hit "
                           "will be a body hit");
            }
        }
        if (const auto* agent = world.get<CombatAgentComponent>(e); agent != nullptr) {
            // The same rule the camera's `follow=` keeps: a name nothing has is
            // an error, because the feature silently does nothing.
            if (!agent->auto_target && !agent->target.empty() &&
                name_counts.find(agent->target) == name_counts.end()) {
                report.add(ValidationSeverity::Error, label,
                           "targets '" + agent->target +
                               "', which is not the name of any entity — the agent will never "
                               "fire");
            }
            if (agent->auto_target) {
                if (!agent->target.empty()) {
                    report.add(ValidationSeverity::Info, label,
                               "auto_target is on, so target='" + agent->target +
                                   "' is ignored");
                }
                const FactionComponent* own = world.get<FactionComponent>(e);
                if (own == nullptr || !own->enabled || own->team.empty()) {
                    report.add(ValidationSeverity::Warning, label,
                               "auto_target is on but the agent has no team; it will never "
                               "acquire an enemy");
                }
            }
            // A target on the same team is a legal scene but a silent one:
            // faction rules stop the damage, and the author usually meant to
            // flip one of the two sides.
            if (!agent->auto_target) {
                if (const auto it = name_entities.find(agent->target); it != name_entities.end()) {
                    const FactionComponent* self_side = world.get<FactionComponent>(e);
                    const FactionComponent* target_side = world.get<FactionComponent>(it->second);
                    const std::string_view self_team =
                        (self_side != nullptr && self_side->enabled) ? self_side->team
                                                                     : std::string_view{};
                    const std::string_view target_team =
                        (target_side != nullptr && target_side->enabled) ? target_side->team
                                                                         : std::string_view{};
                    if (team_relation(self_team, target_team) == TeamRelation::Friendly &&
                        !(self_side != nullptr && self_side->friendly_fire)) {
                        report.add(ValidationSeverity::Warning, label,
                                   "targets '" + agent->target +
                                       "' on its own team; faction rules block the damage");
                    }
                }
            }
            if (agent->enabled && world.get<WeaponComponent>(e) == nullptr) {
                report.add(ValidationSeverity::Warning, label,
                           "combat agent has no weapon; it will aim but never shoot");
            }
            if (agent->enabled && world.get<HealthComponent>(e) == nullptr) {
                report.add(ValidationSeverity::Info, label,
                           "combat agent has no hit points, so nothing can stop it");
            }
        }
    }

    // --- asset references ----------------------------------------------
    // Only when the caller can answer. A path that looks like a path is
    // checked; a bare name is not, because a name is resolved by the asset
    // system and a file lookup would report a false error on every scene that
    // uses one.
    if (asset_exists) {
        const auto check_path = [&](const std::string& label, const std::string& what,
                                    const std::string& path) {
            if (path.empty() || path.rfind("content://", 0) != 0) {
                return;
            }
            if (!asset_exists(path)) {
                report.add(ValidationSeverity::Error, label,
                           what + " '" + path + "' does not exist in the project");
            }
        };
        for (const ecs::Entity e : entities) {
            const std::string label = entity_label(world, e);
            if (const auto* mesh = world.get<MeshComponent>(e); mesh != nullptr) {
                check_path(label, "material", mesh->material);
            }
            if (const auto* decal = world.get<DecalComponent>(e); decal != nullptr) {
                check_path(label, "decal material", decal->material);
            }
            if (const auto* pp = world.get<PostProcessComponent>(e); pp != nullptr) {
                check_path(label, "LUT", pp->lut_path);
            }
            if (const auto* script = world.get<scripting::ScriptComponent>(e); script != nullptr) {
                if (!script->path.empty()) {
                    check_path(label, "script", script->path);
                } else if (script->enabled && script->source.empty()) {
                    report.add(ValidationSeverity::Warning, label,
                               "script component is enabled but has neither a path nor source");
                }
            }
            if (const auto* terrain = world.get<TerrainComponent>(e); terrain != nullptr) {
                check_path(label, "terrain heightmap", terrain->heightmap);
                check_path(label, "terrain material", terrain->material);
            }
        }
    }

    // Errors first, then warnings, then infos; stable inside a severity so two
    // runs over the same scene print the same report.
    std::stable_sort(report.issues.begin(), report.issues.end(),
                     [](const ValidationIssue& a, const ValidationIssue& b) {
                         return static_cast<u8>(a.severity) > static_cast<u8>(b.severity);
                     });
    return report;
}

std::string format_validation_report(const ValidationReport& report) {
    std::ostringstream out;
    out << report.scene_path << "\n";
    if (report.issues.empty()) {
        out << "  ok — no issues\n";
        return out.str();
    }
    for (const auto& issue : report.issues) {
        out << "  [" << validation_severity_name(issue.severity) << "] ";
        if (!issue.entity.empty()) {
            out << issue.entity << ": ";
        }
        out << issue.message << "\n";
    }
    out << "  " << report.errors << " error(s), " << report.warnings << " warning(s), "
        << report.infos << " info\n";
    return out.str();
}


namespace {

/// JSON string escaping. A hand-rolled escaper is enough here because the
/// output is flat: no nesting, no numbers formatted by hand, and the one thing
/// that must be right is that a quote in a name does not end the string.
std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                // Control characters have no literal form in JSON.
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c) & 0xFFu);
                out += buf;
            } else {
                out += c;
            }
            break;
        }
    }
    return out;
}

} // namespace

std::string format_validation_json(const ValidationReport& report) {
    std::ostringstream out;
    out << "{\"scene\":\"" << json_escape(report.scene_path) << "\""
        << ",\"errors\":" << report.errors << ",\"warnings\":" << report.warnings
        << ",\"infos\":" << report.infos << ",\"issues\":[";
    for (size_t i = 0; i < report.issues.size(); ++i) {
        const auto& issue = report.issues[i];
        if (i != 0) {
            out << ",";
        }
        out << "{\"severity\":\"" << validation_severity_name(issue.severity) << "\""
            << ",\"entity\":\"" << json_escape(issue.entity) << "\""
            << ",\"message\":\"" << json_escape(issue.message) << "\"}";
    }
    out << "]}\n";
    return out.str();
}

} // namespace nf::runtime
