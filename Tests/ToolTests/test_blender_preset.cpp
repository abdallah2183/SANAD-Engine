// Tests/ToolTests/test_blender_preset.cpp — the shipped Blender export preset.
//
// `Templates/Blender/nf_gltf_export.py` is the "one-click" half of the character
// pipeline, and no test on this machine can run it: Blender is not a dependency.
// What a test *can* pin is the part that breaks silently — the option names the
// add-on passes to `bpy.ops.export_scene.gltf`. An unknown keyword makes the
// operator raise TypeError, so the export never runs and the developer sees a
// one-click path that simply does nothing.
//
// Three of these names are easy to write wrong, and were wrong in the first
// revision of this file's subject: `use_selection` (not
// `export_selected_objects`), `export_force_sampling` (not `export_sampling`)
// and `export_def_bones` (not `export_def_bones_only`). Verified against
// Blender's `bpy.ops.export_scene.gltf` API reference.
//
// These are contract checks on the shipped script, not a substitute for opening
// Blender once — see Docs/Blender_Pipeline.md §4 for what stays manual.

#include <NF/Test/TestFramework.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace nf;

#ifndef NF_BLENDER_PRESET
    #define NF_BLENDER_PRESET ""
#endif

namespace {

std::string read_preset() {
    if (std::string(NF_BLENDER_PRESET).empty()) return {};
    std::ifstream in(std::filesystem::path(NF_BLENDER_PRESET), std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool has(const std::string& src, const char* needle) {
    return src.find(needle) != std::string::npos;
}

} // namespace

NF_TEST(blender_preset_passes_only_option_names_blender_declares) {
    const std::string src = read_preset();
    if (src.empty()) NF_SKIP("Blender preset not configured or missing");

    // The three names the exporter actually declares.
    NF_CHECK(has(src, "\"use_selection\""));
    NF_CHECK(has(src, "\"export_force_sampling\""));
    NF_CHECK(has(src, "\"export_def_bones\""));

    // The three that look plausible and do not exist. Any one of them turns the
    // one-click export into a TypeError.
    NF_CHECK(!has(src, "\"export_selected_objects\""));
    NF_CHECK(!has(src, "\"export_sampling\""));
    NF_CHECK(!has(src, "\"export_def_bones_only\""));
}

NF_TEST(blender_preset_is_installable_and_reports_dropped_options) {
    const std::string src = read_preset();
    if (src.empty()) NF_SKIP("Blender preset not configured or missing");

    // A Blender add-on: metadata, a class, and a register/unregister pair that
    // puts the entry in File > Export.
    NF_CHECK(has(src, "bl_info"));
    NF_CHECK(has(src, "bpy.utils.register_class"));
    NF_CHECK(has(src, "bpy.utils.unregister_class"));
    NF_CHECK(has(src, "TOPBAR_MT_file_export.append"));

    // The version-drift guard. Blender renames exporter options between
    // releases; the preset drops the ones the installed build does not declare
    // and NAMES them, so a lost setting is visible instead of silent. If this
    // guard is removed the export starts hard-failing on version drift, so pin
    // it here.
    NF_CHECK(has(src, "get_rna_type()"));
    NF_CHECK(has(src, "\"WARNING\""));
    NF_CHECK(has(src, "has no %s"));
}
