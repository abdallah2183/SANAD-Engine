#include <NF/Gameplay/GameplayAttributes.hpp>

namespace nf::gameplay {

void GameplayAttribute::recalculate() {
    float add_sum = 0.0f;
    float mul_factor = 1.0f;
    bool has_override = false;
    float override_val = 0.0f;

    for (const auto& mod : modifiers) {
        switch (mod.op) {
            case ModifierOp::Add:
                add_sum += mod.value;
                break;
            case ModifierOp::Multiply:
                mul_factor *= mod.value;
                break;
            case ModifierOp::Override:
                has_override = true;
                override_val = mod.value;
                break;
        }
    }

    if (has_override) {
        current_value = std::clamp(override_val, min_value, max_value);
    } else {
        current_value = std::clamp((base_value + add_sum) * mul_factor, min_value, max_value);
    }
}

bool AttributeSet::register_attribute(const std::string& name, float base_val, float min_val, float max_val) {
    if (name.empty() || min_val > max_val) return false;
    if (m_attributes.find(name) != m_attributes.end()) return false;

    GameplayAttribute attr;
    attr.name = name;
    attr.base_value = std::clamp(base_val, min_val, max_val);
    attr.min_value = min_val;
    attr.max_value = max_val;
    attr.current_value = attr.base_value;
    m_attributes[name] = std::move(attr);
    return true;
}

bool AttributeSet::has_attribute(const std::string& name) const {
    return m_attributes.find(name) != m_attributes.end();
}

float AttributeSet::get_current_value(const std::string& name, float fallback) const {
    auto it = m_attributes.find(name);
    return it != m_attributes.end() ? it->second.current_value : fallback;
}

float AttributeSet::get_base_value(const std::string& name, float fallback) const {
    auto it = m_attributes.find(name);
    return it != m_attributes.end() ? it->second.base_value : fallback;
}

bool AttributeSet::set_base_value(const std::string& name, float new_base) {
    auto it = m_attributes.find(name);
    if (it == m_attributes.end()) return false;

    float old_cur = it->second.current_value;
    it->second.base_value = std::clamp(new_base, it->second.min_value, it->second.max_value);
    it->second.recalculate();
    notify_change(name, old_cur, it->second.current_value);
    return true;
}

bool AttributeSet::add_modifier(const std::string& name, const AttributeModifier& mod) {
    auto it = m_attributes.find(name);
    if (it == m_attributes.end()) return false;

    float old_cur = it->second.current_value;
    it->second.modifiers.push_back(mod);
    it->second.recalculate();
    notify_change(name, old_cur, it->second.current_value);
    return true;
}

bool AttributeSet::remove_modifiers_by_source(const std::string& name, uint64_t source_id) {
    auto it = m_attributes.find(name);
    if (it == m_attributes.end()) return false;

    float old_cur = it->second.current_value;
    auto& mods = it->second.modifiers;
    auto orig_size = mods.size();
    mods.erase(
        std::remove_if(mods.begin(), mods.end(), [source_id](const AttributeModifier& m) {
            return m.source_id == source_id;
        }),
        mods.end()
    );

    if (mods.size() != orig_size) {
        it->second.recalculate();
        notify_change(name, old_cur, it->second.current_value);
        return true;
    }
    return false;
}

void AttributeSet::on_change(const std::string& name, ChangeCallback cb) {
    if (cb) {
        m_callbacks[name].push_back(std::move(cb));
    }
}

void AttributeSet::notify_change(const std::string& name, float old_val, float new_val) {
    if (old_val == new_val) return;
    auto it = m_callbacks.find(name);
    if (it != m_callbacks.end()) {
        for (const auto& cb : it->second) {
            cb(name, old_val, new_val);
        }
    }
}

std::vector<std::string> AttributeSet::attribute_names() const {
    std::vector<std::string> names;
    names.reserve(m_attributes.size());
    for (const auto& [name, _] : m_attributes) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

// --- EffectManager ---

void EffectManager::apply_effect(GameplayEffect effect) {
    // Apply instant changes immediately
    for (const auto& [attr_name, delta] : effect.instant_deltas) {
        float cur = m_attributes.get_base_value(attr_name);
        m_attributes.set_base_value(attr_name, cur + delta);
    }

    if (effect.duration_type == EffectDurationType::Instant) {
        return; // Instant effects do not persist
    }

    // Attach active modifiers
    for (const auto& [attr_name, mod] : effect.modifiers) {
        m_attributes.add_modifier(attr_name, mod);
    }

    effect.time_remaining = effect.duration;
    effect.time_to_next_tick = effect.period;
    m_active_effects.push_back(std::move(effect));
}

void EffectManager::update(float dt) {
    for (auto it = m_active_effects.begin(); it != m_active_effects.end();) {
        it->time_remaining -= dt;

        // Periodic ticks
        if (it->duration_type == EffectDurationType::Periodic) {
            it->time_to_next_tick -= dt;
            while (it->time_to_next_tick <= 0.0f && it->time_remaining > 0.0f) {
                for (const auto& [attr_name, delta] : it->instant_deltas) {
                    float cur = m_attributes.get_base_value(attr_name);
                    m_attributes.set_base_value(attr_name, cur + delta);
                }
                it->time_to_next_tick += it->period;
            }
        }

        // Expired effect cleanup
        if (it->time_remaining <= 0.0f) {
            for (const auto& [attr_name, mod] : it->modifiers) {
                m_attributes.remove_modifiers_by_source(attr_name, mod.source_id);
            }
            it = m_active_effects.erase(it);
        } else {
            ++it;
        }
    }
}

void EffectManager::remove_effects_by_source(uint64_t source_id) {
    for (auto it = m_active_effects.begin(); it != m_active_effects.end();) {
        if (it->source_id == source_id) {
            for (const auto& [attr_name, mod] : it->modifiers) {
                m_attributes.remove_modifiers_by_source(attr_name, mod.source_id);
            }
            it = m_active_effects.erase(it);
        } else {
            ++it;
        }
    }
}

void EffectManager::clear() {
    for (const auto& effect : m_active_effects) {
        for (const auto& [attr_name, mod] : effect.modifiers) {
            m_attributes.remove_modifiers_by_source(attr_name, mod.source_id);
        }
    }
    m_active_effects.clear();
}

} // namespace nf::gameplay
