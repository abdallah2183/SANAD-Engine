#include <NF/Test/TestFramework.hpp>
#include <NF/Scene/SpatialOctree.hpp>

using namespace nf;
using namespace nf::scene;

NF_TEST(octree_insert_remove_update) {
    SpatialOctree octree(AABB(Vec3(-100.0f), Vec3(100.0f)), 4, 2);

    ecs::Entity e1(1, 1);
    ecs::Entity e2(2, 1);
    ecs::Entity e3(3, 1);

    octree.insert(e1, AABB(Vec3(-10.0f), Vec3(-5.0f)));
    octree.insert(e2, AABB(Vec3(5.0f), Vec3(10.0f)));
    octree.insert(e3, AABB(Vec3(20.0f), Vec3(25.0f)));

    NF_CHECK_EQ(octree.size(), 3ULL);
    NF_CHECK(octree.contains(e1));
    NF_CHECK(octree.contains(e2));

    // Update e1 to a new position
    octree.update(e1, AABB(Vec3(30.0f), Vec3(35.0f)));
    NF_CHECK_EQ(octree.size(), 3ULL);

    // Remove e2
    bool removed = octree.remove(e2);
    NF_CHECK(removed);
    NF_CHECK(!octree.contains(e2));
    NF_CHECK_EQ(octree.size(), 2ULL);

    octree.clear();
    NF_CHECK_EQ(octree.size(), 0ULL);
}

NF_TEST(octree_spatial_queries) {
    SpatialOctree octree(AABB(Vec3(-100.0f), Vec3(100.0f)), 4, 2);

    ecs::Entity e_center(10, 1);
    ecs::Entity e_far_right(20, 1);
    ecs::Entity e_behind(30, 1);

    octree.insert(e_center, AABB(Vec3(-1.0f), Vec3(1.0f)));
    octree.insert(e_far_right, AABB(Vec3(50.0f, 0.0f, 0.0f), Vec3(52.0f, 2.0f, 2.0f)));
    octree.insert(e_behind, AABB(Vec3(0.0f, 0.0f, 20.0f), Vec3(2.0f, 2.0f, 22.0f)));

    // AABB query around center
    std::vector<ecs::Entity> aabb_results;
    octree.query_aabb(AABB(Vec3(-5.0f), Vec3(5.0f)), aabb_results);
    NF_CHECK_EQ(aabb_results.size(), 1ULL);
    NF_CHECK_EQ(aabb_results[0].id, 10U);

    // Raycast down -Z through center
    Ray ray(Vec3(0.0f, 0.0f, 10.0f), Vec3(0.0f, 0.0f, -1.0f));
    std::vector<std::pair<ecs::Entity, f32>> ray_hits;
    octree.query_ray(ray, ray_hits);
    NF_CHECK_EQ(ray_hits.size(), 1ULL);
    NF_CHECK_EQ(ray_hits[0].first.id, 10U);
    NF_CHECK_NEAR(ray_hits[0].second, 9.0f, 1e-4f);

    // Frustum query facing origin from (0, 0, 10)
    Mat4 proj = Mat4::perspective(to_radians(60.0f), 1.0f, 0.1f, 50.0f);
    Mat4 view = Mat4::look_at(Vec3(0.0f, 0.0f, 10.0f), Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f));
    Frustum frustum = Frustum::from_view_projection(view * proj);

    std::vector<ecs::Entity> frustum_results;
    octree.query_frustum(frustum, frustum_results);
    NF_CHECK_EQ(frustum_results.size(), 1ULL);
    NF_CHECK_EQ(frustum_results[0].id, 10U);
}
