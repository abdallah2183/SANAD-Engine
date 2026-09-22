// NF/Runtime/BuiltinModules.cpp — the engine-shipped module set.
//
// See the header for why this exists: a static library drops unreferenced
// objects, and module registration depends on the object surviving the link.
// Referencing the types here is the guarantee.

#include <NF/Runtime/BuiltinModules.hpp>

#include <NF/Gameplay/GameplayModuleRegistry.hpp>
#include <NF/Runtime/OrbitCameraModule.hpp>
#include <NF/Runtime/PlayerControllerModule.hpp>

#include <memory>
#include <utility>

namespace nf::gameplay {

void register_builtin_modules() {
    GameplayModuleRegistry& registry = GameplayModuleRegistry::instance();

    // Direct registration beside the static registrar in each module's .cpp:
    // whichever runs first wins, and the registry ignores the duplicate.
    registry.register_module("PlayerController", []() -> std::unique_ptr<GameplayModule> {
        return std::make_unique<PlayerControllerModule>();
    });
    registry.register_module("OrbitCamera", []() -> std::unique_ptr<GameplayModule> {
        return std::make_unique<OrbitCameraModule>();
    });
}

bool builtin_module_registered(const std::string_view name) {
    return GameplayModuleRegistry::instance().contains(name);
}

} // namespace nf::gameplay
