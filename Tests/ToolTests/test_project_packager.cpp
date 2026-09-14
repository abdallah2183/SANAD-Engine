// Tests/ToolTests/test_project_packager.cpp — `nf build`
//
// Packaging is the step that turns "the engine can render a scene" into "a game
// can be shipped". These tests pin the properties that make that true: the
// package is self-contained, it is relocatable, a broken asset stops the build
// instead of shipping, and a rebuild does not leave stale files behind.

#include <NF/Test/TestFramework.hpp>
#include <NF/Project/ProjectPackager.hpp>
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
#ifndef NF_BASIC3D_SHADER_DIR
    #define NF_BASIC3D_SHADER_DIR ""
#endif

namespace {

struct PackSandbox {
    std::filesystem::path root;
    std::filesystem::path project_file;

    explicit PackSandbox(const std::string& name) {
        root = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove_all(root);
        ScaffoldOptions o;
        o.root = root;
        o.name = "Demo";
        o.template_dir = std::string(NF_TEMPLATE_DIR);
        std::string err;
        ok = scaffold_project(o, err);
        project_file = root / "Demo.nfproj";
    }
    ~PackSandbox() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    bool ok = false;

    BuildOptions options(const std::string& out_dir = {}) const {
        BuildOptions o;
        o.project_file = project_file;
        o.shader_dir = std::string(NF_BASIC3D_SHADER_DIR);
        if (!out_dir.empty()) o.output_dir = out_dir;
        return o;
    }

    std::filesystem::path dist() const { return root / "dist"; }
};

} // namespace

NF_TEST(packager_produces_a_self_contained_package) {
    PackSandbox sb("nf_pack_basic");
    if (!sb.ok) { NF_SKIP("template unavailable"); }
    if (std::string(NF_BASIC3D_SHADER_DIR).empty()) { NF_SKIP("no shader dir configured"); }

    BuildReport report;
    std::string err;
    NF_CHECK(build_project(sb.options(), report, err));

    // Cooked assets the runtime actually reads.
    NF_CHECK(std::filesystem::exists(sb.dist() / "Cache" / "Meshes" / "cube.nfmesh"));
    NF_CHECK(std::filesystem::exists(sb.dist() / "Cache" / "Scenes" / "Main.nfscene"));
    // Source assets, so the package can be re-cooked in place.
    NF_CHECK(std::filesystem::exists(sb.dist() / "Content" / "AssetRegistry.nfreg"));
    // The descriptor, copied in: its mounts are relative, which is what makes
    // the package relocatable.
    NF_CHECK(std::filesystem::exists(sb.dist() / "Demo.nfproj"));
    // Shaders ship, so the hardcoded build-tree search is not load-bearing.
    NF_CHECK(std::filesystem::exists(sb.dist() / "Shaders" / "Basic3D" / "gbuffer_vert.spv"));
    NF_CHECK(std::filesystem::exists(sb.dist() / "manifest.txt"));

    NF_CHECK_EQ(report.cook.failed, size_t{0});
    NF_CHECK(report.cook.cooked > 0);
    NF_CHECK(report.shaders_copied > 0);
    NF_CHECK(report.manifest_entries > 0);
}

NF_TEST(packager_package_is_relocatable) {
    PackSandbox sb("nf_pack_reloc");
    if (!sb.ok) { NF_SKIP("template unavailable"); }
    if (std::string(NF_BASIC3D_SHADER_DIR).empty()) { NF_SKIP("no shader dir configured"); }

    BuildReport report;
    std::string err;
    NF_CHECK(build_project(sb.options(), report, err));

    const auto moved = std::filesystem::temp_directory_path() / "nf_pack_reloc_moved";
    std::filesystem::remove_all(moved);
    std::filesystem::copy(sb.dist(), moved, std::filesystem::copy_options::recursive);

    // The descriptor inside the package must resolve its mounts against its own
    // new location, not the directory it was built in.
    auto desc = ProjectDescriptor::load_from_file(moved / "Demo.nfproj", err);
    NF_CHECK(desc.has_value());
    if (desc) {
        VirtualFileSystem vfs;
        NF_CHECK(desc->apply_mounts(vfs, err));
        auto content = vfs.resolve("content://");
        NF_CHECK(content.ok);
        const auto resolved = content.value.string();
        NF_CHECK(resolved.find("nf_pack_reloc_moved") != std::string::npos);
        NF_CHECK(resolved.find("nf_pack_reloc" + std::string("/")) == std::string::npos);
        // And the cooked asset it needs is actually there.
        NF_CHECK(std::filesystem::exists(moved / "Cache" / "Meshes" / "cube.nfmesh"));
    }
    std::filesystem::remove_all(moved);
}

NF_TEST(packager_manifest_covers_the_shipped_trees) {
    PackSandbox sb("nf_pack_manifest");
    if (!sb.ok) { NF_SKIP("template unavailable"); }
    if (std::string(NF_BASIC3D_SHADER_DIR).empty()) { NF_SKIP("no shader dir configured"); }

    BuildReport report;
    std::string err;
    NF_CHECK(build_project(sb.options(), report, err));

    std::ifstream in(sb.dist() / "manifest.txt");
    NF_CHECK(static_cast<bool>(in));
    std::string line;
    size_t entries = 0;
    bool saw_cache = false, saw_shaders = false, saw_content = false;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        ++entries;
        if (line.find("Cache/") != std::string::npos) saw_cache = true;
        if (line.find("Shaders/") != std::string::npos) saw_shaders = true;
        if (line.find("Content/") != std::string::npos) saw_content = true;
    }
    NF_CHECK_EQ(entries, report.manifest_entries);
    NF_CHECK(saw_cache);
    NF_CHECK(saw_shaders);
    NF_CHECK(saw_content);
}

NF_TEST(packager_refuses_to_ship_a_broken_asset) {
    PackSandbox sb("nf_pack_broken");
    if (!sb.ok) { NF_SKIP("template unavailable"); }

    // A corrupt mesh in the content tree must stop the build. Shipping it would
    // move the failure to load time inside the game, which is strictly worse.
    {
        std::ofstream f(sb.root / "Content" / "Meshes" / "broken.nfmesh", std::ios::binary);
        f << "not a mesh";
    }
    BuildReport report;
    std::string err;
    NF_CHECK(!build_project(sb.options(), report, err));
    NF_CHECK(err.find("broken.nfmesh") != std::string::npos);
    NF_CHECK_EQ(report.cook.failed, size_t{1});
}

NF_TEST(packager_rebuild_does_not_leave_stale_files) {
    PackSandbox sb("nf_pack_stale");
    if (!sb.ok) { NF_SKIP("template unavailable"); }
    if (std::string(NF_BASIC3D_SHADER_DIR).empty()) { NF_SKIP("no shader dir configured"); }

    // Add an extra asset, build, then remove it and rebuild.
    {
        std::ofstream f(sb.root / "Content" / "Materials" / "Extra.nfmat");
        f << "name: Extra\n";
    }
    BuildReport first;
    std::string err;
    NF_CHECK(build_project(sb.options(), first, err));
    NF_CHECK(std::filesystem::exists(sb.dist() / "Cache" / "Materials" / "Extra.nfmat"));

    std::filesystem::remove(sb.root / "Content" / "Materials" / "Extra.nfmat");
    BuildReport second;
    NF_CHECK(build_project(sb.options(), second, err));
    // A removed asset must not survive in the package, or the shipped build
    // carries data the project no longer contains.
    NF_CHECK(!std::filesystem::exists(sb.dist() / "Cache" / "Materials" / "Extra.nfmat"));
}

NF_TEST(packager_reports_a_missing_shader_directory) {
    PackSandbox sb("nf_pack_noshaders");
    if (!sb.ok) { NF_SKIP("template unavailable"); }

    auto opts = sb.options();
    opts.shader_dir = sb.root / "does_not_exist";
    BuildReport report;
    std::string err;
    NF_CHECK(!build_project(opts, report, err));
    NF_CHECK(err.find("shader directory does not exist") != std::string::npos);
}

NF_TEST(packager_reports_a_missing_project_file) {
    BuildOptions opts;
    opts.project_file = std::filesystem::temp_directory_path() / "nf_no_such_project.nfproj";
    BuildReport report;
    std::string err;
    NF_CHECK(!build_project(opts, report, err));
    NF_CHECK(!err.empty());
}

NF_TEST(packager_honours_a_custom_output_directory) {
    PackSandbox sb("nf_pack_outdir");
    if (!sb.ok) { NF_SKIP("template unavailable"); }
    if (std::string(NF_BASIC3D_SHADER_DIR).empty()) { NF_SKIP("no shader dir configured"); }

    const auto custom = sb.root / "out" / "custom";
    BuildReport report;
    std::string err;
    NF_CHECK(build_project(sb.options(custom.string()), report, err));
    NF_CHECK(std::filesystem::exists(custom / "Demo.nfproj"));
    NF_CHECK(std::filesystem::exists(custom / "manifest.txt"));
    NF_CHECK_EQ(report.output_dir.string(), custom.lexically_normal().string());
    // The default dist/ must not also have been populated.
    NF_CHECK(!std::filesystem::exists(sb.dist() / "manifest.txt"));
}
