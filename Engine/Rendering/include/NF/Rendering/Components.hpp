#pragma once

// NF/Rendering/Components.hpp — rendering-related ECS components
//
// These components live in the ECS world (the game's domain) but only ever
// reference render assets through handles — never through backend pointers.
// The renderer reads them once per frame during extraction; after that point
// it works exclusively on RenderWorld data.

#include <NF/Core/Types.hpp>
#include <NF/Rendering/Handles.hpp>

namespace nf::rendering {

/// Marks an entity as renderable. The extraction step turns this into a
/// RenderObject; the renderer never sees this struct again.
struct MeshComponent {
    StaticMeshHandle mesh;
    MaterialHandle material;
    bool visible = true;
    u32 lod = 0; // which LOD to draw (LOD selection is a later milestone)
};

} // namespace nf::rendering
