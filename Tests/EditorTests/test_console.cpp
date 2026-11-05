// Editor console: bounded buffer, level filter, clear.

#include <NF/Test/TestFramework.hpp>
#include <NF/Editor/Console.hpp>
#include <NF/Editor/ConsolePanel.hpp>
#include <NF/UI/Localization.hpp>
#include <imgui.h>
#include <imgui_internal.h>

using namespace nf;

static LogMessage line(LogLevel level, const std::string& text) {
    return LogMessage{level, LogCategory::Editor, text, std::chrono::system_clock::now(), __FILE__,
                      __LINE__};
}

NF_TEST(editor_console_bounded) {
    editor::ConsoleBuffer console;
    for (size_t i = 0; i < editor::ConsoleBuffer::kMaxLines + 50; ++i) {
        console.push(line(LogLevel::Info, "line"));
    }
    NF_CHECK_EQ(console.size(), editor::ConsoleBuffer::kMaxLines);
    NF_CHECK_EQ(console.dropped(), 50u);
    console.clear();
    NF_CHECK_EQ(console.size(), 0u);
    NF_CHECK_EQ(console.dropped(), 0u);
}

NF_TEST(editor_console_filter) {
    editor::ConsoleBuffer console;
    console.push(line(LogLevel::Info, "info"));
    console.push(line(LogLevel::Warn, "warn"));
    console.push(line(LogLevel::Error, "error"));
    NF_CHECK_EQ(console.filtered(LogLevel::Info).size(), 3u);
    NF_CHECK_EQ(console.filtered(LogLevel::Warn).size(), 2u);
    NF_CHECK_EQ(console.filtered(LogLevel::Error).size(), 1u);
}

NF_TEST(editor_console_sink) {
    editor::ConsoleBuffer console;
    LogSink sink = editor::make_console_sink(console);
    sink(line(LogLevel::Error, "boom"));
    NF_CHECK_EQ(console.size(), 1u);
    NF_CHECK(console.filtered(LogLevel::Error)[0].text == "boom");
}

// The panel's Category combo maps to a bitmask: a message in none of the
// selected categories is hidden. All has to mean *every* category, including
// uncategorised ones (has_category(All, None) is false), or the panel would
// silently drop messages while its label claims to show everything.
NF_TEST(editor_console_category_filter) {
    editor::ConsoleBuffer console;
    console.push(LogMessage{LogLevel::Info, LogCategory::None, "uncategorised", {}, {}, 0});
    console.push(LogMessage{LogLevel::Info, LogCategory::Render, "rendered", {}, {}, 0});
    console.push(LogMessage{LogLevel::Info, LogCategory::Physics, "simulated", {}, {}, 0});

    editor::ConsoleBuffer::Filter all;
    all.min_level = LogLevel::Info;
    all.categories = LogCategory::All;
    NF_CHECK_EQ(console.filtered(all).size(), 3u);

    editor::ConsoleBuffer::Filter render;
    render.min_level = LogLevel::Info;
    render.categories = LogCategory::Render;
    const auto only_render = console.filtered(render);
    NF_CHECK_EQ(only_render.size(), 1u);
    NF_CHECK(only_render[0].text == "rendered");
}

// Categories are a bitmask, so a multi-category selection is one filter value.
NF_TEST(editor_console_combined_category_filter) {
    editor::ConsoleBuffer console;
    console.push(LogMessage{LogLevel::Info, LogCategory::Render, "rendered", {}, {}, 0});
    console.push(LogMessage{LogLevel::Info, LogCategory::Physics, "simulated", {}, {}, 0});
    console.push(LogMessage{LogLevel::Info, LogCategory::Audio, "played", {}, {}, 0});

    editor::ConsoleBuffer::Filter both;
    both.min_level = LogLevel::Info;
    both.categories = static_cast<LogCategory>(static_cast<uint16_t>(LogCategory::Render) |
                                              static_cast<uint16_t>(LogCategory::Audio));
    NF_CHECK_EQ(console.filtered(both).size(), 2u);
}

// The search box is a case-insensitive substring, so typing "runtime" finds
// "Runtime: ...". Arabic text has no case, so the ASCII-only lowering cannot
// corrupt it — but a search that ignored case entirely would hide real hits.
NF_TEST(editor_console_text_search) {
    editor::ConsoleBuffer console;
    console.push(line(LogLevel::Warn, "Runtime: mesh upload failed"));
    console.push(line(LogLevel::Warn, "Arabic: \xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7"));

    editor::ConsoleBuffer::Filter f;
    f.min_level = LogLevel::Info;
    f.text = "runtime";
    NF_CHECK_EQ(console.filtered(f).size(), 1u);

    f.text = "RUNTIME";
    NF_CHECK_EQ(console.filtered(f).size(), 1u);

    // A substring of the Arabic message is found byte-for-byte.
    f.text = "\xd9\x85\xd8\xb1";
    NF_CHECK_EQ(console.filtered(f).size(), 1u);

    f.text = "nope";
    NF_CHECK_EQ(console.filtered(f).size(), 0u);
}

// Level floor and text search compose: the panel applies both, and a message
// at a level below the floor is hidden even when it matches the search.
NF_TEST(editor_console_level_and_search_compose) {
    editor::ConsoleBuffer console;
    console.push(line(LogLevel::Info, "mesh cube ready"));
    console.push(line(LogLevel::Error, "mesh upload failed"));

    editor::ConsoleBuffer::Filter f;
    f.min_level = LogLevel::Error;
    f.text = "mesh";
    NF_CHECK_EQ(console.filtered(f).size(), 1u);

    f.min_level = LogLevel::Info;
    NF_CHECK_EQ(console.filtered(f).size(), 2u);
}

NF_TEST(editor_console_snapshot_counts_match_severity_floors_and_query) {
    editor::ConsoleBuffer console;
    console.push(line(LogLevel::Debug, "mesh debug"));
    console.push(line(LogLevel::Info, "mesh ready"));
    console.push(line(LogLevel::Warn, "mesh warning"));
    console.push(line(LogLevel::Error, "mesh error"));
    console.push(line(LogLevel::Fatal, "mesh fatal"));
    console.push(LogMessage{LogLevel::Error, LogCategory::Render, "mesh render", {}, {}, 0});
    console.push(line(LogLevel::Error, "other"));
    editor::ConsoleBuffer::Filter query{LogLevel::Error, LogCategory::Editor, "MESH"};
    const auto snapshot = console.snapshot(query);
    NF_CHECK_EQ(snapshot.total, 7u);
    NF_CHECK_EQ(snapshot.lines.size(), 2u);
    NF_CHECK_EQ(snapshot.level_counts[0], 5u);
    NF_CHECK_EQ(snapshot.level_counts[2], 4u);
    NF_CHECK_EQ(snapshot.level_counts[3], 3u);
    NF_CHECK_EQ(snapshot.level_counts[4], 2u);
    NF_CHECK_EQ(snapshot.level_counts[5], 1u);
    console.clear();
    const auto empty = console.snapshot(query);
    NF_CHECK(empty.lines.empty());
    NF_CHECK_EQ(empty.total, 0u);
    NF_CHECK_EQ(empty.level_counts[4], 0u);
}

NF_TEST(editor_console_copy_preserves_severity_multiline_and_logical_arabic) {
    const std::vector<LogMessage> lines = {
        line(LogLevel::Fatal, "failed\n\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7"),
        {LogLevel::Info, LogCategory::None, "100% done", {}, {}, 0}};
    NF_CHECK(editor::format_console_lines(lines) ==
             "[Fatal][Editor] failed\n\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7\n[Info][-] 100% done\n");
    NF_CHECK(editor::format_console_lines({}).empty());
}

namespace {
struct ConsoleUiFixture {
    ConsoleUiFixture() {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(900, 700);
        io.DeltaTime = 1.0f / 60;
        io.Fonts->AddFontDefault();
        io.Fonts->Build();
    }
    ~ConsoleUiFixture() { ImGui::DestroyContext(); }

    ImGuiWindow* frame(editor::ConsoleBuffer& buffer, editor::ConsolePanelState& state,
                       float width = 420, float height = 400) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(width, height));
        ImGui::Begin("ConsoleTest", nullptr, ImGuiWindowFlags_NoSavedSettings);
        editor::console_panel(buffer, state);
        auto* host = ImGui::GetCurrentWindow();
        auto* child = host->DC.ChildWindows[0];
        ImGui::End();
        ImGui::Render();
        return child;
    }
};
}

NF_TEST(editor_console_ui_scroll_allows_reading_backlog_and_follows_tail) {
    ConsoleUiFixture ui;
    editor::ConsoleBuffer buffer;
    editor::ConsolePanelState state;
    for (int i = 0; i < 80; ++i) buffer.push(line(LogLevel::Info, "message\nsecond line"));
    ImGuiWindow* child = nullptr;
    for (int i = 0; i < 4; ++i) child = ui.frame(buffer, state);
    NF_CHECK(child->Scroll.y > 0);
    NF_CHECK(child->ContentSize.y > 80 * ImGui::GetTextLineHeight() * 2);
    // Simulate a user scrolling to older messages, then receiving another log.
    ImGui::SetScrollY(child, 100.0f);
    ui.frame(buffer, state);
    buffer.push(line(LogLevel::Fatal, "new fatal"));
    child = ui.frame(buffer, state);
    child = ui.frame(buffer, state);
    NF_CHECK_NEAR(child->Scroll.y, 100.0f, 1.0f);
    ImGui::SetScrollY(child, child->ScrollMax.y);
    ui.frame(buffer, state);
    buffer.push(line(LogLevel::Warn, "tail"));
    for (int i = 0; i < 3; ++i) child = ui.frame(buffer, state);
    NF_CHECK_NEAR(child->Scroll.y, child->ScrollMax.y, 1.0f);
}

NF_TEST(editor_console_ui_controls_fit_narrow_dock_in_both_languages) {
    ConsoleUiFixture ui;
    editor::ConsoleBuffer buffer;
    editor::ConsolePanelState state;
    buffer.push(line(LogLevel::Info, "short"));
    for (auto language : {nf::ui::Language::English, nf::ui::Language::Arabic}) {
        nf::ui::set_language(language);
        for (float width : {300.0f, 420.0f, 800.0f}) {
            for (int i = 0; i < 3; ++i) ui.frame(buffer, state, width);
            auto* host = ImGui::FindWindowByName("ConsoleTest");
            NF_CHECK(host->ContentSize.x <= host->InnerRect.GetWidth() + 1.0f);
        }
    }
    nf::ui::set_language(nf::ui::Language::English);
}

NF_TEST(editor_console_ui_default_dock_leaves_room_for_messages_and_clear_works) {
    ConsoleUiFixture ui;
    editor::ConsoleBuffer buffer;
    editor::ConsolePanelState state;
    buffer.push(line(LogLevel::Warn, "warning"));
    buffer.push(line(LogLevel::Fatal, "fatal"));
    ImGuiWindow* child = nullptr;
    for (int i = 0; i < 3; ++i) child = ui.frame(buffer, state, 650, 170);
    NF_CHECK(child->InnerRect.GetHeight() >= ImGui::GetTextLineHeight() * 2);
    auto* host = ImGui::FindWindowByName("ConsoleTest");
    ImGui::ActivateItemByID(host->GetID("###console_level_4"));
    ui.frame(buffer, state, 650, 170);
    NF_CHECK_EQ(state.min_level, 4);
    ImGui::ActivateItemByID(host->GetID("###console_clear"));
    ui.frame(buffer, state, 650, 170);
    NF_CHECK_EQ(buffer.size(), 0u);
}
