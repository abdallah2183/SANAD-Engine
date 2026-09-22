#pragma once

// NF/Editor/Gizmo.hpp — limited transform gizmo model (no ImGuizmo dependency).
//
// v0.1 semantics: the caller feeds pointer deltas in viewport NDC; the gizmo
// maps them to a Transform delta for the active mode/space, and commit()
// folds the whole drag into ONE SetTransformCommand (never per-pixel).
// Translate moves in the camera plane, Rotate yaws/pitches about world axes,
// Scale scales uniformly. Rotation/scale compose through scene::compose_trs,
// so a committed drag is exactly what the viewport renders.

#include <NF/ECS/ECS.hpp>
#include <NF/Scene/Transform.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nf::editor {

class ICommand;

enum class GizmoMode : uint8_t { Translate = 0, Rotate = 1, Scale = 2 };
enum class GizmoSpace : uint8_t { World = 0, Local = 1 };

struct ViewCamera {
    float px = 0, py = 2, pz = 5; // eye
    float tx = 0, ty = 0, tz = 0; // target
    float fov_y_deg = 60.0f;
    float aspect = 16.0f / 9.0f;
    float near_plane = 0.1f;
    float far_plane = 1000.0f;
};

struct Ray {
    float ox = 0, oy = 0, oz = 0;
    float dx = 0, dy = 0, dz = -1;
};

struct AABB {
    float min_x = 0, min_y = 0, min_z = 0;
    float max_x = 0, max_y = 0, max_z = 0;
};

Ray pick_ray(const ViewCamera& cam, float ndc_x, float ndc_y);
bool ray_hit_aabb(const Ray& ray, const AABB& box, float& out_t);

// Viewport NDC (-1..1, +Y up) to framebuffer pixels (row 0 = the image TOP).
//
// The Y negation is the whole point of this helper and it used to be missing:
// Mat4::perspective deliberately negates m[1][1] (the Vulkan Y-flip), so in
// THIS engine a point above the view centre — world +Y — lands in LOW-numbered
// framebuffer rows, i.e. near row 0. That is pinned from both sides:
// RHITests' lighting_pass reads sky in the top rows of the readback and
// EditorTests' render_memory_rows_follow_vulkan_top_left_origin asserts a cube
// above the centre concentrates in rows 0..31. The GPU picker copies a
// framebuffer pixel (GpuPicker::pick -> copy_texture_to_buffer x, y), so an
// NDC y passed through unflipped addresses the MIRRORED pixel: clicking above
// an object selects whatever is below it, and clicking the sky grabs geometry.
// The centre still hits, which is exactly why that bug hides — off-centre
// clicks are where it shows.
inline void viewport_ndc_to_pixel(float ndc_x, float ndc_y, float width, float height, float& out_x,
                                  float& out_y) {
    out_x = (ndc_x * 0.5f + 0.5f) * width;
    out_y = (0.5f - ndc_y * 0.5f) * height;
}

// Nearest mesh-candidate entity hit by the NDC pointer position.
// bounds_of maps an entity to its world-space bounds (absent = skipped).
template <typename BoundsFn>
ecs::Entity pick_entity(const ecs::World& world, BoundsFn&& bounds_of, const ViewCamera& cam,
                        float ndc_x, float ndc_y) {
    const Ray ray = pick_ray(cam, ndc_x, ndc_y);
    ecs::Entity best;
    float best_t = 0.0f;
    bool found = false;
    for (ecs::Entity e : world.all_entities()) {
        std::optional<AABB> b = bounds_of(e);
        if (!b.has_value()) {
            continue;
        }
        float t = 0.0f;
        if (!ray_hit_aabb(ray, *b, t)) {
            continue;
        }
        if (!found || t < best_t) {
            found = true;
            best_t = t;
            best = e;
        }
    }
    return found ? best : ecs::kInvalidEntity;
}

struct GizmoDelta {
    // Camera-plane offset (translate), yaw/pitch degrees (rotate), uniform
    // factor delta (scale: new_scale = old * (1 + dscale)).
    float dx = 0, dy = 0, dz = 0;
    float yaw_deg = 0, pitch_deg = 0;
    float dscale = 0;
};

// Grid snapping for a drag. A step of 0 (the default) leaves that channel
// unsnapped; any() is what a checkbox binds to. The drag rounds the
// ACCUMULATED delta onto the grid (not each pointer event), so the object
// sits on grid lines as the pointer crosses them instead of jittering at
// the sample rate, and a pointer held between two lines holds still.
struct GizmoSnap {
    float translate_step = 0.0f; // world units
    float rotate_step_deg = 0.0f;
    float scale_step = 0.0f; // uniform factor grid (0.25 -> 0.25x, 0.5x, ...)
    bool any() const {
        return translate_step > 0.0f || rotate_step_deg > 0.0f || scale_step > 0.0f;
    }
};

// Maps an NDC pointer drag (start -> current) to a mode delta.
GizmoDelta gizmo_delta_for_drag(GizmoMode mode, const ViewCamera& cam, float distance,
                                float ndc_x0, float ndc_y0, float ndc_x1, float ndc_y1);

// Applies a delta onto a base transform (pure, no world access). `snap`
// rounds the delta onto a grid before applying it. A zero delta returns the
// base untouched, so a no-op gesture never invents float drift through the
// euler round-trip below.
scene::Transform apply_gizmo_delta(const scene::Transform& base, const GizmoDelta& d,
                                   GizmoMode mode, GizmoSpace space,
                                   const GizmoSnap& snap = GizmoSnap{});

// Accumulates a drag and commits it as a single command.
class GizmoDrag {
public:
    GizmoDrag() = default;
    // Multi-entity (P2): every entity in `entities` that carries a Transform
    // moves together, and the whole gesture folds into ONE undo step.
    // Entities without a Transform are skipped (out_err names them; not
    // fatal — a group selection can legitimately include a camera-less
    // holder). Returns false when nothing in the set is draggable.
    bool begin(ecs::World& world, const std::vector<ecs::Entity>& entities, GizmoMode mode,
               GizmoSpace space, const GizmoSnap& snap, std::string& out_err);
    // Single-entity convenience: the multi-select path with one entity.
    bool begin(ecs::World& world, ecs::Entity e, GizmoMode mode, GizmoSpace space,
               std::string& out_err);
    void accumulate(const GizmoDelta& d);
    bool active() const { return m_active; }
    // The primary (first) entity of the drag — what a single-select caller
    // passed in, and what after_mutation() reports.
    ecs::Entity entity() const {
        return m_entities.empty() ? ecs::kInvalidEntity : m_entities.front();
    }
    const std::vector<ecs::Entity>& entities() const { return m_entities; }
    GizmoMode mode() const { return m_mode; }
    GizmoSpace space() const { return m_space; }
    // Writes the current total onto every entity for live feedback during the
    // drag (no undo entry — commit() folds it all into one command later).
    // Returns false when there is nothing to show (idle/dead/transformless).
    bool live_apply(ecs::World& world);
    // Folds the accumulated delta into one command (nullptr when idle/dead,
    // or when the snapped delta moved nothing). One command for the whole
    // set, never one per entity.
    std::unique_ptr<ICommand> commit(ecs::World& world);
    void cancel();
    // Restores every start transform and ends the drag (Escape / scene
    // switch): unlike cancel(), leaves no moved-without-undo state behind.
    bool abort(ecs::World& world);

private:
    bool m_active = false;
    std::vector<ecs::Entity> m_entities;
    std::vector<scene::Transform> m_starts; // parallel to m_entities
    GizmoMode m_mode = GizmoMode::Translate;
    GizmoSpace m_space = GizmoSpace::World;
    GizmoSnap m_snap{};
    GizmoDelta m_total{};
};

} // namespace nf::editor
