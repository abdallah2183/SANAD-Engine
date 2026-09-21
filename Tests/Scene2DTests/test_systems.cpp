// Scene2DTests/systems — the five 2D systems, and the matrix algebra they share
// with the batcher.
//
// The earlier suite counted vertices and never looked at where they landed, which
// is how a quad-placed-at-the-wrong-point bug survived it. These tests assert
// positions, so a placement regression moves a number instead of hiding behind a
// count.

#include <NF/Scene2D/Components.hpp>
#include <NF/Test/TestFramework.hpp>

#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::scene2d;

// ---------------------------------------------------------------------------
// Matrix algebra the systems depend on
// ---------------------------------------------------------------------------

NF_TEST(scene2d_decompose_affine_round_trips_trs) {
    const Mat3x2 m = Mat3x2::compose(Vec2{3.0f, -2.0f}, 35.0f, Vec2{2.0f, 0.5f});
    Vec2 translation{0.0f, 0.0f};
    f32 rotation_deg = 0.0f;
    Vec2 scale{1.0f, 1.0f};
    NF_CHECK(decompose_affine(m, translation, rotation_deg, scale));
    NF_CHECK_NEAR(translation.x, 3.0f, 1e-5f);
    NF_CHECK_NEAR(translation.y, -2.0f, 1e-5f);
    NF_CHECK_NEAR(rotation_deg, 35.0f, 1e-4f);
    NF_CHECK_NEAR(scale.x, 2.0f, 1e-5f);
    NF_CHECK_NEAR(scale.y, 0.5f, 1e-5f);
}

NF_TEST(scene2d_decompose_affine_reads_mirror_as_negative_scale) {
    // A mirrored parent has a negative determinant. There is no rotation that
    // expresses a mirror, so the factorisation must report it as a negative y
    // scale — the batcher then draws the quad flipped rather than inside out.
    const Mat3x2 m = Mat3x2::compose(Vec2{0.0f, 0.0f}, 0.0f, Vec2{-1.0f, 1.0f});
    Vec2 translation{0.0f, 0.0f};
    f32 rotation_deg = 0.0f;
    Vec2 scale{1.0f, 1.0f};
    NF_CHECK(decompose_affine(m, translation, rotation_deg, scale));
    NF_CHECK(scale.y < 0.0f);
    NF_CHECK_NEAR(scale.x, 1.0f, 1e-5f);
}

NF_TEST(scene2d_decompose_affine_collapses_to_identity) {
    // A zero scale has no rotation; the contract is "identity out, no NaN",
    // because a NaN scale reaches the GPU as a degenerate quad.
    const Mat3x2 m = Mat3x2::compose(Vec2{1.0f, 1.0f}, 0.0f, Vec2{0.0f, 0.0f});
    Vec2 translation{9.0f, 9.0f};
    f32 rotation_deg = 45.0f;
    Vec2 scale{7.0f, 7.0f};
    NF_CHECK(!decompose_affine(m, translation, rotation_deg, scale));
    NF_CHECK(std::isfinite(translation.x) && std::isfinite(scale.x));
    NF_CHECK_NEAR(scale.x, 1.0f, 1e-5f);
    NF_CHECK_NEAR(scale.y, 1.0f, 1e-5f);
}

// ---------------------------------------------------------------------------
// transform2d_system
// ---------------------------------------------------------------------------

NF_TEST(scene2d_transform_system_resolves_parent_chain) {
    ecs::World world;
    const ecs::Entity root = world.create_entity();
    world.add<Transform2D>(root, Transform2D{Vec2{10.0f, 0.0f}, 0.0f, Vec2{1.0f, 1.0f}});
    const ecs::Entity child = world.create_entity();
    world.add<Transform2D>(child, Transform2D{Vec2{1.0f, 2.0f}, 0.0f, Vec2{1.0f, 1.0f}});
    WorldTransform2D cw;
    cw.parent = root;
    world.add<WorldTransform2D>(child, cw);
    const ecs::Entity grandchild = world.create_entity();
    world.add<Transform2D>(grandchild, Transform2D{Vec2{0.0f, 3.0f}, 90.0f, Vec2{1.0f, 1.0f}});
    WorldTransform2D gw;
    gw.parent = child;
    world.add<WorldTransform2D>(grandchild, gw);

    transform2d_system(world);

    // Translations compose additively down an unroted chain; the grandchild's own
    // 90 degree spin moves its origin nowhere.
    const WorldTransform2D* wt = world.get<WorldTransform2D>(grandchild);
    NF_CHECK(wt != nullptr);
    NF_CHECK(!wt->dirty);
    const Vec2 origin = wt->world.transform_point(Vec2{0.0f, 0.0f});
    NF_CHECK_NEAR(origin.x, 11.0f, 1e-5f);
    NF_CHECK_NEAR(origin.y, 5.0f, 1e-5f);
    // A point one unit right of the grandchild's origin is 90 degrees clockwise
    // from there — the rotation survived the parenting.
    const Vec2 right = wt->world.transform_point(Vec2{1.0f, 0.0f});
    NF_CHECK_NEAR(right.x, 11.0f, 1e-4f);
    NF_CHECK_NEAR(right.y, 6.0f, 1e-4f);
}

NF_TEST(scene2d_transform_system_treats_transformless_parent_as_root) {
    // A 2D entity parented to something without a Transform2D — a 3D camera, a
    // folder — must still get a world matrix instead of blocking the chain.
    ecs::World world;
    const ecs::Entity parent = world.create_entity();
    const ecs::Entity child = world.create_entity();
    world.add<Transform2D>(child, Transform2D{Vec2{4.0f, -1.0f}, 0.0f, Vec2{1.0f, 1.0f}});
    WorldTransform2D cw;
    cw.parent = parent;
    world.add<WorldTransform2D>(child, cw);

    transform2d_system(world);

    const WorldTransform2D* wt = world.get<WorldTransform2D>(child);
    NF_CHECK(wt != nullptr);
    NF_CHECK(!wt->dirty);
    const Vec2 origin = wt->world.transform_point(Vec2{0.0f, 0.0f});
    NF_CHECK_NEAR(origin.x, 4.0f, 1e-5f);
    NF_CHECK_NEAR(origin.y, -1.0f, 1e-5f);
}

NF_TEST(scene2d_transform_system_terminates_on_cycle) {
    // Two entities parenting each other can never resolve. The system must
    // return, not hang the frame; the leftover `dirty` flag is the signal a
    // debug overlay keys off to find the loop.
    ecs::World world;
    const ecs::Entity a = world.create_entity();
    world.add<Transform2D>(a, Transform2D{Vec2{1.0f, 0.0f}, 0.0f, Vec2{1.0f, 1.0f}});
    const ecs::Entity b = world.create_entity();
    world.add<Transform2D>(b, Transform2D{Vec2{0.0f, 1.0f}, 0.0f, Vec2{1.0f, 1.0f}});
    WorldTransform2D aw;
    aw.parent = b;
    world.add<WorldTransform2D>(a, aw);
    WorldTransform2D bw;
    bw.parent = a;
    world.add<WorldTransform2D>(b, bw);

    transform2d_system(world);

    NF_CHECK(world.get<WorldTransform2D>(a)->dirty);
    NF_CHECK(world.get<WorldTransform2D>(b)->dirty);
}

// ---------------------------------------------------------------------------
// SpriteBatcher placement — the regression the count-only tests missed
// ---------------------------------------------------------------------------

NF_TEST(scene2d_batcher_places_centre_anchored_quad) {
    SpriteBatcher batch;
    batch.begin();
    SpriteDraw s;
    s.size = Vec2{2.0f, 2.0f};
    s.anchor = Vec2{0.5f, 0.5f};
    s.position = Vec2{2.0f, 0.0f};
    s.u1 = 1.0f;
    s.v1 = 1.0f;
    batch.push(s);
    batch.end_identity();

    const std::vector<SpriteVertex>& v = batch.vertices();
    NF_CHECK(v.size() == 4u);
    // A 2x2 quad centred on (2,0) spans x [1,3], y [-1,1]. The position must not
    // be scaled by the size.
    NF_CHECK_NEAR(v[0].x, 1.0f, 1e-5f);
    NF_CHECK_NEAR(v[0].y, -1.0f, 1e-5f);
    NF_CHECK_NEAR(v[2].x, 3.0f, 1e-5f);
    NF_CHECK_NEAR(v[2].y, 1.0f, 1e-5f);
}

NF_TEST(scene2d_batcher_rotates_about_the_anchor) {
    SpriteBatcher batch;
    batch.begin();
    SpriteDraw s;
    s.size = Vec2{2.0f, 2.0f};
    s.anchor = Vec2{1.0f, 1.0f};
    s.position = Vec2{5.0f, 5.0f};
    s.rotation_deg = 90.0f;
    s.u1 = 1.0f;
    s.v1 = 1.0f;
    batch.push(s);
    batch.end_identity();

    const std::vector<SpriteVertex>& v = batch.vertices();
    NF_CHECK(v.size() == 4u);
    // The anchor corner pins to the position under any rotation...
    NF_CHECK_NEAR(v[2].x, 5.0f, 1e-5f);
    NF_CHECK_NEAR(v[2].y, 5.0f, 1e-5f);
    // ...and the quad is still 2x2, not sheared or collapsed.
    NF_CHECK_NEAR(v[0].x, 7.0f, 1e-5f);
    NF_CHECK_NEAR(v[0].y, 3.0f, 1e-5f);
    NF_CHECK_NEAR(v[1].x, 7.0f, 1e-5f);
    NF_CHECK_NEAR(v[1].y, 5.0f, 1e-5f);
}

// ---------------------------------------------------------------------------
// sprite_system
// ---------------------------------------------------------------------------

NF_TEST(scene2d_sprite_system_folds_world_transform) {
    ecs::World world;
    const ecs::Entity parent = world.create_entity();
    world.add<Transform2D>(parent, Transform2D{Vec2{10.0f, 0.0f}, 0.0f, Vec2{1.0f, 1.0f}});
    const ecs::Entity e = world.create_entity();
    world.add<Transform2D>(e, Transform2D{Vec2{2.0f, 0.0f}, 0.0f, Vec2{1.0f, 1.0f}});
    WorldTransform2D wt;
    wt.parent = parent;
    world.add<WorldTransform2D>(e, wt);
    SpriteComponent sc;
    sc.draw.size = Vec2{2.0f, 2.0f};
    sc.draw.anchor = Vec2{0.5f, 0.5f};
    sc.draw.position = Vec2{2.0f, 0.0f};
    sc.draw.u1 = 1.0f;
    sc.draw.v1 = 1.0f;
    world.add<SpriteComponent>(e, sc);

    transform2d_system(world);
    Camera2D camera;   // default camera is centred on the world origin
    SpriteBatcher batch;
    batch.begin();
    sprite_system(world, batch, camera);
    batch.end_identity();

    const std::vector<SpriteVertex>& v = batch.vertices();
    NF_CHECK(v.size() == 4u);
    // The quad's anchor pivot — the point `SpriteDraw.position` parks — is at
    // local (2,0) parented to (10,0), so the sprite's own placement is folded
    // in once: the world pivot is 12, and a size-2 centred quad spans 11..13
    // *before* the sprite's draw.position offset of 2, giving 13..15. The
    // batcher then re-bakes exactly that matrix, so the two corners below are
    // the world image of the unit square, not of the authored placement.
    NF_CHECK_NEAR(v[0].x, 13.0f, 1e-4f);
    NF_CHECK_NEAR(v[2].x, 15.0f, 1e-4f);
}

NF_TEST(scene2d_sprite_system_culls_offscreen_sprites) {
    // The default camera sees roughly x in [-40, 40]. One sprite inside, one far
    // outside; only the visible one reaches the bake.
    ecs::World world;
    const ecs::Entity near_e = world.create_entity();
    world.add<Transform2D>(near_e, Transform2D{Vec2{0.0f, 0.0f}, 0.0f, Vec2{1.0f, 1.0f}});
    SpriteComponent near_sc;
    near_sc.draw.u1 = 1.0f;
    near_sc.draw.v1 = 1.0f;
    world.add<SpriteComponent>(near_e, near_sc);
    const ecs::Entity far_e = world.create_entity();
    world.add<Transform2D>(far_e, Transform2D{Vec2{500.0f, 0.0f}, 0.0f, Vec2{1.0f, 1.0f}});
    SpriteComponent far_sc;
    far_sc.draw.u1 = 1.0f;
    far_sc.draw.v1 = 1.0f;
    world.add<SpriteComponent>(far_e, far_sc);

    transform2d_system(world);
    Camera2D camera;
    SpriteBatcher batch;
    batch.begin();
    sprite_system(world, batch, camera);
    batch.end_identity();

    NF_CHECK(batch.vertices().size() == 4u);
}

// ---------------------------------------------------------------------------
// tilemap_render_system
// ---------------------------------------------------------------------------

namespace {

/// A 3-tile floor with an atlas whose UV math is exact in f32: 256 texels, 16
/// per tile, so one cell is exactly 1/16 of the page.
TilemapComponent make_small_map_component() {
    Tilemap map;
    map.tileset.set_material(1, TileMaterial{});
    TilemapLayer& layer = map.add_layer("terrain", 1.0f, true, false);
    layer.set(0, 0, pack_tile(1));
    layer.set(1, 0, pack_tile(2));
    layer.set(2, 0, pack_tile(3, TileFlag_FlipX));

    TilemapComponent tc;
    tc.map = map;
    tc.atlas_page = 0;
    tc.atlas_w = 256;
    tc.atlas_h = 256;
    tc.atlas_tile_count = 16;
    tc.tile_texel_size = 16;
    return tc;
}

} // namespace

NF_TEST(scene2d_tilemap_render_emits_tiles_with_atlas_uvs) {
    ecs::World world;
    const ecs::Entity e = world.create_entity();
    world.add<TilemapComponent>(e, make_small_map_component());

    Camera2D camera;
    SpriteBatcher batch;
    batch.begin();
    tilemap_render_system(world, batch, camera);
    batch.end_identity();

    const std::vector<SpriteVertex>& v = batch.vertices();
    NF_CHECK(v.size() == 12u);

    // Tile 1 fills the first atlas cell: [inset, cell - inset] on both axes.
    const f32 cell = 16.0f / 256.0f;
    const f32 inset = 0.5f / 256.0f;
    NF_CHECK_NEAR(v[0].x, 0.0f, 1e-6f);
    NF_CHECK_NEAR(v[0].y, 0.0f, 1e-6f);
    NF_CHECK_NEAR(v[0].u, inset, 1e-6f);
    NF_CHECK_NEAR(v[2].x, 1.0f, 1e-6f);
    NF_CHECK_NEAR(v[2].y, 1.0f, 1e-6f);

    // Tile 2 is one cell to the right in the world and one cell to the right in
    // the atlas.
    NF_CHECK_NEAR(v[4].x, 1.0f, 1e-6f);
    NF_CHECK_NEAR(v[4].u, cell + inset, 1e-6f);

    // Tile 3 is flipped horizontally: UVs swap, positions do not. The emitted
    // u0 is the cell's far edge, so the quad samples the tile backwards.
    NF_CHECK_NEAR(v[8].x, 2.0f, 1e-6f);
    NF_CHECK(v[8].u > v[9].u);
    NF_CHECK_NEAR(v[8].u, 3.0f * cell - inset, 1e-6f);
    NF_CHECK_NEAR(v[9].u, 2.0f * cell + inset, 1e-6f);
}

NF_TEST(scene2d_tilemap_render_culls_chunks_outside_the_camera) {
    ecs::World world;
    const ecs::Entity e = world.create_entity();
    TilemapComponent tc;
    tc.map.tileset.set_material(1, TileMaterial{});
    TilemapLayer& layer = tc.map.add_layer("terrain");
    // A tile 200 columns away sits in its own chunk, entirely offscreen. A map a
    // thousand chunks wide should only touch the visible ones.
    layer.set(200, 0, pack_tile(1));
    world.add<TilemapComponent>(e, tc);

    Camera2D camera;
    SpriteBatcher batch;
    batch.begin();
    tilemap_render_system(world, batch, camera);
    batch.end_identity();

    NF_CHECK(batch.vertices().empty());
}

// ---------------------------------------------------------------------------
// physics2d_system
// ---------------------------------------------------------------------------

NF_TEST(scene2d_physics_builds_static_bodies_from_tiles) {
    ecs::World world;
    PhysicsWorld2D phys;
    const ecs::Entity map_e = world.create_entity();
    TilemapComponent tc;
    tc.map.tileset.set_material(1, TileMaterial{});
    TilemapLayer& layer = tc.map.add_layer("terrain");
    for (i64 x = 0; x < 8; ++x) layer.set(x, 5, pack_tile(1));
    world.add<TilemapComponent>(map_e, tc);

    const ecs::Entity box_e = world.create_entity();
    world.add<Transform2D>(box_e, Transform2D{Vec2{4.0f, 2.0f}, 0.0f, Vec2{1.0f, 1.0f}});
    Body2DComponent bc;
    bc.body.shape = Shape2D::box(0.5f, 0.5f);
    bc.body.restitution = 0.0f;
    world.add<Body2DComponent>(box_e, bc);

    for (int i = 0; i < 120; ++i) {
        physics2d_system(world, phys, 1.0f / 64.0f, Vec2{0.0f, 25.0f});
    }

    // Eight tiles merged into one floor rect, so one static body.
    const TilemapComponent* post_map = world.get<TilemapComponent>(map_e);
    NF_CHECK(post_map != nullptr);
    NF_CHECK(!post_map->dirty);
    NF_CHECK(post_map->collision_handles.size() == 1u);

    // The box falls 3 units and rests on the floor's top face at y = 5, so its
    // centre sits half a unit above it.
    const Body2D* b = phys.body(world.get<Body2DComponent>(box_e)->handle);
    NF_CHECK(b != nullptr);
    NF_CHECK(b->position.y > 4.0f);
    NF_CHECK(b->position.y < 4.9f);
    NF_CHECK(std::abs(b->velocity.y) < 2.0f);
    // And the write-back kept the transform in sync with the body.
    NF_CHECK_NEAR(world.get<Transform2D>(box_e)->position.y, b->position.y, 1e-4f);
}

NF_TEST(scene2d_physics_kinematic_drive_lands_on_authored_transform) {
    // A driven platform: the transform is the authority and the body follows.
    // The implied motion becomes the body's velocity so a rider standing on it
    // is carried — but integrating that velocity is also what moves the body, so
    // writing the target position as well applied the displacement twice and
    // the platform overshot by 100%.
    ecs::World world;
    PhysicsWorld2D phys;
    const ecs::Entity e = world.create_entity();
    world.add<Transform2D>(e, Transform2D{Vec2{0.0f, 0.0f}, 0.0f, Vec2{1.0f, 1.0f}});
    Body2DComponent bc;
    bc.body.shape = Shape2D::box(0.5f, 0.5f);
    bc.body.linear_damping = 0.0f;
    bc.body.angular_damping = 0.0f;
    bc.drive_from_transform = true;
    world.add<Body2DComponent>(e, bc);

    // First step creates the body, seeded from the transform at the origin.
    physics2d_system(world, phys, 1.0f / 60.0f, Vec2{0.0f, 0.0f});
    const BodyHandle h = world.get<Body2DComponent>(e)->handle;
    NF_CHECK(h.valid());
    NF_CHECK_NEAR(phys.body(h)->position.x, 0.0f, 1e-5f);

    // Author a move to x = 5 and step once. Zero damping means the derived
    // velocity integrates to exactly the target.
    world.get<Transform2D>(e)->position = Vec2{5.0f, 0.0f};
    physics2d_system(world, phys, 1.0f / 60.0f, Vec2{0.0f, 0.0f});

    NF_CHECK_NEAR(phys.body(h)->position.x, 5.0f, 1e-4f);
    // The authored pose survives: a driven body is not written back from the
    // solver, or the platform would drift away from its animation.
    NF_CHECK_NEAR(world.get<Transform2D>(e)->position.x, 5.0f, 1e-5f);
}

NF_TEST(scene2d_physics_rebuilds_body_after_shape_edit) {
    // `needs_rebuild` is how an edited collider takes effect: the old body is
    // destroyed (generation bumps) and a new one created in its place.
    ecs::World world;
    PhysicsWorld2D phys;
    const ecs::Entity e = world.create_entity();
    world.add<Transform2D>(e, Transform2D{Vec2{0.0f, 0.0f}, 0.0f, Vec2{1.0f, 1.0f}});
    Body2DComponent bc;
    bc.body.shape = Shape2D::circle(0.5f);
    world.add<Body2DComponent>(e, bc);

    physics2d_system(world, phys, 1.0f / 60.0f, Vec2{0.0f, 0.0f});
    const BodyHandle first = world.get<Body2DComponent>(e)->handle;
    NF_CHECK(first.valid());
    NF_CHECK(phys.body(first)->shape.kind == Shape2D::Kind::Circle);

    // Re-author the shape and flag the rebuild.
    world.get<Body2DComponent>(e)->body.shape = Shape2D::box(1.0f, 1.0f);
    world.get<Body2DComponent>(e)->needs_rebuild = true;
    physics2d_system(world, phys, 1.0f / 60.0f, Vec2{0.0f, 0.0f});

    const BodyHandle second = world.get<Body2DComponent>(e)->handle;
    NF_CHECK(second.valid());
    NF_CHECK(phys.body(second)->shape.kind == Shape2D::Kind::Box);
    NF_CHECK(phys.body(first) == nullptr);   // the stale handle fails loudly
}

// ---------------------------------------------------------------------------
// particles2d_system
// ---------------------------------------------------------------------------

NF_TEST(scene2d_particles_follow_the_entity_in_world_space) {
    ecs::World world;
    const ecs::Entity e = world.create_entity();
    world.add<Transform2D>(e, Transform2D{Vec2{7.0f, 0.0f}, 0.0f, Vec2{1.0f, 1.0f}});
    ParticlesComponent2D pc;
    pc.use_world_position = true;
    pc.particles.resize(64);
    ParticleEmitter2D& em = pc.particles.emitter();
    em.rate = 100.0f;
    em.spread_deg = 0.0f;
    em.direction_deg = 90.0f;
    em.speed_min = 0.0f;
    em.speed_max = 0.0f;
    em.life_min = 1.0f;
    em.life_max = 2.0f;
    em.drag = 0.0f;
    em.gravity_scale = 0.0f;
    world.add<ParticlesComponent2D>(e, pc);

    // The emitter must read the *world* position, which means the transform
    // system has to run first — that ordering is the pipeline contract.
    transform2d_system(world);
    particles2d_system(world, 0.1f, Vec2{0.0f, 0.0f});

    std::vector<SpriteDraw> draws;
    world.get<ParticlesComponent2D>(e)->particles.collect_draws(draws);
    NF_CHECK(draws.size() == 10u);   // 100 per second over a tenth of a second
    for (const SpriteDraw& d : draws) {
        NF_CHECK_NEAR(d.position.x, 7.0f, 1e-4f);
        NF_CHECK_NEAR(d.position.y, 0.0f, 1e-4f);
    }
}

NF_TEST(scene2d_particles_honour_explicit_origin) {
    ecs::World world;
    const ecs::Entity e = world.create_entity();
    world.add<Transform2D>(e, Transform2D{Vec2{7.0f, 0.0f}, 0.0f, Vec2{1.0f, 1.0f}});
    ParticlesComponent2D pc;
    pc.use_world_position = false;
    pc.emit_origin = Vec2{3.0f, 3.0f};
    pc.particles.resize(64);
    ParticleEmitter2D& em = pc.particles.emitter();
    em.rate = 100.0f;
    em.spread_deg = 0.0f;
    em.speed_min = 0.0f;
    em.speed_max = 0.0f;
    em.life_min = 1.0f;
    em.life_max = 2.0f;
    em.drag = 0.0f;
    em.gravity_scale = 0.0f;
    world.add<ParticlesComponent2D>(e, pc);

    transform2d_system(world);
    particles2d_system(world, 0.1f, Vec2{0.0f, 0.0f});

    std::vector<SpriteDraw> draws;
    world.get<ParticlesComponent2D>(e)->particles.collect_draws(draws);
    NF_CHECK(draws.size() == 10u);
    for (const SpriteDraw& d : draws) {
        NF_CHECK_NEAR(d.position.x, 3.0f, 1e-4f);
        NF_CHECK_NEAR(d.position.y, 3.0f, 1e-4f);
    }
}
