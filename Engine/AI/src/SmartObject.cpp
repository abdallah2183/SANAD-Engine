#include <NF/AI/SmartObject.hpp>

#include <algorithm>
#include <cmath>

namespace nf::ai {

u32 SmartObjectManager::register_smart_object(SmartObjectInstance obj) {
    if (obj.id == 0) {
        obj.id = m_next_id++;
    } else {
        m_next_id = std::max(m_next_id, obj.id + 1);
        for (auto& existing : m_objects) {
            if (existing.id == obj.id) {
                existing = std::move(obj);
                return existing.id;
            }
        }
    }

    if (obj.slots.empty()) {
        SmartObjectSlot default_slot;
        default_slot.slot_index = 0;
        obj.slots.push_back(default_slot);
    } else {
        for (u32 i = 0; i < obj.slots.size(); ++i) {
            obj.slots[i].slot_index = i;
        }
    }

    const u32 id = obj.id;
    m_objects.push_back(std::move(obj));
    return id;
}

bool SmartObjectManager::unregister_smart_object(u32 id) {
    auto it = std::remove_if(m_objects.begin(), m_objects.end(), [id](const SmartObjectInstance& o) {
        return o.id == id;
    });
    if (it != m_objects.end()) {
        m_objects.erase(it, m_objects.end());
        return true;
    }
    return false;
}

const SmartObjectInstance* SmartObjectManager::get_smart_object(u32 id) const {
    for (const auto& o : m_objects) {
        if (o.id == id) {
            return &o;
        }
    }
    return nullptr;
}

SmartObjectInstance* SmartObjectManager::get_smart_object_mut(u32 id) {
    for (auto& o : m_objects) {
        if (o.id == id) {
            return &o;
        }
    }
    return nullptr;
}

bool SmartObjectManager::claim_slot(u32 object_id, u32 slot_index, u32 agent_id) {
    if (agent_id == 0) {
        return release_slot(object_id, slot_index);
    }

    SmartObjectInstance* obj = get_smart_object_mut(object_id);
    if (!obj || !obj->is_enabled || obj->current_cooldown > 0.0f) {
        return false;
    }

    if (slot_index >= obj->slots.size()) {
        return false;
    }

    auto& slot = obj->slots[slot_index];
    if (slot.claimed_by == 0 || slot.claimed_by == agent_id) {
        slot.claimed_by = agent_id;
        return true;
    }

    return false;
}

bool SmartObjectManager::release_slot(u32 object_id, u32 slot_index) {
    SmartObjectInstance* obj = get_smart_object_mut(object_id);
    if (!obj || slot_index >= obj->slots.size()) {
        return false;
    }

    auto& slot = obj->slots[slot_index];
    if (slot.claimed_by != 0) {
        slot.claimed_by = 0;
        obj->current_cooldown = obj->definition.cooldown;
        return true;
    }

    return false;
}

void SmartObjectManager::release_all_for_agent(u32 agent_id) {
    if (agent_id == 0) return;
    for (auto& obj : m_objects) {
        for (auto& slot : obj.slots) {
            if (slot.claimed_by == agent_id) {
                slot.claimed_by = 0;
                obj.current_cooldown = obj.definition.cooldown;
            }
        }
    }
}

bool SmartObjectManager::get_agent_claim(u32 agent_id, u32& out_object_id, u32& out_slot_index) const {
    if (agent_id == 0) return false;
    for (const auto& obj : m_objects) {
        for (const auto& slot : obj.slots) {
            if (slot.claimed_by == agent_id) {
                out_object_id = obj.id;
                out_slot_index = slot.slot_index;
                return true;
            }
        }
    }
    return false;
}

bool SmartObjectManager::evaluate_object(
    const SmartObjectInstance& obj,
    const SmartObjectQuery& query,
    u32& out_best_slot,
    f32& out_score
) const {
    if (!obj.is_enabled || obj.current_cooldown > 0.0f) {
        return false;
    }

    if (!query.activity_name.empty() && obj.definition.activity_name != query.activity_name) {
        return false;
    }

    // Check required tags
    for (const auto& req : obj.definition.required_tags) {
        const bool has_req = std::find(query.actor_tags.begin(), query.actor_tags.end(), req) != query.actor_tags.end();
        if (!has_req) {
            return false;
        }
    }

    // Distance check
    const f32 dist = (obj.world_position - query.querier_position).length();
    if (dist > query.max_distance) {
        return false;
    }

    // Find first available slot
    bool found_slot = false;
    u32 chosen_slot = 0;
    for (const auto& s : obj.slots) {
        if (!query.require_available_slot || s.is_available()) {
            chosen_slot = s.slot_index;
            found_slot = true;
            break;
        }
    }

    if (!found_slot) {
        return false;
    }

    out_best_slot = chosen_slot;

    const f32 dist_factor = 1.0f - std::clamp(dist / std::max(1.0f, query.max_distance), 0.0f, 1.0f);
    out_score = obj.definition.priority * (1.0f + dist_factor * 2.0f);

    return true;
}

SmartObjectQueryResult SmartObjectManager::find_best_smart_object(const SmartObjectQuery& query) const {
    SmartObjectQueryResult best;
    f32 best_score = -1e9f;

    for (const auto& obj : m_objects) {
        u32 slot_idx = 0;
        f32 score = 0.0f;
        if (evaluate_object(obj, query, slot_idx, score)) {
            if (score > best_score) {
                best_score = score;
                best.object_id = obj.id;
                best.slot_index = slot_idx;
                best.activity_name = obj.definition.activity_name;
                best.score = score;
                best.valid = obj.get_slot_transform(slot_idx, best.slot_world_pos, best.slot_world_rot);
            }
        }
    }

    return best;
}

void SmartObjectManager::find_all_matching(
    const SmartObjectQuery& query,
    std::vector<SmartObjectQueryResult>& out_results
) const {
    out_results.clear();

    for (const auto& obj : m_objects) {
        u32 slot_idx = 0;
        f32 score = 0.0f;
        if (evaluate_object(obj, query, slot_idx, score)) {
            SmartObjectQueryResult r;
            r.object_id = obj.id;
            r.slot_index = slot_idx;
            r.activity_name = obj.definition.activity_name;
            r.score = score;
            r.valid = obj.get_slot_transform(slot_idx, r.slot_world_pos, r.slot_world_rot);
            out_results.push_back(r);
        }
    }

    std::sort(out_results.begin(), out_results.end(), [](const SmartObjectQueryResult& a, const SmartObjectQueryResult& b) {
        return a.score > b.score;
    });
}

void SmartObjectManager::update(f32 dt) {
    if (dt <= 0.0f) return;
    for (auto& obj : m_objects) {
        if (obj.current_cooldown > 0.0f) {
            obj.current_cooldown = std::max(0.0f, obj.current_cooldown - dt);
        }
    }
}

void SmartObjectManager::clear() {
    m_objects.clear();
}

} // namespace nf::ai
