// TestFramework.cpp — Test runner entry point.
// The NF_TEST macro registers cases at static-init time; this main() just
// runs them all and returns the exit code.

#include <NF/Test/TestFramework.hpp>
#include <NF/Core/Logger.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

/// Reads an environment variable, or returns "" when unset.
/// MSVC's getenv is deprecated under /WX (C4996), so use _dupenv_s there.
std::string read_env(const char* name) {
    std::string value;
#if defined(_MSC_VER)
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, name) == 0 && buf != nullptr) {
        value.assign(buf);
        std::free(buf);
    }
#else
    if (const char* raw = std::getenv(name)) value.assign(raw);
#endif
    return value;
}

/// Console log level, overridable with NF_TEST_LOG_LEVEL.
///
/// Defaults to Warn so a normal run stays quiet. Benchmarks report their
/// timings at Info, which the default level drops — meaning the numbers were
/// previously unobservable and no performance regression could ever be
/// diagnosed. Set NF_TEST_LOG_LEVEL=info to surface them.
nf::LogLevel log_level_from_env() {
    const std::string v = read_env("NF_TEST_LOG_LEVEL");
    if (v == "trace") return nf::LogLevel::Trace;
    if (v == "debug") return nf::LogLevel::Debug;
    if (v == "info") return nf::LogLevel::Info;
    if (v == "warn") return nf::LogLevel::Warn;
    if (v == "error") return nf::LogLevel::Error;
    return nf::LogLevel::Warn;
}

} // namespace

int main(int argc, char** argv) {
    // Flush after every write: if a test hard-crashes, the log must show
    // exactly which test was running.
    std::cout << std::unitbuf;
    // Warnings and errors reach the console so GPU-side failures (validation,
    // init problems) are diagnosable from ctest output alone.
    nf::Logger::instance().add_sink(nf::Logger::make_console_sink());
    nf::Logger::instance().set_min_level(log_level_from_env());
    // Optional filter: `RHITests some_name` runs only matching tests, each in
    // its own process — GPU tests poison a shared device when they misbehave,
    // so per-test isolation is the reliable way to bisect.
    const std::string filter = argc > 1 ? argv[1] : "";
    const int ret = nf::test::TestRegistry::instance().run_all(filter);
    std::cout.flush();
    // Use std::_Exit / _Exit to immediately terminate the test process.
    // Vulkan drivers (e.g. NVIDIA/AMD) spawn worker and telemetry threads that can
    // deadlock or hang during C-runtime exit / DLL unload when static singletons destruct.
    std::_Exit(ret);
}
