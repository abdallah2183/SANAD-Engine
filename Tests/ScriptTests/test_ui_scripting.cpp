// ScriptTests — the runtime game UI driven from script (Game-Ready G3).
//
// The point of this suite: prove that a game developer can drive
// main -> play -> pause -> game-over through the SAME nf::ui::GameFlow the
// C++ sample uses, from Lua (nf.ui.*) and from C# (the UiHostApi table),
// without touching engine source. Both paths are exercised against one
// GameFlow instance per case, so there is no second behaviour to trust.
//
// C# cases need the .NET runtime + the built managed sandbox and NF_SKIP
// otherwise (a missing runtime must never look like a passing suite).

#include <NF/Test/TestFramework.hpp>
#include <NF/Scripting/CSharpHost.hpp>
#include <NF/Scripting/ScriptEngine.hpp>
#include <NF/Scripting/UiBindings.hpp>
#include <NF/UI/GameUI.hpp>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace nf;
using namespace nf::scripting;
using namespace nf::ui;

namespace {

// Screen enum values as the scripts see them (mirrors ui::Screen).
constexpr double kMainMenu = 0.0;
constexpr double kSettings = 1.0;
constexpr double kPlaying = 2.0;
constexpr double kPaused = 3.0;
constexpr double kGameOver = 4.0;

// A Lua driver a game would ship: it maps nf.ui.screen() to a number so the
// host can assert the transition, and calls nf.ui.action() for input.
const char* kUiDriverScript = R"LUA(
function screen_code()
  local s = nf.ui.screen()
  if s == "main_menu" then return 0 end
  if s == "settings" then return 1 end
  if s == "playing" then return 2 end
  if s == "paused" then return 3 end
  if s == "game_over" then return 4 end
  return -1
end
function ui_confirm() nf.ui.action("confirm") return screen_code() end
function ui_back()    nf.ui.action("back")    return screen_code() end
function ui_down()    nf.ui.action("down")    return screen_code() end
function ui_up()      nf.ui.action("up")      return screen_code() end
function ui_left()    nf.ui.action("left")    return screen_code() end
function ui_right()   nf.ui.action("right")   return screen_code() end
function ui_end_run() nf.ui.game_over()       return screen_code() end
)LUA";

double call0(LuaVM& vm, const char* fn) {
    std::vector<double> out;
    std::string err;
    if (!vm.call_numbers(fn, {}, out, 1, &err) || out.empty()) return -12345.0;
    return out[0];
}

} // namespace

// ---------------------------------------------------------------------------
// Lua: nf.ui.* drives the flow
// ---------------------------------------------------------------------------

NF_TEST(ui_lua_bindings_install_and_report_screen) {
    GameFlow flow;
    LuaVM vm;
    NF_CHECK(vm.valid());
    install_ui_bindings(vm, flow);
    std::string err;
    NF_CHECK(vm.run_string(kUiDriverScript, "ui_driver", &err));
    NF_CHECK_NEAR(call0(vm, "screen_code"), kMainMenu, 1e-9);

    // main -> settings -> main, all from Lua.
    NF_CHECK_NEAR(call0(vm, "ui_down"), kMainMenu, 1e-9); // moves selection, same screen
    NF_CHECK_NEAR(call0(vm, "ui_confirm"), kSettings, 1e-9);
    NF_CHECK_NEAR(call0(vm, "ui_back"), kMainMenu, 1e-9);
}

NF_TEST(ui_lua_drives_main_play_pause_game_over) {
    GameFlow flow;
    LuaVM vm;
    install_ui_bindings(vm, flow);
    std::string err;
    NF_CHECK(vm.run_string(kUiDriverScript, "ui_driver", &err));

    int started = 0, resumed = 0, restarted = 0;
    flow.on_start_game = [&] { ++started; };
    flow.on_resume = [&] { ++resumed; };
    flow.on_restart = [&] { ++restarted; };

    // main -> play
    NF_CHECK_NEAR(call0(vm, "ui_confirm"), kPlaying, 1e-9);
    NF_CHECK(started == 1);
    // play -> pause -> play (Back resumes, like Esc)
    NF_CHECK_NEAR(call0(vm, "ui_back"), kPaused, 1e-9);
    NF_CHECK_NEAR(call0(vm, "ui_back"), kPlaying, 1e-9);
    NF_CHECK(resumed == 1);
    // play -> game over
    NF_CHECK_NEAR(call0(vm, "ui_end_run"), kGameOver, 1e-9);
    // game over -> restart
    NF_CHECK_NEAR(call0(vm, "ui_confirm"), kPlaying, 1e-9);
    NF_CHECK(restarted == 1);

    // The C++ side sees exactly the same object the Lua script moved.
    NF_CHECK(flow.screen() == Screen::Playing);
}

NF_TEST(ui_lua_hud_and_message_state) {
    GameFlow flow;
    LuaVM vm;
    install_ui_bindings(vm, flow);
    std::string err;
    NF_CHECK(vm.run_string(R"LUA(
function set_hp() nf.ui.set_health(25, 100) return nf.ui.health() end
function set_ammo() nf.ui.set_ammo(7, 21)
  local m, r = nf.ui.ammo()
  return m * 1000 + r
end
function say() nf.ui.message("reload!", 1.0) return 0 end
function tick(dt) nf.ui.update(dt) return 0 end
)LUA",
                           "ui_hud", &err));

    NF_CHECK_NEAR(call0(vm, "set_hp"), 0.25, 1e-6);
    NF_CHECK_NEAR(flow.hud().health_fraction(), 0.25f, 1e-6f);
    NF_CHECK_NEAR(call0(vm, "set_ammo"), 7021.0, 1e-9);
    NF_CHECK(flow.hud().ammo_magazine() == 7 && flow.hud().ammo_reserve() == 21);

    NF_CHECK_NEAR(call0(vm, "say"), 0.0, 1e-9);
    NF_CHECK(flow.hud().message() == "reload!");
    NF_CHECK(flow.hud().canvas().find_label("hud_message")->visible);
    {
        std::vector<double> out;
        NF_CHECK(vm.call_numbers("tick", {2.0}, out, 1, &err));
    }
    NF_CHECK(!flow.hud().canvas().find_label("hud_message")->visible);
}

NF_TEST(ui_lua_settings_volume_contract) {
    GameFlow flow;
    LuaVM vm;
    install_ui_bindings(vm, flow);
    std::string err;
    NF_CHECK(vm.run_string(R"LUA(
function set_music(v) return nf.ui.set_volume("music", v) and 1 or 0 end
function get_music() return nf.ui.volume("music") end
function set_unknown(v) return nf.ui.set_volume("nope", v) and 1 or 0 end
function get_unknown() return nf.ui.volume("nope") end
function overdrive() nf.ui.set_volume("sfx", 9.0) return nf.ui.volume("sfx") end
)LUA",
                           "ui_vol", &err));

    {
        std::vector<double> out;
        NF_CHECK(vm.call_numbers("set_music", {0.25}, out, 1, &err));
        NF_CHECK_NEAR(out[0], 1.0, 1e-9);
    }
    NF_CHECK_NEAR(call0(vm, "get_music"), 0.25, 1e-6);
    NF_CHECK_NEAR(flow.settings().music_volume, 0.25f, 1e-6f);
    {
        std::vector<double> out;
        NF_CHECK(vm.call_numbers("set_unknown", {0.5}, out, 1, &err));
        NF_CHECK_NEAR(out[0], 0.0, 1e-9); // unknown bus rejected
    }
    NF_CHECK_NEAR(call0(vm, "get_unknown"), -1.0, 1e-6); // unknown bus sentinel
    NF_CHECK_NEAR(call0(vm, "overdrive"), 1.0, 1e-6);    // clamped to [0, 1]
}

NF_TEST(ui_lua_dialogue_presentation_controls) {
    GameFlow flow;
    LuaVM vm;
    install_ui_bindings(vm, flow);
    std::string err;
    NF_CHECK(vm.run_string(R"LUA(
function dlg_active() return nf.ui.dialogue_active() and 1 or 0 end
function dlg_text() return #nf.ui.dialogue_text() end
function dlg_speaker_len() return #nf.ui.dialogue_speaker() end
function dlg_choices() return nf.ui.dialogue_choices() end
function dlg_down() nf.ui.dialogue_move(1) return nf.ui.dialogue_selected() end
function dlg_confirm() return nf.ui.dialogue_confirm() and 1 or 0 end
function dlg_close() nf.ui.dialogue_close() return nf.ui.dialogue_active() and 1 or 0 end
)LUA",
                           "ui_dlg", &err));

    // The game (C++) owns the tree + tags and starts the conversation; the
    // Lua script presents and advances it — the documented split.
    gameplay::DialogueTree tree;
    tree.add_node({"hub", "Guide", "What now?", "gate",
                   {{"go", "Go north", "gate", "", ""}}});
    tree.add_node({"gate", "Guide", "The gate is north.", "", {}});
    gameplay::TagContainer tags;
    NF_CHECK(flow.dialogue().start(tree, "hub", tags));

    NF_CHECK_NEAR(call0(vm, "dlg_active"), 1.0, 1e-9);
    NF_CHECK_NEAR(call0(vm, "dlg_speaker_len"), 5.0, 1e-9);  // "Guide"
    NF_CHECK_NEAR(call0(vm, "dlg_text"), 9.0, 1e-9);         // "What now?"
    NF_CHECK_NEAR(call0(vm, "dlg_choices"), 1.0, 1e-9);
    NF_CHECK_NEAR(call0(vm, "dlg_down"), 0.0, 1e-9);         // one choice wraps to 0
    NF_CHECK_NEAR(call0(vm, "dlg_confirm"), 1.0, 1e-9);      // jumps to gate, still open
    NF_CHECK(flow.dialogue().text() == "The gate is north.");
    NF_CHECK_NEAR(call0(vm, "dlg_choices"), 0.0, 1e-9);
    NF_CHECK_NEAR(call0(vm, "dlg_confirm"), 0.0, 1e-9);      // no next: ends
    NF_CHECK(!flow.dialogue().active());
    NF_CHECK_NEAR(call0(vm, "dlg_close"), 0.0, 1e-9);        // idempotent
}

NF_TEST(ui_lua_unknown_action_is_inert) {
    GameFlow flow;
    LuaVM vm;
    install_ui_bindings(vm, flow);
    std::string err;
    NF_CHECK(vm.run_string(R"LUA(
function bad() nf.ui.action("teleport") return screen_code() end
function screen_code()
  local s = nf.ui.screen()
  if s == "main_menu" then return 0 end
  if s == "settings" then return 1 end
  if s == "playing" then return 2 end
  if s == "paused" then return 3 end
  if s == "game_over" then return 4 end
  return -1
end
)LUA",
                           "ui_bad", &err));
    // An unknown action name maps to Action::None: the screen does not move
    // and the call still answers a screen name.
    NF_CHECK_NEAR(call0(vm, "bad"), kMainMenu, 1e-9);
    NF_CHECK(flow.screen() == Screen::MainMenu);
}

// ---------------------------------------------------------------------------
// C#: the UiHostApi table — the native half of the C# contract
// ---------------------------------------------------------------------------
//
// UiHostApi (Engine/Scripting/include/NF/Scripting/CSharpMarshal.hpp) is the
// ABI a managed game script calls: every entry is a Cdecl pointer, and the
// managed mirror (Engine/Scripting/CSharp/NFGameScript/NFUiDriver.cs) invokes
// them with exactly this argument list. The cases below drive the WHOLE flow
// through the table, so the contract the managed side consumes is verified
// natively on every run — no .NET required.
//
// The managed -> native hop itself (a C# method resolving this table and
// calling back) additionally needs the managed sandbox assembly rebuilt with
// NFUiDriver.cs; the NF_SKIP-guarded case at the bottom explains why that
// cannot happen on this machine today.

// Cdecl entry-point signatures (see UiBindings.hpp / NFUiDriver.cs).
namespace {

using CSDriveActionsFn = int (*)(void* api, int* actions, int count);
using CSScreenFn = int (*)(void* api);
using CSGameOverFn = int (*)(void* api);
using CSSetHealthFn = int (*)(void* api, float current, float max);
using CSSetVolumeFn = int (*)(void* api, const u8* bus, int len, float value);
using CSVolumeFn = float (*)(void* api, const u8* bus, int len);

} // namespace

NF_TEST(ui_csharp_abi_layout_is_sequential_and_user_first) {
    // The managed mirror is LayoutKind.Sequential, so field order and the
    // zero offset of `user` are the whole contract.
    NF_CHECK(std::is_standard_layout_v<UiHostApi>);
    NF_CHECK(offsetof(UiHostApi, user) == 0);
    NF_CHECK(sizeof(UiHostApi) == sizeof(void*) + 9 * sizeof(void*));

    GameFlow flow;
    UiHostApi api = make_ui_host_api(flow);
    NF_CHECK(api.user == &flow);
    NF_CHECK(api.handle != nullptr && api.screen != nullptr && api.set_health != nullptr);
    NF_CHECK(api.set_ammo != nullptr && api.show_message != nullptr);
    NF_CHECK(api.notify_game_over != nullptr && api.update != nullptr);
    NF_CHECK(api.set_volume != nullptr && api.volume != nullptr);

    UiHostApi blank;
    NF_CHECK(blank.user == nullptr && blank.handle == nullptr && blank.screen == nullptr);
}

NF_TEST(ui_csharp_abi_drives_main_play_pause_game_over) {
    GameFlow flow;
    int started = 0, resumed = 0, restarted = 0;
    flow.on_start_game = [&] { ++started; };
    flow.on_resume = [&] { ++resumed; };
    flow.on_restart = [&] { ++restarted; };
    UiHostApi api = make_ui_host_api(flow);

    auto drive = [&](int* actions, int count) {
        // Exactly what NFUiDriver.DriveActions does: loop the array through
        // the table's handle entry, then report the screen.
        for (int i = 0; i < count; ++i) api.handle(api.user, actions[i]);
        return api.screen(api.user);
    };

    NF_CHECK(api.screen(api.user) == static_cast<int>(Screen::MainMenu));

    int start_seq[] = {static_cast<int>(Action::Confirm)};
    NF_CHECK(drive(start_seq, 1) == static_cast<int>(Screen::Playing));
    NF_CHECK(started == 1);

    int pause_seq[] = {static_cast<int>(Action::Back)};
    NF_CHECK(drive(pause_seq, 1) == static_cast<int>(Screen::Paused));
    NF_CHECK(drive(pause_seq, 1) == static_cast<int>(Screen::Playing));
    NF_CHECK(resumed == 1);

    api.notify_game_over(api.user);
    NF_CHECK(api.screen(api.user) == static_cast<int>(Screen::GameOver));

    NF_CHECK(drive(start_seq, 1) == static_cast<int>(Screen::Playing));
    NF_CHECK(restarted == 1);

    // A multi-step sequence in one call, as the managed driver sends it.
    int end_seq[] = {static_cast<int>(Action::Back), static_cast<int>(Action::Back)};
    NF_CHECK(drive(end_seq, 2) == static_cast<int>(Screen::Playing));
    NF_CHECK(flow.screen() == Screen::Playing);

    // An out-of-range action value is ignored, never an out-of-bounds switch.
    int bogus[] = {99, -5};
    NF_CHECK(drive(bogus, 2) == static_cast<int>(Screen::Playing));
    NF_CHECK(flow.screen() == Screen::Playing);
}

NF_TEST(ui_csharp_abi_hud_and_volume_contract) {
    GameFlow flow;
    UiHostApi api = make_ui_host_api(flow);

    api.set_health(api.user, 25.0f, 100.0f);
    NF_CHECK_NEAR(flow.hud().health_fraction(), 0.25f, 1e-6f);

    api.set_ammo(api.user, 9, 27);
    NF_CHECK(flow.hud().ammo_magazine() == 9 && flow.hud().ammo_reserve() == 27);

    const std::string msg = "brace for impact";
    api.show_message(api.user, reinterpret_cast<const u8*>(msg.data()),
                     static_cast<int>(msg.size()), 1.0f);
    NF_CHECK(flow.hud().message() == msg);
    api.update(api.user, 2.0f);
    NF_CHECK(!flow.hud().canvas().find_label("hud_message")->visible);

    const std::string music = "music";
    NF_CHECK(api.set_volume(api.user, reinterpret_cast<const u8*>(music.data()),
                            static_cast<int>(music.size()), 0.25f) == 1);
    NF_CHECK_NEAR(flow.settings().music_volume, 0.25f, 1e-6f);
    NF_CHECK_NEAR(api.volume(api.user, reinterpret_cast<const u8*>(music.data()),
                             static_cast<int>(music.size())),
                  0.25f, 1e-6f);

    const std::string bogus = "nope";
    NF_CHECK(api.set_volume(api.user, reinterpret_cast<const u8*>(bogus.data()),
                            static_cast<int>(bogus.size()), 0.5f) == 0);
    NF_CHECK_NEAR(api.volume(api.user, reinterpret_cast<const u8*>(bogus.data()),
                             static_cast<int>(bogus.size())),
                  -1.0f, 1e-6f);

    // Nulls and negative lengths are refused by every entry, never dereferenced.
    NF_CHECK(api.set_volume(api.user, nullptr, 5, 0.5f) == 0);
    NF_CHECK(api.set_volume(api.user, reinterpret_cast<const u8*>(music.data()), -1, 0.5f) == 0);
    NF_CHECK_NEAR(api.volume(api.user, nullptr, 5), -1.0f, 1e-6f);
    api.set_health(nullptr, 1.0f, 1.0f);       // no-op, no crash
    api.handle(nullptr, static_cast<int>(Action::Confirm));
    NF_CHECK(api.screen(nullptr) == -1);
    api.set_ammo(nullptr, 1, 1);
    api.show_message(nullptr, reinterpret_cast<const u8*>(msg.data()), 5, 1.0f);
    api.notify_game_over(nullptr);
    api.update(nullptr, 1.0f);
    NF_CHECK_NEAR(api.volume(nullptr, reinterpret_cast<const u8*>(music.data()), 5), -1.0f, 1e-6f);
}

#if NF_CSHARP_HOSTING

namespace {

bool csharp_ready(std::string& asm_dir, std::string& why) {
    if (!CSharpHost::runtime_available()) {
        why = "no .NET runtime on this machine";
        return false;
    }
#ifdef NF_CSHARP_ASM_DIR
    asm_dir = NF_CSHARP_ASM_DIR;
#else
    why = "managed sandbox dir unknown";
    return false;
#endif
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(fs::path(asm_dir) / "NFGameScript.dll", ec) || ec) {
        why = "managed sandbox DLL missing in " + asm_dir;
        return false;
    }
    if (!fs::exists(fs::path(asm_dir) / "NFGameScript.runtimeconfig.json", ec) || ec) {
        why = "managed runtimeconfig missing in " + asm_dir;
        return false;
    }
    return true;
}

// The G3 managed driver lives in its own translation unit (NFUiDriver.cs).
const char* driver_type() { return "NF.GameScript.NFUiDriver, NFGameScript"; }

// Resolves one managed method, or nullptr (error text is discarded: the case
// below decides between NF_SKIP and an assertion on nullptr).
void* method(CSharpHost& host, const std::string& asm_path, const char* name) {
    std::string err;
    return host.get_method(asm_path, driver_type(), name, &err);
}

} // namespace


NF_TEST(ui_csharp_managed_driver_drives_the_flow) {
    std::string asm_dir, why;
    if (!csharp_ready(asm_dir, why)) NF_SKIP(why);

    CSharpHost host;
    std::string err;
    NF_CHECK(host.initialize(asm_dir + "/NFGameScript.runtimeconfig.json", &err));
    const std::string asm_path = asm_dir + "/NFGameScript.dll";

    // NFUiDriver.cs ships the managed driver, but the sandbox assembly in this
    // tree was last built before it existed, and this machine cannot run
    // `dotnet build` (NuGet resolves a machine-wide config directory through
    // the registry, which the agent sandbox blocks; see the G3 report). NF_SKIP
    // — never a false pass — until the assembly is rebuilt with a working SDK.
    void* drive_raw = method(host, asm_path, "DriveActions");
    if (drive_raw == nullptr) {
        NF_SKIP("managed sandbox has no NFUiDriver (stale NFGameScript.dll; "
                "dotnet build is unavailable in this environment)");
    }
    void* screen_raw = method(host, asm_path, "UiScreen");
    void* over_raw = method(host, asm_path, "UiGameOver");
    void* hp_raw = method(host, asm_path, "UiSetHealth");
    void* sv_raw = method(host, asm_path, "UiSetVolume");
    void* gv_raw = method(host, asm_path, "UiVolume");
    NF_CHECK(screen_raw != nullptr && over_raw != nullptr && hp_raw != nullptr);
    NF_CHECK(sv_raw != nullptr && gv_raw != nullptr);
    auto drive = reinterpret_cast<CSDriveActionsFn>(drive_raw);
    auto screen = reinterpret_cast<CSScreenFn>(screen_raw);
    auto game_over = reinterpret_cast<CSGameOverFn>(over_raw);
    auto set_health = reinterpret_cast<CSSetHealthFn>(hp_raw);
    auto set_volume = reinterpret_cast<CSSetVolumeFn>(sv_raw);
    auto volume = reinterpret_cast<CSVolumeFn>(gv_raw);

    GameFlow flow;
    int started = 0, resumed = 0, restarted = 0;
    flow.on_start_game = [&] { ++started; };
    flow.on_resume = [&] { ++resumed; };
    flow.on_restart = [&] { ++restarted; };
    UiHostApi api = make_ui_host_api(flow);

    NF_CHECK(screen(&api) == static_cast<int>(Screen::MainMenu));

    // The managed method loops over the action array and calls back into
    // GameFlow::handle for each entry.
    int start_seq[] = {static_cast<int>(Action::Confirm)};
    NF_CHECK(drive(&api, start_seq, 1) == static_cast<int>(Screen::Playing));
    NF_CHECK(started == 1);

    int pause_seq[] = {static_cast<int>(Action::Back)};
    NF_CHECK(drive(&api, pause_seq, 1) == static_cast<int>(Screen::Paused));
    NF_CHECK(drive(&api, pause_seq, 1) == static_cast<int>(Screen::Playing));
    NF_CHECK(resumed == 1);

    NF_CHECK(game_over(&api) == static_cast<int>(Screen::GameOver));
    NF_CHECK(drive(&api, start_seq, 1) == static_cast<int>(Screen::Playing));
    NF_CHECK(restarted == 1);

    NF_CHECK(set_health(&api, 25.0f, 100.0f) == 1);
    NF_CHECK_NEAR(flow.hud().health_fraction(), 0.25f, 1e-6f);
    const std::string music = "music";
    NF_CHECK(set_volume(&api, reinterpret_cast<const u8*>(music.data()),
                        static_cast<int>(music.size()), 0.25f) == 1);
    NF_CHECK_NEAR(volume(&api, reinterpret_cast<const u8*>(music.data()),
                         static_cast<int>(music.size())),
                  0.25f, 1e-6f);

    // Hostile inputs are sentinels, never a crash.
    NF_CHECK(screen(nullptr) == -1);
    NF_CHECK(drive(nullptr, start_seq, 1) == -1);
    NF_CHECK(drive(&api, nullptr, 3) == -1);
    NF_CHECK(drive(&api, start_seq, -1) == -1);
    NF_CHECK(set_health(nullptr, 1.0f, 1.0f) == 0);
    NF_CHECK(set_volume(&api, nullptr, 5, 0.5f) == 0);
    NF_CHECK_NEAR(volume(&api, nullptr, 5), -1.0f, 1e-6f);
}

#else // !NF_CSHARP_HOSTING

NF_TEST(ui_csharp_ui_host_api_reports_unconfigured) {
    NF_CHECK(!CSharpHost::runtime_available());
    // The table itself is pure C++ and still builds/behaves without .NET.
    GameFlow flow;
    UiHostApi api = make_ui_host_api(flow);
    NF_CHECK(api.user == &flow);
    NF_CHECK(api.handle != nullptr && api.screen != nullptr && api.volume != nullptr);
    NF_CHECK(api.screen(api.user) == static_cast<int>(Screen::MainMenu));
}

#endif // NF_CSHARP_HOSTING
