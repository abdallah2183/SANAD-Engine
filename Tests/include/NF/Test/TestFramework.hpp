#pragma once

#include <NF/Core/Logger.hpp>
#include <NF/Core/Types.hpp>

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <iostream>
#include <cstdint>

namespace nf::test {

/// Thrown by NF_SKIP. A skipped test is neither a pass nor a failure, and is
/// reported in its own bucket: an environment-dependent no-op must never be
/// counted as verified behaviour. Deliberately does NOT derive from
/// std::exception so it cannot be swallowed by the failure handler.
struct TestSkipped {
    std::string reason;
};

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

class TestRegistry {
public:
    static TestRegistry& instance() {
        static TestRegistry reg;
        return reg;
    }

    void add(std::string_view name, std::function<void()> fn) {
        m_cases.push_back({std::string(name), std::move(fn)});
    }

    /// Runs every registered case (optionally only those whose name contains
    /// `filter`, for isolating a single test in its own process — GPU tests
    /// poison a shared device when they misbehave, so isolation matters).
    int run_all(const std::string& filter = {}) {
        u32 passed = 0;
        u32 failed = 0;
        u32 skipped = 0;
        u32 run = 0;

        for (const auto& tc : m_cases) {
            if (!filter.empty() && tc.name.find(filter) == std::string::npos) continue;
            ++run;
            std::cout << "[ RUN      ] " << tc.name << "\n";
            try {
                tc.fn();
                std::cout << "[       OK ] " << tc.name << "\n";
                ++passed;
            } catch (const TestSkipped& s) {
                std::cout << "[  SKIPPED ] " << tc.name << " — " << s.reason << "\n";
                ++skipped;
            } catch (const std::exception& e) {
                std::cout << "[  FAILED  ] " << tc.name << " — " << e.what() << "\n";
                ++failed;
            }
        }

        // "Total" counts the cases actually run, not every registered case, so
        // a filtered run does not report the full suite size.
        std::cout << "\n========================================\n";
        std::cout << "Passed: " << passed << " | Failed: " << failed
                  << " | Skipped: " << skipped << " | Total: " << run << "\n";
        std::cout << "========================================\n";

        return failed == 0 ? 0 : 1;
    }

private:
    std::vector<TestCase> m_cases;
};

struct TestRegistrar {
    TestRegistrar(std::string_view name, std::function<void()> fn) {
        TestRegistry::instance().add(name, std::move(fn));
    }
};

#define NF_TEST(name) \
    static void name(); \
    static ::nf::test::TestRegistrar s_registrar_##name(#name, name); \
    static void name()

/// Marks a case as not applicable in this environment (missing GPU, absent
/// hardware feature, opt-in heavy benchmark). Reported as SKIPPED — never as a
/// pass — so a suite that verifies nothing cannot look green.
#define NF_SKIP(reason) \
    do { \
        throw ::nf::test::TestSkipped{std::string(reason)}; \
    } while (0)

#define NF_CHECK(cond) \
    do { \
        if (!(cond)) { \
            throw std::runtime_error(std::string("NF_CHECK failed: " #cond " at ") + \
                std::string(__FILE__) + ":" + std::to_string(__LINE__)); \
        } \
    } while (0)

#define NF_CHECK_EQ(a, b) \
    do { \
        if (!((a) == (b))) { \
            throw std::runtime_error(std::string("NF_CHECK_EQ failed: " #a " != " #b " at ") + \
                std::string(__FILE__) + ":" + std::to_string(__LINE__)); \
        } \
    } while (0)

#define NF_CHECK_NEAR(a, b, eps) \
    do { \
        if (std::abs((a) - (b)) > (eps)) { \
            throw std::runtime_error(std::string("NF_CHECK_NEAR failed: |" #a " - " #b "| > " #eps " at ") + \
                std::string(__FILE__) + ":" + std::to_string(__LINE__)); \
        } \
    } while (0)

} // namespace nf::test

