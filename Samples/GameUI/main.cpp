// Samples/GameUI/main.cpp — G3 acceptance sample: the shipped-game UI spine.
//
// A game developer ships a game by assembling, not by editing the engine. This
// sample is the whole assembly: one nf::ui::GameFlow gives the main menu, the
// HUD, the pause menu, the settings screen and the game-over screen, and the
// same object is driven — unchanged — from C++ and from Lua.
//
// It walks the acceptance path twice and checks the two walks agree:
//
//   1. C++ path    — the canonical game-state update.
//   2. Lua path    — a script calling nf.ui.* (what a modder/gameplay scripter
//                    writes). The C# side consumes the same GameFlow through
//                    the UiHostApi table; see nf::scripting::make_ui_host_api
//                    and Tests/ScriptTests/test_ui_scripting.cpp.
//
//    main -> play -> pause -> play -> game over -> restart
//
// Headless and deterministic: no window, no GPU, no RNG, no files. Exits 0
// when every check holds and 1 otherwise, so it doubles as a smoke test.
//
// Usage: NFSampleGameUI

#include <NF/Scripting/ScriptEngine.hpp>
#include <NF/Scripting/UiBindings.hpp>
#include <NF/UI/GameUI.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::scripting;
using namespace nf::ui;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_failures;
    std::printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
}

bool near(float a, float b, float eps = 1e-6f) { return std::fabs(a - b) <= eps; }

// The Lua a game designer would write: input in, screen name out.
const char* kLuaDriver = R"LUA(
function code()
  local s = nf.ui.screen()
  if s == "main_menu" then return 0 end
  if s == "settings" then return 1 end
  if s == "playing" then return 2 end
  if s == "paused" then return 3 end
  if s == "game_over" then return 4 end
  return -1
end
function start_game() nf.ui.action("confirm") return code() end
function pause()      nf.ui.action("back")    return code() end
function resume()     nf.ui.action("back")    return code() end
function die()        nf.ui.set_health(0, 100) nf.ui.game_over() return code() end
function restart()    nf.ui.action("confirm") return code() end
function hud_demo()
  nf.ui.set_health(40, 100)
  nf.ui.set_ammo(12, 48)
  local m, r = nf.ui.ammo()
  return nf.ui.health() * 1000 + m + r
end
function music(v) nf.ui.set_volume("music", v) return nf.ui.volume("music") end
)LUA";

double call0(LuaVM& vm, const char* fn) {
    std::vector<double> out;
    std::string err;
    if (!vm.call_numbers(fn, {}, out, 1, &err) || out.empty()) {
        std::printf("  lua error in %s: %s\n", fn, err.c_str());
        return -9999.0;
    }
    return out[0];
}

double call1(LuaVM& vm, const char* fn, double arg) {
    std::vector<double> out;
    std::string err;
    if (!vm.call_numbers(fn, {arg}, out, 1, &err) || out.empty()) {
        std::printf("  lua error in %s: %s\n", fn, err.c_str());
        return -9999.0;
    }
    return out[0];
}

void run_cpp_path() {
    std::printf("C++ path (the game's own state update):\n");
    GameFlow flow;
    int started = 0, resumed = 0, restarted = 0;
    flow.on_start_game = [&] { ++started; };
    flow.on_resume = [&] { ++resumed; };
    flow.on_restart = [&] { ++restarted; };

    check(flow.screen() == Screen::MainMenu, "boots on the main menu");

    flow.handle(Action::Confirm);
    check(flow.screen() == Screen::Playing, "confirm on the main menu starts play");
    check(started == 1, "on_start_game fired exactly once");

    // A frame of play: HUD state, a timed message, then the player dies.
    flow.hud().set_health(100, 100);
    flow.hud().set_ammo(30, 90);
    flow.hud().show_message("Find the gate", 2.0f);
    flow.hud().update(1.0f);
    check(near(flow.hud().health_fraction(), 1.0f), "HUD health reads full");
    check(flow.hud().ammo_magazine() == 30 && flow.hud().ammo_reserve() == 90,
          "HUD ammo reads 30 / 90");
    check(flow.hud().canvas().find_label("hud_message")->visible, "message line is up");
    flow.hud().update(2.0f);
    check(!flow.hud().canvas().find_label("hud_message")->visible,
          "message line hides after its timer");

    flow.handle(Action::Back);
    check(flow.screen() == Screen::Paused, "back pauses the run");
    flow.handle(Action::Back);
    check(flow.screen() == Screen::Playing && resumed == 1, "back resumes the run");

    flow.hud().set_health(0.0f, 100.0f);
    if (flow.hud().health_fraction() <= 0.0f) flow.notify_game_over();
    check(flow.screen() == Screen::GameOver, "zero health ends the run");

    flow.handle(Action::Confirm);
    check(flow.screen() == Screen::Playing && restarted == 1, "restart begins a new run");
    std::printf("  final screen: %s\n", screen_name(flow.screen()));
}

void run_lua_path() {
    std::printf("Lua path (nf.ui.* — the same GameFlow, no C++ from the designer):\n");
    GameFlow flow;
    LuaVM vm;
    if (!vm.valid()) {
        check(false, "Lua VM came up");
        return;
    }
    install_ui_bindings(vm, flow);
    std::string err;
    if (!vm.run_string(kLuaDriver, "game_ui_driver", &err)) {
        std::printf("  lua compile error: %s\n", err.c_str());
        check(false, "driver script compiles");
        return;
    }
    check(true, "driver script compiles");

    check(near(static_cast<float>(call0(vm, "start_game")), 2.0f), "confirm starts play");
    check(near(static_cast<float>(call0(vm, "pause")), 3.0f), "back pauses");
    check(near(static_cast<float>(call0(vm, "resume")), 2.0f), "back resumes");
    check(near(static_cast<float>(call0(vm, "die")), 4.0f), "zero health ends the run");
    check(near(static_cast<float>(call0(vm, "restart")), 2.0f), "restart begins a new run");

    // health() is a FRACTION: 40/100 -> 0.4, so 0.4*1000 + 12 mag + 48 reserve == 460.
    check(near(static_cast<float>(call0(vm, "hud_demo")), 460.0f),
          "HUD health/ammo set and read back from Lua");
    check(near(static_cast<float>(flow.hud().ammo_magazine()), 12.0f) &&
              near(static_cast<float>(flow.hud().ammo_reserve()), 48.0f),
          "C++ sees the HUD values Lua wrote");
    check(near(flow.hud().health_fraction(), 0.4f), "C++ sees the health Lua set");

    // The G5 contract, from the script side: volume data lives in the flow.
    check(near(static_cast<float>(call1(vm, "music", 0.25)), 0.25f), "music bus set to 0.25");
    check(near(flow.settings().music_volume, 0.25f), "C++ sees the music bus value");
    std::printf("  final screen: %s\n", screen_name(flow.screen()));
}

} // namespace

int main() {
    std::printf("NOVAForge — G3 runtime game UI sample\n");
    std::printf("flow: main -> play -> pause -> play -> game over -> restart\n\n");

    run_cpp_path();
    std::printf("\n");
    run_lua_path();

    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
