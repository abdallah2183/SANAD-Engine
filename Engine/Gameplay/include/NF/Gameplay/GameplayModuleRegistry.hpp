#pragma once

// NF/Gameplay/GameplayModuleRegistry.hpp — name -> factory table (Phase 10, W2)
//
// Registration happens during static initialisation, so the Runtime can
// instantiate every gameplay module in the build without the engine knowing
// their names, and the editor can offer them in a dropdown. A module that is
// linked into the binary but never mentioned in engine code still runs.

#include <NF/Gameplay/GameplayModule.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nf::gameplay {

class GameplayModuleRegistry {
public:
    using Factory = std::unique_ptr<GameplayModule> (*)();

    static GameplayModuleRegistry& instance() noexcept;

    /// Registers `name`. A duplicate name is ignored rather than replaced: two
    /// modules claiming one identity is a bug, and silently keeping the last one
    /// would make the winner depend on link order.
    void register_module(std::string_view name, Factory factory);

    [[nodiscard]] bool contains(std::string_view name) const noexcept;

    /// Instantiates the named module, or returns null when it is not registered.
    /// Returns a unique_ptr rather than a reference so a caller that cannot use
    /// the module has something to check.
    [[nodiscard]] std::unique_ptr<GameplayModule> create(std::string_view name) const;

    /// Registered names, sorted. Sorted rather than insertion-ordered because
    /// static-init order across translation units is unspecified, and the
    /// inspector's dropdown must not reshuffle between runs.
    [[nodiscard]] std::vector<std::string> names() const;

    [[nodiscard]] usize size() const noexcept { return m_entries.size(); }

    // No clear(). Registration happens during static initialisation and cannot
    // be re-run, so a reset would permanently disable every module for the rest
    // of the process — a test that used it would silently break every test after
    // it. Tests register uniquely-named probes instead.

private:
    struct Entry {
        std::string name;
        Factory     factory = nullptr;
    };

    std::vector<Entry> m_entries;
};

struct GameplayModuleRegistrar {
    GameplayModuleRegistrar(std::string_view name, GameplayModuleRegistry::Factory factory) {
        GameplayModuleRegistry::instance().register_module(name, factory);
    }
};

} // namespace nf::gameplay

// ---------------------------------------------------------------------------
// Registration macro
// ---------------------------------------------------------------------------
//
// Used at namespace scope, next to the module's definition:
//
//     class OrbitCameraModule final : public GameplayModule {
//     public:
//         const char* name() const override { return "OrbitCamera"; }
//         void on_update(GameplayContext& ctx) override { ... }
//     };
//
//     NF_GAMEPLAY_MODULE(OrbitCameraModule, "OrbitCamera")
//
// `inline` gives one definition across translation units, so including the
// header in several places still registers exactly once.

#define NF_GAMEPLAY_MODULE(ModuleType, Name)                                    \
    namespace nf_gameplay_module_registry {                                     \
    [[maybe_unused]] inline std::unique_ptr<::nf::gameplay::GameplayModule>      \
    nf_gameplay_make_##ModuleType() {                                           \
        return std::make_unique<ModuleType>();                                  \
    }                                                                           \
    [[maybe_unused]] inline const ::nf::gameplay::GameplayModuleRegistrar        \
        nf_gameplay_registrar_##ModuleType{ Name, &nf_gameplay_make_##ModuleType }; \
    } // namespace nf_gameplay_module_registry
