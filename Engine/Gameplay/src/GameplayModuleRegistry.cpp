// NF/Gameplay/GameplayModuleRegistry.cpp — name -> factory table (Phase 10, W2)

#include <NF/Gameplay/GameplayModuleRegistry.hpp>

#include <algorithm>

namespace nf::gameplay {

GameplayModuleRegistry& GameplayModuleRegistry::instance() noexcept {
    static GameplayModuleRegistry registry;
    return registry;
}

void GameplayModuleRegistry::register_module(std::string_view name, Factory factory) {
    if (name.empty() || factory == nullptr) return;

    for (const Entry& entry : m_entries) {
        if (entry.name == name) return;  // first registration wins, deliberately
    }
    m_entries.push_back(Entry{std::string(name), factory});
}

bool GameplayModuleRegistry::contains(std::string_view name) const noexcept {
    for (const Entry& entry : m_entries) {
        if (entry.name == name) return true;
    }
    return false;
}

std::unique_ptr<GameplayModule> GameplayModuleRegistry::create(std::string_view name) const {
    for (const Entry& entry : m_entries) {
        if (entry.name == name) return entry.factory();
    }
    return nullptr;
}

std::vector<std::string> GameplayModuleRegistry::names() const {
    std::vector<std::string> result;
    result.reserve(m_entries.size());
    for (const Entry& entry : m_entries) {
        result.push_back(entry.name);
    }
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace nf::gameplay
