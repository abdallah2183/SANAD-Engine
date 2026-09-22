// Editor console: bounded buffer, level filter, clear.

#include <NF/Test/TestFramework.hpp>
#include <NF/Editor/Console.hpp>

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
