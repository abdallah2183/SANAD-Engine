// Scene2DTests — atlas, batcher, tilemap, 2D solver, determinism.

#include <NF/ECS/ECS.hpp>
#include <NF/Scene2D/Camera2D.hpp>
#include <NF/Scene2D/Components.hpp>
#include <NF/Scene2D/Light2D.hpp>
#include <NF/Scene2D/Physics2D.hpp>
#include <NF/Scene2D/SpriteAtlas.hpp>
#include <NF/Scene2D/SpriteBatcher.hpp>
#include <NF/Scene2D/Tilemap.hpp>
#include <NF/Test/TestFramework.hpp>

#include <cmath>
#include <cstring>

using namespace nf;
using namespace nf::scene2d;

NF_TEST(scene2d_atlas_pack_is_deterministic) {
    SpriteAtlas a;
    a.add_pending("hero", 16, 16);
    a.add_pending("coin", 8, 8);
    a.add_pending("block", 16, 16);
    NF_CHECK(a.pack(64, 64) >= 1u);

    SpriteAtlas b;
    b.add_pending("block", 16, 16);
    b.add_pending("hero", 16, 16);
    b.add_pending("coin", 8, 8);
    NF_CHECK(b.pack(64, 64) >= 1u);

    const AtlasRegion* ra = a.region("hero");
    const AtlasRegion* rb = b.region("hero");
    NF_CHECK(ra && rb);
    NF_CHECK(ra->x == rb->x && ra->y == rb->y && ra->page == rb->page);
}

NF_TEST(scene2d_batcher_collapses_same_page) {
    SpriteBatcher batch;
    batch.begin();
    SpriteDraw s;
    s.page = 0;
    s.size = Vec2{1.0f, 1.0f};
    s.u1 = 1.0f;
    s.v1 = 1.0f;
    s.position = Vec2{0.0f, 0.0f};
    batch.push(s);
    s.position = Vec2{2.0f, 0.0f};
    s.depth = 1.0f;
    batch.push(s);
    s.page = 1;
    batch.push(s);
    batch.end_identity();
    NF_CHECK(batch.vertices().size() == 12u);
    NF_CHECK(batch.indices().size() == 18u);
    NF_CHECK(batch.draw_call_count() == 2u);
}

NF_TEST(scene2d_tilemap_merges_floor_rects) {
    Tilemap map;
    map.tileset.set_material(1, TileMaterial{});
    TilemapLayer& terrain = map.add_layer("terrain");
    for (i64 x = 0; x < 8; ++x) terrain.set(x, 3, pack_tile(1));
    const CollisionMesh mesh = map.extract_collision(terrain);
    NF_CHECK(mesh.rects.size() == 1u);
    NF_CHECK_NEAR(mesh.rects[0].w, 8.0f, 1e-5f);
    NF_CHECK_NEAR(mesh.rects[0].h, 1.0f, 1e-5f);
}

NF_TEST(scene2d_tilemap_nav_finds_detour) {
    Tilemap map;
    map.tileset.set_material(1, TileMaterial{});
    TilemapLayer& terrain = map.add_layer("terrain");
    for (i64 y = 0; y < 5; ++y) terrain.set(2, y, pack_tile(1));
    terrain.set(2, 4, kEmptyTile);
    const NavGrid2D grid = map.build_nav_grid(Rect{0.0f, 0.0f, 6.0f, 6.0f}, 1.0f);
    const NavPath path =
        map.find_path(grid, Vec2{0.5f, 0.5f}, Vec2{4.5f, 0.5f}, 0.0f);
    NF_CHECK(path.found);
    NF_CHECK(path.waypoints.size() >= 2u);
}

NF_TEST(scene2d_negative_chunk_key) {
    const ChunkKey a = ChunkKey::from_tile(-1, -1);
    NF_CHECK(a.x == -1 && a.y == -1);
    TilemapLayer layer;
    layer.set(-1, -1, pack_tile(7));
    NF_CHECK(tile_id(layer.get(-1, -1)) == 7u);
    NF_CHECK(layer.get(0, 0) == kEmptyTile);
}

NF_TEST(scene2d_circle_rests_on_static_box) {
    PhysicsWorld2D world;
    world.set_gravity(Vec2{0.0f, 40.0f});
    Body2D floor;
    floor.shape = Shape2D::box(8.0f, 0.5f);
    floor.position = Vec2{0.0f, 4.0f};
    floor.is_static = true;
    world.create_body(floor);

    Body2D ball;
    ball.shape = Shape2D::circle(0.5f);
    ball.position = Vec2{0.0f, 0.0f};
    ball.restitution = 0.0f;
    const BodyHandle h = world.create_body(ball);
    for (int i = 0; i < 120; ++i) world.step(1.0f / 64.0f);
    const Body2D* b = world.body(h);
    NF_CHECK(b != nullptr);
    NF_CHECK(b->position.y < 3.6f);
    NF_CHECK(std::abs(b->velocity.y) < 2.0f);
}

NF_TEST(scene2d_solver_is_bit_identical) {
    auto run = []() {
        PhysicsWorld2D world;
        world.set_gravity(Vec2{0.0f, 25.0f});
        Body2D floor;
        floor.shape = Shape2D::box(6.0f, 0.5f);
        floor.position = Vec2{0.0f, 5.0f};
        floor.is_static = true;
        world.create_body(floor);
        Body2D a;
        a.shape = Shape2D::box(0.4f, 0.4f);
        a.position = Vec2{-0.2f, 1.0f};
        world.create_body(a);
        Body2D c;
        c.shape = Shape2D::circle(0.35f);
        c.position = Vec2{0.8f, 0.2f};
        c.velocity = Vec2{-1.5f, 0.0f};
        const BodyHandle h = world.create_body(c);
        for (int i = 0; i < 80; ++i) world.step(1.0f / 64.0f);
        return *world.body(h);
    };
    const Body2D x = run();
    const Body2D y = run();
    NF_CHECK(std::memcmp(&x.position, &y.position, sizeof(Vec2)) == 0);
    NF_CHECK(std::memcmp(&x.velocity, &y.velocity, sizeof(Vec2)) == 0);
    NF_CHECK(std::memcmp(&x.angle, &y.angle, sizeof(f32)) == 0);
    PhysicsWorld2D probe;
    NF_CHECK(probe.is_deterministic());
}

NF_TEST(scene2d_stale_handle_is_null) {
    PhysicsWorld2D world;
    Body2D b;
    b.shape = Shape2D::circle(0.5f);
    const BodyHandle h = world.create_body(b);
    world.destroy_body(h);
    NF_CHECK(world.body(h) == nullptr);
    const BodyHandle h2 = world.create_body(b);
    NF_CHECK(h2.index == h.index);
    NF_CHECK(h2.generation != h.generation);
    NF_CHECK(world.body(h) == nullptr);
    NF_CHECK(world.body(h2) != nullptr);
}

NF_TEST(scene2d_distance_constraint_holds_length) {
    PhysicsWorld2D world;
    world.set_gravity(Vec2{0.0f, 0.0f});
    Body2D a;
    a.shape = Shape2D::circle(0.25f);
    a.position = Vec2{0.0f, 0.0f};
    Body2D b;
    b.shape = Shape2D::circle(0.25f);
    b.position = Vec2{3.0f, 0.0f};
    const BodyHandle ha = world.create_body(a);
    const BodyHandle hb = world.create_body(b);
    DistanceConstraint2D c;
    c.a = ha;
    c.b = hb;
    c.rest_length = 1.5f;
    world.add_distance_constraint(c);
    for (int i = 0; i < 40; ++i) world.step(1.0f / 64.0f);
    const f32 d = (world.body(hb)->position - world.body(ha)->position).length();
    NF_CHECK_NEAR(d, 1.5f, 0.15f);
}

NF_TEST(scene2d_raycast_hits_box) {
    PhysicsWorld2D world;
    Body2D box;
    box.shape = Shape2D::box(1.0f, 1.0f);
    box.position = Vec2{4.0f, 0.0f};
    world.create_body(box);
    const RaycastHit2D hit = world.raycast(Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}, 10.0f);
    NF_CHECK(hit.hit);
    NF_CHECK(hit.distance > 2.0f && hit.distance < 4.1f);
}

NF_TEST(scene2d_broadphase_skips_static_static_but_not_triggers) {
    // A tilemap injects one static body per collision rect, so an untouched pair
    // of terrain bodies is the common case rather than a pathological one. The
    // solver can resolve nothing between two massless bodies, so the pair is
    // dropped before the narrowphase rather than after it.
    PhysicsWorld2D world;
    Body2D floor_a;
    floor_a.shape = Shape2D::box(2.0f, 0.5f);
    floor_a.position = Vec2{0.0f, 0.0f};
    floor_a.is_static = true;
    Body2D floor_b = floor_a;
    floor_b.position = Vec2{0.5f, 0.0f};   // overlaps floor_a
    world.create_body(floor_a);
    world.create_body(floor_b);

    // A static sensor region still has to report an overlap against static
    // geometry — that is the gameplay query a pickup zone makes.
    Body2D sensor;
    sensor.shape = Shape2D::box(2.0f, 2.0f);
    sensor.position = Vec2{0.0f, 0.0f};
    sensor.is_static = true;
    sensor.is_trigger = true;
    world.create_body(sensor);

    world.step(1.0f / 64.0f);

    // The only surviving contacts involve the sensor. The floor-floor pair is
    // gone entirely rather than present-but-unresolved.
    bool saw_sensor = false;
    for (const ContactManifold& c : world.contacts()) {
        if (c.body_a == 0u && c.body_b == 1u) continue;
        if (c.body_a == 2u || c.body_b == 2u) {
            saw_sensor = true;
            continue;
        }
    }
    NF_CHECK(world.contacts().size() == 2u);
    NF_CHECK(saw_sensor);
}

NF_TEST(scene2d_camera_pixel_snap) {
    Camera2D cam;
    cam.pixels_per_unit = 16;
    cam.zoom = 1.0f;
    cam.position = Vec2{0.03f, 0.07f};
    const Vec2 s = cam.snapped_position();
    NF_CHECK_NEAR(s.x, 0.0f, 1e-5f);
    NF_CHECK_NEAR(s.y, 1.0f / 16.0f, 1e-5f);
}

// ---------------------------------------------------------------------------
// Lighting — design doc Section 54: "2D lights", "Shadows".
// ---------------------------------------------------------------------------

/// A wall between a light and a point has to actually darken the point, both
/// through the exact ray test and through the baked shadow quads.
NF_TEST(scene2d_light_occludes_behind_wall) {
    Lighting2D lighting;
    Light2D l;
    l.position = Vec2{0.0f, 0.0f};
    l.radius = 10.0f;
    lighting.add_light(l);
    lighting.set_occluders({{{5.0f, -2.0f}, {5.0f, 2.0f}}});

    const Vec3 in_front = lighting.sample(Vec2{2.0f, 0.0f});
    const Vec3 behind = lighting.sample(Vec2{8.0f, 0.0f});

    NF_CHECK(in_front.x > 0.5f);
    // Behind the wall: ambient only.
    NF_CHECK_NEAR(behind.x, lighting.ambient().x, 1e-5f);
    NF_CHECK_NEAR(behind.y, lighting.ambient().y, 1e-5f);

    // The bake must agree with the ray test.
    lighting.bake(0.0f);
    const Vec3 baked = lighting.sample(Vec2{8.0f, 0.0f});
    NF_CHECK_NEAR(baked.x, behind.x, 1e-4f);
    NF_CHECK(lighting.shadow_quad_count(0u) == 1u);
}

/// Attenuation is exactly zero at the radius and full at the source; the smooth
/// taper is the hermite one and stays inside [0, 1].
NF_TEST(scene2d_light_falloff_off_at_radius) {
    Light2D l;
    l.radius = 4.0f;
    NF_CHECK_NEAR(l.attenuation(Vec2{0.0f, 0.0f}), 1.0f, 1e-5f);
    NF_CHECK(l.attenuation(Vec2{4.0f, 0.0f}) == 0.0f);
    NF_CHECK(l.attenuation(Vec2{10.0f, 0.0f}) == 0.0f);
    for (f32 d = 0.25f; d < 4.0f; d += 0.25f) {
        const f32 a = l.attenuation(l.position + Vec2{d, 0.0f});
        NF_CHECK(a >= 0.0f && a <= 1.0f);
    }
    // Distance-independent along the sphere: same radius, same answer.
    NF_CHECK_NEAR(l.attenuation(Vec2{0.0f, 3.0f}), l.attenuation(Vec2{3.0f, 0.0f}), 1e-5f);
}

/// A cone light lights its arc and nothing on the other side of the axis.
NF_TEST(scene2d_cone_light_only_in_arc) {
    Light2D l;
    l.position = Vec2{0.0f, 0.0f};
    l.radius = 10.0f;
    l.direction_deg = 0.0f;      // shining +x
    l.spread_deg = 90.0f;        // +/- 45 degrees
    NF_CHECK(l.is_cone());

    NF_CHECK(l.attenuation(Vec2{5.0f, 0.0f}) > 0.0f);       // on axis
    NF_CHECK(l.attenuation(Vec2{5.0f, 4.0f}) > 0.0f);       // inside 45 deg
    NF_CHECK(l.attenuation(Vec2{5.0f, 6.0f}) == 0.0f);      // outside
    NF_CHECK(l.attenuation(Vec2{-5.0f, 0.0f}) == 0.0f);     // behind

    l.spread_deg = 360.0f;
    NF_CHECK(!l.is_cone());
    NF_CHECK(l.attenuation(Vec2{-5.0f, 0.0f}) > 0.0f);
}

/// The bake is the optimisation, not a second implementation: every point the
/// light reaches must resolve identically with and without it. The grid stays
/// inside the lit disc — the bake truncates each shadow wedge at the radius,
/// which is exact where the light reaches and undefined past it.
NF_TEST(scene2d_lighting_bake_matches_raycast) {
    Lighting2D lighting;
    Light2D l;
    l.position = Vec2{-3.0f, 0.5f};
    l.radius = 20.0f;
    lighting.add_light(l);
    lighting.set_occluders_from_rects({Rect{0.1f, -2.9f, 2.0f, 5.8f},
                                       Rect{6.2f, 2.1f, 2.6f, 2.6f}});
    lighting.bake(1.0f);

    // A query whose ray grazes a wall corner, or whose point sits on a wall
    // surface, is a boundary case and the two tests are free to disagree there:
    // the ray test wants the crossing strictly inside the edge, the bake's
    // interior test is inclusive of the wedge boundary. Everything else has to
    // match, so those are the only points filtered out.
    const f32 kBoundary = 1e-3f;
    const std::vector<OccluderSegment>& segs = lighting.occluders();
    auto is_boundary = [&](Vec2 p) {
        for (const OccluderSegment& s : segs) {
            if ((closest_point_on_segment(s.a, s.b, p) - p).length_sq()
                < kBoundary * kBoundary) return true;
            for (const Vec2 end : {s.a, s.b}) {
                if ((closest_point_on_segment(l.position, p, end) - end).length_sq()
                    < kBoundary * kBoundary) return true;
            }
        }
        return false;
    };

    usize mismatches = 0u;
    for (f32 y = -8.0f; y <= 8.0f; y += 0.37f) {
        for (f32 x = -10.0f; x <= 12.0f; x += 0.37f) {
            const Vec2 p{x, y};
            if (is_boundary(p)) continue;
            if (lighting.in_shadow(0u, p) != lighting.is_occluded(l.position, p)) {
                ++mismatches;
            }
        }
    }
    NF_CHECK(mismatches == 0u);
}

/// A lit sprite keeps its own hue, only its brightness changes, and its alpha
/// survives the pass untouched.
NF_TEST(scene2d_lighting_tints_sprite) {
    Lighting2D lighting;
    Light2D l;
    l.position = Vec2{0.0f, 0.0f};
    l.radius = 10.0f;
    lighting.add_light(l);

    ecs::World world;
    const ecs::Entity e = world.create_entity();
    world.add<Transform2D>(e);
    world.add<WorldTransform2D>(e);
    SpriteComponent sc;
    sc.draw.size = Vec2{1.0f, 1.0f};
    sc.draw.color = pack_color(255, 0, 0, 128);   // red, half transparent
    world.add<SpriteComponent>(e, sc);
    transform2d_system(world);

    Camera2D cam;
    SpriteBatcher lit_batch;
    lit_batch.begin();
    sprite_system(world, lit_batch, cam, lighting);
    lit_batch.end_identity();
    NF_CHECK(lit_batch.vertices().size() == 4u);

    const u32 lit_color = lit_batch.vertices()[0].color;
    const Vec3 lit_rgb = unpack_color_rgb(lit_color);
    NF_CHECK(lit_rgb.x > 0.0f);              // red survives
    NF_CHECK(lit_rgb.y < 0.1f);              // green stays out
    NF_CHECK(lit_rgb.z < 0.1f);
    // Alpha is preserved exactly: lighting a thing invisible is meaningless.
    NF_CHECK(((lit_color >> 24u) & 0xFFu) == 128u);

    // The same sprite in the dark is ambient-dim, and the unlit overload leaves
    // the authored colour alone entirely.
    lighting.clear_lights();
    SpriteBatcher dark_batch;
    dark_batch.begin();
    sprite_system(world, dark_batch, cam, lighting);
    dark_batch.end_identity();
    NF_CHECK(dark_batch.vertices()[0].color != lit_color);

    SpriteBatcher plain_batch;
    plain_batch.begin();
    sprite_system(world, plain_batch, cam);
    plain_batch.end_identity();
    NF_CHECK(plain_batch.vertices()[0].color == pack_color(255, 0, 0, 128));
}

/// Two identical timelines bake identical lighting, bit for bit.
NF_TEST(scene2d_lighting_is_deterministic) {
    auto run = []() {
        Lighting2D lighting;
        Light2D a;
        a.position = Vec2{-2.0f, 1.0f};
        a.radius = 9.0f;
        a.flicker_amount = 0.3f;
        a.flicker_speed = 5.0f;
        a.flicker_phase = 17.0f;
        lighting.add_light(a);
        Light2D b;
        b.position = Vec2{3.0f, -2.0f};
        b.radius = 7.0f;
        b.color = Vec3{0.4f, 0.6f, 1.0f};
        b.flicker_amount = 0.2f;
        b.flicker_phase = 91.0f;
        lighting.add_light(b);
        lighting.set_occluders_from_rects({Rect{0.0f, -4.0f, 1.0f, 8.0f}});
        lighting.bake(0.125f);

        Vec3 acc{0.0f, 0.0f, 0.0f};
        for (f32 y = -5.0f; y <= 5.0f; y += 0.25f) {
            for (f32 x = -6.0f; x <= 6.0f; x += 0.25f) {
                acc = acc + lighting.sample(Vec2{x, y});
            }
        }
        return acc;
    };
    const Vec3 x = run();
    const Vec3 y = run();
    NF_CHECK(std::memcmp(&x, &y, sizeof(Vec3)) == 0);
}

/// A tilemap's merged collision rects become occluders, so a built level casts
/// shadows without a second authoring pass.
NF_TEST(scene2d_occluders_from_tilemap) {
    Tilemap map;
    map.tileset.set_material(1, TileMaterial{});
    TilemapLayer& terrain = map.add_layer("terrain");
    for (i64 x = 0; x < 4; ++x) terrain.set(x, 3, pack_tile(1));
    const CollisionMesh mesh = map.extract_collision(terrain);
    NF_CHECK(mesh.rects.size() == 1u);

    Lighting2D lighting;
    Light2D l;
    l.position = Vec2{0.5f, 0.0f};
    l.radius = 12.0f;
    lighting.add_light(l);
    lighting.set_occluders_from_rects(mesh.rects);
    NF_CHECK(lighting.occluder_count() == 4u);

    // In front of the floor row: lit. Behind it: ambient only, both through the
    // ray test and through the bake the system would have run by now.
    NF_CHECK(lighting.sample(Vec2{0.5f, 1.0f}).x > 0.5f);
    NF_CHECK_NEAR(lighting.sample(Vec2{0.5f, 6.0f}).x, lighting.ambient().x, 1e-5f);

    lighting.bake(0.0f);
    NF_CHECK(lighting.shadow_quad_count(0u) == 4u);
    NF_CHECK(lighting.sample(Vec2{0.5f, 1.0f}).x > 0.5f);
    NF_CHECK_NEAR(lighting.sample(Vec2{0.5f, 6.0f}).x, lighting.ambient().x, 1e-4f);
}

/// Flicker is a hash-seeded sinusoid: same phase, same value every run, and two
/// different phases strobe out of step instead of in unison.
NF_TEST(scene2d_flicker_is_deterministic) {
    Light2D l;
    l.flicker_amount = 0.4f;
    l.flicker_speed = 3.0f;
    l.flicker_phase = 5.0f;
    NF_CHECK(l.flicker(7.0f) == l.flicker(7.0f));

    Light2D other = l;
    other.flicker_phase = 99.0f;
    bool differ = false;
    for (i64 i = 0; i < 64; ++i) {
        const f32 t = static_cast<f32>(i) * 0.1f;
        if (std::abs(l.flicker(t) - other.flicker(t)) > 1e-4f) differ = true;
    }
    NF_CHECK(differ);

    // No flicker is exactly 1, always — so a steady light is steady.
    Light2D steady;
    NF_CHECK(steady.flicker(13.0f) == 1.0f);
}
