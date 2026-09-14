// CoreTests/test_filesystem.cpp

#include <NF/Test/TestFramework.hpp>
#include <NF/Core/FileSystem.hpp>

using namespace nf;

NF_TEST(test_filesystem_extension) {
    NF_CHECK_EQ(FileSystem::extension("test.txt"), std::string_view(".txt"));
    NF_CHECK_EQ(FileSystem::extension("/path/to/image.png"), std::string_view(".png"));
    NF_CHECK_EQ(FileSystem::extension("noextension"), std::string_view(""));
}

NF_TEST(test_filesystem_filename) {
    NF_CHECK_EQ(FileSystem::filename("path/to/file.txt"), std::string_view("file.txt"));
    NF_CHECK_EQ(FileSystem::filename("file.txt"), std::string_view("file.txt"));
}

NF_TEST(test_filesystem_stem) {
    NF_CHECK_EQ(FileSystem::stem("path/to/file.txt"), std::string_view("file"));
}

NF_TEST(test_filesystem_write_read) {
    const char* test_path = "nf_test_filesystem_write_read.tmp";
    std::string text = "Hello NOVAForge!";

    bool written = FileSystem::write_text(test_path, text);
    NF_CHECK(written);

    std::string read = FileSystem::read_text(test_path);
    NF_CHECK_EQ(read, text);
}

NF_TEST(test_filesystem_cwd) {
    std::string cwd = FileSystem::get_working_directory();
    NF_CHECK(!cwd.empty());
}
