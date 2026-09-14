#pragma once

// NF/Gameplay/Components.hpp — scene-side module state (Phase 10, W2)

#include <NF/Core/Types.hpp>

#include <string>
#include <unordered_map>

namespace nf::gameplay {

/// Records that a scene uses a gameplay module, and carries that module's
/// persisted state.
///
/// Two sources of truth exist on purpose, and the direction of the copy is
/// explicit: while a session runs, the module instance owns its state; the
/// component is the *serialized snapshot*. `capture_module_state` writes
/// module -> component before a save, `apply_module_state` writes
/// component -> module after a load. Nothing syncs implicitly, so a test can
/// assert on either side without wondering which one won.
struct GameplayModuleComponent {
    /// Matches `GameplayModule::name()`. A name with no registered factory is
    /// kept rather than dropped, so loading a scene authored with a module the
    /// build does not have degrades to "module missing" instead of silently
    /// discarding the scene's data.
    std::string module_name;

    /// Property name -> text form, in the order the module's class declares
    /// them. Text rather than bytes because the save format is text and because
    /// an unreadable value should be skippable rather than fatal.
    std::unordered_map<std::string, std::string> properties;

    /// A disabled module keeps its state but is not stepped.
    bool enabled = true;
};

} // namespace nf::gameplay
