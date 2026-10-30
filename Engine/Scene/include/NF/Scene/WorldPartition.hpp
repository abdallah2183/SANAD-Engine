#pragma once

#include <NF/Core/Geometry.hpp>
#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/ECS/Entity.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nf::scene {

/// Runtime state of a Data Layer or Streaming Cell
enum class DataLayerRuntimeState : u8 {
    Unloaded = 0, // Not loaded in memory
    Loaded,       // Loaded in memory but inactive (invisible/no collision)
    Activated     // Fully active, rendered, simulated, and ticking
};

/// Type and persistence policy of a Data Layer
enum class DataLayerType : u8 {
    Runtime = 0,  // Dynamically loaded/activated by gameplay logic, quests, or seasons
    AlwaysLoaded  // Always retained in memory once initialized
};

/// Definition of a Data Layer
struct DataLayerDef {
    std::string name;
    DataLayerType type = DataLayerType::Runtime;
    DataLayerRuntimeState initial_state = DataLayerRuntimeState::Unloaded;
    bool is_spatial = true; // True = governed by spatial streaming grid; False = global
};

/// A streaming source (e.g. player camera, streaming focus volume, surveillance vehicle)
struct StreamingSource {
    u32 id = 0;
    Vec3 location{0.0f, 0.0f, 0.0f};
    f32 activation_radius = 64.0f;  // Cells within this distance are Activated
    f32 loading_radius = 96.0f;     // Cells within this distance are Loaded
    f32 unload_radius = 120.0f;     // Cells beyond this distance are Unloaded (hysteresis)
    bool is_active = true;

    [[nodiscard]] f32 effective_unload_radius() const noexcept {
        return std::max(unload_radius, loading_radius);
    }
};

/// Coordinates of a cell in the World Partition grid
struct PartitionCoord {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;

    constexpr bool operator==(const PartitionCoord& o) const noexcept {
        return x == o.x && y == o.y && z == o.z;
    }
    constexpr bool operator!=(const PartitionCoord& o) const noexcept {
        return !(*this == o);
    }
    constexpr bool operator<(const PartitionCoord& o) const noexcept {
        if (x != o.x) return x < o.x;
        if (y != o.y) return y < o.y;
        return z < o.z;
    }
};

} // namespace nf::scene

// Hash specialization for PartitionCoord
template <>
struct std::hash<nf::scene::PartitionCoord> {
    size_t operator()(const nf::scene::PartitionCoord& c) const noexcept {
        size_t h = 14695981039346656037ull;
        h = (h ^ static_cast<size_t>(c.x)) * 1099511628211ull;
        h = (h ^ static_cast<size_t>(c.y)) * 1099511628211ull;
        h = (h ^ static_cast<size_t>(c.z)) * 1099511628211ull;
        return h;
    }
};

namespace nf::scene {

/// A spatial cell within the World Partition grid
struct PartitionCell {
    PartitionCoord coord;
    AABB bounds;
    DataLayerRuntimeState current_state = DataLayerRuntimeState::Unloaded;
    std::unordered_set<std::string> data_layers; // Attached layers
    std::vector<ecs::Entity> entities;
};

/// World Partition and Data Layer Manager
class WorldPartition {
public:
    explicit WorldPartition(f32 cell_size = 64.0f);
    ~WorldPartition() = default;

    void set_cell_size(f32 size);
    [[nodiscard]] f32 cell_size() const noexcept { return m_cell_size; }

    /// Register a Data Layer definition
    bool register_data_layer(DataLayerDef def);
    [[nodiscard]] bool has_data_layer(const std::string& name) const;
    [[nodiscard]] DataLayerRuntimeState get_data_layer_state(const std::string& name) const;

    /// Set runtime target state for a Data Layer
    void set_data_layer_state(const std::string& name, DataLayerRuntimeState state);

    /// Streaming Source Management
    void set_streaming_source(u32 id, const StreamingSource& source);
    void remove_streaming_source(u32 id);
    void clear_streaming_sources();

    /// Entity Assignment
    void assign_entity_to_cell(ecs::Entity entity, const Vec3& world_pos, const std::string& data_layer = "");
    void remove_entity(ecs::Entity entity);

    /// Update tick: evaluates all cells against all streaming sources and active data layers
    void update();

    /// Observers & Callbacks
    using StateChangeCallback = std::function<void(const PartitionCoord& coord, DataLayerRuntimeState old_state, DataLayerRuntimeState new_state)>;
    using LayerChangeCallback = std::function<void(const std::string& layer_name, DataLayerRuntimeState old_state, DataLayerRuntimeState new_state)>;

    void on_cell_state_changed(StateChangeCallback cb) { m_cell_callbacks.push_back(std::move(cb)); }
    void on_layer_state_changed(LayerChangeCallback cb) { m_layer_callbacks.push_back(std::move(cb)); }

    [[nodiscard]] size_t total_cell_count() const noexcept { return m_cells.size(); }
    [[nodiscard]] size_t active_cell_count() const noexcept;
    [[nodiscard]] size_t loaded_cell_count() const noexcept;
    [[nodiscard]] size_t active_entity_count() const noexcept;
    [[nodiscard]] const PartitionCell* get_cell(const PartitionCoord& coord) const;

    /// Clear all cells, sources, and layers
    void clear();

private:
    [[nodiscard]] PartitionCoord get_coord(const Vec3& pos) const;
    [[nodiscard]] AABB get_cell_bounds(const PartitionCoord& coord) const;

    f32 m_cell_size = 64.0f;
    std::unordered_map<std::string, DataLayerDef> m_layer_defs;
    std::unordered_map<std::string, DataLayerRuntimeState> m_layer_states;
    std::unordered_map<u32, StreamingSource> m_streaming_sources;
    std::unordered_map<PartitionCoord, PartitionCell> m_cells;
    std::unordered_map<ecs::Entity, PartitionCoord> m_entity_to_coord;

    std::vector<StateChangeCallback> m_cell_callbacks;
    std::vector<LayerChangeCallback> m_layer_callbacks;
};

} // namespace nf::scene
