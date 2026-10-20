// Recovery draft discovery. The write/open path needs a Runtime (and thus a
// device); this pins the VFS switch the File menu keys its enabled state off, so
// a missing cache mount disables Recover instead of offering a dead command.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Editor/Console.hpp>
#include <NF/Editor/EditorApp.hpp>

#include <filesystem>
#include <string>

#ifdef _WIN32
    #include <process.h>
#else
    #include <unistd.h>
#endif

using namespace nf;

namespace {

// The id of this process, so two runs never share a directory.
unsigned long current_process_id() {
#ifdef _WIN32
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

// A scratch directory that removes itself.
//
// The name carries the process id AND the case name. It used to be a
// constant ("nf_ed_recovery"), so a parallel `ctest -j N` run deleted the
// directory another case was using, and the case failed for a reason that
// had nothing to do with what it tests.
class TempDir {
public:
    TempDir() : root_(nf::test::temp_root() /
                       ("nf_ed_recovery_" + std::to_string(current_process_id()) + "_draft")) {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(root_, ec);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return root_; }

private:
    std::filesystem::path root_;
};

} // namespace

NF_TEST(editor_recovery_draft_is_discovered_only_when_present) {
    assets::VirtualFileSystem vfs;
    TempDir tmp;
    const auto root = tmp.path();
    std::filesystem::create_directories(root / "Content");
    std::filesystem::create_directories(root / "Cache");
    NF_CHECK(vfs.mount("content://", root / "Content").ok);
    NF_CHECK(vfs.mount("cache://", root / "Cache").ok);

    assets::AssetRegistry registry;
    assets::AssetManager manager(vfs, registry);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, registry, manager, console);

    NF_CHECK(!app.has_recovery_copy());
    NF_CHECK(vfs.write_text("cache://EditorRecovery.nfscene", "# SANAD Scene v1\n").ok);
    NF_CHECK(app.has_recovery_copy());
}