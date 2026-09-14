#pragma once

// NF/Rendering/Extraction.hpp — game::GameWorld → RenderWorld extraction
//
// The renderer never sees the game world directly. This keeps gameplay state
// (Inventory, Quest, Health) from leaking into rendering and allows the two
// worlds to be double-buffered.
//
//   game::GameWorld -> extract_render_world -> RenderWorld -> cull -> Renderer3D
//
// SCOPE NOTE (Phase 11, W2). This header used to also declare an ECS path,
// `extract_render_objects(const ecs::World&, ...)`, which made the renderer
// include `<NF/ECS/ECS.hpp>` and `<NF/Scene/Transform.hpp>` and link both. That
// was a layering inversion: the renderer could see the world it was drawing, and
// `Engine/Rendering` could not be built without the ECS and the scene graph —
// which blocks a dedicated-server build and any offscreen tooling that wants the
// renderer without the game world.
//
// The ECS bridge now lives in `NF/Runtime/SceneExtraction.hpp`, in the layer that
// legitimately knows about both sides at once. `game::GameWorld` below is a
// *rendering* type (NF/Rendering/GameWorld.hpp), so this path has no such problem
// and stays here.

#include <NF/Rendering/GameWorld.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/RenderWorld.hpp>

namespace nf::rendering {

// Minimal game-world stand-in → render world. The lightweight path, for callers
// that have a `GameWorld` and no ECS.
void extract_render_world(const game::GameWorld& game_world, RenderWorld& render_world);
void extract_render_world_double_buffered(const game::GameWorld& game_world, RenderWorldBuffer& buffer);

} // namespace nf::rendering
