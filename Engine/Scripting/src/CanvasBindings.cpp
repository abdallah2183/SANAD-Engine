#include <NF/Scripting/CanvasBindings.hpp>
#include <cmath>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
namespace nf::scripting {
namespace {
ui::ScriptCanvas* canvas(lua_State* state) {
    lua_getfield(state, LUA_REGISTRYINDEX, "nf_script_canvas");
    auto* result = static_cast<ui::ScriptCanvas*>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    return result;
}
std::string_view string_arg(lua_State* state, int index, size_t maximum) {
    size_t size = 0;
    const char* text = luaL_checklstring(state, index, &size);
    luaL_argcheck(state, size <= maximum, index, "UI string is too long");
    if (index == 1) luaL_argcheck(state, size > 0, index, "UI ID must not be empty");
    return std::string_view(text, size);
}
double number_arg(lua_State* state, int index) {
    const double value = luaL_checknumber(state, index);
    luaL_argcheck(state, std::isfinite(value), index, "UI number must be finite");
    return value;
}
float finite_arg(lua_State* state, int index) {
    const double value = luaL_checknumber(state, index);
    luaL_argcheck(state, std::isfinite(value) && std::abs(value) <= 1e8, index, "UI number must be finite and in range");
    return static_cast<float>(value);
}
ui::UiRect rect_arg(lua_State* state, int first) {
    return {finite_arg(state, first), finite_arg(state, first + 1), finite_arg(state, first + 2), finite_arg(state, first + 3)};
}
std::string opt_string(lua_State* state, int index, std::string_view fallback) {
    if (lua_isnoneornil(state, index)) return std::string(fallback);
    size_t n = 0;
    const char* t = luaL_checklstring(state, index, &n);
    luaL_argcheck(state, n <= 128, index, "UI string is too long");
    return std::string(t, n);
}
int panel(lua_State* state) {
    auto id = string_arg(state, 1, 128);
    auto rect = rect_arg(state, 2);
    if (auto* ui = canvas(state)) ui->panel(std::string(id), rect);
    return 0;
}
int label(lua_State* state) {
    auto id = string_arg(state, 1, 128), text = string_arg(state, 2, 4096);
    auto rect = rect_arg(state, 3);
    const float size = lua_isnoneornil(state, 7) ? 22 : finite_arg(state, 7);
    auto align = (lua_isnoneornil(state, 8) ? ui::TextAlign::Left : ui::ScriptCanvas::align_from_name(opt_string(state, 8, "left")));
    if (auto* ui = canvas(state)) ui->label(std::string(id), std::string(text), rect, size, align);
    return 0;
}
int button(lua_State* state) {
    auto id = string_arg(state, 1, 128), text = string_arg(state, 2, 4096);
    auto rect = rect_arg(state, 3);
    const bool enabled = lua_isnoneornil(state, 7) || lua_toboolean(state, 7);
    auto align = (lua_isnoneornil(state, 8) ? ui::TextAlign::Center : ui::ScriptCanvas::align_from_name(opt_string(state, 8, "center")));
    auto* ui = canvas(state);
    lua_pushboolean(state, ui && ui->button(std::string(id), std::string(text), rect, enabled, align));
    return 1;
}
int bar(lua_State* state) {
    auto id = string_arg(state, 1, 128), text = string_arg(state, 2, 4096);
    const float value = finite_arg(state, 3);
    auto rect = rect_arg(state, 4);
    if (auto* ui = canvas(state)) ui->bar(std::string(id), std::string(text), value, rect);
    return 0;
}
int slider(lua_State* state) {
    auto id = string_arg(state, 1, 128), text = string_arg(state, 2, 4096);
    const double value = number_arg(state, 3), minimum = number_arg(state, 4), maximum = number_arg(state, 5);
    luaL_argcheck(state, maximum > minimum && std::isfinite(maximum - minimum), 5, "slider maximum must exceed minimum");
    auto rect = rect_arg(state, 6);
    auto* ui = canvas(state);
    lua_pushnumber(state, ui ? ui->slider(std::string(id), std::string(text), value, minimum, maximum, rect) : value);
    return 1;
}
int checkbox(lua_State* state) {
    auto id = string_arg(state, 1, 128), text = string_arg(state, 2, 4096);
    const bool checked = lua_toboolean(state, 3) != 0;
    auto rect = rect_arg(state, 4);
    const bool enabled = lua_isnoneornil(state, 8) || lua_toboolean(state, 8);
    auto* ui = canvas(state);
    const bool out = ui ? ui->checkbox(std::string(id), std::string(text), checked, rect, enabled) : checked;
    lua_pushboolean(state, out);
    return 1;
}
int list(lua_State* state) {
    auto id = string_arg(state, 1, 128);
    luaL_checktype(state, 2, LUA_TTABLE);
    std::vector<std::string> items;
    const int count = (int)lua_rawlen(state, 2);
    luaL_argcheck(state, count >= 0 && count <= 256, 2, "list items must be 1..256 strings");
    lua_rawgeti(state, 2, 0); // index 0 is reserved in our Lua convention (1-based table)
    lua_pop(state, 1);
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(state, 2, i);
        size_t n = 0;
        const char* t = luaL_checklstring(state, -1, &n);
        luaL_argcheck(state, n <= 512, 2, "list item is too long");
        items.emplace_back(t, n);
        lua_pop(state, 1);
    }
    const int selected1 = static_cast<int>(luaL_checkinteger(state, 3)); // 1-based to Lua
    auto rect = rect_arg(state, 4);
    const int visible_rows = lua_isnoneornil(state, 8) ? 4 : static_cast<int>(luaL_checkinteger(state, 8));
    const bool enabled = lua_isnoneornil(state, 9) || lua_toboolean(state, 9);
    auto* ui = canvas(state);
    int out0 = ui ? ui->list(std::string(id), items, selected1 - 1, rect, visible_rows, enabled) : selected1 - 1;
    lua_pushinteger(state, out0 + 1);
    lua_pushinteger(state, (lua_Integer)items.size());
    return 2;
}
int tooltip(lua_State* state) {
    auto id = string_arg(state, 1, 128), text = string_arg(state, 2, 4096);
    auto rect = rect_arg(state, 3);
    if (auto* ui = canvas(state)) ui->tooltip(std::string(id), std::string(text), rect);
    return 0;
}

int text_input(lua_State* state) {
    auto id = string_arg(state, 1, 128);
    size_t n = 0;
    const char* raw = luaL_checklstring(state, 2, &n);
    luaL_argcheck(state, n <= 8192, 2, "UI text is too long");
    std::string value(raw, n);
    auto rect = rect_arg(state, 3);
    const bool enabled = lua_isnoneornil(state, 7) || lua_toboolean(state, 7);
    const size_t max_length = lua_isnoneornil(state, 8) ? 256 : static_cast<size_t>(luaL_checkinteger(state, 8));
    luaL_argcheck(state, max_length <= 4096, 8, "max_length must be 0..4096");
    auto* ui = canvas(state);
    if (!ui) {
        lua_pushstring(state, value.c_str());
        lua_pushboolean(state, 0);
        lua_pushboolean(state, 0);
        return 3;
    }
    ui::TextInputResult r = ui->text_input(std::string(id), value, rect, enabled, max_length);
    lua_pushlstring(state, r.value.data(), r.value.size());
    lua_pushboolean(state, r.committed ? 1 : 0);
    lua_pushboolean(state, r.editing ? 1 : 0);
    return 3;
}
int number_input(lua_State* state) {
    auto id = string_arg(state, 1, 128), label = string_arg(state, 2, 256);
    const double value = number_arg(state, 3), minimum = number_arg(state, 4), maximum = number_arg(state, 5);
    luaL_argcheck(state, std::isfinite(minimum) && std::isfinite(maximum) && maximum > minimum, 5, "number_input maximum must exceed minimum");
    auto rect = rect_arg(state, 6);
    const bool enabled = lua_isnoneornil(state, 10) || lua_toboolean(state, 10);
    const double step = lua_isnoneornil(state, 11) ? 1.0 : number_arg(state, 11);
    const bool integer_mode = !lua_isnoneornil(state, 12) && lua_toboolean(state, 12);
    auto* ui = canvas(state);
    if (!ui) {
        lua_pushnumber(state, value);
        lua_pushboolean(state, 0);
        lua_pushboolean(state, 1);
        lua_pushnil(state);
        return 4;
    }
    ui::NumberInputResult r = ui->number_input(std::string(id), std::string(label), value, minimum, maximum, rect, enabled, step, integer_mode);
    lua_pushnumber(state, r.value);
    lua_pushboolean(state, r.committed ? 1 : 0);
    lua_pushboolean(state, r.valid ? 1 : 0);
    if (r.error.empty()) lua_pushnil(state);
    else lua_pushstring(state, r.error.c_str());
    return 4;
}

int anchor_rect(lua_State* state) {
    auto name = string_arg(state, 1, 32);
    ui::UiAnchor anchor;
    luaL_argcheck(state, ui::ScriptCanvas::anchor_from_name(name, anchor), 1,
                  "anchor must be topleft, topright, bottomleft, bottomright, topcenter, bottomcenter, leftcenter, rightcenter or center");
    const float w = finite_arg(state, 2), h = finite_arg(state, 3);
    const float margin = lua_isnoneornil(state, 4) ? 0.02f : finite_arg(state, 4);
    const float min_w = lua_isnoneornil(state, 5) ? 0.0f : finite_arg(state, 5);
    const float min_h = lua_isnoneornil(state, 6) ? 0.0f : finite_arg(state, 6);
    luaL_argcheck(state, w >= 0 && w <= 1 && h >= 0 && h <= 1, 2, "anchor size must be in 0..1");
    // Viewport-independent fallback when no host: assume 1280x720 for min size.
    ui::UiRect r;
    if (auto* ui = canvas(state)) {
        r = ui->anchor_rect(anchor, w, h, margin, min_w, min_h);
    } else {
        // Resolve without a canvas (tests / headless without UI host).
        ui::ScriptCanvas tmp;
        tmp.set_viewport(1280.0f, 720.0f);
        r = tmp.anchor_rect(anchor, w, h, margin, min_w, min_h);
    }
    lua_pushnumber(state, r.x);
    lua_pushnumber(state, r.y);
    lua_pushnumber(state, r.w);
    lua_pushnumber(state, r.h);
    return 4;
}
int cursor(lua_State* state) {
    if (auto* ui = canvas(state)) ui->cursor(lua_toboolean(state, 1) != 0);
    return 0;
}
}
void install_canvas_bindings(LuaVM& vm, ui::ScriptCanvas* target) {
    auto* state = vm.state();
    if (!state) return;
    lua_pushlightuserdata(state, target);
    lua_setfield(state, LUA_REGISTRYINDEX, "nf_script_canvas");
    lua_getglobal(state, "nf");
    lua_getfield(state, -1, "ui");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        lua_newtable(state);
    }
    struct Entry { const char* name; lua_CFunction function; };
    const Entry entries[] = {{"panel", panel}, {"label", label}, {"button", button}, {"bar", bar}, {"slider", slider}, {"checkbox", checkbox}, {"list", list}, {"tooltip", tooltip}, {"text_input", text_input}, {"number_input", number_input}, {"anchor_rect", anchor_rect}, {"cursor", cursor}};
    for (const auto& entry : entries) { lua_pushcfunction(state, entry.function); lua_setfield(state, -2, entry.name); }
    lua_setfield(state, -2, "ui");
    lua_pop(state, 1);
}
}
