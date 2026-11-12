#pragma once
#include <NF/Scripting/ScriptEngine.hpp>
#include <NF/UI/ScriptCanvas.hpp>
namespace nf::scripting {
void install_canvas_bindings(LuaVM& vm, ui::ScriptCanvas* canvas);
}
