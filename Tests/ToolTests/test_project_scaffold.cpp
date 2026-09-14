// Tests/ToolTests/test_project_scaffold.cpp — `nf new`

#include <NF/Test/TestFramework.hpp>
#include <NF/Project/ProjectScaffold.hpp>
#include <NF/Assets/ProjectDescriptor.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>

#include <filesystem>
#include <fstream>

using namespace nf;
using namespace nf::project;
using namespace nf::assets;

#ifndef NF_TEMPLATE_DIR
    #define NF_TEMPLATE_DIR ""
#endif

namespace {

std::filesystem::path scaffold_dir(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(p);
    return p;
}

ScaffoldOptions opts_for(const std::filesystem::path& root, const std::string& name) {
    ScaffoldOptions o;
    o.root = root;
    o.name = name;
    o.template_dir = std::string(NF_TEMPLATE_DIR);
    return o;
}

} // namespace

NF_TEST(scaffold_creates_a_loadable_project) {
    const auto root = scaffold_dir("nf_scaffold_ok");
    std::string err;
    NF_CHECK(scaffold_project(opts_for(root, "Demo"), err));

    const auto file = root / "Demo.nfproj";
    NF_CHECK(std::filesystem::exists(file));

    // The whole point is that the produced project loads — a scaffold that
    // writes something the runtime cannot parse is worse than no scaffold.
    auto desc = ProjectDescriptor::load_from_file(file, err);
    NF_CHECK(desc.has_value());
    if (desc) {
        NF_CHECK_EQ(desc->name(), std::string("Demo"));
        NF_CHECK_EQ(desc->startup_scene(), std::string("content://Scenes/Main.nfscene"));
        NF_CHECK(desc->has_mount("content://"));
        NF_CHECK(desc->has_mount("cache://"));
        NF_CHECK(desc->has_mount("shaders://"));
    }
    std::filesystem::remove_all(root);
}

NF_TEST(scaffold_creates_the_mount_directories) {
    const auto root = scaffold_dir("nf_scaffold_dirs");
    std::string err;
    NF_CHECK(scaffold_project(opts_for(root, "Demo"), err));

    // Created up front so a fresh project can be cooked and run immediately.
    for (const char* sub : {"Content", "Cache", "Shaders", "dist"}) {
        NF_CHECK(std::filesystem::is_directory(root / sub));
    }
    // Generated output must not be committed.
    NF_CHECK(std::filesystem::exists(root / ".gitignore"));
    std::filesystem::remove_all(root);
}

NF_TEST(scaffold_copies_the_template_content) {
    if (std::string(NF_TEMPLATE_DIR).empty()) {
        NF_SKIP("built without NF_TEMPLATE_DIR");
    }
    const auto root = scaffold_dir("nf_scaffold_template");
    std::string err;
    NF_CHECK(scaffold_project(opts_for(root, "Demo"), err));

    // A project with no scene and no mesh cannot render anything, so the
    // template is what makes a scaffolded project immediately runnable.
    NF_CHECK(std::filesystem::exists(root / "Content" / "Scenes" / "Main.nfscene"));
    NF_CHECK(std::filesystem::exists(root / "Content" / "Meshes" / "cube.nfmesh"));
    NF_CHECK(std::filesystem::exists(root / "Content" / "AssetRegistry.nfreg"));
    std::filesystem::remove_all(root);
}

NF_TEST(scaffold_refuses_to_overwrite_an_existing_project) {
    const auto root = scaffold_dir("nf_scaffold_clobber");
    std::string err;
    NF_CHECK(scaffold_project(opts_for(root, "Demo"), err));

    // Replacing a project file is not something the user can undo by re-running
    // the command, so the second attempt must fail loudly.
    std::string err2;
    NF_CHECK(!scaffold_project(opts_for(root, "Demo"), err2));
    NF_CHECK(err2.find("already exists") != std::string::npos);
    std::filesystem::remove_all(root);
}

NF_TEST(scaffold_rejects_an_invalid_name) {
    const auto root = scaffold_dir("nf_scaffold_badname");
    std::string err;

    ScaffoldOptions sep = opts_for(root, "Bad/Name");
    NF_CHECK(!scaffold_project(sep, err));

    ScaffoldOptions dots = opts_for(root, "..");
    NF_CHECK(!scaffold_project(dots, err));

    ScaffoldOptions empty = opts_for(root, "");
    NF_CHECK(!scaffold_project(empty, err));

    std::filesystem::remove_all(root);
}

NF_TEST(scaffold_produces_a_project_whose_mounts_resolve) {
    const auto root = scaffold_dir("nf_scaffold_mounts");
    std::string err;
    NF_CHECK(scaffold_project(opts_for(root, "Demo"), err));

    auto desc = ProjectDescriptor::load_from_file(root / "Demo.nfproj", err);
    NF_CHECK(desc.has_value());
    if (!desc) return;

    VirtualFileSystem vfs;
    NF_CHECK(desc->apply_mounts(vfs, err));
    // The declared mounts must actually exist on disk, or the first cook fails.
    auto content = vfs.resolve("content://");
    NF_CHECK(content.ok);
    NF_CHECK(std::filesystem::is_directory(content.value));

    std::filesystem::remove_all(root);
}
