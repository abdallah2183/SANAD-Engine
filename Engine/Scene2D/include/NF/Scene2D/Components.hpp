#pragma once

// NF/Scene2D/Components.hpp — ECS components and systems for the 2D layer.
// Design doc Section 54 (2D Engine): "with a layer independent of 3D gameplay".
//
// The 2D layer does not own a second ECS; it reuses nf::ecs::World and adds the
// component types a 2D game needs. A scene can hold a 3D gameplay world and a
// 2D world side by side, each with its own systems, exactly as the design doc
// describes: 2D is a peer layer, not a degraded 3D.
//
// Ownership rule: components hold *descriptions*, systems hold *state*. A
// Body2DComponent describes the collider; the PhysicsWorld2D owns the body and
// the component caches only the handle. Keeping the description in the
// component is what lets a scene be saved and reloaded with its physics intact.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/ECS/ECS.hpp>

#include "Camera2D.hpp"
#include "Light2D.hpp"
#include "Math2D.hpp"
#include "Particles2D.hpp"
#include "Physics2D.hpp"
#include "SpriteBatcher.hpp"
#include "Tilemap.hpp"

namespace nf::scene2d {

/// Local transform of a 2D entity: translation, rotation and scale, in that
/// order, matching nf::scene::Transform's TRS convention so a converter between
/// the 2D and 3D layers is a plain field copy.
struct Transform2D {
    Vec2 position{0.0f, 0.0f};
    f32 rotation_deg = 0.0f;   ///< Clockwise in y-down space.
    Vec2 scale{1.0f, 1.0f};

    /// Local transform matrix for this entity.
    Mat3x2 local_matrix() const {
        return Mat3x2::compose(position, rotation_deg, scale);
    }
};

/// Cached world transform. Written by `transform2d_system`, read by every
/// rendering system, so a parent chain is walked once per frame instead of once
/// per consumer.
struct WorldTransform2D {
    Mat3x2 world{Mat3x2::identity()};
    /// Optional parent; Entity() (invalid) means "no parent".
    ecs::Entity parent{};
    bool dirty = true;
};

/// A drawable quad. It *is* a SpriteDraw: the system copies it into the batcher
/// and overrides position with the world transform, so an author editing a
/// sprite in the inspector is editing the exact struct the renderer consumes.
struct SpriteComponent {
    SpriteDraw draw;
};

/// Describes a physics body. The handle is filled in by `physics2d_system` on
/// first run and stays valid for the entity's lifetime.
struct Body2DComponent {
    /// Body description: shape, density, flags. Position/angle are seeded from
    /// the entity's Transform2D at creation and then owned by the world.
    Body2D body;
    /// Handle into PhysicsWorld2D; invalid until the system creates the body.
    BodyHandle handle;
    /// When true, Transform2D drives the body each step (kinematic-style
    /// platforms moved by animation or a script). When false, the body drives
    /// Transform2D — the usual case for dynamic props.
    bool drive_from_transform = false;
    /// Recreate the body next step. Set after editing shape/material fields.
    bool needs_rebuild = true;
};

/// Holds a complete tilemap. Big by design: a layer's chunks live here.
struct TilemapComponent {
    Tilemap map;
    /// Atlas page and UV table used to render the map's tiles. Kept on the
    /// component because a map's tileset is shared by every layer of the map.
    /// Tile ids are 1-based: id 1 is the top-left cell of the atlas, and
    /// `kEmptyTile` (0) renders as nothing.
    u32 atlas_page = 0;
    u32 atlas_w = 256;        ///< Page width in texels, for UV math.
    u32 atlas_h = 256;        ///< Page height in texels.
    u32 atlas_tile_count = 16;  ///< Tiles per row in the atlas.
    u32 tile_texel_size = 16;   ///< Texels per tile edge (square tiles).
    bool dirty = true;          ///< Re-extract collision next physics step.
    /// Static bodies created for the map's collision rects. A handle cache,
    /// like Body2DComponent::handle: the map owns the description, the physics
    /// world owns the bodies, and this is how a re-extract finds the old ones
    /// to destroy them.
    std::vector<BodyHandle> collision_handles;
};

/// A 2D camera. Usually one per scene; the component form lets a world hold a
/// main camera and a UI camera in the same ECS world.
struct CameraComponent2D {
    Camera2D camera;
    bool is_active = true;
};

/// A particle emitter instance.
struct ParticlesComponent2D {
    Particles2D particles;
    /// Emit position override; when false the entity's world position is used.
    bool use_world_position = true;
    Vec2 emit_origin{0.0f, 0.0f};
};

/// One 2D light. The position is seeded from the entity's Transform2D (or its
/// world transform, if it has one) and then owned by the component, so a script
/// can move a torch by writing `light.position` directly.
struct LightComponent2D {
    Light2D light;
    /// When true, Transform2D/WorldTransform2D drives `light.position` each
    /// lighting step; a swinging lantern hung from a parent bone wants this on,
    /// a flickering pickup that owns its position wants it off.
    bool follow_transform = true;
};

/// Scene-wide lighting: the light list, the occluder set and the optional shadow
/// bake. One per 2D world — it is the *scene's* lighting, not a per-entity
/// property, which is also why `sprite_system`'s lit overload takes it by const
/// reference rather than querying for it.
struct LightingComponent2D {
    Lighting2D lighting;
    /// Advances every lighting step and feeds flicker (design doc Section 114:
    /// time is a deterministic function of step count, not of wall clock).
    f32 time = 0.0f;
    /// Re-extract occluders from every TilemapComponent next step. Set by the
    /// tilemap system when a tile changes; the occluder set is expensive enough
    /// to rebuild that it is worth waiting for the caller to ask.
    bool occluders_dirty = true;
};

// ---------------------------------------------------------------------------
// Systems
// ---------------------------------------------------------------------------

/// Recomputes WorldTransform2D for every entity with a Transform2D, honouring
/// the parent chain. Two passes over the storage: roots first, then children,
/// which handles chains of arbitrary depth without recursion.
void transform2d_system(ecs::World& world);

/// Collects every SpriteComponent into the batcher against `camera`. Culls
/// sprites entirely outside the visible bounds — a thousand off-screen tiles
/// should never reach the vertex bake.
void sprite_system(ecs::World& world, SpriteBatcher& batcher,
                   const Camera2D& camera);

/// As above, but each visible sprite's packed colour is re-lit by `lighting`
/// first: `unpack_color_rgb` × `sample` → `pack_color_vec`. A sprite's own tint
/// survives — lighting multiplies it, it does not replace it — so a red sprite
/// stays red under a white light and goes orange under a firelight. Alpha is
/// untouched, because lighting a thing invisible is meaningless and a lit
/// alpha would break UI fades.
void sprite_system(ecs::World& world, SpriteBatcher& batcher,
                   const Camera2D& camera, const Lighting2D& lighting);

/// Gathers LightComponent2D into `lighting`, syncs occluders from dirty
/// TilemapComponents, advances time by `dt` and re-bakes the shadows. Run after
/// `physics2d_system` and `transform2d_system` and before the sprite systems:
/// lights move with the bodies that carry them, and the bake has to be current
/// before anything samples it.
void lighting2d_system(ecs::World& world, LightingComponent2D& lighting, f32 dt);

/// Renders the visible region of every TilemapComponent: culls chunks outside
/// the camera, resolves auto-tile variants, and pushes one sprite per tile.
void tilemap_render_system(ecs::World& world, SpriteBatcher& batcher,
                           const Camera2D& camera);

/// Creates and refreshes bodies from Body2DComponent, steps `phys` by `dt`, then
/// writes results back to Transform2D. Rebuilds collision meshes of dirty
/// TilemapComponents into `phys` via `extract_all_collision`.
void physics2d_system(ecs::World& world, PhysicsWorld2D& phys, f32 dt,
                     Vec2 gravity);

/// Steps all particle emitters by `dt`.
void particles2d_system(ecs::World& world, f32 dt, Vec2 gravity);

} // namespace nf::scene2d
