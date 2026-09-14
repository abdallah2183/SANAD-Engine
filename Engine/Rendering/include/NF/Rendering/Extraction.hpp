#pragma once

// NF/Rendering/Extraction.hpp — Game/ECS → RenderWorld extraction
//
// Design:
//   void ExtractRenderWorld(const GameWorld& game, RenderWorld& render);
//
// The renderer never sees GameWorld or ecs::World directly. This keeps
// gameplay state (Inventory, Quest, Health) from leaking into rendering and
// allows the two worlds to be double-buffered.
//
// Two extraction paths exist:
//   1. extract_render_world           — minimal game::GameWorld stand-in
//   2. extract_render_objects         — real ECS path (Transform + MeshComponent)
//
// Path 2 is the production boundary:
//
//   ecs::World → extract_render_objects → RenderWorld → cull → Renderer3D
//
// After extraction, the renderer works only on RenderWorld data. Replacing
// the extraction implementation (multithreaded, delta-based) must not touch
// anything downstream.

#include <NF/ECS/ECS.hpp>
#include <NF/Rendering/GameWorld.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/RenderWorld.hpp>

namespace nf::rendering {

// Legacy path: minimal game world → render world
void extract_render_world(const game::GameWorld& game_world, RenderWorld& render_world);
void extract_render_world_double_buffered(const game::GameWorld& game_world, RenderWorldBuffer& buffer);

/// ECS path: every entity with scene::Transform + MeshComponent becomes a
/// RenderObject carrying its world translation, mesh/material handles and
/// world-space bounds (transformed from the mesh's local bounds via the mesh
/// library). Entities without a Transform or MeshComponent — and invisible
/// ones — are skipped entirely: gameplay-only components never cross this line.
void extract_render_objects(const ecs::World& world, const MeshLibrary& meshes, RenderWorld& out);

} // namespace nf::rendering
