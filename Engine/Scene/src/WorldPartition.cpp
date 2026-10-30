#include <NF/Scene/WorldPartition.hpp>

#include <cmath>

namespace nf::scene {

namespace {

static inline f32 distance_to_box(const Vec3& pt, const AABB& box) {
    f32 sq_dist = 0.0f;
    const f32* pt_arr = &pt.x;
    const f32* min_arr = &box.min.x;
    const f32* max_arr = &box.max.x;
    for (int i = 0; i < 3; ++i) {
        const f32 v = pt_arr[i];
        const f32 min_v = min_arr[i];
        const f32 max_v = max_arr[i];
        if (v < min_v) {
            sq_dist += (min_v - v) * (min_v - v);
        } else if (v > max_v) {
            sq_dist += (v - max_v) * (v - max_v);
        }
    }
    return std::sqrt(sq_dist);
}

} // namespace

WorldPartition::WorldPartition(f32 cell_size) {
    set_cell_size(cell_size);
}

void WorldPartition::set_cell_size(f32 size) {
    m_cell_size = std::max(4.0f, size);
}

bool WorldPartition::register_data_layer(DataLayerDef def) {
    if (def.name.empty() || m_layer_defs.find(def.name) != m_layer_defs.end()) {
        return false;
    }
    m_layer_states[def.name] = def.initial_state;
    m_layer_defs[def.name] = std::move(def);
    return true;
}

bool WorldPartition::has_data_layer(const std::string& name) const {
    return m_layer_defs.find(name) != m_layer_defs.end();
}

DataLayerRuntimeState WorldPartition::get_data_layer_state(const std::string& name) const {
    auto it = m_layer_states.find(name);
    return it != m_layer_states.end() ? it->second : DataLayerRuntimeState::Unloaded;
}

void WorldPartition::set_data_layer_state(const std::string& name, DataLayerRuntimeState state) {
    auto it = m_layer_states.find(name);
    if (it == m_layer_states.end()) {
        return;
    }

    if (it->second != state) {
        const DataLayerRuntimeState old_state = it->second;
        it->second = state;
        for (const auto& cb : m_layer_callbacks) {
            if (cb) {
                cb(name, old_state, state);
            }
        }
    }
}

void WorldPartition::set_streaming_source(u32 id, const StreamingSource& source) {
    m_streaming_sources[id] = source;
}

void WorldPartition::remove_streaming_source(u32 id) {
    m_streaming_sources.erase(id);
}

void WorldPartition::clear_streaming_sources() {
    m_streaming_sources.clear();
}

PartitionCoord WorldPartition::get_coord(const Vec3& pos) const {
    const auto floor_div = [this](f32 v) -> i32 {
        return static_cast<i32>(std::floor(v / m_cell_size));
    };
    return PartitionCoord{floor_div(pos.x), floor_div(pos.y), floor_div(pos.z)};
}

AABB WorldPartition::get_cell_bounds(const PartitionCoord& coord) const {
    const Vec3 bmin{
        static_cast<f32>(coord.x) * m_cell_size,
        static_cast<f32>(coord.y) * m_cell_size,
        static_cast<f32>(coord.z) * m_cell_size
    };
    const Vec3 bmax{
        bmin.x + m_cell_size,
        bmin.y + m_cell_size,
        bmin.z + m_cell_size
    };
    return AABB(bmin, bmax);
}

void WorldPartition::assign_entity_to_cell(ecs::Entity entity, const Vec3& world_pos, const std::string& data_layer) {
    remove_entity(entity);

    const PartitionCoord coord = get_coord(world_pos);
    auto it = m_cells.find(coord);
    if (it == m_cells.end()) {
        PartitionCell cell;
        cell.coord = coord;
        cell.bounds = get_cell_bounds(coord);
        cell.current_state = DataLayerRuntimeState::Unloaded;
        it = m_cells.emplace(coord, std::move(cell)).first;
    }

    it->second.entities.push_back(entity);
    if (!data_layer.empty()) {
        it->second.data_layers.insert(data_layer);
    }
    m_entity_to_coord[entity] = coord;
}

void WorldPartition::remove_entity(ecs::Entity entity) {
    auto it = m_entity_to_coord.find(entity);
    if (it == m_entity_to_coord.end()) {
        return;
    }

    auto cell_it = m_cells.find(it->second);
    if (cell_it != m_cells.end()) {
        auto& ents = cell_it->second.entities;
        ents.erase(std::remove(ents.begin(), ents.end(), entity), ents.end());
    }
    m_entity_to_coord.erase(it);
}

void WorldPartition::update() {
    for (auto& pair : m_cells) {
        PartitionCell& cell = pair.second;

        // 1. Data Layer gating
        DataLayerRuntimeState layer_state = DataLayerRuntimeState::Activated;
        if (!cell.data_layers.empty()) {
            bool any_active = false;
            bool any_loaded = false;
            for (const auto& layer_name : cell.data_layers) {
                const DataLayerRuntimeState s = get_data_layer_state(layer_name);
                if (s == DataLayerRuntimeState::Activated) {
                    any_active = true;
                } else if (s == DataLayerRuntimeState::Loaded) {
                    any_loaded = true;
                }
            }

            if (any_active) {
                layer_state = DataLayerRuntimeState::Activated;
            } else if (any_loaded) {
                layer_state = DataLayerRuntimeState::Loaded;
            } else {
                layer_state = DataLayerRuntimeState::Unloaded;
            }
        }

        // 2. Spatial Streaming gating
        DataLayerRuntimeState spatial_state = DataLayerRuntimeState::Unloaded;
        if (!m_streaming_sources.empty()) {
            bool any_source_active = false;
            bool wants_active = false;
            bool wants_loaded = false;
            bool in_hysteresis = false;

            for (const auto& source_pair : m_streaming_sources) {
                const StreamingSource& src = source_pair.second;
                if (!src.is_active) {
                    continue;
                }
                any_source_active = true;

                const f32 dist = distance_to_box(src.location, cell.bounds);
                if (dist <= src.activation_radius) {
                    wants_active = true;
                } else if (dist <= src.loading_radius) {
                    wants_loaded = true;
                } else if (dist <= src.effective_unload_radius()) {
                    in_hysteresis = true;
                }
            }

            if (any_source_active) {
                if (wants_active) {
                    spatial_state = DataLayerRuntimeState::Activated;
                } else if (wants_loaded) {
                    spatial_state = DataLayerRuntimeState::Loaded;
                } else if (in_hysteresis) {
                    // Maintain Loaded state in hysteresis band if already loaded/active
                    if (cell.current_state != DataLayerRuntimeState::Unloaded) {
                        spatial_state = DataLayerRuntimeState::Loaded;
                    } else {
                        spatial_state = DataLayerRuntimeState::Unloaded;
                    }
                } else {
                    spatial_state = DataLayerRuntimeState::Unloaded;
                }
            }
        }

        // 3. Resolve target state = min(layer_state, spatial_state)
        const DataLayerRuntimeState target_state = static_cast<DataLayerRuntimeState>(
            std::min(static_cast<u8>(layer_state), static_cast<u8>(spatial_state))
        );

        if (cell.current_state != target_state) {
            const DataLayerRuntimeState old_s = cell.current_state;
            cell.current_state = target_state;
            for (const auto& cb : m_cell_callbacks) {
                if (cb) {
                    cb(cell.coord, old_s, target_state);
                }
            }
        }
    }
}

size_t WorldPartition::active_cell_count() const noexcept {
    size_t count = 0;
    for (const auto& pair : m_cells) {
        if (pair.second.current_state == DataLayerRuntimeState::Activated) {
            count++;
        }
    }
    return count;
}

size_t WorldPartition::loaded_cell_count() const noexcept {
    size_t count = 0;
    for (const auto& pair : m_cells) {
        if (pair.second.current_state == DataLayerRuntimeState::Loaded ||
            pair.second.current_state == DataLayerRuntimeState::Activated) {
            count++;
        }
    }
    return count;
}

size_t WorldPartition::active_entity_count() const noexcept {
    size_t count = 0;
    for (const auto& pair : m_cells) {
        if (pair.second.current_state == DataLayerRuntimeState::Activated) {
            count += pair.second.entities.size();
        }
    }
    return count;
}

const PartitionCell* WorldPartition::get_cell(const PartitionCoord& coord) const {
    auto it = m_cells.find(coord);
    return it != m_cells.end() ? &it->second : nullptr;
}

void WorldPartition::clear() {
    m_layer_defs.clear();
    m_layer_states.clear();
    m_streaming_sources.clear();
    m_cells.clear();
    m_entity_to_coord.clear();
    m_cell_callbacks.clear();
    m_layer_callbacks.clear();
}

} // namespace nf::scene
