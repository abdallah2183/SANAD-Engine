#pragma once

// NF/Runtime/BuiltinModules.hpp — guaranteed registration of engine modules.
//
// Gameplay modules register through static initialisers in their own
// translation units. That mechanism has a hole: a static library only pulls an
// object file into the final binary when something references a symbol from
// it. A module that no other translation unit names — which is the normal
// case, since the whole point of the registry is that nothing must name the
// types — is silently dropped by the linker, and the module simply does not
// exist in that binary. "Play" then runs with zero gameplay modules while the
// code is right there in the project.
//
// register_builtin_modules() names the engine-shipped modules directly, which
// both registers them (first registration wins, duplicates are ignored, so it
// composes with the static registrars) and forces the linker to pull their
// objects into every binary that links NFGameplay and calls it.
//
// Runtime::init_gameplay() calls this before walking the registry.

#include <string_view>

namespace nf::gameplay {

/// Registers every engine-shipped gameplay module. Idempotent.
void register_builtin_modules();

/// True when the named built-in is registered in this build. Test-facing.
bool builtin_module_registered(std::string_view name);

} // namespace nf::gameplay
