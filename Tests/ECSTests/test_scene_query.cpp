#include <NF/Test/TestFramework.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Scene/SceneQuery.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/SpatialOctree.hpp>

namespace {

using namespace nf;
using namespace nf::ecs;
using namespace nf::scene;

} // namespace

NF_TEST(scene_query_find_by_name) {
    World world;
    Entity e1 = world.create_entity();
    world.add<NameComponent>(e1, NameComponent{"Player"});

    Entity e2 = world.create_entity();
    world.add<NameComponent>(e2, NameComponent{"Enemy"});

    Entity e3 = world.create_entity();
    world.add<NameComponent>(e3, NameComponent{"Enemy"});

    Entity found_player = SceneQuery::find_by_name(world, "Player");
    NF_CHECK(found_player.valid());
    NF_CHECK(found_player == e1);

    Entity found_none = SceneQuery::find_by_name(world, "Boss");
    NF_CHECK(!found_none.valid());

    auto enemies = SceneQuery::find_all_by_name(world, "Enemy");
    NF_CHECK_EQ(enemies.size(), 2u);
    NF_CHECK((enemies[0] == e2 && enemies[1] == e3) || (enemies[0] == e3 && enemies[1] == e2));
}

NF_TEST(scene_query_populate_and_raycast) {
    World world;
    SpatialOctree octree(AABB{Vec3{-100.0f, -100.0f, -100.0f}, Vec3{100.0f, 100.0f, 100.0f}});

    Entity e1 = world.create_entity();
    Transform t1{};
    t1.world_x = 0.0f;
    t1.world_y = 0.0f;
    t1.world_z = 10.0f;
    t1.world_scale_x = 2.0f;
    t1.world_scale_y = 2.0f;
    t1.world_scale_z = 2.0f;
    world.add<Transform>(e1, t1);

    Entity e2 = world.create_entity();
    Transform t2{};
    t2.world_x = 50.0f;
    t2.world_y = 0.0f;
    t2.world_z = 10.0f;
    world.add<Transform>(e2, t2);

    SceneQuery::populate_octree(world, octree);

    // Cast ray from origin along +Z
    Ray ray{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
    auto hit = SceneQuery::raycast(world, octree, ray);

    NF_CHECK(hit.has_value());
    NF_CHECK(hit->entity == e1);
    NF_CHECK(hit->distance > 0.0f && hit->distance < 11.0f);

    // Cast ray in opposite direction
    Ray ray_back{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}};
    auto miss = SceneQuery::raycast(world, octree, ray_back);
    NF_CHECK(!miss.has_value());
}

NF_TEST(scene_query_frustum_and_sphere) {
    World world;
    SpatialOctree octree(AABB{Vec3{-50.0f, -50.0f, -50.0f}, Vec3{50.0f, 50.0f, 50.0f}});

    Entity in_front = world.create_entity();
    Transform t_front{};
    t_front.world_z = 15.0f;
    world.add<Transform>(in_front, t_front);

    Entity behind = world.create_entity();
    Transform t_behind{};
    t_behind.world_z = -15.0f;
    world.add<Transform>(behind, t_behind);

    SceneQuery::populate_octree(world, octree);

    // Build a frustum looking down +Z
    Frustum frustum;
    frustum.planes[0] = Plane{1.0f, 0.0f, 0.0f, 20.0f};  // Left
    frustum.planes[1] = Plane{-1.0f, 0.0f, 0.0f, 20.0f}; // Right
    frustum.planes[2] = Plane{0.0f, 1.0f, 0.0f, 20.0f};  // Bottom
    frustum.planes[3] = Plane{0.0f, -1.0f, 0.0f, 20.0f}; // Top
    frustum.planes[4] = Plane{0.0f, 0.0f, 1.0f, -1.0f};  // Near (z >= 1)
    frustum.planes[5] = Plane{0.0f, 0.0f, -1.0f, 50.0f}; // Far (z <= 50)

    std::vector<Entity> visible;
    SceneQuery::query_frustum(world, octree, frustum, visible);

    NF_CHECK_EQ(visible.size(), 1u);
    NF_CHECK(visible[0] == in_front);

    // Sphere query around (0, 0, 15) with radius 5
    BoundingSphere sphere{Vec3{0.0f, 0.0f, 15.0f}, 5.0f};
    std::vector<Entity> in_sphere;
    SceneQuery::query_sphere(world, octree, sphere, in_sphere);

    NF_CHECK_EQ(in_sphere.size(), 1u);
    NF_CHECK(in_sphere[0] == in_front);
}
