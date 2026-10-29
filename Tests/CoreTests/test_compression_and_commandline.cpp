#include <NF/Test/TestFramework.hpp>
#include <NF/Core/Compression.hpp>
#include <NF/Core/CommandLine.hpp>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::core;

NF_TEST(compression_rle_sparse_and_roundtrip) {
    // 500 zeroes, followed by 100 0xFFs, followed by alternating 1, 2, 3
    std::vector<u8> original(500, 0);
    original.insert(original.end(), 100, 0xFF);
    for (int i = 0; i < 50; ++i) {
        original.push_back(static_cast<u8>(i % 10));
    }

    auto compressed = Compression::compress_rle(original.data(), original.size());
    NF_CHECK(!compressed.empty());
    // 650 bytes should compress to ~67 bytes
    NF_CHECK(compressed.size() < 100);

    std::vector<u8> decompressed;
    bool ok = Compression::decompress_rle(compressed.data(), compressed.size(), decompressed);
    NF_CHECK(ok);
    NF_CHECK_EQ(decompressed.size(), original.size());
    NF_CHECK(decompressed == original);
}

NF_TEST(compression_lzss_repetitive_text_and_binary) {
    std::string text = "SANAD_ENGINE_SANAD_ENGINE_SANAD_CORE_ENGINE_SANAD_CORE_ENGINE_SANAD_VFS_SANAD_VFS";
    auto compressed = Compression::compress_lzss(text.data(), text.size());

    NF_CHECK(!compressed.empty());
    NF_CHECK(compressed.size() < text.size());

    std::vector<u8> decompressed;
    bool ok = Compression::decompress_lzss(compressed.data(), compressed.size(), decompressed, text.size());
    NF_CHECK(ok);
    std::string decomp_text(reinterpret_cast<const char*>(decompressed.data()), decompressed.size());
    NF_CHECK_EQ(decomp_text, text);
}

NF_TEST(compression_lzss_large_binary_buffer) {
    std::vector<u8> original(8000);
    for (size_t i = 0; i < original.size(); ++i) {
        original[i] = static_cast<u8>((i * 3 + (i / 16)) % 256);
    }

    auto compressed = Compression::compress(original.data(), original.size(), CompressionType::LZSS);
    NF_CHECK(!compressed.empty());

    std::vector<u8> decompressed;
    bool ok = Compression::decompress(compressed.data(), compressed.size(), decompressed, CompressionType::LZSS, original.size());
    NF_CHECK(ok);
    NF_CHECK_EQ(decompressed.size(), original.size());
    NF_CHECK(decompressed == original);
}

NF_TEST(commandline_flags_and_options_parsing) {
    CommandLineParser parser("SANAD Engine Server");
    parser.add_flag("v", "verbose", "Enable verbose diagnostic logs");
    parser.add_option("p", "port", "UDP listening port", "7777");
    parser.add_option("s", "scene", "Initial level scene", "Default.nfscene");

    std::vector<std::string> args = {
        "--verbose",
        "-p", "9999",
        "--scene=Levels/Arena.nfscene"
    };

    bool ok = parser.parse(args);
    NF_CHECK(ok);
    NF_CHECK(parser.has("verbose"));
    NF_CHECK(parser.has("v"));
    NF_CHECK_EQ(parser.get_int("port"), 9999);
    NF_CHECK_EQ(parser.get_string("scene"), "Levels/Arena.nfscene");
}

NF_TEST(commandline_positional_arguments_and_help) {
    CommandLineParser parser("Asset Compiler");
    parser.add_flag("f", "force", "Force recompile");
    parser.add_option("o", "out", "Output directory", "Cache/Cooked");

    std::vector<std::string> args = {
        "-f",
        "Content/Models/Hero.gltf",
        "Content/Models/Weapon.gltf"
    };

    bool ok = parser.parse(args);
    NF_CHECK(ok);
    NF_CHECK(parser.has("force"));
    NF_CHECK_EQ(parser.get_string("out"), "Cache/Cooked");

    const auto& pos = parser.positional_args();
    NF_CHECK_EQ(pos.size(), 2u);
    NF_CHECK_EQ(pos[0], "Content/Models/Hero.gltf");
    NF_CHECK_EQ(pos[1], "Content/Models/Weapon.gltf");

    std::string help = parser.help_text();
    NF_CHECK(!help.empty());
    NF_CHECK(help.find("Asset Compiler") != std::string::npos);
    NF_CHECK(help.find("force") != std::string::npos);
}
