// Tests/RuntimeTests/test_status_effects.cpp — Phase 54: damage over time.
//
// A fire round is not a different-coloured bullet. These cases pin the three
// rules the direct path does not have:
//
//   * applying the same kind refreshes instead of stacking;
//   * ticks are paid at a fixed interval, independent of frame rate, and the
//     final partial tick pays the exact remaining damage;
//   * an effect removes itself and can be removed by gameplay.

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/StatusEffect.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::runtime;

namespace {

constexpr f32 kEps = 1e-4f;

} // namespace

NF_TEST(status_names_round_trip_and_refuse_unknown_values) {
    StatusKind kind = StatusKind::None;
    NF_CHECK(status_kind_from_name("burn", kind));
    NF_CHECK(kind == StatusKind::Burning);
    NF_CHECK(status_kind_from_name("bleed", kind));
    NF_CHECK(kind == StatusKind::Bleeding);
    NF_CHECK(status_kind_from_name("corrode", kind));
    NF_CHECK(kind == StatusKind::Corroding);
    NF_CHECK(status_kind_from_name("none", kind));
    NF_CHECK(kind == StatusKind::None);

    NF_CHECK(!status_kind_from_name("poison", kind));
    NF_CHECK(kind == StatusKind::None);
    NF_CHECK(std::string(status_kind_name(StatusKind::Burning)) == "burn");
    NF_CHECK(status_damage_type(StatusKind::Burning) == DamageType::Fire);
    NF_CHECK(status_damage_type(StatusKind::Bleeding) == DamageType::Ballistic);
    NF_CHECK(status_damage_type(StatusKind::Corroding) == DamageType::Explosive);
}

NF_TEST(status_add_refreshes_and_keeps_the_stronger_values) {
    StatusEffectSet set;
    NF_CHECK(status_add(set, make_status(StatusKind::Burning, 4.0f, 3.0f, 0.5f)));
    NF_CHECK_EQ(set.count, 1u);

    // A second fire source does not stack into a furnace: stronger values win.
    NF_CHECK(status_add(set, make_status(StatusKind::Burning, 2.0f, 5.0f, 0.25f)));
    NF_CHECK_EQ(set.count, 1u);
    const StatusEffect* effect = status_find(set, StatusKind::Burning);
    NF_CHECK(effect != nullptr);
    if (effect != nullptr) {
        NF_CHECK_NEAR(effect->damage_per_second, 4.0f, kEps);
        NF_CHECK_NEAR(effect->remaining_seconds, 5.0f, kEps);
        NF_CHECK_NEAR(effect->tick_seconds, 0.25f, kEps);
    }
    NF_CHECK_NEAR(status_remaining(set, StatusKind::Burning), 5.0f, kEps);
    NF_CHECK_NEAR(status_remaining(set, StatusKind::Bleeding), 0.0f, kEps);
}

NF_TEST(status_ticks_at_a_fixed_interval_and_expires) {
    StatusEffectSet set;
    NF_CHECK(status_add(set, make_status(StatusKind::Burning, 4.0f, 1.0f, 0.5f)));
    std::vector<StatusTick> ticks;

    NF_CHECK_EQ(status_step(set, 0.25f, ticks), 0u);
    NF_CHECK(ticks.empty());
    NF_CHECK_NEAR(status_remaining(set, StatusKind::Burning), 0.75f, kEps);

    NF_CHECK_EQ(status_step(set, 0.25f, ticks), 1u);
    NF_CHECK_EQ(ticks.size(), 1u);
    NF_CHECK_NEAR(ticks[0].damage, 2.0f, kEps);
    NF_CHECK(ticks[0].damage_type == DamageType::Fire);

    NF_CHECK_EQ(status_step(set, 0.5f, ticks), 1u);
    NF_CHECK_NEAR(ticks[0].damage, 2.0f, kEps);
    NF_CHECK_EQ(set.count, 0u); // expired
    NF_CHECK_NEAR(status_remaining(set, StatusKind::Burning), 0.0f, kEps);
}

NF_TEST(a_partial_final_tick_pays_the_exact_remaining_damage) {
    StatusEffectSet set;
    NF_CHECK(status_add(set, make_status(StatusKind::Burning, 4.0f, 0.75f, 0.5f)));
    std::vector<StatusTick> ticks;
    NF_CHECK_EQ(status_step(set, 0.75f, ticks), 2u);
    // One full tick (2) plus the 0.25 s remainder (1) = 4 dps * 0.75 s.
    NF_CHECK_NEAR(ticks[0].damage, 2.0f, kEps);
    NF_CHECK_NEAR(ticks[1].damage, 1.0f, kEps);
    NF_CHECK_EQ(set.count, 0u);
}

NF_TEST(a_long_frame_can_pay_several_ticks_at_once) {
    StatusEffectSet set;
    NF_CHECK(status_add(set, make_status(StatusKind::Bleeding, 2.0f, 3.0f, 0.5f)));
    std::vector<StatusTick> ticks;
    NF_CHECK_EQ(status_step(set, 1.25f, ticks), 2u);
    NF_CHECK_NEAR(ticks[0].damage, 1.0f, kEps);
    NF_CHECK_NEAR(ticks[1].damage, 1.0f, kEps);
    NF_CHECK_NEAR(status_remaining(set, StatusKind::Bleeding), 1.75f, kEps);
}

NF_TEST(status_remove_and_clear_drop_the_right_effects) {
    StatusEffectSet set;
    NF_CHECK(status_add(set, make_status(StatusKind::Burning, 4.0f, 3.0f)));
    NF_CHECK(status_add(set, make_status(StatusKind::Bleeding, 2.0f, 3.0f)));
    NF_CHECK_EQ(set.count, 2u);
    NF_CHECK(status_remove(set, StatusKind::Burning));
    NF_CHECK_EQ(set.count, 1u);
    NF_CHECK(status_find(set, StatusKind::Burning) == nullptr);
    NF_CHECK(status_find(set, StatusKind::Bleeding) != nullptr);
    NF_CHECK(!status_remove(set, StatusKind::Burning));
    status_clear(set);
    NF_CHECK_EQ(set.count, 0u);
}

NF_TEST(invalid_status_values_are_refused) {
    StatusEffectSet set;
    NF_CHECK(!status_add(set, make_status(StatusKind::Burning, 0.0f, 3.0f)));
    NF_CHECK(!status_add(set, make_status(StatusKind::Burning, 4.0f, 0.0f)));
    NF_CHECK(!status_add(set, make_status(StatusKind::Burning, 4.0f, 3.0f, 0.0f)));
    NF_CHECK(!status_add(set, make_status(StatusKind::None, 4.0f, 3.0f)));
    NF_CHECK_EQ(set.count, 0u);
}

NF_TEST(status_effects_round_trip_through_the_scene_format) {
    scene::Scene source("StatusScene");
    const ecs::Entity entity = source.world().create_entity();
    source.world().add<scene::NameComponent>(entity, scene::NameComponent{"Hazard"});
    source.world().add<scene::Transform>(entity);

    StatusEffectComponent status;
    NF_CHECK(status_add(status.effects, make_status(StatusKind::Burning, 4.0f, 3.0f, 0.5f)));
    NF_CHECK(status_add(status.effects, make_status(StatusKind::Bleeding, 2.0f, 5.0f, 1.0f)));
    source.world().add<StatusEffectComponent>(entity, status);

    const std::string text = serialize_scene_to_text(source);
    NF_CHECK(text.find("Status:") != std::string::npos);
    NF_CHECK(text.find("burn=") != std::string::npos);
    NF_CHECK(text.find("bleed=") != std::string::npos);

    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    if (!loaded.success || !loaded.scene) {
        return;
    }
    ecs::Entity found = ecs::kInvalidEntity;
    for (ecs::Entity e : loaded.scene->world().all_entities()) {
        const scene::NameComponent* n = loaded.scene->world().get<scene::NameComponent>(e);
        if (n != nullptr && n->name == "Hazard") {
            found = e;
        }
    }
    const StatusEffectComponent* loaded_status =
        loaded.scene->world().get<StatusEffectComponent>(found);
    NF_CHECK(loaded_status != nullptr);
    if (loaded_status != nullptr) {
        NF_CHECK_EQ(loaded_status->effects.count, 2u);
        const StatusEffect* burn = status_find(loaded_status->effects, StatusKind::Burning);
        const StatusEffect* bleed = status_find(loaded_status->effects, StatusKind::Bleeding);
        NF_CHECK(burn != nullptr);
        NF_CHECK(bleed != nullptr);
        if (burn != nullptr) {
            NF_CHECK_NEAR(burn->damage_per_second, 4.0f, kEps);
            NF_CHECK_NEAR(burn->remaining_seconds, 3.0f, kEps);
        }
        if (bleed != nullptr) {
            NF_CHECK_NEAR(bleed->damage_per_second, 2.0f, kEps);
            NF_CHECK_NEAR(bleed->remaining_seconds, 5.0f, kEps);
        }
    }
}
