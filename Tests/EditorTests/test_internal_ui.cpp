#include <NF/Test/TestFramework.hpp>
#include <NF/Editor/InputBindingCapture.hpp>
#include <NF/Editor/ToolbarUi.hpp>
#include <NF/Editor/UiEditCache.hpp>
#include <NF/Editor/UiPathText.hpp>
#include <NF/Scene/Scene.hpp>
#include <imgui.h>
#include <imgui_internal.h>

using namespace nf;

NF_TEST(editor_internal_export_extension_changes_only_the_filename) {
    using editor::export_path_for_extension;
    NF_CHECK(export_path_for_extension("C:/folder.v2/model.obj", "glb") == "C:/folder.v2/model.glb");
    NF_CHECK(export_path_for_extension("C:\\folder.v2\\model", "fbx") == "C:\\folder.v2\\model.fbx");
    NF_CHECK(export_path_for_extension(".model", "glb") == ".model.glb");
    NF_CHECK(export_path_for_extension("model.part.obj", "gltf") == "model.part.gltf");
    NF_CHECK(export_path_for_extension("model.glb", "glb") == "model.glb");
}

NF_TEST(editor_internal_utf8_shortening_preserves_complete_characters) {
    std::string name = "A\xd9\x85\xf0\x9f\x8e\xae";
    editor::remove_last_utf8_character(name);
    NF_CHECK(name == "A\xd9\x85");
    editor::remove_last_utf8_character(name);
    NF_CHECK(name == "A");
    editor::remove_last_utf8_character(name);
    editor::remove_last_utf8_character(name);
    NF_CHECK(name.empty());
}

NF_TEST(editor_internal_drafts_refresh_after_undo_scene_reload_and_entity_reuse) {
    scene::Scene scene("Draft");
    editor::UiEditCacheKey key;
    const ecs::Entity entity{7, 1};
    NF_CHECK(key.refresh(&scene.world(), entity, 1, 2));
    NF_CHECK(!key.refresh(&scene.world(), entity, 1, 2));
    NF_CHECK(key.same_scene(&scene.world(), 1));
    NF_CHECK(!key.same_scene(&scene.world(), 2));
    NF_CHECK(key.refresh(&scene.world(), entity, 1, 3));
    NF_CHECK(key.refresh(&scene.world(), entity, 2, 3));
    NF_CHECK(key.refresh(&scene.world(), ecs::Entity{7, 2}, 2, 3));
    scene::Scene another("Another");
    NF_CHECK(key.refresh(&another.world(), ecs::Entity{7, 2}, 2, 3));
}

NF_TEST(editor_internal_camera_panel_prefers_the_active_camera) {
    scene::Scene scene("Cameras");
    auto& world = scene.world();
    NF_CHECK(!editor::scene_camera_entity(world).valid());
    auto inactive = world.create_entity();
    runtime::CameraComponent first;
    first.is_active = false;
    world.add<runtime::CameraComponent>(inactive, first);
    auto active = world.create_entity();
    runtime::CameraComponent second;
    second.is_active = true;
    world.add<runtime::CameraComponent>(active, second);
    NF_CHECK(editor::scene_camera_entity(world) == active);
    world.get<runtime::CameraComponent>(active)->is_active = false;
    NF_CHECK(editor::scene_camera_entity(world) == inactive);
}

namespace {
struct CaptureFixture {
    editor::InputBindingCapture capture;
    runtime::InputBindings bindings = runtime::InputBindings::defaults();
    CaptureFixture() {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(900, 700);
        io.DeltaTime = 1.0f / 60;
        io.Fonts->AddFontDefault();
        io.Fonts->Build();
    }
    ~CaptureFixture() { ImGui::DestroyContext(); }
    void frame(bool begin = false) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(800, 600));
        ImGui::Begin("CaptureTest");
        if (begin) capture.begin("jump");
        editor::input_binding_capture_popup(capture, bindings);
        ImGui::End();
        ImGui::Render();
    }
};
}

NF_TEST(editor_internal_capture_waits_for_a_new_input_and_binds_keyboard) {
    CaptureFixture f;
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    f.frame(true);
    NF_CHECK(!f.capture.action.empty()); // opening Enter is not a new binding
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    f.frame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_K, true);
    f.frame();
    NF_CHECK(f.capture.action.empty());
    const auto* jump = f.bindings.find("jump");
    NF_CHECK(jump != nullptr && jump->size() == 1);
    NF_CHECK(jump->front().source == runtime::BindingSource::Key);
    NF_CHECK_EQ(jump->front().code, static_cast<u16>('K'));
}

NF_TEST(editor_internal_capture_escape_preserves_the_existing_binding) {
    CaptureFixture f;
    const auto original = *f.bindings.find("jump");
    f.frame(true);
    f.frame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    f.frame();
    NF_CHECK(f.capture.action.empty());
    NF_CHECK_EQ(f.bindings.find("jump")->size(), original.size());
    NF_CHECK_EQ(f.bindings.find("jump")->front().code, original.front().code);
}

NF_TEST(editor_internal_capture_ignores_clicks_outside_the_capture_area) {
    CaptureFixture f;
    f.frame(true);
    f.frame();
    ImGui::GetIO().AddMousePosEvent(5, 5);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    f.frame();
    NF_CHECK(f.capture.action == "jump");
}

NF_TEST(editor_internal_capture_cancel_button_never_binds_mouse_left) {
    CaptureFixture f;
    const auto original = *f.bindings.find("jump");
    f.frame(true);
    f.frame();
    auto* popup = ImGui::FindWindowByName("Rebind###NFBindingCapture");
    NF_CHECK(popup != nullptr);
    ImGui::ActivateItemByID(popup->GetID("###capture_cancel"));
    f.frame();
    NF_CHECK(f.capture.action.empty());
    NF_CHECK_EQ(f.bindings.find("jump")->front().code, original.front().code);
}
