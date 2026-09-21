// NF/Scene2D/Systems.cpp — ECS systems for the 2D layer.
// Design doc Section 54 (2D Engine) and Section 55 (Tilemap).
//
// Pipeline order the systems assume, from the scene's fixed update:
//   1. physics2d_system  — moves bodies, writes results into Transform2D
//   2. transform2d_system — lifts local transforms into WorldTransform2D
//   3. lighting2d_system — moves lights with their carriers, re-bakes shadows
//   4. sprite_system / tilemap_render_system — consume WorldTransform2D
//   5. particles2d_system
// Physics before transforms so a render pass never draws a sprite where the
// body was a step ago; transforms before rendering so a parent chain is walked
// once per frame instead of once per sprite; lighting before sprites because a
// lit sprite samples a bake that has to be one step current, not two.

#include <NF/Scene2D/Components.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace nf::scene2d {

namespace {

/// World-space position of an entity: the cached world transform when there is
/// one (so a child emitter follows its parent), else its local position.
Vec2 entity_world_position(ecs::World& world, ecs::Entity e) {
    if (const WorldTransform2D* wt = world.get<WorldTransform2D>(e)) {
        return Vec2{wt->world.tx, wt->world.ty};
    }
    if (const Transform2D* t = world.get<Transform2D>(e)) return t->position;
    return Vec2{0.0f, 0.0f};
}

/// Componentwise product — the one place this layer needs it (a sprite tint
/// times a light colour). `Vec3` deliberately does not define it: the 3D code
/// keeps colours and directions as distinct uses of the same shape, and a `*`
/// between them would be ambiguous.
Vec3 hadamard(Vec3 a, Vec3 b) {
    return Vec3{a.x * b.x, a.y * b.y, a.z * b.z};
}

/// Resolves one sprite's world placement: folds the world transform into the
/// authored sprite transform, factors the product back into the (position,
/// rotation, size) triple the batcher consumes, and culls against the visible
/// bounds at the sprite's own parallax. Returns false when the quad is entirely
/// off-screen, in which case nothing is drawn and `out_centre` is untouched;
/// otherwise `out_centre` is the world-space quad centre, which is the point
/// lighting is sampled at.
bool bake_sprite(SpriteDraw& d, const WorldTransform2D& wt, const Camera2D& camera,
                 Vec2& out_centre) {
    // This rebuilds the exact matrix the batcher bakes — S(size),
    // T(-anchor*size), R, T(position), then the world transform — and factors
    // the product back. Exact whenever the combined linear part keeps
    // orthogonal rows (a scaled, rotated parent); a sheared product —
    // non-uniform parent scale followed by sprite rotation — has no
    // rotation-scale factorisation, and `decompose_affine` returns the nearest
    // one, which is what a sprite renderer wants anyway.
    const Vec2 anchor_offset{d.anchor.x * d.size.x, d.anchor.y * d.size.y};
    const Mat3x2 local = Mat3x2::scaling(d.size) *
                         Mat3x2::translation(-anchor_offset) *
                         Mat3x2::rotation_deg(d.rotation_deg) *
                         Mat3x2::translation(d.position);
    const Mat3x2 full = local * wt.world;

    Vec2 translation{0.0f, 0.0f};
    f32 rotation_deg = 0.0f;
    Vec2 size{1.0f, 1.0f};
    decompose_affine(full, translation, rotation_deg, size);

    // Cull at the sprite's own parallax: a far background and the gameplay
    // layer are visible under different camera positions, so one shared rect
    // would pop sprites at the edge of a scrolling layer. The circumradius is
    // the conservative bound once the quad can rotate; the centre is the
    // translation plus the linear part on the unit square's middle, which stays
    // the quad's centre for every anchor.
    out_centre = translation + full.transform_vector(Vec2{0.5f, 0.5f});
    const Rect vis = camera.visible_bounds(d.parallax);
    const f32 radius = std::hypot(size.x, size.y) * 0.5f;
    if (out_centre.x + radius < vis.left() || out_centre.x - radius > vis.right()) return false;
    if (out_centre.y + radius < vis.top() || out_centre.y - radius > vis.bottom()) return false;

    // The emitted position is the world image of the unit square's anchor point
    // — the point the batcher's T(position) parks — so keeping the authored
    // anchor means a corner-anchored sprite stays anchored to that corner after
    // parenting instead of jumping to its centre.
    d.position = translation + full.transform_vector(d.anchor);
    d.size = size;
    d.rotation_deg = rotation_deg;
    return true;
}

} // namespace

void sprite_system(ecs::World& world, SpriteBatcher& batcher,
                   const Camera2D& camera) {
    for (const ecs::Entity e : world.query<SpriteComponent, WorldTransform2D>()) {
        const SpriteComponent* sc = world.get<SpriteComponent>(e);
        const WorldTransform2D* wt = world.get<WorldTransform2D>(e);
        if (sc == nullptr || wt == nullptr) continue;

        SpriteDraw d = sc->draw;
        Vec2 centre{0.0f, 0.0f};
        if (!bake_sprite(d, *wt, camera, centre)) continue;
        batcher.push(d);
    }
}

void sprite_system(ecs::World& world, SpriteBatcher& batcher,
                   const Camera2D& camera, const Lighting2D& lighting) {
    for (const ecs::Entity e : world.query<SpriteComponent, WorldTransform2D>()) {
        const SpriteComponent* sc = world.get<SpriteComponent>(e);
        const WorldTransform2D* wt = world.get<WorldTransform2D>(e);
        if (sc == nullptr || wt == nullptr) continue;

        SpriteDraw d = sc->draw;
        Vec2 centre{0.0f, 0.0f};
        if (!bake_sprite(d, *wt, camera, centre)) continue;

        // One lighting sample per sprite, at the quad's centre: the alternative
        // is four samples for a gradient nobody can read at sprite resolution,
        // and the difference disappears under the saturating pack. The sprite's
        // own tint multiplies the light — lighting darkens and colours, it never
        // replaces — and alpha is preserved exactly, because a lit alpha would
        // break a UI fade and make invisibility brightness-dependent.
        const Vec3 lit = hadamard(unpack_color_rgb(d.color), lighting.sample(centre));
        const f32 alpha = static_cast<f32>((d.color >> 24u) & 0xFFu) / 255.0f;
        d.color = pack_color_vec(lit, alpha);
        batcher.push(d);
    }
}

void transform2d_system(ecs::World& world) {
    std::vector<ecs::Entity> entities = world.query<Transform2D>();

    // Every entity that can be placed gets a world-transform cache. Adding it
    // here rather than requiring it at construction means a script can spawn a
    // bare Transform2D and it simply works.
    for (const ecs::Entity e : entities) {
        if (!world.has<WorldTransform2D>(e)) world.add<WorldTransform2D>(e);
    }

    // Everything starts unresolved; resolution clears the flag as it goes, so
    // when this system returns, `dirty == false` is a promise to the rendering
    // systems that the cached matrix is current.
    for (const ecs::Entity e : entities) {
        world.get<WorldTransform2D>(e)->dirty = true;
    }

    // Pass 1 — roots. A parent that has left the world, or never had a
    // transform, is treated as "no parent" rather than as a blocked chain: a
    // 2D entity parented to a 3D one (a minimap anchored to the player camera)
    // still needs a world matrix.
    for (const ecs::Entity e : entities) {
        WorldTransform2D* wt = world.get<WorldTransform2D>(e);
        if (world.is_alive(wt->parent) && world.has<WorldTransform2D>(wt->parent)) continue;
        const Transform2D* t = world.get<Transform2D>(e);
        wt->world = t->local_matrix();
        wt->dirty = false;
    }

    // Pass 2 — children, repeated until the walk stops progressing. Each pass
    // resolves every entity whose parent is already resolved, so a chain of N
    // resolves in at most N passes. The same bound is the cycle guard: two
    // entities parenting each other never resolve, and the flag staying set is
    // how a debug overlay finds them instead of the loop hanging the frame.
    const usize max_passes = entities.size() + 1u;
    for (usize pass = 0u; pass < max_passes; ++pass) {
        bool progressed = false;
        for (const ecs::Entity e : entities) {
            WorldTransform2D* wt = world.get<WorldTransform2D>(e);
            if (!wt->dirty) continue;
            const WorldTransform2D* parent = world.get<WorldTransform2D>(wt->parent);
            if (parent == nullptr || parent->dirty) continue;
            const Transform2D* t = world.get<Transform2D>(e);
            wt->world = t->local_matrix() * parent->world;
            wt->dirty = false;
            progressed = true;
        }
        if (!progressed) break;
    }
}

void lighting2d_system(ecs::World& world, LightingComponent2D& lighting, f32 dt) {
    // --- occluders --------------------------------------------------------
    // Rebuilt from every tilemap at once, not per map: a wall casts the same
    // shadow whichever layer it was authored in, and one merged set keeps the
    // ray tests and the bake over a single list. The extraction is a full scan
    // of the collidable layers, so it runs only when a tile actually changed.
    if (lighting.occluders_dirty) {
        lighting.occluders_dirty = false;
        std::vector<Rect> rects;
        for (const ecs::Entity e : world.query<TilemapComponent>()) {
            const TilemapComponent* tc = world.get<TilemapComponent>(e);
            if (tc == nullptr) continue;
            const std::vector<Rect> layer_rects = tc->map.extract_all_collision();
            rects.insert(rects.end(), layer_rects.begin(), layer_rects.end());
        }
        lighting.lighting.set_occluders_from_rects(rects);
    }

    // --- lights -----------------------------------------------------------
    // Collected fresh every step rather than cached: the cache would need
    // invalidation on every spawn, destroy and re-parent, and the collection
    // itself is one query. Insertion order is the sample order, which is the
    // determinism contract (Section 114) — the ECS query order, not a sort, so
    // a scene replays with the same light list.
    lighting.lighting.clear_lights();
    for (const ecs::Entity e : world.query<LightComponent2D>()) {
        LightComponent2D* lc = world.get<LightComponent2D>(e);
        if (lc == nullptr) continue;

        // A light that follows its carrier takes the entity's resolved world
        // position, which is already in the carrier's parallax space — the same
        // space `bake_sprite` produces sprite centres in — so a torch parented
        // to a platform lights the sprites standing on it. A light that owns its
        // position (a flickering pickup, a scripted beacon) keeps it, which is
        // why the flag exists at all.
        if (lc->follow_transform) {
            lc->light.position = entity_world_position(world, e);
        }
        lighting.lighting.add_light(lc->light);
    }

    // --- bake -------------------------------------------------------------
    // Steps, not wall clock: a fixed `dt` makes the accumulated time an exact
    // binary fraction, so the flicker phase resolves identically on every run
    // and on a headless test (Section 114).
    lighting.time += dt;
    lighting.lighting.bake(lighting.time);
}

void tilemap_render_system(ecs::World& world, SpriteBatcher& batcher,
                           const Camera2D& camera) {
    for (const ecs::Entity e : world.query<TilemapComponent>()) {
        const TilemapComponent* tc = world.get<TilemapComponent>(e);
        if (tc == nullptr) continue;
        const Tilemap& map = tc->map;
        if (map.layers.empty()) continue;

        // One tile's UV footprint in the atlas, plus a half-texel inset so a
        // bilinear filter never bleeds into the neighbouring tile.
        const f32 cell_u = static_cast<f32>(tc->tile_texel_size) /
                           static_cast<f32>(tc->atlas_w);
        const f32 cell_v = static_cast<f32>(tc->tile_texel_size) /
                           static_cast<f32>(tc->atlas_h);
        const f32 inset_u = 0.5f / static_cast<f32>(tc->atlas_w);
        const f32 inset_v = 0.5f / static_cast<f32>(tc->atlas_h);
        const u32 atlas_tiles = tc->atlas_tile_count *
                                (tc->atlas_h / tc->tile_texel_size);

        for (usize layer_index = 0u; layer_index < map.layers.size();
             ++layer_index) {
            const TilemapLayer& layer = map.layers[layer_index];
            const f32 ts = layer.tile_size;
            const Rect vis = camera.visible_bounds(layer.parallax);

            // Chunk keys are gathered and sorted rather than iterated in map
            // order: the batcher's stable sort only fixes the order of sprites
            // sharing (page, depth), and every tile in a layer shares both, so
            // unordered_map order would decide the vertex order. Sorting by
            // (y, x) makes the baked buffer reproducible on any standard
            // library, which is what a recorded demo and a byte-compared
            // render need.
            std::vector<ChunkKey> keys;
            keys.reserve(layer.chunks.size());
            for (const auto& kv : layer.chunks) keys.push_back(kv.first);
            std::sort(keys.begin(), keys.end(),
                      [](const ChunkKey& a, const ChunkKey& b) {
                          if (a.y != b.y) return a.y < b.y;
                          return a.x < b.x;
                      });

            const f32 chunk_world = static_cast<f32>(Chunk::kSize) * ts;

            for (const ChunkKey& key : keys) {
                // Chunk cull before touching a single tile: a map a thousand
                // chunks wide costs 16x16 lookups only where it is visible.
                const Rect chunk_rect{
                    static_cast<f32>(key.x) * chunk_world,
                    static_cast<f32>(key.y) * chunk_world,
                    chunk_world, chunk_world};
                if (!chunk_rect.overlaps(vis)) continue;

                const Chunk& chunk = layer.chunks.at(key);
                for (u32 ly = 0u; ly < Chunk::kSize; ++ly) {
                    for (u32 lx = 0u; lx < Chunk::kSize; ++lx) {
                        const u32 packed = chunk.at(lx, ly);
                        const u32 id = tile_id(packed);
                        if (id == kEmptyTile) continue;

                        // Auto-tiling picks the variant from the
                        // neighbourhood; hand-authored layers draw the id as
                        // stored, flips and all.
                        const i64 tx = key.x * static_cast<i64>(Chunk::kSize) +
                                       static_cast<i64>(lx);
                        const i64 ty = key.y * static_cast<i64>(Chunk::kSize) +
                                       static_cast<i64>(ly);
                        const u32 variant = layer.auto_tile
                                                ? map.resolve_variant(layer, tx, ty)
                                                : id;
                        const u32 cell = variant - 1u;
                        if (cell >= atlas_tiles) continue;

                        const u32 row = cell / tc->atlas_tile_count;
                        const u32 col = cell % tc->atlas_tile_count;

                        SpriteDraw d;
                        d.page = layer.atlas_page != 0u ? layer.atlas_page
                                                        : tc->atlas_page;
                        d.u0 = static_cast<f32>(col) * cell_u + inset_u;
                        d.u1 = d.u0 + cell_u - inset_u - inset_u;
                        d.v0 = static_cast<f32>(row) * cell_v + inset_v;
                        d.v1 = d.v0 + cell_v - inset_v - inset_v;
                        d.position = Vec2{
                            chunk_rect.x + static_cast<f32>(lx) * ts,
                            chunk_rect.y + static_cast<f32>(ly) * ts};
                        d.size = Vec2{ts, ts};
                        d.anchor = Vec2{0.0f, 0.0f};
                        d.rotation_deg =
                            (tile_flags(packed) & TileFlag_Rot90) != 0u ? 90.0f
                                                                       : 0.0f;
                        d.flip_x = (tile_flags(packed) & TileFlag_FlipX) != 0u;
                        d.flip_y = (tile_flags(packed) & TileFlag_FlipY) != 0u;
                        // Layer 0 furthest back, later layers in front; the
                        // batcher sorts ascending depth within a page.
                        d.depth = static_cast<f32>(layer_index);
                        d.parallax = layer.parallax;
                        batcher.push(d);
                    }
                }
            }
        }
    }
}

void physics2d_system(ecs::World& world, PhysicsWorld2D& phys, f32 dt,
                      Vec2 gravity) {
    phys.set_gravity(gravity);

    // --- Terrain ----------------------------------------------------------
    // A dirty map re-extracts its collision rects and rebuilds the static
    // bodies for them. Extracting is a scan of the collidable layers, so it
    // only happens when something actually changed a tile.
    for (const ecs::Entity e : world.query<TilemapComponent>()) {
        TilemapComponent* tc = world.get<TilemapComponent>(e);
        if (tc == nullptr || !tc->dirty) continue;
        tc->dirty = false;

        for (const BodyHandle old : tc->collision_handles) phys.destroy_body(old);
        tc->collision_handles.clear();

        const std::vector<Rect> rects = tc->map.extract_all_collision();
        tc->collision_handles.reserve(rects.size());
        for (const Rect& r : rects) {
            Body2D body;
            body.shape = Shape2D::box(r.w * 0.5f, r.h * 0.5f);
            body.position = r.center();
            // Terrain is massless: it never integrates, only presents a
            // negative-infinity-mass surface to the dynamic bodies.
            body.is_static = true;
            body.friction = 0.7f;
            tc->collision_handles.push_back(phys.create_body(body));
        }
    }

    // --- Bodies: create / rebuild / drive ---------------------------------
    for (const ecs::Entity e : world.query<Body2DComponent>()) {
        Body2DComponent* c = world.get<Body2DComponent>(e);
        if (c == nullptr) continue;
        Transform2D* t = world.get<Transform2D>(e);

        // A rebuild is a destroy-then-create so the generation bumps and any
        // handle still held by gameplay code fails loudly instead of driving
        // the replacement body. Also covers a handle left dangling by a
        // cleared world.
        if (c->handle.valid() && (c->needs_rebuild || phys.body(c->handle) == nullptr)) {
            phys.destroy_body(c->handle);
            c->handle = BodyHandle{};
        }
        if (!c->handle.valid()) {
            Body2D body = c->body;
            // Seed from the entity's placement, once. After this the body owns
            // its pose unless `drive_from_transform` says otherwise.
            if (t != nullptr) {
                body.position = t->position;
                body.angle = t->rotation_deg * DEG_TO_RAD;
            }
            c->handle = phys.create_body(body);
            c->needs_rebuild = false;
        }

        Body2D* b = phys.body(c->handle);
        if (b == nullptr) continue;

        if (c->drive_from_transform && t != nullptr && dt > 0.0f) {
            // Kinematic: the transform is the authority. The movement it implies
            // becomes the body's velocity this step, and `integrate_positions`
            // then lands the body exactly on the target — writing the position
            // here too would apply the displacement twice. The velocity is what
            // lets a player standing on a moving platform ride it: the contact
            // solver reads it at the support point.
            b->velocity = (t->position - b->position) / dt;
            b->angular_velocity = (t->rotation_deg * DEG_TO_RAD - b->angle) / dt;
        }
    }

    phys.step(dt);

    // --- Write back -------------------------------------------------------
    for (const ecs::Entity e : world.query<Body2DComponent>()) {
        const Body2DComponent* c = world.get<Body2DComponent>(e);
        if (c == nullptr) continue;
        const Body2D* b = phys.body(c->handle);
        if (b == nullptr) continue;
        Transform2D* t = world.get<Transform2D>(e);
        // Driven bodies already wrote the transform; copying back would feed
        // the body's own position to itself and erase the authored pose.
        if (t != nullptr && !c->drive_from_transform) {
            t->position = b->position;
            t->rotation_deg = b->angle * RAD_TO_DEG;
        }
    }
}

void particles2d_system(ecs::World& world, f32 dt, Vec2 gravity) {
    for (const ecs::Entity e : world.query<ParticlesComponent2D>()) {
        ParticlesComponent2D* pc = world.get<ParticlesComponent2D>(e);
        if (pc == nullptr) continue;
        const Vec2 origin = pc->use_world_position
                                ? entity_world_position(world, e)
                                : pc->emit_origin;
        pc->particles.update(origin, dt, gravity);
    }
}

} // namespace nf::scene2d
