#pragma once

// NF/Runtime/SceneExtraction.hpp — the game-world → render-world bridge (Phase 11, W2)
//
// This used to live in `NF/Rendering/Extraction.hpp`, which meant the renderer
// included `<NF/ECS/ECS.hpp>` and `<NF/Scene/Transform.hpp>` and linked the ECS
// and the scene graph. That is backwards: the renderer consumes `RenderWorld`,
// which contains no game types, and it should not be able to see the world it is
// drawing.
//
// The cost of the old arrangement was structural, not aesthetic. `Engine/Rendering`
// could not be built or used without `NFEcs` and `NFScene`, which blocks:
//   * a dedicated-server build (a server has no business compiling a renderer to
//     walk a scene graph),
//   * offscreen tooling — asset preview, lightmap bake — that wants the renderer
//     without the game world,
//   * any future second game-world representation, because the bridge would have
//     to be rewritten inside the renderer.
//
// Extraction is a *bridge*, and a bridge belongs above both sides. It lives here,
// in the top layer of the engine, which is the only place that legitimately knows
// about the ECS, the scene graph and the render world at once.
//
//   ecs::World -> extract_render_objects -> RenderWorld -> cull -> Renderer3D
//
// After this call the renderer touches only `RenderWorld` data. Replacing the
// implementation (multithreaded, delta-based, GPU-driven) must not require a
// change downstream.

#include <NF/ECS/ECS.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Rendering/RenderWorld.hpp>

namespace nf::runtime {

/// Every entity carrying `scene::Transform` + `rendering::MeshComponent` becomes
/// a `RenderObject` with its world translation, mesh/material handles, and
/// world-space bounds taken from the mesh's local bounds through the mesh
/// library.
///
/// Entities without a transform or a mesh, and invisible ones, are skipped
/// entirely — gameplay-only components never cross this line. A mesh handle that
/// does not resolve in `meshes` is skipped rather than emitted with null bounds,
/// so a bad handle cannot produce an object that culls wrongly.
///
/// Note: this is translation-only. `RenderObject::world` is filled as a
/// translation matrix, matching the current `scene::Transform` model. Full TRS
/// lands with the transform upgrade; the field already carries a matrix so
/// nothing downstream changes shape when that happens.
void extract_render_objects(const ecs::World& world, const rendering::MeshLibrary& meshes,
                            rendering::RenderWorld& out);

/// The scene's local lights, already in the shape the renderer takes (Phase 26).
///
/// A point or spot light is positioned by the entity's world Transform and aimed
/// by its own `dir` — see `PointLightComponent` for why one comes from the graph
/// and the other does not. Disabled lights are left out entirely: they cost
/// nothing downstream instead of costing a shader iteration that multiplies by
/// zero, and the entity stays selected and named in the editor.
///
/// Order is entity id, ascending — NOT storage order. A sparse set reorders on
/// removal, so storage order would decide *which* of twenty torches lost its
/// shadow map, and closing one debris shard or undoing a paste could move the
/// surviving lights around the level. Sorting makes the ones that fit stable.
///
/// `kMaxPointLights` / `kMaxSpotLights` are the renderer's slot count, and it
/// clamps silently. That is right for the renderer — it must never be able to
/// overflow a GPU array — but wrong for an author, whose 9th torch would simply
/// not exist and nothing would say so. So the cap is applied here, counted, and
/// reported: `dropped_points` / `dropped_spots` are what the scene asked for and
/// the frame could not draw. The runtime logs it; a headless tool reads it.
struct LocalLights {
    std::vector<rendering::PointLight> points;
    std::vector<rendering::SpotLight> spots;
    uint32_t dropped_points = 0;
    uint32_t dropped_spots = 0;
};

LocalLights collect_local_lights(const ecs::World& world);

} // namespace nf::runtime
