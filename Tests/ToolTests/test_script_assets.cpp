// Tests/ToolTests/test_script_assets.cpp — Phase 24 (Lua as a first-class asset)
//
// Scripts are content the cooker must copy and the registry must type: a .lua
// the cooker refuses to cook is a script the packaged game silently loses.

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Project/ProjectCooker.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace nf;
using namespace nf::assets;
using namespace nf::project;

#ifndef NF_TEMPLATE_DIR
    #define NF_TEMPLATE_DIR ""
#endif

NF_TEST(cooker_cooks_lua_as_a_script_asset) {
    auto tmp = std::filesystem::temp_directory_path() / "nf_cook_lua";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp / "Content" / "Scripts");
    std::filesystem::create_directories(tmp / "Cache");

    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    NF_CHECK(vfs.write_text("content://Scripts/spin.lua", "function update(dt) end\n").ok);

    AssetRegistry registry;
    std::string err;
    bool skipped = false;
    NF_CHECK(cook_one(vfs, registry, "content://Scripts/spin.lua", "cache://Scripts/spin.lua", err, skipped));
    NF_CHECK(err.empty());
    NF_CHECK(!skipped);
    const AssetMetadata* meta = registry.find_by_path("content://Scripts/spin.lua");
    NF_CHECK(meta != nullptr);
    if (meta != nullptr) {
        NF_CHECK(meta->type == AssetType::Script);
        NF_CHECK_EQ(meta->format, std::string("lua-v1"));
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(default_template_ships_a_runnable_example_script) {
    const std::string template_dir = NF_TEMPLATE_DIR;
    if (template_dir.empty()) {
        NF_SKIP("no template dir configured");
        return;
    }
    const auto example = std::filesystem::path(template_dir) / "Content" / "Scripts" / "example.lua";
    NF_CHECK(std::filesystem::exists(example));
    if (!std::filesystem::exists(example)) {
        return;
    }
    std::ifstream in(example);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    NF_CHECK(text.find("function update(dt)") != std::string::npos);
}
