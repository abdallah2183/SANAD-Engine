// Tests/AssetTests/test_vfs.cpp — VFS tests

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>

#include <filesystem>

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
