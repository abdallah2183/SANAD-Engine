#pragma once

// NF/Test/TestPaths.hpp — a writable scratch root for the test suites.
//
// The tests used to write into std::filesystem::temp_directory_path() and
// nothing else. That works on a normal machine and FAILS on one where the
// process is not allowed to write outside its own tree — a sandboxed CI, a
// locked-down workstation, a machine under an application-control policy —
// and the failure reads as a broken test ("create_directories: Access is
// denied") when nothing about the code under test is wrong. Eighty-odd cases
// failed that way, all of them for the same reason, and none of them for the
// reason they exist.
//
// So the scratch root is chosen at RUN time: the system temp directory when
// the process may write there, and a directory inside the repository when it
// may not. The cases themselves are unchanged: they still ask for a directory
// by name, and they still remove it when they are done.
//
// The fallback lives in a DOT directory at the repository root (.nftest) and
// NOT under build/. The editor's asset listing skips every path with a
// "build", "dist", "Cache" or ".git" component — those are generated output,
// never authored content — so a scratch root under build/ would make the
// listing return nothing at all and every case that mounts a project there
// would fail for a reason that has nothing to do with the code.

#include <cstdint>
#include <filesystem>
#include <string>

namespace nf::test {

// The scratch root every test writes into.
inline std::filesystem::path temp_root() {
    static const std::filesystem::path root = [] {
        const std::filesystem::path sys = std::filesystem::temp_directory_path();
        // Probe the system temp directory: a directory that already exists
        // proves nothing, so a fresh one is created and removed.
        const std::filesystem::path probe = sys / "nf_test_probe";
        std::error_code ec;
        std::filesystem::create_directories(probe, ec);
        if (!ec) {
            std::error_code rm;
            std::filesystem::remove_all(probe, rm);
            return sys;
        }
        // Not writable: fall back into the repository. The root is found by
        // walking up from the working directory (the suite runs from the repo
        // root or from the build tree), and the scratch lives in a dot
        // directory the asset listing ignores.
        std::filesystem::path dir = std::filesystem::current_path();
        for (int i = 0; i < 8; ++i) {
            if (std::filesystem::exists(dir / "Editor" / "CMakeLists.txt")) {
                const std::filesystem::path fallback = dir / ".nftest";
                std::error_code fec;
                std::filesystem::create_directories(fallback, fec);
                return fallback;
            }
            if (!dir.has_parent_path()) {
                break;
            }
            dir = dir.parent_path();
        }
        // Nothing better: the system temp directory, and the failure it
        // brings with it.
        return sys;
    }();
    return root;
}

// A scratch directory named `name`, empty on entry.
inline std::filesystem::path temp_dir(const std::string& name) {
    const std::filesystem::path p = temp_root() / name;
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

} // namespace nf::test
