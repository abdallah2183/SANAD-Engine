// NF/Scripting/UiBindings.cpp — nf.ui.* (Lua) + the C# UiHostApi table,
// both bound to one nf::ui::GameFlow (Game-Ready G3).
//
// Every Lua entry point and every C# table entry is a thin forwarder to a
// GameFlow method — there is deliberately no game logic here. A game in Lua,
// a game in C#, and the C++ sample therefore exercise the SAME state machine,
// which is what makes "drivable from Lua AND C#" a testable claim rather than
// a slogan.

#include <NF/Scripting/UiBindings.hpp>

#include <NF/Core/Logger.hpp>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
} // extern "C"

#include <string>

namespace nf::scripting {

namespace {

// --- Lua: bound flow -------------------------------------------------------------

ui::GameFlow* bound_flow(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "nf_ui_flow");
    auto* f = static_cast<ui::GameFlow*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return f;
}

std::string arg_string(lua_State* L, int index) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, index, &len);
    return (s != nullptr) ? std::string(s, len) : std::string();
}

int l_ui_action(lua_State* L) {
    ui::GameFlow* f = bound_flow(L);
    const std::string name = arg_string(L, 1);
    if (f == nullptr) {
        lua_pushstring(L, "unknown");
        return 1;
    }
    f->handle(ui::action_from_name(name.c_str()));
    lua_pushstring(L, ui::screen_name(f->screen()));
    return 1;
}

int l_ui_screen(lua_State* L) {
    ui::GameFlow* f = bound_flow(L);
    lua_pushstring(L, f != nullptr ? ui::screen_name(f->screen()) : "unknown");
    return 1;
}

int l_ui_set_health(lua_State* L) {
    ui::GameFlow* f = bound_flow(L);
    if (f == nullptr) return 0;
    const float current = static_cast<float>(luaL_checknumber(L, 1));
    const float max = static_cast<float>(luaL_checknumber(L, 2));
    f->hud().set_health(current, max);
    return 0;
}

int l_ui_health(lua_State* L) {
    ui::GameFlow* f = bound_flow(L);
    lua_pushnumber(L, f != nullptr ? static_cast<lua_Number>(f->hud().health_fraction())
                                   : static_cast<lua_Number>(0.0));
    return 1;
}

int l_ui_set_ammo(lua_State* L) {
    ui::GameFlow* f = bound_flow(L);
    if (f == nullptr) return 0;
    const int mag = static_cast<int>(luaL_checkinteger(L, 1));
    const int reserve = static_cast<int>(luaL_checkinteger(L, 2));
    f->hud().set_ammo(mag, reserve);
    return 0;
}

int l_ui_ammo(lua_State* L) {
    ui::GameFlow* f = bound_flow(L);
    lua_pushinteger(L, f != nullptr ? f->hud().ammo_magazine() : 0);
    lua_pushinteger(L, f != nullptr ? f->hud().ammo_reserve() : 0);
    return 2;
}

int l_ui_message(lua_State* L) {
    ui::GameFlow* f = bound_flow(L);
    if (f == nullptr) return 0;
    const std::string text = arg_string(L, 1);
    const float seconds = static_cast<float>(luaL_optnumber(L, 2, 3.0));
    f->hud().show_message(text, seconds);
    return 0;
}

int l_ui_game_over(lua_State* L) {
    if (ui::GameFlow* f = bound_flow(L)) f->notify_game_over();
    return 0;
}

int l_ui_update(lua_State* L) {
    ui::GameFlow* f = bound_flow(L);
    if (f == nullptr) return 0;
    f->hud().update(static_cast<float>(luaL_checknumber(L, 1)));
    return 0;
}

int l_ui_set_volume(lua_State* L) {
    ui::GameFlow* f = bound_flow(L);
    if (f == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const std::string bus = arg_string(L, 1);
    const float value = static_cast<float>(luaL_checknumber(L, 2));
    lua_pushboolean(L, f->settings().set_volume(bus.c_str(), value) ? 1 : 0);
    return 1;
}

int l_ui_volume(lua_State* L) {
    ui::GameFlow* f = bound_flow(L);
    const std::string bus = arg_string(L, 1);
    lua_pushnumber(L, f != nullptr
                           ? static_cast<lua_Number>(f->settings().volume(bus.c_str()))
                           : static_cast<lua_Number>(-1.0));
    return 1;
}

// --- Lua: dialogue presentation controls -----------------------------------------
// The tree/tags are game-owned (C++ or a future data loader); Lua drives the
// presentation the same way it drives a menu.

int l_ui_dialogue_active(lua_State* L) {
    ui::GameFlow* f = bound_flow(L);
    lua_pushboolean(L, (f != nullptr && f->dialogue().active()) ? 1 : 0);
    return 1;
}

int l_ui_dialogue_speaker(lua_State* L) {
    ui::GameFlow* f = bound_flow(L);
    lua_pushstring(L, f != nullptr ? f->dialogue().speaker().c_str() : "");
    return 1;
}

int l_ui_dialogue_text(lua_State* L) {
    ui::GameFlow* f = bound_flow(L);
    lua_pushstring(L, f != nullptr ? f->dialogue().text().c_str() : "");
    return 1;
}

int l_ui_dialogue_choices(lua_State* L) {
    ui::GameFlow* f = bound_flow(L);
    lua_pushinteger(L, f != nullptr ? static_cast<lua_Integer>(f->dialogue().choice_count()) : 0);
    return 1;
}

int l_ui_dialogue_selected(lua_State* L) {
    ui::GameFlow* f = bound_flow(L);
    lua_pushinteger(L, f != nullptr ? static_cast<lua_Integer>(f->dialogue().selected_choice()) : 0);
    return 1;
}

int l_ui_dialogue_move(lua_State* L) {
    ui::GameFlow* f = bound_flow(L);
    if (f == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const int delta = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, f->dialogue().move_selection(delta) ? 1 : 0);
    return 1;
}

int l_ui_dialogue_confirm(lua_State* L) {
    ui::GameFlow* f = bound_flow(L);
    lua_pushboolean(L, (f != nullptr && f->dialogue().confirm()) ? 1 : 0);
    return 1;
}

int l_ui_dialogue_close(lua_State* L) {
    if (ui::GameFlow* f = bound_flow(L)) f->dialogue().close();
    return 0;
}

// --- C#: the host API table ------------------------------------------------------
// Plain Cdecl functions (no captures) so their addresses are stable.

void cs_handle(void* user, int action) {
    auto* f = static_cast<ui::GameFlow*>(user);
    if (f == nullptr) return;
    if (action < 0 || action > 6) return; // outside the Action enum: ignore
    f->handle(static_cast<ui::Action>(action));
}

int cs_screen(void* user) {
    auto* f = static_cast<ui::GameFlow*>(user);
    return (f != nullptr) ? static_cast<int>(f->screen()) : -1;
}

void cs_set_health(void* user, float current, float max) {
    if (auto* f = static_cast<ui::GameFlow*>(user)) f->hud().set_health(current, max);
}

void cs_set_ammo(void* user, int magazine, int reserve) {
    if (auto* f = static_cast<ui::GameFlow*>(user)) f->hud().set_ammo(magazine, reserve);
}

void cs_show_message(void* user, const u8* text, int len, float seconds) {
    auto* f = static_cast<ui::GameFlow*>(user);
    if (f == nullptr || text == nullptr || len < 0) return;
    f->hud().show_message(std::string(reinterpret_cast<const char*>(text),
                                      static_cast<usize>(len)),
                          seconds);
}

void cs_notify_game_over(void* user) {
    if (auto* f = static_cast<ui::GameFlow*>(user)) f->notify_game_over();
}

void cs_update(void* user, float dt) {
    if (auto* f = static_cast<ui::GameFlow*>(user)) f->hud().update(dt);
}

int cs_set_volume(void* user, const u8* bus, int len, float value) {
    auto* f = static_cast<ui::GameFlow*>(user);
    if (f == nullptr || bus == nullptr || len < 0) return 0;
    const std::string name(reinterpret_cast<const char*>(bus), static_cast<usize>(len));
    return f->settings().set_volume(name.c_str(), value) ? 1 : 0;
}

float cs_volume(void* user, const u8* bus, int len) {
    auto* f = static_cast<ui::GameFlow*>(user);
    if (f == nullptr || bus == nullptr || len < 0) return -1.0f;
    const std::string name(reinterpret_cast<const char*>(bus), static_cast<usize>(len));
    return f->settings().volume(name.c_str());
}

} // namespace

void install_ui_bindings(LuaVM& vm, ui::GameFlow& flow) {
    lua_State* L = vm.state();
    if (!L) return;
    lua_pushlightuserdata(L, &flow);
    lua_setfield(L, LUA_REGISTRYINDEX, "nf_ui_flow");

    lua_getglobal(L, "nf");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        NF_LOG_ERROR(LogCategory::Script, "install_ui_bindings: nf table missing");
        return;
    }
    lua_newtable(L); // nf.ui
    struct Reg {
        const char* name;
        lua_CFunction fn;
    };
    const Reg regs[] = {
        {"action", l_ui_action},
        {"screen", l_ui_screen},
        {"set_health", l_ui_set_health},
        {"health", l_ui_health},
        {"set_ammo", l_ui_set_ammo},
        {"ammo", l_ui_ammo},
        {"message", l_ui_message},
        {"game_over", l_ui_game_over},
        {"update", l_ui_update},
        {"set_volume", l_ui_set_volume},
        {"volume", l_ui_volume},
        {"dialogue_active", l_ui_dialogue_active},
        {"dialogue_speaker", l_ui_dialogue_speaker},
        {"dialogue_text", l_ui_dialogue_text},
        {"dialogue_choices", l_ui_dialogue_choices},
        {"dialogue_selected", l_ui_dialogue_selected},
        {"dialogue_move", l_ui_dialogue_move},
        {"dialogue_confirm", l_ui_dialogue_confirm},
        {"dialogue_close", l_ui_dialogue_close},
    };
    for (const Reg& r : regs) {
        lua_pushcfunction(L, r.fn);
        lua_setfield(L, -2, r.name);
    }
    lua_setfield(L, -2, "ui"); // nf.ui = table
    lua_pop(L, 1);             // pop nf
}

UiHostApi make_ui_host_api(ui::GameFlow& flow) {
    UiHostApi api;
    api.user = &flow;
    api.handle = &cs_handle;
    api.screen = &cs_screen;
    api.set_health = &cs_set_health;
    api.set_ammo = &cs_set_ammo;
    api.show_message = &cs_show_message;
    api.notify_game_over = &cs_notify_game_over;
    api.update = &cs_update;
    api.set_volume = &cs_set_volume;
    api.volume = &cs_volume;
    return api;
}

} // namespace nf::scripting
