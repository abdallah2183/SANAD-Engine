// Tools/ModelImporter/main.cpp — NFModelImporter CLI (Phase 14).
//
// Converts glTF 2.0 sources (.gltf + .bin, .glb) into the engine's cooked
// mesh format (.nfmesh, MeshAsset v1). Thin front end: parsing/conversion
// live in NFAssets (GltfImport) and are unit-tested there.
//
// Usage:
//   NFModelImporter --input <model.gltf|model.glb> --output <mesh.nfmesh> [--mesh N|all]
//   NFModelImporter --input <model.gltf|model.glb> --info
//
// Multi-mesh files: default writes mesh 0; --mesh N picks one; --mesh all
// writes <stem>_<i>.nfmesh beside the output path.
//
// --info prints the full import report (meshes, materials, skins/joints,
// clips, and every skip count) without writing anything — the visible half of
// the "no silent format substitution" rule.

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
              << "  NFModelImporter --input <model.gltf|model.glb> --info\n"
              << "\n"
              << "Examples:\n"
              << "  NFModelImporter --input Content/Models/Box.glb --output Content/Meshes/Box.nfmesh\n"
              << "  NFModelImporter --input scene.gltf --output out.nfmesh --mesh all\n"
              << "  NFModelImporter --input Hero.glb --info\n";
}

// Prints the whole import result — including everything the importer could not
// use. Nothing here is optional: a static mesh says so, and every skip is
// counted. This is the CLI half of the no-silent-substitution rule.
void print_report(const GltfImportResult& r, const std::string& source) {
    std::cout << "\n=== NFModelImporter report: " << source << " ===\n";

    std::cout << "meshes: " << r.meshes.size() << "\n";
    for (usize i = 0; i < r.meshes.size(); ++i) {
        const MeshAsset& m = *r.meshes[i];
        std::cout << "  [" << i << "] "
                  << (m.logical_path.empty() ? std::string("<unnamed>") : m.logical_path) << ": "
                  << m.vertices.size() << " verts, " << m.indices.size() / 3 << " tris, "
                  << m.submeshes.size() << " submesh(es)\n";
        std::cout << "      bounds min (" << m.bounds.min_x << ", " << m.bounds.min_y << ", "
                  << m.bounds.min_z << ") max (" << m.bounds.max_x << ", " << m.bounds.max_y
                  << ", " << m.bounds.max_z << ")\n";
        for (usize s = 0; s < m.submeshes.size(); ++s) {
            const AssetSubMesh& sub = m.submeshes[s];
            std::cout << "      submesh " << s << ": material slot " << sub.material_slot << ", "
                      << sub.index_count / 3 << " tris, " << sub.vertex_count << " verts\n";
        }
        if (i < r.mesh_skins.size() && r.mesh_skins[i].skin_index >= 0) {
            const GltfMeshSkin& ms = r.mesh_skins[i];
            std::cout << "      skin: skin " << ms.skin_index << ", " << ms.vertex_count
                      << " bound verts\n";
        } else {
            std::cout << "      skin: none — static mesh (skin_index = -1)\n";
        }
    }

    std::cout << "materials: " << r.materials.size() << "\n";
    for (usize i = 0; i < r.materials.size(); ++i) {
        const GltfMaterialInfo& mt = r.materials[i];
        std::cout << "  [" << i << "] " << (mt.name.empty() ? std::string("<unnamed>") : mt.name)
                  << ": baseColor (" << mt.base_color[0] << ", " << mt.base_color[1] << ", "
                  << mt.base_color[2] << ", " << mt.base_color[3] << "), metallic " << mt.metallic
                  << ", roughness " << mt.roughness << "\n";
    }

    std::cout << "skins: " << r.skins.size() << "\n";
    for (usize i = 0; i < r.skins.size(); ++i) {
        const GltfSkinInfo& sk = r.skins[i];
        std::cout << "  [" << i << "] " << (sk.name.empty() ? std::string("<unnamed>") : sk.name)
                  << ": " << sk.joint_nodes.size() << " joint(s), root node " << sk.root_node
                  << "\n";
        for (usize j = 0; j < sk.joint_nodes.size(); ++j) {
            const int node = sk.joint_nodes[j];
            const bool in_range = node >= 0 && static_cast<usize>(node) < r.nodes.size();
            const std::string jn = in_range ? r.nodes[static_cast<usize>(node)].name
                                            : std::string("<out of range>");
            std::cout << "      joint " << j << " -> node " << node << " '" << jn << "'\n";
        }
    }

    std::cout << "animations: " << r.animations.size() << "\n";
    for (usize i = 0; i < r.animations.size(); ++i) {
        const GltfAnimationInfo& a = r.animations[i];
        std::cout << "  [" << i << "] " << (a.name.empty() ? std::string("<unnamed>") : a.name)
                  << ": duration " << a.duration << "s, " << a.channels.size() << " channel(s)\n";
    }

    std::cout << "skipped (counted, never silently substituted):\n";
    std::cout << "  primitives (non-triangle / unusable): " << r.primitives_skipped << "\n";
    std::cout << "  animation channels (CUBICSPLINE / morph / unsupported): "
              << r.anim_channels_skipped << "\n";
    std::cout << "  skin bindings rejected (malformed JOINTS_0/WEIGHTS_0): "
              << r.skin_bindings_rejected << "\n";
    std::cout << "nodes: " << r.nodes.size() << "\n";
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
    bool info_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if ((arg == "--input") && i + 1 < argc) {
            input = argv[++i];
        } else if ((arg == "--output") && i + 1 < argc) {
            output = argv[++i];
        } else if ((arg == "--mesh") && i + 1 < argc) {
            mesh_sel = argv[++i];
        } else if (arg == "--info") {
            info_only = true;
        } else if (arg == "--help" || arg == "-h") {
            print_help();
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            print_help();
            return 2;
        }
    }
    if (input.empty() || (output.empty() && !info_only)) {
        print_help();
        return 2;
    }

    GltfImportResult result = import_gltf_file(input);
    if (!result.ok) {
        std::cerr << "import failed: " << result.error << "\n";
        return 1;
    }

    if (info_only) {
        print_report(result, input);
        if (output.empty()) return 0; // report only; nothing to cook
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
