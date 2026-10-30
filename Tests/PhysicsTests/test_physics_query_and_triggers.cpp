// Tests/PhysicsTests/test_physics_query_and_triggers.cpp — Unit tests for raycasting, sweeping, overlap & triggers

#include <NF/Test/TestFramework.hpp>
#include <NF/Physics/PhysicsWorld.hpp>
#include <NF/Physics/PhysicsQuery.hpp>
#include <NF/Physics/TriggerVolume.hpp>

#include <cmath>

using namespace nf;
using namespace nf::physics;

NF_TEST(physics_query_raycast_sphere_box_plane) {
    PhysicsWorld world;

    // 1. Sphere at (0, 0, 10), radius = 2.0
    BodyDesc sphere_desc;
    sphere_desc.type = BodyType::Static;
    sphere_desc.shape = Shape::make_sphere(2.0f);
    sphere_desc.position = Vec3(0.0f, 0.0f, 10.0f);
    BodyHandle h_sphere = world.add_body(sphere_desc);

    // Direct hit towards sphere
    Ray3D ray1{ Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), 50.0f };
    auto hit1 = PhysicsQuery::raycast(world, ray1);
    NF_CHECK(hit1.has_value());
    NF_CHECK(hit1->has_hit);
    NF_CHECK(hit1->body == h_sphere);
    NF_CHECK_NEAR(hit1->distance, 8.0f, 1e-4f); // 10.0 - 2.0 = 8.0
    NF_CHECK_NEAR(hit1->point.z, 8.0f, 1e-4f);
    NF_CHECK_NEAR(hit1->normal.z, -1.0f, 1e-4f);

    // Ray pointing away
    Ray3D ray_miss{ Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), 50.0f };
    auto hit_miss = PhysicsQuery::raycast(world, ray_miss);
    NF_CHECK(!hit_miss.has_value());

    // 2. Box at (15, 0, 0), half-extents = (1, 2, 3)
    BodyDesc box_desc;
    box_desc.type = BodyType::Static;
    box_desc.shape = Shape::make_box(Vec3(1.0f, 2.0f, 3.0f));
    box_desc.position = Vec3(15.0f, 0.0f, 0.0f);
    BodyHandle h_box = world.add_body(box_desc);

    Ray3D ray_box{ Vec3(0.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), 50.0f };
    auto hit_box = PhysicsQuery::raycast(world, ray_box);
    NF_CHECK(hit_box.has_value());
    NF_CHECK(hit_box->body == h_box);
    NF_CHECK_NEAR(hit_box->distance, 14.0f, 1e-4f); // 15.0 - 1.0 = 14.0
    NF_CHECK_NEAR(hit_box->normal.x, -1.0f, 1e-4f);

    // 3. Plane at Y = -5, normal = (0, 1, 0)
    BodyDesc plane_desc;
    plane_desc.type = BodyType::Static;
    plane_desc.shape = Shape::make_plane(Vec3(0.0f, 1.0f, 0.0f));
    plane_desc.position = Vec3(0.0f, -5.0f, 0.0f);
    BodyHandle h_plane = world.add_body(plane_desc);

    Ray3D ray_down{ Vec3(0.0f, 5.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 50.0f };
    auto hit_plane = PhysicsQuery::raycast(world, ray_down);
    NF_CHECK(hit_plane.has_value());
    NF_CHECK(hit_plane->body == h_plane);
    NF_CHECK_NEAR(hit_plane->distance, 10.0f, 1e-4f); // 5.0 - (-5.0) = 10.0
    NF_CHECK_NEAR(hit_plane->normal.y, 1.0f, 1e-4f);
}

NF_TEST(physics_query_raycast_all_sorting) {
    PhysicsWorld world;

    // Place three spheres in a line along +Z
    BodyDesc d1; d1.type = BodyType::Static; d1.shape = Shape::make_sphere(0.5f); d1.position = Vec3(0, 0,  5);
    BodyDesc d2; d2.type = BodyType::Static; d2.shape = Shape::make_sphere(0.5f); d2.position = Vec3(0, 0, 10);
    BodyDesc d3; d3.type = BodyType::Static; d3.shape = Shape::make_sphere(0.5f); d3.position = Vec3(0, 0, 15);

    BodyHandle h1 = world.add_body(d1);
    BodyHandle h2 = world.add_body(d2);
    BodyHandle h3 = world.add_body(d3);

    Ray3D ray{ Vec3(0, 0, 0), Vec3(0, 0, 1), 30.0f };
    auto hits = PhysicsQuery::raycast_all(world, ray);

    NF_CHECK_EQ(hits.size(), 3u);
    NF_CHECK(hits[0].body == h1);
    NF_CHECK(hits[1].body == h2);
    NF_CHECK(hits[2].body == h3);

    NF_CHECK(hits[0].distance < hits[1].distance);
    NF_CHECK(hits[1].distance < hits[2].distance);
}

NF_TEST(physics_query_sweep_sphere) {
    PhysicsWorld world;

    // Obstacle box at (0, 0, 10) with half extents (2, 2, 2) => spans z from 8 to 12
    BodyDesc box_desc;
    box_desc.type = BodyType::Static;
    box_desc.shape = Shape::make_box(Vec3(2.0f, 2.0f, 2.0f));
    box_desc.position = Vec3(0.0f, 0.0f, 10.0f);
    BodyHandle h_box = world.add_body(box_desc);

    // Sweep sphere of radius 1 from (0, 0, 0) to (0, 0, 20)
    auto sweep_hit = PhysicsQuery::sweep_sphere(world, Vec3(0, 0, 0), Vec3(0, 0, 20), 1.0f);
    NF_CHECK(sweep_hit.has_value());
    NF_CHECK(sweep_hit->has_hit);
    NF_CHECK(sweep_hit->body == h_box);

    // Box starts at z=8. Sphere touches when its center reaches z = 8 - 1 = 7
    NF_CHECK_NEAR(sweep_hit->distance, 7.0f, 1e-4f);
    NF_CHECK_NEAR(sweep_hit->normal.z, -1.0f, 1e-4f);

    // Sweep that passes over the box (y = 5)
    auto sweep_miss = PhysicsQuery::sweep_sphere(world, Vec3(0, 5, 0), Vec3(0, 5, 20), 1.0f);
    NF_CHECK(!sweep_miss.has_value());
}

NF_TEST(physics_query_overlap_sphere_and_box) {
    PhysicsWorld world;

    BodyDesc d1; d1.type = BodyType::Static; d1.shape = Shape::make_sphere(1.0f); d1.position = Vec3(2, 0, 0);
    BodyDesc d2; d2.type = BodyType::Static; d2.shape = Shape::make_box(Vec3(1, 1, 1)); d2.position = Vec3(-2, 0, 0);
    BodyDesc d3; d3.type = BodyType::Static; d3.shape = Shape::make_sphere(1.0f); d3.position = Vec3(0, 20, 0); // far

    BodyHandle h1 = world.add_body(d1);
    BodyHandle h2 = world.add_body(d2);
    BodyHandle h3 = world.add_body(d3);
    (void)h3;

    // Overlap sphere at origin with radius 4.0
    auto sphere_overlaps = PhysicsQuery::overlap_sphere(world, Vec3(0, 0, 0), 4.0f);
    NF_CHECK_EQ(sphere_overlaps.size(), 2u);
    NF_CHECK(std::find(sphere_overlaps.begin(), sphere_overlaps.end(), h1) != sphere_overlaps.end());
    NF_CHECK(std::find(sphere_overlaps.begin(), sphere_overlaps.end(), h2) != sphere_overlaps.end());

    // Overlap box at origin with half extents (1.5, 1.5, 1.5)
    // d1 sphere is at x=2, radius 1 => surface reaches x=1.0 <= 1.5 => overlaps!
    auto box_overlaps = PhysicsQuery::overlap_box(world, Vec3(0, 0, 0), Vec3(1.5f, 1.5f, 1.5f), Quat::identity());
    NF_CHECK_EQ(box_overlaps.size(), 2u);
}

NF_TEST(trigger_manager_enter_stay_exit_lifecycle) {
    PhysicsWorld world;
    TriggerManager triggers;

    // Create a Trigger Volume at (0, 0, 0) of size 2x2x2 (half-extents 1, 1, 1)
    TriggerVolume tv;
    tv.name = "ZoneA";
    tv.position = Vec3(0.0f, 0.0f, 0.0f);
    tv.shape = Shape::make_box(Vec3(1.0f, 1.0f, 1.0f));
    u32 tv_id = triggers.add_volume(tv);
    NF_CHECK_EQ(tv_id, 1u);

    int enter_count = 0;
    int stay_count = 0;
    int exit_count = 0;

    triggers.register_callback([&](const TriggerEvent& ev) {
        if (ev.trigger_id == tv_id) {
            if (ev.type == TriggerEventType::Enter) enter_count++;
            else if (ev.type == TriggerEventType::Stay) stay_count++;
            else if (ev.type == TriggerEventType::Exit) exit_count++;
        }
    });

    // Body starts outside at (10, 0, 0)
    BodyDesc b_desc;
    b_desc.type = BodyType::Kinematic;
    b_desc.shape = Shape::make_sphere(0.5f);
    b_desc.position = Vec3(10.0f, 0.0f, 0.0f);
    BodyHandle body = world.add_body(b_desc);

    // Frame 1: Outside
    triggers.update(world);
    NF_CHECK_EQ(enter_count, 0);
    NF_CHECK_EQ(stay_count, 0);
    NF_CHECK_EQ(exit_count, 0);
    NF_CHECK(!triggers.is_overlapping(tv_id, body));

    // Move body inside trigger: (0, 0, 0)
    world.set_state(body, BodyState{ Vec3(0, 0, 0), Quat::identity(), Vec3::zero, Vec3::zero, false });

    // Frame 2: Enter!
    triggers.update(world);
    NF_CHECK_EQ(enter_count, 1);
    NF_CHECK_EQ(stay_count, 0);
    NF_CHECK_EQ(exit_count, 0);
    NF_CHECK(triggers.is_overlapping(tv_id, body));

    // Frame 3: Stay!
    triggers.update(world);
    NF_CHECK_EQ(enter_count, 1);
    NF_CHECK_EQ(stay_count, 1);
    NF_CHECK_EQ(exit_count, 0);

    // Move body back outside: (10, 0, 0)
    world.set_state(body, BodyState{ Vec3(10, 0, 0), Quat::identity(), Vec3::zero, Vec3::zero, false });

    // Frame 4: Exit!
    triggers.update(world);
    NF_CHECK_EQ(enter_count, 1);
    NF_CHECK_EQ(stay_count, 1);
    NF_CHECK_EQ(exit_count, 1);
    NF_CHECK(!triggers.is_overlapping(tv_id, body));
}

NF_TEST(trigger_manager_removal_and_disable) {
    PhysicsWorld world;
    TriggerManager triggers;

    TriggerVolume tv;
    tv.name = "ZoneB";
    tv.position = Vec3(0, 0, 0);
    tv.shape = Shape::make_sphere(3.0f);
    u32 tv_id = triggers.add_volume(tv);

    int exits = 0;
    triggers.register_callback([&](const TriggerEvent& ev) {
        if (ev.type == TriggerEventType::Exit) exits++;
    });

    BodyDesc b_desc;
    b_desc.type = BodyType::Dynamic;
    b_desc.shape = Shape::make_sphere(0.5f);
    b_desc.position = Vec3(0, 0, 0);
    BodyHandle body = world.add_body(b_desc);

    // Enter
    triggers.update(world);
    NF_CHECK(triggers.is_overlapping(tv_id, body));

    // Disable trigger volume
    triggers.get_volume(tv_id)->enabled = false;
    triggers.update(world);
    NF_CHECK_EQ(exits, 1);
    NF_CHECK(!triggers.is_overlapping(tv_id, body));
}
