// Tools/ModelImporter/main.cpp — NFModelImporter CLI (Phase 14).
//
// Converts glTF 2.0 sources (.gltf + .bin, .glb) into the engine's cooked
// mesh format (.nfmesh, MeshAsset v1). Thin front end: parsing/conversion
// live in NFAssets (GltfImport) and are unit-tested there.
//
// Usage:
//   NFModelImporter --input <model.gltf|model.glb> --output <mesh.nfmesh> [--mesh N|all]
//
// Multi-mesh files: default writes mesh 0; --mesh N picks one; --mesh all
// writes <stem>_<i>.nfmesh beside the output path.

#include <NF/Assets/GltfImport.hpp>
#include <NF/Assets/MeshAsset.hpp>
#include <NF/Core/Logger.hpp>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

using namespace nf;
using namespace nf::assets;

namespace {

void print_help() {
    std::cout << "NFModelImporter — NOVAForge glTF importer\n"
              << "Usage:\n"
              << "  NFModelImporter --input <model.gltf|model.glb> --output <mesh.nfmesh> [--mesh N|all]\n"
              << "\n"
              << "Examples:\n"
              << "  NFModelImporter --input Content/Models/Box.glb --output Content/Meshes/Box.nfmesh\n"
              << "  NFModelImporter --input scene.gltf --output out.nfmesh --mesh all\n";
}

bool save_mesh(const MeshAsset& mesh, const std::string& path, std::string& err) {
    if (!mesh.save_to_file(path, err)) {
        return false;
    }
    std::cout << "wrote " << path << " (" << mesh.vertices.size() << " verts, "
              << mesh.indices.size() / 3 << " tris, " << mesh.submeshes.size()
              << " submeshes)\n";
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Logger::instance().add_sink(Logger::make_console_sink());

    std::string input;
    std::string output;
    std::string mesh_sel = "0";
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if ((arg == "--input") && i + 1 < argc) {
            input = argv[++i];
        } else if ((arg == "--output") && i + 1 < argc) {
            output = argv[++i];
        } else if ((arg == "--mesh") && i + 1 < argc) {
            mesh_sel = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_help();
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            print_help();
            return 2;
        }
    }
    if (input.empty() || output.empty()) {
        print_help();
        return 2;
    }

    GltfImportResult result = import_gltf_file(input);
    if (!result.ok) {
        std::cerr << "import failed: " << result.error << "\n";
        return 1;
    }
    if (result.meshes.empty()) {
        std::cerr << "import ok but no triangle meshes found (" << result.primitives_skipped
                  << " primitives skipped)\n";
        return 1;
    }
    std::cout << "imported " << result.meshes.size() << " mesh(es), "
              << result.materials.size() << " material(s), " << result.nodes.size()
              << " node(s)\n";

    std::string err;
    if (mesh_sel == "all") {
        const std::filesystem::path out_path(output);
        const std::string stem = out_path.stem().string();
        const std::string ext = out_path.extension().string();
        const std::filesystem::path dir = out_path.parent_path();
        for (usize i = 0; i < result.meshes.size(); ++i) {
            std::filesystem::path p = dir / (stem + "_" + std::to_string(i) + ext);
            if (!save_mesh(*result.meshes[i], p.string(), err)) {
                std::cerr << "save failed: " << err << "\n";
                return 1;
            }
        }
        return 0;
    }

    usize index = 0;
    try {
        index = static_cast<usize>(std::stoul(mesh_sel));
    } catch (...) {
        std::cerr << "invalid --mesh selector: " << mesh_sel << " (expected N or all)\n";
        return 2;
    }
    if (index >= result.meshes.size()) {
        std::cerr << "mesh index " << index << " out of range (" << result.meshes.size()
                  << " meshes)\n";
        return 1;
    }
    if (!save_mesh(*result.meshes[index], output, err)) {
        std::cerr << "save failed: " << err << "\n";
        return 1;
    }
    return 0;
}
