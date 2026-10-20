#pragma once

// NF/Core/Logger.hpp — Logging system with levels, categories, and multiple sinks

#include <NF/Core/Types.hpp>

#include <atomic>
#include <chrono>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace nf {

enum class LogLevel : u8 {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Fatal = 5
};

enum class LogCategory : u16 {
    None      = 0,
    Core      = 1 << 0,
    Render    = 1 << 1,
    Physics   = 1 << 2,
    Audio     = 1 << 3,
    Network   = 1 << 4,
    Asset     = 1 << 5,
    Editor    = 1 << 6,
    Script    = 1 << 7,
    ECS       = 1 << 8,
    Platform  = 1 << 9,
    RHI       = 1 << 10,
    Jobs      = 1 << 11,
    Scene     = 1 << 12,
    All       = 0xFFFF
};

inline LogCategory operator|(LogCategory a, LogCategory b) {
    return static_cast<LogCategory>(static_cast<u16>(a) | static_cast<u16>(b));
}

inline bool has_category(LogCategory flags, LogCategory test) {
    return (static_cast<u16>(flags) & static_cast<u16>(test)) != 0;
}

struct LogMessage {
    LogLevel level;
    LogCategory category;
    std::string text;
    std::chrono::system_clock::time_point timestamp;
    std::string file;
    u32 line;
};

using LogSink = std::function<void(const LogMessage&)>;

class Logger {
public:
    static Logger& instance();

    void add_sink(LogSink sink);
    void set_min_level(LogLevel level);
    void set_category_filter(LogCategory cats);

    /// True when a message at `level` passes the level filter. This is the
    /// cheap gate the log macros use so a filtered-out call costs an atomic
    /// load instead of a std::format plus a heap allocation. The category
    /// filter is still applied inside log().
    bool enabled(LogLevel level) const {
        return static_cast<u8>(level) >= static_cast<u8>(m_min_level.load(std::memory_order_relaxed));
    }
    /// The current minimum level (messages below it are dropped).
    LogLevel min_level() const { return m_min_level.load(std::memory_order_relaxed); }

    void log(LogLevel level, LogCategory cat, std::string_view msg,
             std::string_view file = "", u32 line = 0);

    // Convenience
    void trace(LogCategory cat, std::string_view msg, std::string_view file = "", u32 line = 0);
    void debug(LogCategory cat, std::string_view msg, std::string_view file = "", u32 line = 0);
    void info(LogCategory cat, std::string_view msg, std::string_view file = "", u32 line = 0);
    void warn(LogCategory cat, std::string_view msg, std::string_view file = "", u32 line = 0);
    void error(LogCategory cat, std::string_view msg, std::string_view file = "", u32 line = 0);
    void fatal(LogCategory cat, std::string_view msg, std::string_view file = "", u32 line = 0);

    // Console sink factory
    static LogSink make_console_sink();
    // File sink factory
    static LogSink make_file_sink(std::string_view path);

private:
    Logger();

    std::vector<LogSink> m_sinks;
    std::mutex m_mutex;
    std::atomic<LogLevel> m_min_level{LogLevel::Trace};
    std::atomic<u16> m_category_filter{static_cast<u16>(LogCategory::All)};
};

// --- Macros for file/line capture with std::format support ---

// Helper: format message (if format args present, use std::format; else pass through)
//
// The level gate comes FIRST on purpose. The filter lives inside log(), which
// runs AFTER the message has been formatted — so a filtered-out NF_LOG_TRACE
// (there are several in hot loops) used to pay for a std::format and the heap
// allocation its result needs, only to be thrown away. enabled() is an atomic
// load, so a dropped call now costs a load and a compare.
//
// The form stays an EXPRESSION (not a do/while): the macros are used as
// statements everywhere, and keeping them expressions means no existing call
// site can change meaning. `level` is the LogLevel value, `method` the
// Logger member that logs at it — the two cannot be derived from each other
// (the enum is Trace/Debug/..., the methods are trace/debug/...).
#define NF_LOG_FMT(level, method, cat, ...) \
    (::nf::Logger::instance().enabled(level) \
         ? ::nf::Logger::instance().method(cat, std::format(__VA_ARGS__), __FILE__, __LINE__) \
         : (void)0)

#define NF_LOG_TRACE(cat, ...) NF_LOG_FMT(::nf::LogLevel::Trace, trace, cat, __VA_ARGS__)
#define NF_LOG_DEBUG(cat, ...) NF_LOG_FMT(::nf::LogLevel::Debug, debug, cat, __VA_ARGS__)
#define NF_LOG_INFO(cat, ...)  NF_LOG_FMT(::nf::LogLevel::Info,  info,  cat, __VA_ARGS__)
#define NF_LOG_WARN(cat, ...)  NF_LOG_FMT(::nf::LogLevel::Warn,  warn,  cat, __VA_ARGS__)
#define NF_LOG_ERROR(cat, ...) NF_LOG_FMT(::nf::LogLevel::Error, error, cat, __VA_ARGS__)
#define NF_LOG_FATAL(cat, ...) NF_LOG_FMT(::nf::LogLevel::Fatal, fatal, cat, __VA_ARGS__)

} // namespace nf
