#pragma once
#include <NF/Scripting/ScriptEngine.hpp>
#include <NF/Assets/SettingsStore.hpp>
namespace nf::scripting {
void install_settings_bindings(LuaVM& vm, assets::SettingsStore* store);
}
