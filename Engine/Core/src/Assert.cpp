// NF/Core/Assert.cpp

#include <NF/Core/Assert.hpp>
#include <NF/Core/Logger.hpp>

#include <cstdio>
#include <cstdlib>
#include <iostream>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace nf {

namespace {

AssertHandler s_handler = nullptr;

void default_assert_handler(std::string_view msg, std::string_view file, u32 line) {
    std::cerr << "========================================\n";
    std::cerr << "ASSERTION FAILED\n";
    std::cerr << "Message: " << msg << "\n";
    std::cerr << "File:    " << file << "\n";
    std::cerr << "Line:    " << line << "\n";
    std::cerr << "========================================\n";

    // Also log it
    Logger::instance().error(LogCategory::Core, msg, file, line);

    // Break into debugger if present
    if (IsDebuggerPresent()) {
        __debugbreak();
    }

    std::abort();
}

} // namespace

void set_assert_handler(AssertHandler handler) {
    s_handler = handler;
}

[[noreturn]] void assert_failed(std::string_view msg, std::string_view file, u32 line) {
    if (s_handler) {
        s_handler(msg, file, line);
    } else {
        default_assert_handler(msg, file, line);
    }
    std::abort();
}

} // namespace nf
