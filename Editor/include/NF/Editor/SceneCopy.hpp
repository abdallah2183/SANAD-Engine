#pragma once

// NF/Editor/SceneCopy.hpp — copy one entity's components between worlds.
//
// This is deliberately a component-by-component copy rather than a raw ECS
// record copy: live handles (physics bodies, vehicle views, character intent)
// are session state and must be scrubbed, while every authored component has to
// survive. Both the Play snapshot and Edit > Duplicate use it, so a component
// added to a scene is copied in exactly one place.

#include <NF/ECS/ECS.hpp>

namespace nf::editor {

/// Copies every authored component of `from` in `src` onto `to` in `dst`.
/// Transform is copied too (including its parent); callers that are rebuilding
/// a hierarchy overwrite the parent afterwards. Live handles are reset:
/// RigidBody.body, Character wish/jump/grounded, Vehicle inputs/view.
void copy_entity_components(const ecs::World& src, ecs::Entity from, ecs::World& dst,
                            ecs::Entity to);

} // namespace nf::editor
