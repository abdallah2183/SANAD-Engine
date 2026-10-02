// Tests/AssetTests/test_vfs.cpp — VFS tests

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>

#include <filesystem>
#include <fstream>

using namespace nf;
using namespace nf::assets;

NF_TEST(vfs_mount_resolution) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_vfs_test_mount";
    std::filesystem::create_directories(tmp);
    auto r = vfs.mount("content://", tmp);
    NF_CHECK(r.ok);
    auto resolved = vfs.resolve("content://Meshes/cube.nfmesh");
    NF_CHECK(resolved.ok);
    NF_CHECK(resolved.value.string().find("cube.nfmesh") != std::string::npos);
    std::filesystem::remove_all(tmp);
}

NF_TEST(vfs_path_traversal_rejection) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_vfs_test_traversal";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);
    auto r1 = vfs.resolve("content://../outside.txt");
    NF_CHECK(!r1.ok);
    auto r2 = vfs.resolve("content://a/../../b.txt");
    NF_CHECK(!r2.ok);
    auto r3 = vfs.resolve("content://a/./b.txt");
    // This should succeed (canonicalization of . should not escape)
    NF_CHECK(r3.ok);
    std::filesystem::remove_all(tmp);
}

NF_TEST(vfs_read_write_inside_mount) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_vfs_test_rw";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);
    std::string text = "hello world";
    auto w = vfs.write_text("content://test.txt", text);
    NF_CHECK(w.ok);
    auto r = vfs.read_text("content://test.txt");
    NF_CHECK(r.ok);
    NF_CHECK(r.value == text);
    std::filesystem::remove_all(tmp);
}

NF_TEST(vfs_missing_file) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_vfs_test_missing";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);
    auto r = vfs.read_text("content://does_not_exist.txt");
    NF_CHECK(!r.ok);
    auto e = vfs.exists("content://does_not_exist.txt");
    NF_CHECK(e.ok && !e.value);
    std::filesystem::remove_all(tmp);
}

NF_TEST(vfs_windows_path_normalization) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_vfs_test_win";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);
    // Windows backslashes should be normalized to forward slashes
    auto r1 = vfs.resolve("content://a\\b\\c.txt");
    auto r2 = vfs.resolve("content://a/b/c.txt");
    NF_CHECK(r1.ok && r2.ok);
    NF_CHECK(r1.value == r2.value);
    std::filesystem::remove_all(tmp);
}

// --- Delete / rename --------------------------------------------------------
//
// These back the FileSystem dock's Delete and Rename menu items, so the cases
// that matter are the destructive ones: a mount root must survive, a single-file
// delete must not be able to take a tree, and a rename must never clobber.

namespace {

// A mount with one file and one folder holding one more file, for the
// destructive tests to act on.
struct DeleteFixture {
    VirtualFileSystem vfs;
    std::filesystem::path tmp;
    explicit DeleteFixture(const char* name) {
        tmp = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove_all(tmp);
        std::filesystem::create_directories(tmp / "Meshes" / "Props");
        vfs.mount("content://", tmp);
        (void)vfs.write_text("content://top.txt", "top");
        (void)vfs.write_text("content://Meshes/cube.nfmesh", "mesh");
        (void)vfs.write_text("content://Meshes/Props/rock.nfmesh", "rock");
    }
    ~DeleteFixture() { std::filesystem::remove_all(tmp); }
};

} // namespace

NF_TEST(vfs_remove_file) {
    DeleteFixture f("nf_vfs_test_remove");
    NF_CHECK(f.vfs.remove("content://top.txt").ok);
    auto e = f.vfs.exists("content://top.txt");
    NF_CHECK(e.ok && !e.value);
    // The rest of the tree is untouched.
    NF_CHECK(f.vfs.exists("content://Meshes/cube.nfmesh").ok);
}

NF_TEST(vfs_remove_refuses_a_directory) {
    // The single-file delete must NOT be able to take a tree, even when pointed
    // at a folder. remove_all is the separate, explicitly-named call that does.
    DeleteFixture f("nf_vfs_test_remove_dir");
    auto r = f.vfs.remove("content://Meshes");
    NF_CHECK(!r.ok);
    auto e = f.vfs.exists("content://Meshes/cube.nfmesh");
    NF_CHECK(e.ok && e.value);
}

NF_TEST(vfs_remove_all_takes_the_tree) {
    DeleteFixture f("nf_vfs_test_remove_all");
    NF_CHECK(f.vfs.remove_all("content://Meshes").ok);
    auto e = f.vfs.exists("content://Meshes");
    NF_CHECK(e.ok && !e.value);
    NF_CHECK(f.vfs.exists("content://top.txt").ok);
}

NF_TEST(vfs_refuses_to_delete_a_mount_root) {
    DeleteFixture f("nf_vfs_test_root_del");
    auto r1 = f.vfs.remove("content://");
    auto r2 = f.vfs.remove_all("content://");
    NF_CHECK(!r1.ok);
    NF_CHECK(!r2.ok);
    // The tree is still there — this is the assertion that matters.
    NF_CHECK(f.vfs.exists("content://top.txt").ok);
    NF_CHECK(f.vfs.exists("content://Meshes/Props/rock.nfmesh").ok);
}

NF_TEST(vfs_remove_outside_the_mount_is_rejected) {
    DeleteFixture f("nf_vfs_test_del_escape");
    // A sibling of the mount directory, one level up: exactly what a `../`
    // traversal would reach if resolve() did not reject it.
    const std::string sib = f.tmp.filename().string() + "_outside";
    const auto outside = f.tmp.parent_path() / sib;
    std::filesystem::remove_all(outside);
    std::filesystem::create_directories(outside);
    {
        std::ofstream o(outside / "precious.txt");
        o << "do not delete";
    }
    auto r = f.vfs.remove_all("content://../" + sib);
    NF_CHECK(!r.ok);
    // The assertion that matters: the traversal was refused AND the victim is
    // still on disk.
    NF_CHECK(std::filesystem::exists(outside / "precious.txt"));
    std::filesystem::remove_all(outside);
}

NF_TEST(vfs_rename_moves_a_file) {
    DeleteFixture f("nf_vfs_test_rename");
    NF_CHECK(f.vfs.rename("content://Meshes/cube.nfmesh",
                           "content://Meshes/box.nfmesh").ok);
    NF_CHECK(!f.vfs.exists("content://Meshes/cube.nfmesh").value);
    auto r = f.vfs.read_text("content://Meshes/box.nfmesh");
    NF_CHECK(r.ok && r.value == "mesh");
}

NF_TEST(vfs_rename_moves_a_folder_with_its_contents) {
    DeleteFixture f("nf_vfs_test_rename_dir");
    NF_CHECK(f.vfs.rename("content://Meshes", "content://Models").ok);
    auto r = f.vfs.read_text("content://Models/Props/rock.nfmesh");
    NF_CHECK(r.ok && r.value == "rock");
}

NF_TEST(vfs_rename_refuses_to_clobber) {
    // Silently overwriting would be unrecoverable, so this is a hard failure
    // even though the source exists and the target does too.
    DeleteFixture f("nf_vfs_test_rename_clobber");
    auto r = f.vfs.rename("content://top.txt", "content://Meshes/cube.nfmesh");
    NF_CHECK(!r.ok);
    // Both originals survive untouched.
    NF_CHECK(f.vfs.read_text("content://top.txt").value == "top");
    NF_CHECK(f.vfs.read_text("content://Meshes/cube.nfmesh").value == "mesh");
}

NF_TEST(vfs_rename_refuses_a_missing_destination_folder) {
    // A typo must not create a tree: "content://Meshes/Cubee" as a FOLDER
    // destination is an error, not a new folder.
    DeleteFixture f("nf_vfs_test_rename_typo");
    auto r = f.vfs.rename("content://Meshes", "content://Typo/Meshes");
    NF_CHECK(!r.ok);
    NF_CHECK(!f.vfs.exists("content://Typo").value);
}

NF_TEST(vfs_rename_refuses_a_mount_root) {
    DeleteFixture f("nf_vfs_test_rename_root");
    auto r = f.vfs.rename("content://", "content://Renamed");
    NF_CHECK(!r.ok);
    NF_CHECK(f.vfs.exists("content://top.txt").ok);
}
