// Tests/ECSTests/test_transform.cpp — Transform hierarchy

#include <NF/Test/TestFramework.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Scene/Transform.hpp>

namespace {

using namespace nf;
using namespace nf::ecs;
using namespace nf::scene;

} // namespace

NF_TEST(transform_parent_child) {
    World world;
    Entity parent = world.create_entity();
    Entity child = world.create_entity();
    world.add<Transform>(parent, Transform{});
    world.add<Transform>(child, Transform{});
    auto* pt = world.get<Transform>(parent);
    auto* ct = world.get<Transform>(child);
    pt->local_x = 10; pt->local_y = 0; pt->local_z = 0;
    ct->local_x = 5; ct->local_y = 0; ct->local_z = 0;
    set_parent(world, child, parent);
    NF_CHECK(get_parent(world, child) == parent);
    auto children = get_children(world, parent);
    NF_CHECK(children.size()==1 && children[0]==child);
    propagate_transforms(world);
    NF_CHECK(ct->world_x == 15);
    NF_CHECK(pt->world_x == 10);
}

NF_TEST(transform_hierarchy_propagation_three_levels) {
    World world;
    Entity grand = world.create_entity();
    Entity parent = world.create_entity();
    Entity child = world.create_entity();
    world.add<Transform>(grand, Transform{});
    world.add<Transform>(parent, Transform{});
    world.add<Transform>(child, Transform{});
    world.get<Transform>(grand)->local_x = 10;
    world.get<Transform>(parent)->local_x = 5;
    world.get<Transform>(child)->local_x = 2;
    set_parent(world, parent, grand);
    set_parent(world, child, parent);
    propagate_transforms(world);
    NF_CHECK(world.get<Transform>(grand)->world_x == 10);
    NF_CHECK(world.get<Transform>(parent)->world_x == 15);
    NF_CHECK(world.get<Transform>(child)->world_x == 17);
}

NF_TEST(transform_reparent) {
    World world;
    Entity a = world.create_entity();
    Entity b = world.create_entity();
    Entity c = world.create_entity();
    world.add<Transform>(a, Transform{}); world.add<Transform>(b, Transform{}); world.add<Transform>(c, Transform{});
    world.get<Transform>(a)->local_x = 10;
    world.get<Transform>(b)->local_x = 5;
    world.get<Transform>(c)->local_x = 1;
    set_parent(world, b, a);
    set_parent(world, c, b);
    propagate_transforms(world);
    NF_CHECK(world.get<Transform>(c)->world_x == 16);
    // Reparent c to a directly
    set_parent(world, c, a);
    propagate_transforms(world);
    NF_CHECK(world.get<Transform>(c)->world_x == 11);
    NF_CHECK(get_parent(world, c) == a);
    auto children_of_a = get_children(world, a);
    NF_CHECK(children_of_a.size()==2); // b and c
}

NF_TEST(transform_detach) {
    World world;
    Entity p = world.create_entity();
    Entity ch = world.create_entity();
    world.add<Transform>(p, Transform{}); world.add<Transform>(ch, Transform{});
    world.get<Transform>(p)->local_x = 7;
    world.get<Transform>(ch)->local_x = 3;
    set_parent(world, ch, p);
    propagate_transforms(world);
    NF_CHECK(world.get<Transform>(ch)->world_x == 10);
    remove_parent(world, ch);
    NF_CHECK(!get_parent(world, ch).valid());
    propagate_transforms(world);
    NF_CHECK(world.get<Transform>(ch)->world_x == 3);
}

NF_TEST(transform_prevents_cycle) {
    World world;
    Entity a = world.create_entity();
    Entity b = world.create_entity();
    world.add<Transform>(a, Transform{}); world.add<Transform>(b, Transform{});
    set_parent(world, b, a);
    // Try to make a child of b (would create cycle a->b->a)
    set_parent(world, a, b);
    // Should have been rejected, so a should still have no parent
    NF_CHECK(!get_parent(world, a).valid());
}
