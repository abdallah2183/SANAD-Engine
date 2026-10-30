#include <NF/Gameplay/GameplayCue.hpp>

namespace nf::gameplay {

void GameplayCueManager::register_handler(const std::string& cue_tag, GameplayCueHandler handler) {
    if (cue_tag.empty() || !handler) {
        return;
    }
    m_handlers[cue_tag].push_back(std::move(handler));
}

void GameplayCueManager::unregister_handler(const std::string& cue_tag) {
    m_handlers.erase(cue_tag);
}

void GameplayCueManager::clear_handlers() {
    m_handlers.clear();
}

size_t GameplayCueManager::handler_count() const noexcept {
    size_t count = 0;
    for (const auto& pair : m_handlers) {
        count += pair.second.size();
    }
    return count;
}

bool GameplayCueManager::has_handler_for_tag(const std::string& cue_tag) const {
    auto it = m_handlers.find(cue_tag);
    return it != m_handlers.end() && !it->second.empty();
}

void GameplayCueManager::execute_cue(const std::string& cue_tag, const GameplayCueParameters& params) {
    dispatch(GameplayCueType::Executed, cue_tag, params);
}

void GameplayCueManager::add_cue(const std::string& cue_tag, const GameplayCueParameters& params) {
    dispatch(GameplayCueType::OnActive, cue_tag, params);
}

void GameplayCueManager::update_cue(const std::string& cue_tag, const GameplayCueParameters& params) {
    dispatch(GameplayCueType::WhileActive, cue_tag, params);
}

void GameplayCueManager::remove_cue(const std::string& cue_tag, const GameplayCueParameters& params) {
    dispatch(GameplayCueType::Removed, cue_tag, params);
}

void GameplayCueManager::dispatch(GameplayCueType type, const std::string& cue_tag, const GameplayCueParameters& params) {
    if (cue_tag.empty()) {
        return;
    }

    if (m_recording_enabled) {
        m_recorded_events.push_back(RecordedCueEvent{type, cue_tag, params});
    }

    // 1. Invoke exact match handlers
    auto it = m_handlers.find(cue_tag);
    if (it != m_handlers.end()) {
        for (const auto& handler : it->second) {
            if (handler) {
                handler(type, params);
            }
        }
    }

    // 2. Hierarchical propagation to parent ancestor tags (e.g. "A.B.C" -> "A.B" -> "A")
    if (m_hierarchical) {
        std::string current = cue_tag;
        while (true) {
            const size_t dot = current.rfind('.');
            if (dot == std::string::npos) {
                break;
            }
            current = current.substr(0, dot);
            if (current.empty()) {
                break;
            }

            auto parent_it = m_handlers.find(current);
            if (parent_it != m_handlers.end()) {
                for (const auto& handler : parent_it->second) {
                    if (handler) {
                        handler(type, params);
                    }
                }
            }
        }
    }
}

} // namespace nf::gameplay
