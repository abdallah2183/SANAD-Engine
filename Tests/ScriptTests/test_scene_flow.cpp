#include <NF/Test/TestFramework.hpp>
#include <NF/Scripting/SceneFlow.hpp>
#include <NF/Scripting/ScriptEngine.hpp>
#include <limits>

using namespace nf::scripting;

NF_TEST(scene_flow_validates_paths_and_first_request_wins) {
    SceneFlow flow;
    std::string error;
    for (const auto* path : {"../outside.nfscene", "project://Scenes/A.nfscene", "content://Scenes/../A.nfscene",
                             "content://Scenes//A.nfscene", "content://Scenes/A\\B.nfscene", "content://Scenes/A.lua"}) {
        NF_CHECK(!flow.request_scene(path, error));
        NF_CHECK(!error.empty());
        NF_CHECK(!flow.pending());
    }
    NF_CHECK(flow.request_scene("content://Scenes/A.nfscene", error));
    NF_CHECK(!flow.request_scene("content://Scenes/B.nfscene", error));
    NF_CHECK(!flow.request_quit(error));
    NF_CHECK(flow.pending_path() == "content://Scenes/A.nfscene");
    flow.complete(false, "missing");
    NF_CHECK(flow.status() == "failed");
    NF_CHECK(flow.error() == "missing");
    NF_CHECK(flow.request_scene("content://Scenes/B.nfscene", error));
}

NF_TEST(scene_flow_session_is_typed_bounded_and_preserved_on_adopt) {
    SceneFlow flow;
    std::string error;
    NF_CHECK(flow.set("score", 42.0, error));
    NF_CHECK(flow.set("won", true, error));
    NF_CHECK(flow.set("name", std::string("Player"), error));
    flow.scene_adopted("content://Scenes/B.nfscene");
    NF_CHECK(std::get<double>(*flow.get("score")) == 42.0);
    NF_CHECK(std::get<bool>(*flow.get("won")));
    NF_CHECK(!flow.set("bad/key", 0.0, error));
    NF_CHECK(!flow.set("score", std::numeric_limits<double>::infinity(), error));
    NF_CHECK(std::get<double>(*flow.get("score")) == 42.0);
    NF_CHECK(!flow.set("oversize", std::string(4097, 'x'), error));
    for (int i = 0; i < 253; ++i) NF_CHECK(flow.set("k" + std::to_string(i), false, error));
    NF_CHECK(!flow.set("too_many", false, error));
    NF_CHECK(flow.set("score", 43.0, error)); // replacements still work at the cap
    flow.reset_session();
    NF_CHECK(flow.size() == 0);
    NF_CHECK(!flow.pending());
    NF_CHECK(!flow.quit_requested());
    NF_CHECK(flow.current() == "content://Scenes/B.nfscene");
    for (int i = 0; i < 15; ++i) NF_CHECK(flow.set("s" + std::to_string(i), std::string(4096, 'x'), error));
    NF_CHECK(!flow.set("over_bytes", std::string(4096, 'x'), error));
}

NF_TEST(scene_flow_lua_binding_round_trip_and_no_host) {
    LuaVM vm;
    SceneFlow flow;
    flow.scene_adopted("content://Scenes/Main.nfscene");
    install_game_flow_bindings(vm, &flow);
    std::string error;
    NF_CHECK(vm.run_string(R"lua(
        assert(nf.session.set('score', 12.5))
        assert(nf.session.set('won', true))
        assert(nf.session.set('name', 'عبدالله'))
        assert(nf.session.get('score') == 12.5)
        assert(nf.session.get('won') == true)
        assert(type(nf.session.get('name')) == 'string')
        assert(nf.session.get('missing', 9) == 9)
        assert(nf.session.set('score', nil))
        assert(nf.session.get('score') == nil)
        assert(not nf.session.set('n', 0/0))
        assert(not pcall(nf.session.set, 'table', {}))
        assert(not pcall(nf.session.set, 3, 'value'))
        assert(nf.scene.current() == 'content://Scenes/Main.nfscene')
        assert(nf.scene.restart())
        assert(not nf.scene.load('content://Scenes/Other.nfscene'))
        local status, target, err = nf.scene.status()
        assert(status == 'queued' and target == nf.scene.current() and err == '')
        nf.session.clear()
        assert(nf.session.get('name') == nil)
    )lua", "scene-flow", &error));
    install_game_flow_bindings(vm, nullptr);
    NF_CHECK(vm.run_string(R"lua(
        assert(not nf.scene.load('content://Scenes/A.nfscene'))
        assert(not nf.scene.quit())
        assert(not nf.session.set('score', 1))
        assert(nf.session.get('missing', 7) == 7)
        assert(nf.scene.current() == nil)
    )lua", "no-host", &error));
}
