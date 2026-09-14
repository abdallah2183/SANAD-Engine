// Tests/AssetTests/test_project_descriptor.cpp — project descriptor parsing and mounts
//
// The descriptor exists because the runtime used to *guess* its project by
// walking up the filesystem for a directory containing both Engine/ and
// Content/ — the engine source tree layout. A shipped game has neither, so
// these tests pin the properties that make an explicit declaration work:
// relocatability, refusal to escape the project root, and loud rejection of a
// malformed or future-versioned file instead of a half-parse.

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/ProjectDescriptor.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>

#include <filesystem>
#include <fstream>

using namespace nf;
using namespace nf::assets;

namespace {

std::filesystem::path proj_temp_dir(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

std::optional<ProjectDescriptor> parse_text(const std::string& text,
                                           const std::filesystem::path& base,
                                           std::string& err) {
    return ProjectDescriptor::parse(text, base, err);
}

} // namespace

NF_TEST(project_descriptor_parses_minimal_and_applies_defaults) {
    const auto base = proj_temp_dir("nf_proj_minimal");
    std::string err;
    auto d = parse_text("version: 1\nname: Demo\n", base, err);
    NF_CHECK(d.has_value());
    if (!d) return;

    NF_CHECK_EQ(d->name(), std::string("Demo"));
    // Title falls back to the name so a window always has something to show.
    NF_CHECK_EQ(d->title(), std::string("Demo"));
    NF_CHECK_EQ(d->startup_scene(), std::string("content://Scenes/Main.nfscene"));
    NF_CHECK_EQ(d->window_width(), 1280u);
    NF_CHECK_EQ(d->window_height(), 720u);

    // The four standard mounts exist without being written in the file.
    NF_CHECK(d->has_mount("project://"));
    NF_CHECK(d->has_mount("content://"));
    NF_CHECK(d->has_mount("cache://"));
    NF_CHECK(d->has_mount("shaders://"));
    // engine:// is deliberately NOT defaulted: it only means anything inside the
    // engine source tree, so a project that needs it must say so.
    NF_CHECK(!d->has_mount("engine://"));

    NF_CHECK(d->mount_path("content://") == (base / "Content").lexically_normal());
    std::filesystem::remove_all(base);
}

NF_TEST(project_descriptor_rejects_missing_version) {
    const auto base = proj_temp_dir("nf_proj_noversion");
    std::string err;
    auto d = parse_text("name: Demo\n", base, err);
    NF_CHECK(!d.has_value());
    NF_CHECK(err.find("version") != std::string::npos);
    std::filesystem::remove_all(base);
}

NF_TEST(project_descriptor_rejects_future_version) {
    const auto base = proj_temp_dir("nf_proj_future");
    std::string err;
    // A newer file must be refused outright, not half-parsed: silently dropping
    // fields we do not understand is how a project opens "mostly fine" and then
    // renders the wrong thing.
    auto d = parse_text("version: 99\nname: Demo\n", base, err);
    NF_CHECK(!d.has_value());
    NF_CHECK(err.find("unsupported project version") != std::string::npos);
    std::filesystem::remove_all(base);
}

NF_TEST(project_descriptor_rejects_missing_name) {
    const auto base = proj_temp_dir("nf_proj_noname");
    std::string err;
    auto d = parse_text("version: 1\n", base, err);
    NF_CHECK(!d.has_value());
    NF_CHECK(err.find("name") != std::string::npos);
    std::filesystem::remove_all(base);
}

NF_TEST(project_descriptor_rejects_malformed_mount) {
    const auto base = proj_temp_dir("nf_proj_badmount");
    std::string err;

    // No '->' separator.
    auto d1 = parse_text("version: 1\nname: D\nmount: content:// Content\n", base, err);
    NF_CHECK(!d1.has_value());
    NF_CHECK(err.find("->") != std::string::npos);

    // Prefix without '://'.
    auto d2 = parse_text("version: 1\nname: D\nmount: content -> Content\n", base, err);
    NF_CHECK(!d2.has_value());

    // No path.
    auto d3 = parse_text("version: 1\nname: D\nmount: content:// ->\n", base, err);
    NF_CHECK(!d3.has_value());

    std::filesystem::remove_all(base);
}

NF_TEST(project_descriptor_rejects_duplicate_mount) {
    const auto base = proj_temp_dir("nf_proj_dupmount");
    std::string err;
    auto d = parse_text("version: 1\nname: D\nmount: content:// -> A\nmount: content:// -> B\n",
                        base, err);
    NF_CHECK(!d.has_value());
    NF_CHECK(err.find("duplicate mount") != std::string::npos);
    std::filesystem::remove_all(base);
}

NF_TEST(project_descriptor_rejects_traversal_mount) {
    const auto base = proj_temp_dir("nf_proj_traversal");
    std::string err;
    // A project file must not be able to point a mount outside its own
    // directory by climbing with '..'.
    auto d = parse_text("version: 1\nname: D\nmount: content:// -> ../../etc\n", base, err);
    NF_CHECK(!d.has_value());
    NF_CHECK(err.find("escapes the project root") != std::string::npos);
    std::filesystem::remove_all(base);
}

NF_TEST(project_descriptor_explicit_mount_overrides_default) {
    const auto base = proj_temp_dir("nf_proj_override");
    std::string err;
    auto d = parse_text("version: 1\nname: D\nmount: content:// -> Assets\n", base, err);
    NF_CHECK(d.has_value());
    if (!d) return;

    NF_CHECK(d->mount_path("content://") == (base / "Assets").lexically_normal());
    // Overriding one mount must not disturb the others.
    NF_CHECK(d->mount_path("cache://") == (base / "Cache").lexically_normal());
    // And it must not appear twice.
    size_t content_mounts = 0;
    for (const auto& m : d->mounts()) {
        if (m.logical == "content://") ++content_mounts;
    }
    NF_CHECK_EQ(content_mounts, size_t{1});
    std::filesystem::remove_all(base);
}

NF_TEST(project_descriptor_ignores_unknown_keys_and_comments) {
    const auto base = proj_temp_dir("nf_proj_unknown");
    std::string err;
    auto d = parse_text("# a comment\nversion: 1\nname: D\nfuture_field: whatever\n"
                        "\n   \nwindow_width: 800\n",
                        base, err);
    // An older engine must still open a file that a newer one wrote.
    NF_CHECK(d.has_value());
    if (!d) return;
    NF_CHECK_EQ(d->window_width(), 800u);
    std::filesystem::remove_all(base);
}

NF_TEST(project_descriptor_round_trips_through_file) {
    const auto base = proj_temp_dir("nf_proj_roundtrip");
    auto desc = ProjectDescriptor::make_default(base, "RoundTrip");
    desc.set_window(1920, 1080);
    desc.set_startup_scene("content://Scenes/Level1.nfscene");

    const auto file = base / "RoundTrip.nfproj";
    std::string err;
    NF_CHECK(desc.save_to_file(file, err));
    NF_CHECK(std::filesystem::exists(file));

    auto loaded = ProjectDescriptor::load_from_file(file, err);
    NF_CHECK(loaded.has_value());
    if (!loaded) return;

    NF_CHECK_EQ(loaded->name(), std::string("RoundTrip"));
    NF_CHECK_EQ(loaded->window_width(), 1920u);
    NF_CHECK_EQ(loaded->window_height(), 1080u);
    NF_CHECK_EQ(loaded->startup_scene(), std::string("content://Scenes/Level1.nfscene"));
    NF_CHECK_EQ(loaded->mounts().size(), desc.mounts().size());
    NF_CHECK(loaded->mount_path("content://") == desc.mount_path("content://"));
    std::filesystem::remove_all(base);
}

NF_TEST(project_descriptor_is_relocatable) {
    const auto dir_a = proj_temp_dir("nf_proj_reloc_a");
    const auto dir_b = proj_temp_dir("nf_proj_reloc_b");

    auto desc = ProjectDescriptor::make_default(dir_a, "Reloc");
    std::string err;
    NF_CHECK(desc.save_to_file(dir_a / "Reloc.nfproj", err));

    // Move the project directory. Mounts are written relative to the descriptor,
    // so they must follow it rather than still pointing at the old location.
    std::filesystem::copy(dir_a / "Reloc.nfproj", dir_b / "Reloc.nfproj",
                          std::filesystem::copy_options::overwrite_existing);

    auto moved = ProjectDescriptor::load_from_file(dir_b / "Reloc.nfproj", err);
    NF_CHECK(moved.has_value());
    if (!moved) return;

    const auto content = moved->mount_path("content://").string();
    NF_CHECK(content.find("nf_proj_reloc_b") != std::string::npos);
    NF_CHECK(content.find("nf_proj_reloc_a") == std::string::npos);

    std::filesystem::remove_all(dir_a);
    std::filesystem::remove_all(dir_b);
}

NF_TEST(project_descriptor_apply_mounts_drives_the_vfs) {
    const auto base = proj_temp_dir("nf_proj_apply");
    std::filesystem::create_directories(base / "Content" / "Scenes");
    {
        std::ofstream f(base / "Content" / "Scenes" / "Main.nfscene");
        f << "version: 1\n";
    }

    auto desc = ProjectDescriptor::make_default(base, "Apply");
    VirtualFileSystem vfs;
    std::string err;
    NF_CHECK(desc.apply_mounts(vfs, err));

    auto resolved = vfs.resolve("content://Scenes/Main.nfscene");
    NF_CHECK(resolved.ok);
    NF_CHECK(std::filesystem::exists(resolved.value));

    // The mounts the project declares are exactly the ones the VFS ends up with.
    NF_CHECK_EQ(vfs.mounts().size(), desc.mounts().size());

    std::filesystem::remove_all(base);
}

NF_TEST(project_descriptor_apply_mounts_names_the_failing_mount) {
    auto desc = ProjectDescriptor::make_default(
        std::filesystem::temp_directory_path() / "nf_proj_apply_fail", "Fail");
    VirtualFileSystem vfs;
    std::string err;

    // Declaring the same prefix twice on one VFS is the cheapest way to make a
    // mount fail. The error must name the mount: "VFS setup failed" alone is
    // useless when a project declares five of them.
    NF_CHECK(desc.apply_mounts(vfs, err));
    std::string second_err;
    NF_CHECK(!desc.apply_mounts(vfs, second_err));
    NF_CHECK(second_err.find("project://") != std::string::npos);

    std::filesystem::remove_all(std::filesystem::temp_directory_path() / "nf_proj_apply_fail");
}

NF_TEST(project_descriptor_load_reports_a_missing_file) {
    std::string err;
    auto d = ProjectDescriptor::load_from_file(
        std::filesystem::temp_directory_path() / "nf_does_not_exist.nfproj", err);
    NF_CHECK(!d.has_value());
    NF_CHECK(!err.empty());
}
