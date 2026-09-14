// Tests/ECSTests/test_prefab.cpp — Prefab System

#include <NF/Test/TestFramework.hpp>
#include <NF/Scene/Prefab.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/ECS/ECS.hpp>

namespace {

using namespace nf;
using namespace nf::ecs;
using namespace nf::scene;

} // namespace

NF_TEST(prefab_instance_creation) {
    World source;
    Entity src = source.create_entity();
    source.add<Transform>(src, Transform{});
    source.get<Transform>(src)->local_x = 10;

    PrefabSystem psys;
    auto prefab = psys.create_prefab("TestPrefab", source);
    NF_CHECK(prefab && prefab->world().alive_entity_count()==1);

    World target;
    Entity inst = psys.instantiate(*prefab, target);
    NF_CHECK(inst.valid());
    NF_CHECK(target.alive_entity_count()==1);
    auto* t = target.get<Transform>(inst);
    NF_CHECK(t && t->local_x==10);
    // Instance should have PrefabInstanceComponent
    NF_CHECK(target.has<PrefabInstanceComponent>(inst));
}

NF_TEST(prefab_overrides) {
    World source;
    Entity src = source.create_entity();
    source.add<Transform>(src, Transform{});
    source.get<Transform>(src)->local_x = 5;

    PrefabSystem psys;
    auto prefab = psys.create_prefab("P1", source);
    World target;
    Entity inst = psys.instantiate(*prefab, target);
    auto* t = target.get<Transform>(inst);
    NF_CHECK(t->local_x==5);

    PrefabOverride ov;
    ov.component_name="Transform"; ov.field_name="local_x"; ov.override_value="42"; ov.base_value="5";
    psys.set_override(target, inst, ov);
    NF_CHECK(target.get<Transform>(inst)->local_x==42);
    NF_CHECK(psys.has_override(target, inst, "local_x"));
}

NF_TEST(prefab_reset_override) {
    World source;
    Entity src = source.create_entity();
    source.add<Transform>(src, Transform{});
    source.get<Transform>(src)->local_x = 7;
    PrefabSystem psys;
    auto prefab = psys.create_prefab("P2", source);
    World target;
    Entity inst = psys.instantiate(*prefab, target);
    PrefabOverride ov; ov.component_name="Transform"; ov.field_name="local_x"; ov.override_value="99"; ov.base_value="7";
    psys.set_override(target, inst, ov);
    NF_CHECK(target.get<Transform>(inst)->local_x==99);
    psys.reset_override(target, inst, "local_x");
    NF_CHECK(target.get<Transform>(inst)->local_x==7);
    NF_CHECK(!psys.has_override(target, inst, "local_x"));
}

NF_TEST(prefab_nested) {
    World base;
    Entity a = base.create_entity();
    base.add<Transform>(a, Transform{});
    base.get<Transform>(a)->local_x = 1;
    PrefabSystem psys;
    auto prefabA = psys.create_prefab("A", base);

    World mid;
    Entity instA = psys.instantiate(*prefabA, mid);
    // Add another entity to mid and create prefab B that contains A's instance
    Entity extra = mid.create_entity();
    mid.add<Transform>(extra, Transform{});
    mid.get<Transform>(extra)->local_x = 2;
    auto prefabB = psys.create_prefab("B", mid);
    NF_CHECK(prefabB->world().alive_entity_count()==2);

    World target;
    Entity instB = psys.instantiate(*prefabB, target);
    NF_CHECK(target.alive_entity_count()==2);
}

NF_TEST(prefab_variant) {
    World base;
    Entity e = base.create_entity();
    base.add<Transform>(e, Transform{});
    base.get<Transform>(e)->local_x = 10;
    PrefabSystem psys;
    auto basePrefab = psys.create_prefab("Base", base);
    auto variant = psys.create_variant("Variant", *basePrefab);
    NF_CHECK(variant->world().alive_entity_count()==1);
    // Variant can be instantiated independently
    World target;
    Entity inst = psys.instantiate(*variant, target);
    NF_CHECK(target.get<Transform>(inst)->local_x==10);
}
