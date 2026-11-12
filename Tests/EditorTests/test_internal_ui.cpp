#include <NF/Test/TestFramework.hpp>
#include <NF/Editor/InputBindingCapture.hpp>
#include <NF/Editor/ToolbarUi.hpp>
#include <NF/Editor/UiEditCache.hpp>
#include <NF/Editor/UiPathText.hpp>
#include <NF/Editor/UiPanelControls.hpp>
#include <NF/Editor/OutlinerBadges.hpp>
#include <NF/Editor/OutlinerRowUi.hpp>
#include <NF/Editor/ComponentPicker.hpp>
#include <NF/Editor/AddObjectIcon.hpp>
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

NF_TEST(editor_add_object_icons_all_draw_within_their_preview) {
    CaptureFixture f;
    ImGui::NewFrame();
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    constexpr float center = 300.0f;
    constexpr float size = 68.0f;
    for (const auto& entry : editor::kAddObjects) {
        const int first_vertex = draw->VtxBuffer.Size;
        editor::draw_add_object_icon(*draw, entry.kind, ImVec2(center, center), size);
        NF_CHECK(draw->VtxBuffer.Size > first_vertex);
        for (int i = first_vertex; i < draw->VtxBuffer.Size; ++i) {
            const ImVec2 pos = draw->VtxBuffer[i].pos;
            NF_CHECK(std::isfinite(pos.x) && std::isfinite(pos.y));
            NF_CHECK(std::abs(pos.x - center) <= size * 0.55f);
            NF_CHECK(std::abs(pos.y - center) <= size * 0.55f);
        }
    }
    ImGui::Render();
}

NF_TEST(editor_add_object_card_selects_on_click_and_creates_on_double_click) {
    CaptureFixture f;
    int selected = 1;
    bool create_requested = false;
    ImVec2 sphere_point;
    const auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(400, 220));
        ImGui::Begin("Object cards", nullptr, ImGuiWindowFlags_NoTitleBar);
        editor::draw_add_object_card(editor::kAddObjects[1], 1, 150, selected, create_requested, false, false);
        ImGui::SameLine();
        editor::draw_add_object_card(editor::kAddObjects[2], 2, 150, selected, create_requested, false, false);
        const ImVec2 min = ImGui::GetItemRectMin();
        sphere_point = ImVec2(min.x + 40, min.y + 40);
        ImGui::End();
        ImGui::Render();
    };
    frame();
    frame();
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(sphere_point.x, sphere_point.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
    NF_CHECK(selected == 2);
    NF_CHECK(!create_requested);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    NF_CHECK(selected == 2);
    NF_CHECK(create_requested);
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

NF_TEST(editor_internal_component_search_matches_both_languages_and_all_words) {
    NF_CHECK(editor::uiw::component_matches("  POINT light  ", "point_light"));
    NF_CHECK(editor::uiw::component_matches("ضوء", "point_light"));
    NF_CHECK(!editor::uiw::component_matches("point camera", "point_light"));
    NF_CHECK(editor::uiw::component_matches(" \t", "transform"));
}

NF_TEST(editor_internal_inspector_search_preserves_fold_state_and_language_ids) {
    CaptureFixture f;
    editor::uiw::InspectorSections sections;
    const auto frame = [&](bool force_closed) {
        ImGui::NewFrame();
        ImGui::Begin("Sections");
        sections.begin_frame();
        if (force_closed) sections.bulk_open = 0;
        const bool open = sections.header("transform", "Transform", ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::End();
        ImGui::Render();
        return open;
    };
    NF_CHECK(!frame(true));
    sections.query = "transform";
    NF_CHECK(frame(false));
    sections.query.clear();
    NF_CHECK(!frame(false));
    ui::set_language(ui::Language::Arabic);
    NF_CHECK(!frame(false));
    sections.query = "transform";
    NF_CHECK(frame(false));
    ui::set_language(ui::Language::English);
}

NF_TEST(editor_internal_action_rows_and_vectors_fit_narrow_panels) {
    CaptureFixture f;
    for (float width : {180.0f, 280.0f, 420.0f}) {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(ImVec2(width, 600));
        ImGui::Begin("Responsive", nullptr, ImGuiWindowFlags_NoSavedSettings);
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        ImGui::Button("First action");
        editor::uiw::next_action("Another action");
        ImGui::Button("Another action");
        NF_CHECK(ImGui::GetItemRectMax().x <= right + 1);
        float values[] = {1, 2, 3};
        editor::uiw::vector3_control("Position", values, 0, .1f);
        NF_CHECK(ImGui::GetItemRectMax().x <= right + 1);
        NF_CHECK_NEAR(values[2], 3, .001f);
        ImGui::End();
        ImGui::Render();
    }
}

NF_TEST(editor_internal_outliner_badges_fit_rows_and_preserve_the_tree_item) {
    CaptureFixture f;
    for (float width : {110.0f, 240.0f, 440.0f}) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(width, 200));
        ImGui::Begin("Badges", nullptr, ImGuiWindowFlags_NoSavedSettings);
        const char* label = "CrateB";
        ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                  ImGuiTreeNodeFlags_SpanAvailWidth);
        const ImGuiID row_id = ImGui::GetItemID();
        const auto minimum = ImGui::GetItemRectMin(), maximum = ImGui::GetItemRectMax();
        const float label_end = minimum.x + ImGui::GetTreeNodeToLabelSpacing() + ImGui::CalcTextSize(label).x;
        auto* draw = ImGui::GetWindowDrawList();
        const int first_vertex = draw->VtxBuffer.Size;
        const bool drawn = editor::uiw::draw_outliner_badge(0xff, minimum, maximum, label_end);
        if (width >= 240) NF_CHECK(drawn);
        NF_CHECK(ImGui::GetItemID() == row_id);
        for (int i = first_vertex; i < draw->VtxBuffer.Size; ++i) {
            const auto point = draw->VtxBuffer[i].pos;
            NF_CHECK(point.x >= label_end && point.x <= maximum.x);
            NF_CHECK(point.y >= minimum.y && point.y <= maximum.y);
        }
        NF_CHECK(!editor::uiw::draw_outliner_badge(0xff, minimum, maximum, maximum.x));
        NF_CHECK(!editor::uiw::draw_outliner_badge(1 << 10, minimum, maximum, label_end));
        ImGui::End();
        ImGui::Render();
    }
}

NF_TEST(editor_internal_outliner_badge_descriptions_shape_arabic_once) {
    const auto previous = ui::current_language();
    ui::set_language(ui::Language::Arabic);
    NF_CHECK(editor::uiw::outliner_badge_description(1) == editor::AV("filter_weather"));
    NF_CHECK(editor::uiw::outliner_badge_description(3) ==
             editor::AV("filter_weather") + "\n" + editor::AV("filter_cable"));
    NF_CHECK_EQ(editor::uiw::outliner_badge_count(0xffff), 8u);
    ui::set_language(previous);
}

NF_TEST(editor_internal_outliner_rows_indent_and_remember_fold_changes) {
    CaptureFixture f;
    editor::OutlinerState state;
    editor::OutlinerRow parent{{1, 0}, 0, true, false, "Parent"};
    editor::OutlinerRow child{{2, 0}, 1, false, false, "Child"};
    editor::OutlinerRow other{{3, 0}, 0, false, false, "Other root"};
    ImGuiID parent_id = 0;
    const auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(ImVec2(340, 400));
        ImGui::Begin("Hierarchy");
        float parent_x = 0;
        {
            editor::uiw::OutlinerRowUi row(parent, state, false);
            parent_x = row.minimum.x;
            parent_id = ImGui::GetItemID();
        }
        {
            editor::uiw::OutlinerRowUi row(child, state, false);
            NF_CHECK(row.minimum.x > parent_x);
        }
        {
            editor::uiw::OutlinerRowUi row(other, state, false);
            NF_CHECK_NEAR(row.minimum.x, parent_x, .01f);
        }
        ImGui::End();
        ImGui::Render();
    };
    frame();
    NF_CHECK(state.is_expanded(parent.entity));
    ImGui::ActivateItemByID(parent_id);
    frame();
    NF_CHECK(!state.is_expanded(parent.entity));
    frame();
    NF_CHECK(!state.is_expanded(parent.entity));
}

NF_TEST(editor_internal_outliner_long_labels_fit_and_keep_a_stable_identity) {
    CaptureFixture f;
    editor::OutlinerState state;
    editor::OutlinerRow entity{{7, 0}, 0, false, false, "A very long entity name with ## literal hashes"};
    ImGuiID previous_id = 0;
    for (float width : {180.0f, 240.0f, 400.0f}) {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(ImVec2(width, 300));
        ImGui::Begin("Names");
        {
            entity.feature_mask = 0xff;
            editor::uiw::OutlinerRowUi row(entity, state, false);
            if (width <= 240) NF_CHECK(row.clipped);
            NF_CHECK(row.label_end < row.maximum.x);
            NF_CHECK(row.full_text.find("## literal hashes") != std::string::npos);
            if (previous_id) NF_CHECK(ImGui::GetItemID() == previous_id);
            previous_id = ImGui::GetItemID();
        }
        ImGui::End();
        ImGui::Render();
    }
    ImGui::NewFrame();
    ImGui::Begin("Names");
    const auto shortened = editor::uiw::fit_outliner_text("اسم عنصر طويل جدا باللغة العربية", 70);
    NF_CHECK(ImGui::CalcTextSize(shortened.c_str()).x <= 70);
    NF_CHECK(shortened.ends_with("..."));
    NF_CHECK(editor::uiw::fit_outliner_text("Name", 1).empty());
    ImGui::End();
    ImGui::Render();
}

NF_TEST(editor_internal_outliner_root_drop_accepts_only_the_empty_area) {
    CaptureFixture f;
    editor::OutlinerState state;
    editor::OutlinerRow entity{{3, 0}, 0, false, false, "Target"};
    const ecs::Entity source{6, 0};
    ImVec2 row_point{}, empty_point{};
    const auto frame = [&](bool drag) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(340, 400));
        ImGui::Begin("Drop");
        if (drag && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern)) {
            ImGui::SetDragDropPayload("NF_ENTITY", &source, sizeof(source));
            ImGui::EndDragDropSource();
        }
        {
            editor::uiw::OutlinerRowUi row(entity, state, false);
            row_point = ImVec2(row.minimum.x + 60, (row.minimum.y + row.maximum.y) * .5f);
        }
        ecs::Entity delivered_entity;
        const bool delivered = editor::uiw::outliner_root_drop_target(delivered_entity);
        empty_point = ImVec2(ImGui::GetItemRectMin().x + 60, ImGui::GetItemRectMin().y + 60);
        if (delivered) NF_CHECK(delivered_entity == source);
        ImGui::End();
        ImGui::Render();
        return delivered;
    };
    frame(false);
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(row_point.x, row_point.y);
    io.AddMouseButtonEvent(0, true);
    NF_CHECK(!frame(true));
    NF_CHECK(!frame(true));
    io.AddMouseButtonEvent(0, false);
    NF_CHECK(!frame(true)); // releasing over the row must never unparent it
    frame(false);
    io.AddMousePosEvent(empty_point.x, empty_point.y);
    io.AddMouseButtonEvent(0, true);
    NF_CHECK(!frame(true));
    NF_CHECK(!frame(true));
    io.AddMouseButtonEvent(0, false);
    NF_CHECK(frame(true));
}

NF_TEST(editor_internal_component_catalog_search_is_bilingual_and_uses_all_terms) {
    bool found_body = false, found_camera = false;
    for (const auto& entry : editor::kComponents) {
        if (entry.kind == editor::ComponentKind::Body2D) {
            found_body = true;
            NF_CHECK(editor::component_entry_matches(entry, "  2D PHYSICS body  "));
            NF_CHECK(!editor::component_entry_matches(entry, "2d camera"));
        }
        if (entry.kind == editor::ComponentKind::Camera) {
            found_camera = true;
            NF_CHECK(editor::component_entry_matches(entry, "كاميرا"));
        }
        NF_CHECK(editor::component_entry_matches(entry, " \t"));
    }
    NF_CHECK(found_body && found_camera);
}

NF_TEST(editor_internal_component_picker_rejects_a_changed_scene_and_hides_existing_components) {
    CaptureFixture f;
    scene::Scene scene("Picker");
    const auto entity = scene.world().create_entity();
    scene.world().add<runtime::CameraComponent>(entity, runtime::CameraComponent{});
    editor::ComponentPicker picker;
    const auto frame = [&](bool open, uint64_t serial) {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(ImVec2(700, 650));
        ImGui::Begin("PickerTest");
        if (open) {
            picker.begin(&scene.world(), entity, serial);
            picker.query = "camera";
            ImGui::OpenPopup("##add_component_menu");
        }
        auto choice = editor::component_picker_popup(picker, &scene.world(), entity, serial);
        const bool popup_open = ImGui::IsPopupOpen("##add_component_menu");
        NF_CHECK(!choice);
        ImGui::End();
        ImGui::Render();
        return popup_open;
    };
    NF_CHECK(frame(true, 1));
    NF_CHECK_EQ(picker.visible, 0u);
    picker.show_attached = true;
    NF_CHECK(frame(false, 1));
    NF_CHECK_EQ(picker.visible, 1u);
    NF_CHECK(!frame(false, 2)); // same pointer and entity do not make a new scene safe
}

NF_TEST(editor_internal_component_fields_have_unique_ids_and_restore_the_parent_scope) {
    CaptureFixture f;
    editor::uiw::InspectorSections sections;
    ImGui::NewFrame();
    ImGui::Begin("FieldIds");
    sections.begin_frame();
    const auto parent_id = ImGui::GetID("Enabled");
    ImGuiID first = 0, second = 0;
    if (editor::uiw::InspectorSection section{sections, "sprite_2d", "Sprite2D", ImGuiTreeNodeFlags_DefaultOpen})
        first = ImGui::GetID("Enabled");
    if (editor::uiw::InspectorSection section{sections, "collider_2d", "Collider2D", ImGuiTreeNodeFlags_DefaultOpen})
        second = ImGui::GetID("Enabled");
    NF_CHECK(first && second && first != second);
    NF_CHECK(ImGui::GetID("Enabled") == parent_id);
    ImGui::End();
    ImGui::Render();
}

NF_TEST(editor_internal_2d_field_labels_have_both_translations) {
    for (const char* key : {"texture", "size", "body_dynamic", "body_static", "body_kinematic",
                            "collider_box", "collider_circle", "revert_changes"}) {
        NF_CHECK(ui::tr_for_language(key, ui::Language::English) != key);
        NF_CHECK(ui::tr_for_language(key, ui::Language::Arabic) != key);
    }
}
