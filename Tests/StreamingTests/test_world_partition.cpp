#include <NF/Test/TestFramework.hpp>
#include <NF/Scene/WorldPartition.hpp>

#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::scene;

NF_TEST(partition_cell_coordinates_and_bounds) {
    WorldPartition wp(64.0f);
    NF_CHECK_NEAR(wp.cell_size(), 64.0f, 1e-4f);

    wp.assign_entity_to_cell(ecs::Entity{1, 0}, Vec3{10.0f, 2.0f, 10.0f});
    wp.assign_entity_to_cell(ecs::Entity{2, 0}, Vec3{-10.0f, 0.0f, -10.0f});

    const PartitionCell* cell_pos = wp.get_cell(PartitionCoord{0, 0, 0});
    NF_CHECK(cell_pos != nullptr);
    NF_CHECK_NEAR(cell_pos->bounds.min.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(cell_pos->bounds.max.x, 64.0f, 1e-4f);
    NF_CHECK_EQ(cell_pos->entities.size(), 1u);
    NF_CHECK(cell_pos->entities[0] == (ecs::Entity{1, 0}));

    const PartitionCell* cell_neg = wp.get_cell(PartitionCoord{-1, 0, -1});
    NF_CHECK(cell_neg != nullptr);
    NF_CHECK_NEAR(cell_neg->bounds.min.x, -64.0f, 1e-4f);
    NF_CHECK_NEAR(cell_neg->bounds.max.x, 0.0f, 1e-4f);
    NF_CHECK_EQ(cell_neg->entities.size(), 1u);
    NF_CHECK(cell_neg->entities[0] == (ecs::Entity{2, 0}));
}

NF_TEST(partition_streaming_source_activation_and_loading) {
    WorldPartition wp(64.0f);
    wp.assign_entity_to_cell(ecs::Entity{10, 0}, Vec3{32.0f, 32.0f, 32.0f}); // Center of cell (0, 0, 0)

    StreamingSource source;
    source.id = 1;
    source.location = Vec3{32.0f, 32.0f, 32.0f};
    source.activation_radius = 50.0f;
    source.loading_radius = 100.0f;
    source.unload_radius = 120.0f;
    wp.set_streaming_source(1, source);

    // 1. Initial update: source is inside cell -> Activated
    wp.update();
    NF_CHECK_EQ(wp.active_cell_count(), 1u);
    NF_CHECK_EQ(wp.loaded_cell_count(), 1u);
    NF_CHECK_EQ(wp.active_entity_count(), 1u);

    // 2. Move source to distance 75m (outside 50m activation, inside 100m loading)
    source.location = Vec3{64.0f + 75.0f, 32.0f, 32.0f}; // box max.x is 64, distance to box is 75m
    wp.set_streaming_source(1, source);
    wp.update();
    NF_CHECK_EQ(wp.active_cell_count(), 0u);
    NF_CHECK_EQ(wp.loaded_cell_count(), 1u); // Still Loaded
    NF_CHECK_EQ(wp.active_entity_count(), 0u);

    // 3. Move source to distance 110m (inside hysteresis band [100, 120])
    source.location = Vec3{64.0f + 110.0f, 32.0f, 32.0f};
    wp.set_streaming_source(1, source);
    wp.update();
    NF_CHECK_EQ(wp.loaded_cell_count(), 1u); // Kept Loaded due to hysteresis

    // 4. Move source to distance 150m (beyond 120m unload radius)
    source.location = Vec3{64.0f + 150.0f, 32.0f, 32.0f};
    wp.set_streaming_source(1, source);
    wp.update();
    NF_CHECK_EQ(wp.loaded_cell_count(), 0u);
    NF_CHECK_EQ(wp.active_cell_count(), 0u);
}

NF_TEST(partition_data_layer_runtime_states_and_gating) {
    WorldPartition wp(64.0f);

    DataLayerDef siege_layer;
    siege_layer.name = "Quest_Siege";
    siege_layer.initial_state = DataLayerRuntimeState::Unloaded;
    wp.register_data_layer(siege_layer);

    // Assign entity to cell under "Quest_Siege" layer
    wp.assign_entity_to_cell(ecs::Entity{42, 0}, Vec3{10.0f, 0.0f, 10.0f}, "Quest_Siege");

    // Source is right next to cell
    StreamingSource src;
    src.id = 1;
    src.location = Vec3{10.0f, 0.0f, 10.0f};
    src.activation_radius = 100.0f;
    wp.set_streaming_source(1, src);

    // 1. Layer is Unloaded -> Cell remains Unloaded even though source is close
    wp.update();
    NF_CHECK_EQ(wp.active_cell_count(), 0u);
    NF_CHECK_EQ(wp.loaded_cell_count(), 0u);

    // 2. Set Layer to Loaded
    wp.set_data_layer_state("Quest_Siege", DataLayerRuntimeState::Loaded);
    wp.update();
    NF_CHECK_EQ(wp.active_cell_count(), 0u);
    NF_CHECK_EQ(wp.loaded_cell_count(), 1u);

    // 3. Set Layer to Activated
    wp.set_data_layer_state("Quest_Siege", DataLayerRuntimeState::Activated);
    wp.update();
    NF_CHECK_EQ(wp.active_cell_count(), 1u);
    NF_CHECK_EQ(wp.loaded_cell_count(), 1u);
    NF_CHECK_EQ(wp.active_entity_count(), 1u);
}

NF_TEST(partition_multi_source_cooperative_streaming) {
    WorldPartition wp(64.0f);

    // Entity A at origin
    wp.assign_entity_to_cell(ecs::Entity{1, 0}, Vec3{10.0f, 0.0f, 10.0f});
    // Entity B far away at x = 1000
    wp.assign_entity_to_cell(ecs::Entity{2, 0}, Vec3{1010.0f, 0.0f, 10.0f});
    // Entity C in between at x = 500
    wp.assign_entity_to_cell(ecs::Entity{3, 0}, Vec3{500.0f, 0.0f, 0.0f});

    StreamingSource p1{1, Vec3{10.0f, 0.0f, 10.0f}, 60.0f, 80.0f, 100.0f, true};
    StreamingSource p2{2, Vec3{1010.0f, 0.0f, 10.0f}, 60.0f, 80.0f, 100.0f, true};

    wp.set_streaming_source(1, p1);
    wp.set_streaming_source(2, p2);
    wp.update();

    // Cells at 0 and 1000 are active; cell at 500 is unloaded
    NF_CHECK_EQ(wp.active_cell_count(), 2u);
    NF_CHECK_EQ(wp.active_entity_count(), 2u);

    const PartitionCell* cell_mid = wp.get_cell(PartitionCoord{7, 0, 0}); // 500 / 64 = 7
    NF_CHECK(cell_mid != nullptr);
    NF_CHECK_EQ(cell_mid->current_state, DataLayerRuntimeState::Unloaded);
}

NF_TEST(partition_callbacks_and_entity_tracking) {
    WorldPartition wp(64.0f);

    int cell_transitions = 0;
    wp.on_cell_state_changed([&](const PartitionCoord& /*c*/, DataLayerRuntimeState /*old_s*/, DataLayerRuntimeState /*new_s*/) {
        cell_transitions++;
    });

    int layer_transitions = 0;
    wp.on_layer_state_changed([&](const std::string& /*name*/, DataLayerRuntimeState /*old_s*/, DataLayerRuntimeState /*new_s*/) {
        layer_transitions++;
    });

    DataLayerDef def{"NightLighting", DataLayerType::Runtime, DataLayerRuntimeState::Unloaded, true};
    wp.register_data_layer(def);

    wp.set_data_layer_state("NightLighting", DataLayerRuntimeState::Activated);
    NF_CHECK_EQ(layer_transitions, 1);

    wp.assign_entity_to_cell(ecs::Entity{99, 0}, Vec3{0.0f, 0.0f, 0.0f});
    StreamingSource src{1, Vec3{0.0f, 0.0f, 0.0f}, 100.0f, 120.0f, 150.0f, true};
    wp.set_streaming_source(1, src);

    wp.update();
    NF_CHECK_EQ(cell_transitions, 1); // Transitioned from Unloaded to Activated
    NF_CHECK_EQ(wp.active_entity_count(), 1u);

    // Remove entity
    wp.remove_entity(ecs::Entity{99, 0});
    NF_CHECK_EQ(wp.active_entity_count(), 0u);
}
