#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/PackageArchive.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <filesystem>
#include <fstream>

using namespace nf;
using namespace nf::assets;

NF_TEST(package_archive_in_memory_build_and_read) {
    PackageBuilder builder;
    builder.add_file_text("Configs/settings.json", "{\"resolution\": [1920, 1080], \"fullscreen\": true}");
    builder.add_file_text("Shaders/pbr.frag", "// PBR Fragment Shader code");

    std::vector<uint8_t> raw_bytes = {0x00, 0xFF, 0x55, 0xAA, 0x12, 0x34};
    builder.add_file("Textures/noise.dat", raw_bytes);

    NF_CHECK_EQ(builder.file_count(), 3u);

    std::vector<uint8_t> archive_data = builder.build_to_memory();
    NF_CHECK(!archive_data.empty());

    std::string err;
    auto archive = PackageArchive::open_memory(archive_data, err);
    NF_CHECK(archive != nullptr);
    NF_CHECK(err.empty());

    NF_CHECK_EQ(archive->file_count(), 3u);
    NF_CHECK(archive->contains("Configs/settings.json"));
    NF_CHECK(archive->contains("Shaders/pbr.frag"));
    NF_CHECK(archive->contains("Textures/noise.dat"));
    NF_CHECK(!archive->contains("NonExistent.file"));

    std::string config_text = archive->read_text("Configs/settings.json");
    NF_CHECK(config_text.find("1920") != std::string::npos);

    std::vector<uint8_t> texture_bytes = archive->read_bytes("Textures/noise.dat");
    NF_CHECK_EQ(texture_bytes.size(), raw_bytes.size());
    NF_CHECK_EQ(texture_bytes[0], 0x00);
    NF_CHECK_EQ(texture_bytes[1], 0xFF);
    NF_CHECK_EQ(texture_bytes[5], 0x34);

    const PackageEntry* entry = archive->get_entry("Shaders/pbr.frag");
    NF_CHECK(entry != nullptr);
    NF_CHECK(entry->crc32 != 0);
}

NF_TEST(package_archive_corrupt_crc_rejected) {
    PackageBuilder builder;
    builder.add_file_text("hello.txt", "Hello SANAD Engine");
    auto archive_data = builder.build_to_memory();

    // Corrupt the TOC section
    archive_data.back() ^= 0xFF;

    std::string err;
    auto archive = PackageArchive::open_memory(archive_data, err);
    NF_CHECK(archive == nullptr);
    NF_CHECK(!err.empty());
}

NF_TEST(package_archive_disk_file_round_trip) {
    auto temp_dir = std::filesystem::temp_directory_path() / "nf_package_test";
    std::filesystem::create_directories(temp_dir);

    auto pack_path = temp_dir / "assets.nfpak";

    PackageBuilder builder;
    builder.add_file_text("Levels/Level01.nfscene", "Scene: name=\"Level01\"\nEntity: id=1");
    builder.add_file_text("Scripts/main.lua", "print('Hello Lua from NFPAK')");

    std::string err;
    bool built = builder.build_to_file(pack_path, err);
    NF_CHECK(built);
    NF_CHECK(std::filesystem::exists(pack_path));

    auto archive = PackageArchive::open_file(pack_path, err);
    NF_CHECK(archive != nullptr);
    NF_CHECK_EQ(archive->file_count(), 2u);

    std::string scene_text = archive->read_text("Levels/Level01.nfscene");
    NF_CHECK(scene_text.find("Level01") != std::string::npos);

    // Clean up
    archive.reset();
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
}

NF_TEST(vfs_package_archive_mounting) {
    auto temp_dir = std::filesystem::temp_directory_path() / "nf_vfs_pack_test";
    std::filesystem::create_directories(temp_dir);

    auto pack_path = temp_dir / "game_data.nfpak";

    PackageBuilder builder;
    builder.add_file_text("Meshes/Hero.nfmesh", "NFMESH_BINARY_DATA_STUB");
    builder.add_file_text("Materials/Gold.nfmat", "Material: albedo=[1.0, 0.84, 0.0]");
    builder.add_file_text("Audio/Click.wav", "RIFF_WAV_STUB");

    std::string err;
    NF_CHECK(builder.build_to_file(pack_path, err));

    VirtualFileSystem vfs;
    auto mount_res = vfs.mount_package("content://", pack_path);
    NF_CHECK(mount_res.ok);
    NF_CHECK_EQ(vfs.package_mounts().size(), 1u);

    // Verify transparent file existence through VFS
    auto ex_res = vfs.exists("content://Meshes/Hero.nfmesh");
    NF_CHECK(ex_res.ok);
    NF_CHECK(ex_res.value);

    auto missing_res = vfs.exists("content://Meshes/Missing.nfmesh");
    NF_CHECK(missing_res.ok);
    NF_CHECK(!missing_res.value);

    // Verify transparent reading of text
    auto mat_res = vfs.read_text("content://Materials/Gold.nfmat");
    NF_CHECK(mat_res.ok);
    NF_CHECK(mat_res.value.find("albedo") != std::string::npos);

    // Verify transparent reading of raw bytes
    auto audio_res = vfs.read_bytes("content://Audio/Click.wav");
    NF_CHECK(audio_res.ok);
    NF_CHECK_EQ(audio_res.value.size(), 13u);

    // Verify directory detection inside package
    auto is_dir = vfs.is_directory("content://Meshes");
    NF_CHECK(is_dir.ok);
    NF_CHECK(is_dir.value);

    // Unmount
    vfs.unmount_package("content://");
    NF_CHECK_EQ(vfs.package_mounts().size(), 0u);

    auto after_unmount = vfs.exists("content://Meshes/Hero.nfmesh");
    NF_CHECK(!after_unmount.ok || !after_unmount.value);

    // Clean up
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
}

NF_TEST(vfs_overlay_precedence_physical_over_package) {
    auto temp_dir = std::filesystem::temp_directory_path() / "nf_vfs_overlay_test";
    auto pack_path = temp_dir / "base.nfpak";
    auto content_dir = temp_dir / "Content";
    std::filesystem::create_directories(content_dir);

    // Package has version 1
    PackageBuilder builder;
    builder.add_file_text("config.txt", "VERSION=1 (from package)");
    std::string err;
    NF_CHECK(builder.build_to_file(pack_path, err));

    // Physical disk folder has version 2 (mod/override)
    {
        std::ofstream disk_file(content_dir / "config.txt");
        disk_file << "VERSION=2 (from physical disk override)";
    }

    VirtualFileSystem vfs;
    // Mount physical directory
    NF_CHECK(vfs.mount("content://", content_dir).ok);
    // Mount package archive
    NF_CHECK(vfs.mount_package("content://", pack_path).ok);

    // Physical disk file must take precedence over package
    auto read_res = vfs.read_text("content://config.txt");
    NF_CHECK(read_res.ok);
    NF_CHECK(read_res.value.find("physical disk override") != std::string::npos);

    // Clean up
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
}
