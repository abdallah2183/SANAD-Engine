#pragma once

// NF/Editor/Prefabs.hpp — prefab template helpers (Phase 5).
//
// A prefab IS a .nfscene file (same serializer, no new format). An instance
// is a transplanted subtree whose root carries PrefabLinkComponent{path}.
// Local edits are free (overrides); Apply snapshots the subtree back over the
// file; Revert replaces the subtree from the file. Nested links are preserved
// verbatim — only the top link drives Apply/Revert.

#include <NF/ECS/ECS.hpp>
#include <NF/Scene/Scene.hpp>

#include <string>
#include <vector>

namespace nf::editor {

// Root-first pre-order collection of root + all descendants.
std::vector<ecs::Entity> collect_subtree(const ecs::World& world, ecs::Entity root);

// Deep-copies root's subtree into dst (all v0.1 components incl. links),
// remapping internal parents; dst_parent (alive or invalid) becomes the new
// root's parent. Returns the new root (invalid on dead root).
ecs::Entity clone_subtree(const ecs::World& src, ecs::Entity root, ecs::World& dst,
                           ecs::Entity dst_parent);

// Clones every root of a prefab scene under dst_parent. Returns new roots.
std::vector<ecs::Entity> clone_prefab_roots(const scene::Scene& prefab, ecs::World& dst,
                                            ecs::Entity dst_parent);

// Template roots: entities with no live parent inside the prefab scene.
std::vector<ecs::Entity> prefab_template_roots(const scene::Scene& prefab);

} // namespace nf::editor
