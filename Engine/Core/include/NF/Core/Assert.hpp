#pragma once

// NF/Core/Assert.hpp — Assertion macros for engine development

#include <NF/Core/Types.hpp>
#include <string_view>

namespace nf {

using AssertHandler = void(*)(std::string_view msg, std::string_view file, u32 line);

void set_assert_handler(AssertHandler handler);

[[noreturn]] void assert_failed(std::string_view msg, std::string_view file, u32 line);

} // namespace nf

#ifdef NDEBUG
    #define NF_ASSERT(cond, msg) do { (void)(cond); (void)(msg); } while(0)
    #define NF_ASSERT_MSG(msg)  do { (void)(msg); } while(0)
#else
    #define NF_ASSERT(cond, msg) \
        do { \
            if (!(cond)) [[unlikely]] { \
                ::nf::assert_failed(msg, __FILE__, __LINE__); \
            } \
        } while (0)

    #define NF_ASSERT_MSG(msg) \
        do { \
            ::nf::assert_failed(msg, __FILE__, __LINE__); \
        } while (0)
#endif

// Always-on assertions
#define NF_VERIFY(cond, msg) \
    do { \
        if (!(cond)) [[unlikely]] { \
            ::nf::assert_failed(msg, __FILE__, __LINE__); \
        } \
    } while (0)

// Unreachable
#define NF_UNREACHABLE() \
    ::nf::assert_failed("Unreachable code reached", __FILE__, __LINE__); \
    __assume(0)
