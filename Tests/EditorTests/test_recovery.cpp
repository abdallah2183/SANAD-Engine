// Recovery draft discovery. The write/open path needs a Runtime (and thus a
// device); this pins the VFS switch the File menu keys its enabled state off, so
// a missing cache mount disables Recover instead of offering a dead command.

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Editor/Console.hpp>
#include <NF/Editor/EditorApp.hpp>

#include <filesystem>

using namespace nf;

NF_TEST(editor_recovery_draft_is_discovered_only_when_present) {
    assets::VirtualFileSystem vfs;
    const auto tmp = std::filesystem::temp_directory_path() / "nf_ed_recovery";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp / "Content");
    std::filesystem::create_directories(tmp / "Cache");
    NF_CHECK(vfs.mount("content://", tmp / "Content").ok);
    NF_CHECK(vfs.mount("cache://", tmp / "Cache").ok);

    assets::AssetRegistry registry;
    assets::AssetManager manager(vfs, registry);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, registry, manager, console);

    NF_CHECK(!app.has_recovery_copy());
    NF_CHECK(vfs.write_text("cache://EditorRecovery.nfscene", "# SANAD Scene v1\n").ok);
    NF_CHECK(app.has_recovery_copy());

    std::filesystem::remove_all(tmp);
}
