#include <NF/Test/TestFramework.hpp>
#include <NF/Scene/HLOD.hpp>

#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::scene;

NF_TEST(hlod_cluster_creation_and_bounds) {
    HLODManager mgr;
    NF_CHECK_EQ(mgr.cluster_count(), 0u);

    u32 cid = mgr.create_cluster(1, 100.0f, 10.0f);
    NF_CHECK(cid != 0u);
    NF_CHECK_EQ(mgr.cluster_count(), 1u);

    const HLODCluster* cluster = mgr.get_cluster(cid);
    NF_CHECK(cluster != nullptr);
    NF_CHECK_EQ(cluster->level, 1u);
    NF_CHECK_NEAR(cluster->transition_distance, 100.0f, 1e-4f);
    NF_CHECK_NEAR(cluster->hysteresis, 10.0f, 1e-4f);

    // Add entities with bounds
    ecs::Entity e1{1, 0};
    AABB box1(Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, 2.0f, 2.0f});
    mgr.add_entity_to_cluster(cid, e1, box1);

    ecs::Entity e2{2, 0};
    AABB box2(Vec3{8.0f, 0.0f, 0.0f}, Vec3{10.0f, 4.0f, 2.0f});
    mgr.add_entity_to_cluster(cid, e2, box2);

    cluster = mgr.get_cluster(cid);
    NF_CHECK(cluster->bounds.is_valid());
    NF_CHECK_NEAR(cluster->bounds.min.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(cluster->bounds.max.x, 10.0f, 1e-4f);
    NF_CHECK_NEAR(cluster->bounds.max.y, 4.0f, 1e-4f);
    NF_CHECK_NEAR(cluster->sphere.center.x, 5.0f, 1e-4f);
    NF_CHECK_NEAR(cluster->sphere.center.y, 2.0f, 1e-4f);
    NF_CHECK(cluster->sphere.radius > 5.0f);

    // Remove entity
    mgr.remove_entity_from_cluster(cid, e2);
    cluster = mgr.get_cluster(cid);
    NF_CHECK_NEAR(cluster->bounds.max.x, 2.0f, 1e-4f);
}

NF_TEST(hlod_distance_transition_and_hysteresis) {
    HLODManager mgr;
    // Transition at 100m, hysteresis 20m.
    // Half-hysteresis = 10m.
    // Activate proxy at >= 100 + 10 = 110m.
    // Deactivate proxy at < 100 - 10 = 90m.
    u32 cid = mgr.create_cluster(1, 100.0f, 20.0f);

    ecs::Entity e1{1, 0};
    ecs::Entity e2{2, 0};
    ecs::Entity proxy{999, 0};

    // Cluster centered at (0, 0, 0)
    mgr.add_entity_to_cluster(cid, e1, AABB(Vec3{-1.0f}, Vec3{1.0f}));
    mgr.add_entity_to_cluster(cid, e2, AABB(Vec3{-2.0f}, Vec3{2.0f}));
    mgr.set_proxy_entity(cid, proxy);

    // 1. Camera close at (0, 0, 50) -> dist = 50m (< 110m)
    // Detailed entities visible, proxy hidden
    mgr.update(Vec3{0.0f, 0.0f, 50.0f});
    NF_CHECK_EQ(mgr.visible_entities().size(), 2u);
    NF_CHECK_EQ(mgr.visible_proxies().size(), 0u);

    // 2. Camera at distance 105m -> inside hysteresis band [90, 110], was inactive -> remains inactive
    mgr.update(Vec3{0.0f, 0.0f, 105.0f});
    NF_CHECK_EQ(mgr.visible_entities().size(), 2u);
    NF_CHECK_EQ(mgr.visible_proxies().size(), 0u);

    // 3. Camera moves past activation threshold: distance 120m (>= 110m)
    // Proxy activates! Entities hidden!
    mgr.update(Vec3{0.0f, 0.0f, 120.0f});
    NF_CHECK_EQ(mgr.visible_entities().size(), 0u);
    NF_CHECK_EQ(mgr.visible_proxies().size(), 1u);
    NF_CHECK(mgr.visible_proxies()[0] == proxy);

    // 4. Camera moves closer to 100m (inside hysteresis band [90, 110], was active -> remains active!)
    mgr.update(Vec3{0.0f, 0.0f, 100.0f});
    NF_CHECK_EQ(mgr.visible_entities().size(), 0u);
    NF_CHECK_EQ(mgr.visible_proxies().size(), 1u);

    // 5. Camera moves inside deactivation threshold: distance 80m (< 90m)
    // Proxy deactivates! Detailed entities restored!
    mgr.update(Vec3{0.0f, 0.0f, 80.0f});
    NF_CHECK_EQ(mgr.visible_entities().size(), 2u);
    NF_CHECK_EQ(mgr.visible_proxies().size(), 0u);
}

NF_TEST(hlod_hierarchical_nesting) {
    HLODManager mgr;

    // Two Level 1 clusters
    u32 c1 = mgr.create_cluster(1, 100.0f, 10.0f);
    mgr.add_entity_to_cluster(c1, ecs::Entity{1, 0}, AABB(Vec3{0.0f, 0.0f, 0.0f}, Vec3{10.0f, 10.0f, 10.0f}));

    u32 c2 = mgr.create_cluster(1, 100.0f, 10.0f);
    mgr.add_entity_to_cluster(c2, ecs::Entity{2, 0}, AABB(Vec3{20.0f, 0.0f, 0.0f}, Vec3{30.0f, 10.0f, 10.0f}));

    // One Level 2 parent cluster
    u32 parent = mgr.create_cluster(2, 300.0f, 30.0f);
    mgr.add_child_cluster(parent, c1);
    mgr.add_child_cluster(parent, c2);

    NF_CHECK_EQ(mgr.cluster_count_at_level(1), 2u);
    NF_CHECK_EQ(mgr.cluster_count_at_level(2), 1u);

    const HLODCluster* p_cluster = mgr.get_cluster(parent);
    NF_CHECK(p_cluster != nullptr);
    NF_CHECK_NEAR(p_cluster->bounds.min.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(p_cluster->bounds.max.x, 30.0f, 1e-4f);
}

NF_TEST(hlod_frustum_culling) {
    HLODManager mgr;
    u32 cid = mgr.create_cluster(1, 100.0f, 10.0f);
    mgr.add_entity_to_cluster(cid, ecs::Entity{1, 0}, AABB(Vec3{1000.0f, 0.0f, 1000.0f}, Vec3{1010.0f, 10.0f, 1010.0f}));
    mgr.set_proxy_entity(cid, ecs::Entity{555, 0});

    // Frustum looking in direction of -Z around origin
    // Cluster is at (1000, 0, 1000) far outside
    Frustum f;
    // Plane: normal facing inwards. A box at (0, 0, 0)
    f.planes[Frustum::Near] = Plane(Vec3{0.0f, 0.0f, -1.0f}, 10.0f);
    f.planes[Frustum::Far] = Plane(Vec3{0.0f, 0.0f, 1.0f}, 100.0f);
    f.planes[Frustum::Left] = Plane(Vec3{1.0f, 0.0f, 0.0f}, 50.0f);
    f.planes[Frustum::Right] = Plane(Vec3{-1.0f, 0.0f, 0.0f}, 50.0f);
    f.planes[Frustum::Bottom] = Plane(Vec3{0.0f, 1.0f, 0.0f}, 50.0f);
    f.planes[Frustum::Top] = Plane(Vec3{0.0f, -1.0f, 0.0f}, 50.0f);

    mgr.update(Vec3{0.0f, 0.0f, 0.0f}, &f);
    NF_CHECK_EQ(mgr.visible_entities().size(), 0u);
    NF_CHECK_EQ(mgr.visible_proxies().size(), 0u);
    NF_CHECK(mgr.get_cluster(cid)->is_culled);
}

NF_TEST(hlod_spatial_clustering_algorithm) {
    HLODManager mgr;

    std::vector<std::pair<ecs::Entity, AABB>> items;
    // Group A (near origin)
    items.emplace_back(ecs::Entity{1, 0}, AABB(Vec3{0.0f}, Vec3{2.0f}));
    items.emplace_back(ecs::Entity{2, 0}, AABB(Vec3{3.0f}, Vec3{5.0f}));
    items.emplace_back(ecs::Entity{3, 0}, AABB(Vec3{6.0f}, Vec3{8.0f}));

    // Group B (far away at x = 500)
    items.emplace_back(ecs::Entity{4, 0}, AABB(Vec3{500.0f}, Vec3{502.0f}));
    items.emplace_back(ecs::Entity{5, 0}, AABB(Vec3{503.0f}, Vec3{505.0f}));

    // Max cluster radius = 20m
    std::vector<u32> cids = mgr.build_spatial_clusters(items, 20.0f, 150.0f, 15.0f);
    NF_CHECK_EQ(cids.size(), 2u); // Exactly 2 spatial clusters formed

    const HLODCluster* c1 = mgr.get_cluster(cids[0]);
    const HLODCluster* c2 = mgr.get_cluster(cids[1]);
    NF_CHECK_EQ(c1->entities.size(), 3u);
    NF_CHECK_EQ(c2->entities.size(), 2u);
}
