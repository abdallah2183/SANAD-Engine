#pragma once

// NF/Scripting/UiBindings.hpp — script bindings for the runtime game UI (G3).
//
// The game UI has exactly ONE behaviour (nf::ui::GameFlow); this header is how
// Lua and C# reach it, so a game written in either language drives the same
// screens, HUD and settings as the C++ sample — no second code path to test.
//
//   Lua:  install_ui_bindings(vm, flow)   ->  nf.ui.action("confirm"),
//                                              nf.ui.screen(), nf.ui.set_health(...)
//   C#:   make_ui_host_api(flow)          ->  UiHostApi table (CSharpMarshal.hpp)
//                                              that NFSandbox's UI drivers call.
//
// `flow` must outlive the VM / the table's users. The table is a value: copy
// it freely, but every copy still points at the same flow.

#include <NF/Scripting/CSharpMarshal.hpp>
#include <NF/Scripting/ScriptEngine.hpp>
#include <NF/UI/GameUI.hpp>

namespace nf::scripting {

/// Installs `nf.ui.*` on `vm`, bound to `flow`. Re-installing re-targets the
/// existing functions (same convention as install_ecs_bindings). No-op when
/// the VM is invalid.
void install_ui_bindings(LuaVM& vm, ui::GameFlow& flow);

/// Builds the C# host API table bound to `flow`. Every entry point is a plain
/// Cdecl function pointer — blittable across the managed boundary.
UiHostApi make_ui_host_api(ui::GameFlow& flow);

} // namespace nf::scripting
