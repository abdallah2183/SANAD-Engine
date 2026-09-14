// NF/Core/Logger.cpp — Logging implementation

#include <NF/Core/Logger.hpp>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>

namespace nf {

namespace {

std::string_view level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        default:              return "?????";
    }
}

std::string_view category_to_string(LogCategory cat) {
    switch (cat) {
        case LogCategory::Core:     return "Core";
        case LogCategory::Render:   return "Render";
        case LogCategory::Physics:  return "Physics";
        case LogCategory::Audio:    return "Audio";
        case LogCategory::Network:  return "Net";
        case LogCategory::Asset:    return "Asset";
        case LogCategory::Editor:   return "Editor";
        case LogCategory::Script:   return "Script";
        case LogCategory::ECS:      return "ECS";
        case LogCategory::Platform: return "Platform";
        case LogCategory::RHI:      return "RHI";
        case LogCategory::Jobs:     return "Jobs";
        case LogCategory::Scene:    return "Scene";
        default:                    return "Other";
    }
}

std::string format_timestamp(std::chrono::system_clock::time_point tp) {
    auto time = std::chrono::system_clock::to_time_t(tp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        tp.time_since_epoch()) % 1000;

    std::tm tm{};
    localtime_s(&tm, &time);

    std::ostringstream ss;
    ss << std::put_time(&tm, "%H:%M:%S") << '.'
       << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

} // namespace

// --- Logger ---

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::Logger() = default;

void Logger::add_sink(LogSink sink) {
    std::lock_guard lock(m_mutex);
    m_sinks.push_back(std::move(sink));
}

void Logger::set_min_level(LogLevel level) {
    m_min_level.store(level, std::memory_order_relaxed);
}

void Logger::set_category_filter(LogCategory cats) {
    m_category_filter.store(static_cast<u16>(cats), std::memory_order_relaxed);
}

void Logger::log(LogLevel level, LogCategory cat, std::string_view msg,
                 std::string_view file, u32 line) {
    if (static_cast<u8>(level) < static_cast<u8>(m_min_level.load(std::memory_order_relaxed)))
        return;

    if (!has_category(static_cast<LogCategory>(m_category_filter.load(std::memory_order_relaxed)), cat))
        return;

    LogMessage lm;
    lm.level = level;
    lm.category = cat;
    lm.text = std::string(msg);
    lm.timestamp = std::chrono::system_clock::now();
    lm.file = std::string(file);
    lm.line = line;

    std::lock_guard lock(m_mutex);
    for (auto& sink : m_sinks) {
        sink(lm);
    }
}

void Logger::trace(LogCategory cat, std::string_view msg, std::string_view file, u32 line) {
    log(LogLevel::Trace, cat, msg, file, line);
}
void Logger::debug(LogCategory cat, std::string_view msg, std::string_view file, u32 line) {
    log(LogLevel::Debug, cat, msg, file, line);
}
void Logger::info(LogCategory cat, std::string_view msg, std::string_view file, u32 line) {
    log(LogLevel::Info, cat, msg, file, line);
}
void Logger::warn(LogCategory cat, std::string_view msg, std::string_view file, u32 line) {
    log(LogLevel::Warn, cat, msg, file, line);
}
void Logger::error(LogCategory cat, std::string_view msg, std::string_view file, u32 line) {
    log(LogLevel::Error, cat, msg, file, line);
}
void Logger::fatal(LogCategory cat, std::string_view msg, std::string_view file, u32 line) {
    log(LogLevel::Fatal, cat, msg, file, line);
}

LogSink Logger::make_console_sink() {
    return [](const LogMessage& msg) {
        std::ostream& out = (msg.level >= LogLevel::Warn) ? std::cerr : std::cout;

        // Color codes
        const char* color = "";
        switch (msg.level) {
            case LogLevel::Trace: color = "\033[37m"; break; // white
            case LogLevel::Debug: color = "\033[36m"; break; // cyan
            case LogLevel::Info:  color = "\033[32m"; break; // green
            case LogLevel::Warn:  color = "\033[33m"; break; // yellow
            case LogLevel::Error: color = "\033[31m"; break; // red
            case LogLevel::Fatal: color = "\033[35m"; break; // magenta
            default: break;
        }
        const char* reset = "\033[0m";

        out << color << "[" << format_timestamp(msg.timestamp) << "] "
            << level_to_string(msg.level) << " ["
            << category_to_string(msg.category) << "] "
            << msg.text << reset << "\n";

        if (msg.level >= LogLevel::Error && !msg.file.empty()) {
            out << "          at " << msg.file << ":" << msg.line << "\n";
        }

        // Flush every message. Without this, a crash discards the whole log —
        // which is precisely when the log is needed most. Console output is
        // not on any hot path, so the flush costs nothing that matters.
        out << std::flush;
    };
}

LogSink Logger::make_file_sink(std::string_view path) {
    auto stream = std::make_shared<std::ofstream>(std::string(path), std::ios::app);
    if (!stream->is_open()) {
        return [](const LogMessage&) {}; // no-op
    }

    return [stream](const LogMessage& msg) {
        (*stream) << "[" << format_timestamp(msg.timestamp) << "] "
                  << level_to_string(msg.level) << " ["
                  << category_to_string(msg.category) << "] "
                  << msg.text;

        if (!msg.file.empty()) {
            (*stream) << " (" << msg.file << ":" << msg.line << ")";
        }

        (*stream) << "\n";
        stream->flush();
    };
}

} // namespace nf
