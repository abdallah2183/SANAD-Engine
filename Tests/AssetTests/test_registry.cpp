// Tests/AssetTests/test_registry.cpp — AssetRegistry tests

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>

using namespace nf;
using namespace nf::assets;

NF_TEST(registry_save_load_round_trip) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_reg_test";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    AssetRegistry reg;
    AssetMetadata meta;
    meta.id = AssetId::generate();
    meta.type = AssetType::Mesh;
    meta.logical_path = "content://Meshes/cube.nfmesh";
    meta.cooked_path = "cache://Meshes/cube.nfmesh";
    meta.fingerprint = "abc123";
    meta.format = "nfmesh-v1";
    std::string err;
    NF_CHECK(reg.add(meta, err));

    NF_CHECK(reg.save(vfs, "content://AssetRegistry.nfreg", err));
    NF_CHECK(err.empty());

    AssetRegistry loaded;
    NF_CHECK(loaded.load(vfs, "content://AssetRegistry.nfreg", err));
    NF_CHECK(loaded.size()==1);
    auto* found = loaded.find(meta.id);
    NF_CHECK(found && found->logical_path == meta.logical_path);
    NF_CHECK(loaded.version()==AssetRegistry::kCurrentVersion);

    std::filesystem::remove_all(tmp);
}

NF_TEST(registry_duplicate_ids) {
    AssetRegistry reg;
    AssetMetadata m1; m1.id = AssetId::generate(); m1.type=AssetType::Mesh; m1.logical_path="content://a.nfmesh"; m1.cooked_path="cache://a.nfmesh"; m1.fingerprint="111";
    AssetMetadata m2 = m1; m2.logical_path="content://b.nfmesh";
    std::string err;
    NF_CHECK(reg.add(m1, err));
    NF_CHECK(!reg.add(m2, err)); // duplicate id should fail
    NF_CHECK(err.find("Duplicate AssetId") != std::string::npos);

    AssetRegistry reg2;
    AssetMetadata m3; m3.id = AssetId::generate(); m3.type=AssetType::Mesh; m3.logical_path="content://a.nfmesh"; m3.cooked_path="cache://a.nfmesh"; m3.fingerprint="222";
    AssetMetadata m4; m4.id = AssetId::generate(); m4.type=AssetType::Mesh; m4.logical_path="content://a.nfmesh"; m4.cooked_path="cache://b.nfmesh"; m4.fingerprint="333";
    NF_CHECK(reg2.add(m3, err));
    NF_CHECK(!reg2.add(m4, err)); // duplicate logical_path should fail
}

NF_TEST(registry_invalid_corrupt) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_reg_corrupt";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);
    // Write a corrupt registry file
    vfs.write_text("content://AssetRegistry.nfreg", "not a valid registry");
    AssetRegistry reg;
    std::string err;
    NF_CHECK(!reg.load(vfs, "content://AssetRegistry.nfreg", err));
    NF_CHECK(!err.empty());

    // Write a registry with unsupported version
    vfs.write_text("content://AssetRegistry.nfreg", "# NOVAForge Asset Registry\nversion: 999\ncount: 0\n");
    NF_CHECK(!reg.load(vfs, "content://AssetRegistry.nfreg", err));
    NF_CHECK(err.find("Unsupported registry version") != std::string::npos);

    std::filesystem::remove_all(tmp);
}
