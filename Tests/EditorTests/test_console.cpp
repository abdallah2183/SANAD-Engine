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
