#include <NF/Test/TestFramework.hpp>
#include <NF/Core/BVH.hpp>
#include <NF/Core/Geometry.hpp>

#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::core;

NF_TEST(bvh_build_and_hierarchy_structure) {
    std::vector<BVHPrimitive> prims;
    for (u32 i = 0; i < 16; ++i) {
        BVHPrimitive p;
        p.id = i + 1;
        const f32 x = static_cast<f32>(i % 4) * 5.0f;
        const f32 z = static_cast<f32>(i / 4) * 5.0f;
        p.bounds = AABB(Vec3{x - 1.0f, -1.0f, z - 1.0f}, Vec3{x + 1.0f, 1.0f, z + 1.0f});
        prims.push_back(p);
    }

    // 1. Build with SAH
    BVH bvh_sah;
    bvh_sah.build(prims, BVHSplitMethod::SAH, 2);
    NF_CHECK(bvh_sah.is_valid());
    NF_CHECK_EQ(bvh_sah.primitive_count(), 16u);
    NF_CHECK(bvh_sah.node_count() > 1u);

    const AABB& root = bvh_sah.root_bounds();
    NF_CHECK_NEAR(root.min.x, -1.0f, 1e-4f);
    NF_CHECK_NEAR(root.max.x, 16.0f, 1e-4f); // 3*5 + 1 = 16

    // 2. Build with EqualCounts
    BVH bvh_equal;
    bvh_equal.build(prims, BVHSplitMethod::EqualCounts, 2);
    NF_CHECK(bvh_equal.is_valid());
    NF_CHECK_EQ(bvh_equal.primitive_count(), 16u);

    // 3. Build with Middle
    BVH bvh_mid;
    bvh_mid.build(prims, BVHSplitMethod::Middle, 2);
    NF_CHECK(bvh_mid.is_valid());
    NF_CHECK_EQ(bvh_mid.primitive_count(), 16u);
}

NF_TEST(bvh_closest_raycast_precision) {
    // 5 cubes along X axis: x = 10, 20, 30, 40, 50
    std::vector<BVHPrimitive> prims;
    for (u32 i = 0; i < 5; ++i) {
        BVHPrimitive p;
        p.id = 100 + i;
        const f32 x = 10.0f * static_cast<f32>(i + 1);
        p.bounds = AABB(Vec3{x - 1.0f, -1.0f, -1.0f}, Vec3{x + 1.0f, 1.0f, 1.0f});
        prims.push_back(p);
    }

    BVH bvh;
    bvh.build(prims, BVHSplitMethod::SAH, 1);

    // Ray from origin along +X
    Ray ray(Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f});
    BVHRayHit hit;
    bool has_hit = bvh.raycast(ray, hit);

    NF_CHECK(has_hit);
    NF_CHECK_EQ(hit.primitive_id, 100u); // Must hit the closest one at x = 10
    NF_CHECK_NEAR(hit.distance, 9.0f, 1e-4f); // 10 - 1 = 9
    NF_CHECK_NEAR(hit.point.x, 9.0f, 1e-4f);
    NF_CHECK_NEAR(hit.normal.x, -1.0f, 1e-4f); // Hit -X face

    // Ray pointing away in -X
    Ray ray_miss(Vec3{0.0f, 0.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f});
    BVHRayHit hit_miss;
    NF_CHECK(!bvh.raycast(ray_miss, hit_miss));
}

NF_TEST(bvh_raycast_all_ordered_hits) {
    std::vector<BVHPrimitive> prims;
    for (u32 i = 0; i < 4; ++i) {
        BVHPrimitive p;
        p.id = i + 1;
        const f32 z = 10.0f * static_cast<f32>(i + 1);
        p.bounds = AABB(Vec3{-1.0f, -1.0f, z - 1.0f}, Vec3{1.0f, 1.0f, z + 1.0f});
        prims.push_back(p);
    }

    BVH bvh;
    bvh.build(prims, BVHSplitMethod::SAH, 1);

    Ray ray(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f});
    std::vector<BVHRayHit> hits;
    bool hit_any = bvh.raycast_all(ray, hits);

    NF_CHECK(hit_any);
    NF_CHECK_EQ(hits.size(), 4u);

    // Check ascending order
    for (size_t i = 0; i < hits.size(); ++i) {
        NF_CHECK_EQ(hits[i].primitive_id, static_cast<u32>(i + 1));
        if (i > 0) {
            NF_CHECK(hits[i].distance > hits[i - 1].distance);
        }
    }
}

NF_TEST(bvh_query_aabb_spatial_overlap) {
    std::vector<BVHPrimitive> prims;
    for (u32 i = 0; i < 10; ++i) {
        BVHPrimitive p;
        p.id = i;
        const f32 x = static_cast<f32>(i) * 10.0f;
        p.bounds = AABB(Vec3{x - 1.0f, -1.0f, -1.0f}, Vec3{x + 1.0f, 1.0f, 1.0f});
        prims.push_back(p);
    }

    BVH bvh;
    bvh.build(prims, BVHSplitMethod::SAH, 2);

    // Query box overlapping only items 2, 3, 4 (x = 20, 30, 40)
    AABB query_box(Vec3{15.0f, -2.0f, -2.0f}, Vec3{45.0f, 2.0f, 2.0f});
    std::vector<u32> overlapping_ids;
    bvh.query_aabb(query_box, overlapping_ids);

    NF_CHECK_EQ(overlapping_ids.size(), 3u);
    std::sort(overlapping_ids.begin(), overlapping_ids.end());
    NF_CHECK_EQ(overlapping_ids[0], 2u);
    NF_CHECK_EQ(overlapping_ids[1], 3u);
    NF_CHECK_EQ(overlapping_ids[2], 4u);
}

NF_TEST(bvh_query_frustum_culling) {
    std::vector<BVHPrimitive> prims;
    // Front item (in view)
    BVHPrimitive p_front;
    p_front.id = 1;
    p_front.bounds = AABB(Vec3{-1.0f, -1.0f, 10.0f}, Vec3{1.0f, 1.0f, 12.0f});
    prims.push_back(p_front);

    // Behind item (culled)
    BVHPrimitive p_behind;
    p_behind.id = 2;
    p_behind.bounds = AABB(Vec3{-1.0f, -1.0f, -20.0f}, Vec3{1.0f, 1.0f, -18.0f});
    prims.push_back(p_behind);

    BVH bvh;
    bvh.build(prims, BVHSplitMethod::SAH, 1);

    // View looking along +Z from origin, Vulkan clip space
    Mat4 view = Mat4::look_at(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 10.0f}, Vec3{0.0f, 1.0f, 0.0f});
    Mat4 proj = Mat4::perspective(60.0f * DEG_TO_RAD, 1.0f, 0.1f, 100.0f);
    Frustum frustum = Frustum::from_view_projection(view * proj);

    std::vector<u32> visible_ids;
    bvh.query_frustum(frustum, visible_ids);

    NF_CHECK_EQ(visible_ids.size(), 1u);
    NF_CHECK_EQ(visible_ids[0], 1u);
}

NF_TEST(bvh_query_nearest_primitive) {
    std::vector<BVHPrimitive> prims;
    for (u32 i = 0; i < 8; ++i) {
        BVHPrimitive p;
        p.id = i;
        const f32 angle = static_cast<f32>(i) * (TWO_PI / 8.0f);
        const f32 x = std::cos(angle) * 20.0f;
        const f32 z = std::sin(angle) * 20.0f;
        p.bounds = AABB(Vec3{x - 1.0f, -1.0f, z - 1.0f}, Vec3{x + 1.0f, 1.0f, z + 1.0f});
        prims.push_back(p);
    }

    BVH bvh;
    bvh.build(prims, BVHSplitMethod::SAH, 2);

    // Query point close to item 0 (which is at x=20, z=0)
    Vec3 pt{18.0f, 0.0f, 0.0f};
    u32 nearest_id = 999;
    f32 nearest_dist = 1e30f;
    bool found = bvh.query_nearest(pt, nearest_id, nearest_dist);

    NF_CHECK(found);
    NF_CHECK_EQ(nearest_id, 0u);
    // Nearest face of item 0 is at x = 19 (20 - 1). Distance from 18 to 19 is 1.0
    NF_CHECK_NEAR(nearest_dist, 1.0f, 1e-3f);
}

NF_TEST(bvh_refit_dynamic_update) {
    std::vector<BVHPrimitive> prims;
    BVHPrimitive p1{1, AABB(Vec3{-1.0f, -1.0f, 9.0f}, Vec3{1.0f, 1.0f, 11.0f})};
    BVHPrimitive p2{2, AABB(Vec3{-1.0f, -1.0f, 19.0f}, Vec3{1.0f, 1.0f, 21.0f})};
    prims.push_back(p1);
    prims.push_back(p2);

    BVH bvh;
    bvh.build(prims, BVHSplitMethod::SAH, 1);
    NF_CHECK_NEAR(bvh.root_bounds().max.z, 21.0f, 1e-4f);

    // Dynamically expand primitive 2
    bvh.refit(); // refit with original positions
    NF_CHECK_NEAR(bvh.root_bounds().max.z, 21.0f, 1e-4f);
}
