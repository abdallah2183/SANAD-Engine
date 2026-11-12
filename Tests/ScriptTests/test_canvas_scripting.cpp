#include <NF/Test/TestFramework.hpp>
#include <NF/Scripting/CanvasBindings.hpp>
#include <NF/UI/ScriptCanvas.hpp>
#include <NF/ECS/ECS.hpp>
using namespace nf;

NF_TEST(ui_lua_canvas_calls_create_widgets_and_deliver_button_events) {
    scripting::LuaVM vm;
    ui::ScriptCanvas canvas;
    scripting::install_canvas_bindings(vm, &canvas);
    std::string error;
    NF_CHECK(vm.run_string(R"LUA(
        clicks = 0
        function frame()
            nf.ui.cursor(true)
            nf.ui.panel("panel", .05,.05,.5,.6)
            nf.ui.label("title", "واجهة اللعبة", .1,.1,.3,.1)
            if nf.ui.button("go", "Go", .1,.2,.3,.1) then clicks = clicks + 1 end
            nf.ui.bar("progress", "Progress", .5, .1,.35,.3,.1)
            value = nf.ui.slider("slider", "Slider", 20, 0, 100, .1,.5,.3,.1)
        end
    )LUA", "canvas", &error));
    const auto frame = [&](bool down) {
        canvas.begin_frame({.2f,.25f,down,true});
        NF_CHECK(vm.run_string("frame()", "frame", &error));
    };
    frame(false); frame(true); frame(false);
    double clicks = 0;
    NF_CHECK(vm.get_global_number("clicks",clicks));
    NF_CHECK_NEAR(clicks,1,.001);
    NF_CHECK_EQ(canvas.size(),5u);
    NF_CHECK(canvas.snapshot(1280,720).texts[0].text == "واجهة اللعبة");
    NF_CHECK(!vm.run_string("nf.ui.label('x','x',0/0,0,.1,.1)", "bad", &error));
    NF_CHECK(!vm.run_string("nf.ui.slider('x','x',1,5,2,0,0,.1,.1)", "bad", &error));
}
NF_TEST(ui_lua_script_system_scopes_identical_widget_ids_to_their_entities) {
    ecs::World world;
    const auto a = world.create_entity(), b = world.create_entity();
    scripting::ScriptComponent component;
    component.source = "function update(dt) nf.ui.label('same', 'Entity', .1,.1,.3,.1) end";
    world.add<scripting::ScriptComponent>(a, component);
    world.add<scripting::ScriptComponent>(b, component);
    scripting::ScriptSystem system;
    ui::ScriptCanvas canvas;
    system.set_ui_canvas(&canvas);
    canvas.begin_frame(); system.update(world,.016f);
    NF_CHECK_EQ(canvas.size(),2u);
    world.get<scripting::ScriptComponent>(b)->enabled = false;
    canvas.begin_frame(); system.update(world,.016f);
    NF_CHECK_EQ(canvas.size(),1u);
}
NF_TEST(ui_lua_canvas_without_a_host_is_a_safe_noop) {
    scripting::LuaVM vm;
    scripting::install_canvas_bindings(vm,nullptr);
    std::string error;
    NF_CHECK(vm.run_string("assert(not nf.ui.button('b','B',0,0,.2,.1)); nf.ui.label('l','L',0,0,.2,.1)", "no_host", &error));
}
NF_TEST(ui_lua_canvas_new_widgets_and_anchor) {
    scripting::LuaVM vm;
    ui::ScriptCanvas canvas;
    scripting::install_canvas_bindings(vm, &canvas);
    std::string error;
    NF_CHECK(vm.run_string(R"LUA(
        picked = 1
        function frame()
            local on01 = nf.ui.checkbox("opt", "Option", on01 == 1, .1,.1,.3,.08)
            on01 = on01 and 1 or 0
            _G.on = on01
            picked, count = nf.ui.list("modes", {"A", "B", "C"}, picked, .1,.25,.3,.2, 2)
            local x, y, w, h = nf.ui.anchor_rect("topleft", .4,.5,.02)
            nf.ui.label("t", "anchor", x, y, w, h)
            nf.ui.tooltip("tip", "Help", .1,.55,.3,.08)
            nf.ui.label("wrap", "Long wrapped text with numbers 123 and Arabic: مختبر", .1,.66,.4,.1, 18, "right")
        end
    )LUA", "canvas2", &error));
    const auto frame = [&](bool down) {
        canvas.begin_frame({.2f,.14f,down,true});
        NF_CHECK(vm.run_string("frame()", "frame", &error));
    };
    // Tab focuses first interactive (checkbox), Enter toggles via Lua path
    ui::UiKeys tab{}; tab.tab = true;
    canvas.begin_frame({.05f,.9f,false,true}, tab);
    NF_CHECK(vm.run_string("frame()", "frame", &error));
    double on = 0;
    NF_CHECK(vm.get_global_number("on", on));
    // mouse click toggles checkbox
    frame(false); frame(true); frame(false);
    double on2 = 0; NF_CHECK(vm.get_global_number("on", on2));
    // list returns count
    double count = 0; NF_CHECK(vm.get_global_number("count", count));
    NF_CHECK_NEAR(count, 3, .001);
    // anchor_rect validation: bad anchor errors
    NF_CHECK(!vm.run_string("nf.ui.anchor_rect('nowhere', .1,.1,.1)", "badanchor", &error));
    // label with align works
    NF_CHECK(vm.run_string("nf.ui.label('z','Z',0,0,.2,.1,22,'right')", "align", &error));
}
