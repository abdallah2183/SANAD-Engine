// NF/Physics/TriggerVolume.cpp — Spatial Trigger Volumes & Overlap Event Manager

#include <NF/Physics/TriggerVolume.hpp>
#include <NF/Physics/PhysicsQuery.hpp>
#include <algorithm>

namespace nf::physics {

u32 TriggerManager::add_volume(const TriggerVolume& volume) {
    TriggerVolume v = volume;
    v.id = m_next_id++;
    m_volumes.push_back(v);
    m_active_overlaps[v.id] = {};
    return v.id;
}

bool TriggerManager::remove_volume(u32 volume_id) {
    auto it = std::find_if(m_volumes.begin(), m_volumes.end(), [volume_id](const TriggerVolume& v) {
        return v.id == volume_id;
    });
    if (it == m_volumes.end()) {
        return false;
    }

    // Emit Exit for all active bodies
    auto overlap_it = m_active_overlaps.find(volume_id);
    if (overlap_it != m_active_overlaps.end()) {
        for (uint64_t key : overlap_it->second) {
            emit_event({ volume_id, decode_handle(key), TriggerEventType::Exit });
        }
        m_active_overlaps.erase(overlap_it);
    }

    m_volumes.erase(it);
    return true;
}

void TriggerManager::clear() {
    for (const auto& pair : m_active_overlaps) {
        for (uint64_t key : pair.second) {
            emit_event({ pair.first, decode_handle(key), TriggerEventType::Exit });
        }
    }
    m_volumes.clear();
    m_active_overlaps.clear();
}

TriggerVolume* TriggerManager::get_volume(u32 volume_id) {
    for (auto& v : m_volumes) {
        if (v.id == volume_id) return &v;
    }
    return nullptr;
}

const TriggerVolume* TriggerManager::get_volume(u32 volume_id) const {
    for (const auto& v : m_volumes) {
        if (v.id == volume_id) return &v;
    }
    return nullptr;
}

void TriggerManager::register_callback(TriggerCallback callback) {
    m_callbacks.push_back(std::move(callback));
}

void TriggerManager::emit_event(const TriggerEvent& event) {
    for (const auto& cb : m_callbacks) {
        if (cb) cb(event);
    }
}

void TriggerManager::update(const PhysicsWorld& world) {
    for (const auto& v : m_volumes) {
        if (!v.enabled) {
            // If disabled, exit any previously overlapping bodies
            auto& active = m_active_overlaps[v.id];
            for (uint64_t key : active) {
                emit_event({ v.id, decode_handle(key), TriggerEventType::Exit });
            }
            active.clear();
            continue;
        }

        std::unordered_set<uint64_t> current_overlaps;

        world.for_each_body([&](BodyHandle handle, const Shape& body_shape, const BodyState& body_state) {
            bool overlap = false;
            if (v.shape.type == ShapeType::Sphere) {
                overlap = PhysicsQuery::check_overlap_sphere_shape(
                    v.position, v.shape.sphere.radius,
                    body_shape, body_state.position, body_state.orientation
                );
            } else if (v.shape.type == ShapeType::Box) {
                overlap = PhysicsQuery::check_overlap_box_shape(
                    v.position, v.shape.box.half_extents, v.orientation,
                    body_shape, body_state.position, body_state.orientation
                );
            }

            if (overlap) {
                current_overlaps.insert(encode_handle(handle));
            }
        });

        auto& prev_overlaps = m_active_overlaps[v.id];

        // 1. Detect Enter and Stay
        for (uint64_t key : current_overlaps) {
            if (prev_overlaps.find(key) == prev_overlaps.end()) {
                emit_event({ v.id, decode_handle(key), TriggerEventType::Enter });
            } else {
                emit_event({ v.id, decode_handle(key), TriggerEventType::Stay });
            }
        }

        // 2. Detect Exit
        for (uint64_t key : prev_overlaps) {
            if (current_overlaps.find(key) == current_overlaps.end()) {
                emit_event({ v.id, decode_handle(key), TriggerEventType::Exit });
            }
        }

        prev_overlaps = std::move(current_overlaps);
    }
}

bool TriggerManager::is_overlapping(u32 volume_id, BodyHandle body) const {
    auto it = m_active_overlaps.find(volume_id);
    if (it == m_active_overlaps.end()) return false;
    return it->second.find(encode_handle(body)) != it->second.end();
}

std::vector<BodyHandle> TriggerManager::get_overlapping_bodies(u32 volume_id) const {
    std::vector<BodyHandle> result;
    auto it = m_active_overlaps.find(volume_id);
    if (it != m_active_overlaps.end()) {
        result.reserve(it->second.size());
        for (uint64_t key : it->second) {
            result.push_back(decode_handle(key));
        }
    }
    return result;
}

} // namespace nf::physics
