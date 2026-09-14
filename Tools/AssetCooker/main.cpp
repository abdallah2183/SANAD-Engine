// Tools/AssetCooker/main.cpp — NFAssetCooker CLI
// v0.1: supports .nfmesh and .spv, registry management, fingerprint/skip, verify, help

#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/AssetId.hpp>
#include <NF/Assets/AssetTypes.hpp>
#include <NF/Assets/MeshAsset.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Core/UUID.hpp>
#include <NF/Core/Hash.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::assets;

static void print_help() {
    std::cout << "NFAssetCooker — NOVAForge Asset Cooker v0.1\n"
              << "Usage:\n"
              << "  NFAssetCooker --input <logical> --output <logical> --registry <logical>\n"
              << "  NFAssetCooker --verify --registry <logical>\n"
              << "  NFAssetCooker --help\n"
              << "\n"
              << "Examples:\n"
              << "  NFAssetCooker --input content://Meshes/cube.nfmesh --output cache://Meshes/cube.nfmesh --registry content://AssetRegistry.nfreg\n"
              << "  NFAssetCooker --verify --registry content://AssetRegistry.nfreg\n"
              << "\n"
              << "VFS mounts (auto-detected):\n"
              << "  engine://  -> Engine/\n"
              << "  project:// -> <project root>\n"
              << "  content:// -> Content/ (or project/Content)\n"
              << "  cache://   -> Cache/ or build/cache\n"
              << "\n";
}

static std::string compute_fingerprint(const std::vector<u8>& data) {
    // Use FNV-1a 64-bit hash as fingerprint (hex)
    uint64_t hash = 14695981039346656037ULL;
    for (u8 b : data) {
        hash ^= b;
        hash *= 1099511628211ULL;
    }
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)hash);
    return std::string(buf);
}

static std::string compute_file_fingerprint(const std::filesystem::path& physical_path, std::string& out_error) {
    std::ifstream in(physical_path, std::ios::binary | std::ios::ate);
    if (!in) { out_error = "Failed to open file: " + physical_path.string(); return {}; }
    auto size = static_cast<size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    std::vector<u8> data(size);
    if (size>0) {
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
        if (!in) { out_error = "Failed to read file: " + physical_path.string(); return {}; }
    }
    return compute_fingerprint(data);
}

static bool setup_vfs(VirtualFileSystem& vfs, std::string& out_error) {
    // Auto-detect project root as current directory or parent of executable
    std::filesystem::path project_root = std::filesystem::current_path();
    // Try to find the project root by looking for CMakeLists.txt or Engine/ directory
    for (int i=0;i<5;++i) {
        if (std::filesystem::exists(project_root / "CMakeLists.txt") && std::filesystem::exists(project_root / "Engine")) break;
        auto parent = project_root.parent_path();
        if (parent == project_root) break;
        project_root = parent;
    }

    std::filesystem::path engine_path = project_root / "Engine";
    std::filesystem::path content_path = project_root / "Content";
    if (!std::filesystem::exists(content_path)) {
        // Try project/Content
        content_path = project_root / "Content";
        if (!std::filesystem::exists(content_path)) {
            // Fallback to current dir's Content
            content_path = std::filesystem::current_path() / "Content";
        }
    }
    std::filesystem::path cache_path = project_root / "Cache";
    if (!std::filesystem::exists(cache_path)) {
        cache_path = project_root / "build" / "cache";
    }

    std::string err;
    auto r1 = vfs.mount("engine://", engine_path);
    if (!r1.ok) { out_error = r1.error; return false; }
    auto r2 = vfs.mount("project://", project_root);
    if (!r2.ok) { out_error = r2.error; return false; }
    auto r3 = vfs.mount("content://", content_path);
    if (!r3.ok) {
        // Content may not exist yet, create it
        std::filesystem::create_directories(content_path);
        r3 = vfs.mount("content://", content_path);
        if (!r3.ok) { out_error = r3.error; return false; }
    }
    auto r4 = vfs.mount("cache://", cache_path);
    if (!r4.ok) {
        std::filesystem::create_directories(cache_path);
        r4 = vfs.mount("cache://", cache_path);
        if (!r4.ok) { out_error = r4.error; return false; }
    }
    return true;
}

int main(int argc, char** argv) {
    // Setup logger
    Logger::instance().add_sink(Logger::make_console_sink());
    Logger::instance().set_min_level(LogLevel::Info);

    if (argc == 1) { print_help(); return 0; }

    std::string input, output, registry;
    bool verify = false;
    bool help = false;

    for (int i=1;i<argc;++i){
        std::string_view arg = argv[i];
        if (arg=="--input" && i+1<argc) input = argv[++i];
        else if (arg.rfind("--input=",0)==0) input = std::string(arg.substr(8));
        else if (arg=="--output" && i+1<argc) output = argv[++i];
        else if (arg.rfind("--output=",0)==0) output = std::string(arg.substr(9));
        else if (arg=="--registry" && i+1<argc) registry = argv[++i];
        else if (arg.rfind("--registry=",0)==0) registry = std::string(arg.substr(11));
        else if (arg=="--verify") verify = true;
        else if (arg=="--help" || arg=="-h") help = true;
        else if (arg=="--version") { std::cout << "NFAssetCooker v0.1\n"; return 0; }
        else { std::cerr << "Unknown argument: " << arg << "\n"; print_help(); return 1; }
    }

    if (help) { print_help(); return 0; }

    VirtualFileSystem vfs;
    std::string vfs_err;
    if (!setup_vfs(vfs, vfs_err)) {
        std::cerr << "VFS setup failed: " << vfs_err << "\n";
        return 1;
    }

    if (verify) {
        if (registry.empty()) { std::cerr << "--verify requires --registry\n"; return 1; }
        AssetRegistry reg;
        std::string err;
        if (!reg.load(vfs, registry, err)) {
            std::cerr << "Failed to load registry '" << registry << "': " << err << "\n";
            return 1;
        }
        size_t total = reg.size();
        size_t ok = 0, missing = 0, bad_format = 0;
        for (auto& [id, meta] : reg.entries()) {
            auto r = vfs.exists(meta.cooked_path);
            if (!r.ok || !r.value) {
                std::cerr << "Missing cooked file for '" << meta.logical_path << "' -> '" << meta.cooked_path << "'\n";
                ++missing;
                continue;
            }
            // For .nfmesh, verify it can be loaded
            if (meta.type == AssetType::Mesh) {
                auto data_res = vfs.read_bytes(meta.cooked_path);
                if (!data_res.ok) {
                    std::cerr << "Failed to read cooked file '" << meta.cooked_path << "': " << data_res.error << "\n";
                    ++bad_format;
                    continue;
                }
                std::string parse_err;
                auto asset = MeshAsset::load_from_bytes(std::span<const u8>(data_res.value), parse_err);
                if (!asset) {
                    std::cerr << "Corrupt .nfmesh '" << meta.cooked_path << "': " << parse_err << "\n";
                    ++bad_format;
                    continue;
                }
            }
            ++ok;
        }
        std::cout << "Verify: total " << total << ", ok " << ok << ", missing " << missing << ", bad_format " << bad_format << "\n";
        return (missing==0 && bad_format==0) ? 0 : 1;
    }

    // Cook single asset
    if (input.empty() || output.empty() || registry.empty()) {
        std::cerr << "--input, --output, and --registry are required for cooking\n";
        print_help();
        return 1;
    }

    // Load registry (if it exists, otherwise start empty)
    AssetRegistry reg;
    std::string reg_err;
    bool reg_exists = false;
    {
        auto r = vfs.exists(registry);
        if (r.ok && r.value) {
            if (!reg.load(vfs, registry, reg_err)) {
                std::cerr << "Failed to load existing registry '" << registry << "': " << reg_err << "\n";
                return 1;
            }
            reg_exists = true;
        } else {
            // No registry yet, start empty
            reg.set_version(AssetRegistry::kCurrentVersion);
        }
    }

    // Resolve input and output
    auto in_resolve = vfs.resolve(input);
    if (!in_resolve.ok) { std::cerr << "Failed to resolve input '" << input << "': " << in_resolve.error << "\n"; return 1; }
    auto out_resolve = vfs.resolve(output);
    if (!out_resolve.ok) { std::cerr << "Failed to resolve output '" << output << "': " << out_resolve.error << "\n"; return 1; }

    // Check input exists and compute fingerprint
    std::string fp_err;
    std::string fingerprint = compute_file_fingerprint(in_resolve.value, fp_err);
    if (fingerprint.empty()) {
        std::cerr << "Failed to compute fingerprint for input '" << input << "': " << fp_err << "\n";
        return 1;
    }

    // Determine asset type from input extension
    AssetType type = AssetType::Unknown;
    std::string input_str = input;
    if (input_str.ends_with(".nfmesh")) type = AssetType::Mesh;
    else if (input_str.ends_with(".spv")) type = AssetType::Shader;
    else if (input_str.ends_with(".nfscene")) type = AssetType::Scene;
    else {
        // For v0.1, try to infer from output
        if (output.ends_with(".nfmesh")) type = AssetType::Mesh;
        else if (output.ends_with(".spv")) type = AssetType::Shader;
        else {
            std::cerr << "Unknown asset type for input '" << input << "' (expected .nfmesh or .spv)\n";
            return 1;
        }
    }

    // Check if we can skip (asset exists with same fingerprint and cooked file is valid)
    const AssetMetadata* existing = reg.find_by_path(input);
    bool should_skip = false;
    if (existing && existing->fingerprint == fingerprint) {
        auto out_exists = vfs.exists(output);
        if (out_exists.ok && out_exists.value) {
            // Also verify the cooked file's fingerprint matches (for .nfmesh, we could recompute, but for now just check existence)
            should_skip = true;
        }
    }

    size_t cooked=0, skipped=0, failed=0;
    if (should_skip) {
        std::cout << "Skipped (unchanged): " << input << " -> " << output << "\n";
        skipped = 1;
    } else {
        // For v0.1, cooking is just copying the file from input to output (since .nfmesh is already cooked)
        // But we need to ensure the cooked file is written correctly and the registry is updated
        auto read_res = vfs.read_bytes(input);
        if (!read_res.ok) {
            std::cerr << "Failed to read input '" << input << "': " << read_res.error << "\n";
            failed = 1;
        } else {
            // For .nfmesh, validate that it can be parsed
            if (type == AssetType::Mesh) {
                std::string parse_err;
                auto asset = MeshAsset::load_from_bytes(std::span<const u8>(read_res.value), parse_err);
                if (!asset) {
                    std::cerr << "Input file is not a valid .nfmesh: " << parse_err << "\n";
                    failed = 1;
                } else {
                    // Write to output via VFS
                    auto write_res = vfs.write_bytes(output, std::span<const u8>(read_res.value));
                    if (!write_res.ok) {
                        std::cerr << "Failed to write output '" << output << "': " << write_res.error << "\n";
                        failed = 1;
                    } else {
                        // Update registry
                        AssetMetadata meta;
                        if (existing) {
                            meta = *existing;
                            meta.fingerprint = fingerprint;
                            meta.cooked_path = output;
                            // Remove old and re-add to update
                            reg.remove(existing->id);
                            std::string add_err;
                            if (!reg.add(meta, add_err)) {
                                std::cerr << "Failed to update registry: " << add_err << "\n";
                                failed = 1;
                            } else {
                                cooked = 1;
                            }
                        } else {
                            meta.id = AssetId::generate();
                            meta.type = type;
                            meta.logical_path = input;
                            meta.cooked_path = output;
                            meta.fingerprint = fingerprint;
                            meta.format = (type==AssetType::Mesh ? "nfmesh-v1" : "spv-v1");
                            meta.version = 1;
                            std::string add_err;
                            if (!reg.add(meta, add_err)) {
                                std::cerr << "Failed to add to registry: " << add_err << "\n";
                                failed = 1;
                            } else {
                                cooked = 1;
                            }
                        }
                    }
                }
            } else {
                // For .spv, just copy
                auto write_res = vfs.write_bytes(output, std::span<const u8>(read_res.value));
                if (!write_res.ok) {
                    std::cerr << "Failed to write output '" << output << "': " << write_res.error << "\n";
                    failed = 1;
                } else {
                    AssetMetadata meta;
                    if (existing) {
                        meta = *existing;
                        meta.fingerprint = fingerprint;
                        meta.cooked_path = output;
                        reg.remove(existing->id);
                        std::string add_err;
                        if (!reg.add(meta, add_err)) { std::cerr << "Failed to update registry: " << add_err << "\n"; failed=1; } else cooked=1;
                    } else {
                        meta.id = AssetId::generate();
                        meta.type = type;
                        meta.logical_path = input;
                        meta.cooked_path = output;
                        meta.fingerprint = fingerprint;
                        meta.format = "spv-v1";
                        std::string add_err;
                        if (!reg.add(meta, add_err)) { std::cerr << "Failed to add to registry: " << add_err << "\n"; failed=1; } else cooked=1;
                    }
                }
            }
        }
    }

    // Save registry atomically
    if (cooked>0) {
        std::string save_err;
        if (!reg.save(vfs, registry, save_err)) {
            std::cerr << "Failed to save registry '" << registry << "': " << save_err << "\n";
            return 1;
        }
    }

    std::cout << "Cook report: cooked " << cooked << ", skipped " << skipped << ", failed " << failed << ", total " << (cooked+skipped+failed) << "\n";
    return (failed==0) ? 0 : 1;
}
