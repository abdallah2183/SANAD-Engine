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

// Maps an NDC pointer drag (start -> current) to a mode delta.
GizmoDelta gizmo_delta_for_drag(GizmoMode mode, const ViewCamera& cam, float distance,
                                float ndc_x0, float ndc_y0, float ndc_x1, float ndc_y1);

// Applies a delta onto a base transform (pure, no world access).
scene::Transform apply_gizmo_delta(const scene::Transform& base, const GizmoDelta& d,
                                   GizmoMode mode, GizmoSpace space);

// Accumulates a drag and commits it as a single command.
class GizmoDrag {
public:
    GizmoDrag() = default;
    // Returns false (with err) when the entity has no Transform or is dead.
    bool begin(ecs::World& world, ecs::Entity e, GizmoMode mode, GizmoSpace space,
               std::string& out_err);
    void accumulate(const GizmoDelta& d);
    bool active() const { return m_active; }
    ecs::Entity entity() const { return m_entity; }
    GizmoMode mode() const { return m_mode; }
    // Writes the current total onto the entity for live feedback during the
    // drag (no undo entry — commit() folds it all into one command later).
    // Returns false when there is nothing to show (idle/dead/transformless).
    bool live_apply(ecs::World& world);
    // Folds the accumulated delta into one command (nullptr when idle/dead).
    std::unique_ptr<ICommand> commit(ecs::World& world);
    void cancel();
    // Restores the start transform and ends the drag (Escape / scene switch):
    // unlike cancel(), leaves no moved-without-undo state behind.
    bool abort(ecs::World& world);

private:
    bool m_active = false;
    ecs::Entity m_entity = ecs::kInvalidEntity;
    GizmoMode m_mode = GizmoMode::Translate;
    GizmoSpace m_space = GizmoSpace::World;
    scene::Transform m_start{};
    GizmoDelta m_total{};
};

} // namespace nf::editor
