#pragma once

// Samples/MedievalVillage/KitAssets.hpp — the Medieval Village MegaKit, loaded
// through the engine's own asset pipeline.
//
// The kit ships as 176 glTF building pieces. Tools/import_kit.sh converts the
// curated set this game uses into the engine's cooked mesh format (.nfmesh,
// MeshAsset v1) and writes a manifest beside them:
//
//   piece <name> mesh=<rel .nfmesh> tex=<BaseColor stem> min=x,y,z max=x,y,z tris=<n>
//
// This class reads that manifest, walks each mesh through the engine's runtime
// mesh path (assets::MeshAsset -> rendering::make_static_mesh -> MeshLibrary ->
// GPU upload) and uploads one BaseColor map per texture into a material
// instance. The game then asks for a piece by name and gets a mesh handle, a
// material handle and the piece's measured bounds.
//
// The bounds are the reason the manifest carries them at all. A kit like this is
// authored in metres but its pieces are not the size their names suggest —
// Roof_RoundTiles_4x4 is 5.5 m wide, not 4 — so the village layout is driven by
// what the importer measured, never by a guess in the layout table.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/Handles.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nf::rendering {
class MeshLibrary;
class Renderer3D;
} // namespace nf::rendering

namespace nf::sample::medieval {

/// One kit piece: its file, its material, its measured local bounds, and the
/// library handles it resolved to.
struct KitPiece {
    std::string name;    // "Wall_Plaster_Straight"
    std::string mesh_rel;// "Meshes/Wall_Plaster_Straight.nfmesh", relative to content
    std::string texture; // "T_Plaster_BaseColor" (stem, no extension)
    Vec3 bounds_min{};
    Vec3 bounds_max{};
    u32 triangles = 0;

    rendering::StaticMeshHandle mesh{};
    rendering::MaterialHandle material{};

    Vec3 size() const { return bounds_max - bounds_min; }
    Vec3 centre() const { return (bounds_min + bounds_max) * 0.5f; }
    bool valid() const { return mesh.valid() && material.valid(); }
};

/// The loaded kit. Owns the GPU textures it created; meshes and materials live
/// in the libraries the game passed in.
class Kit {
public:
    struct LoadReport {
        usize pieces = 0;
        usize meshes_uploaded = 0;
        usize textures_uploaded = 0;
        usize materials_created = 0;
        std::vector<std::string> failures;
        bool ok() const { return failures.empty() && pieces > 0; }
    };

    /// Parses <content_dir>/MedievalKit.manifest, uploads every mesh and every
    /// distinct BaseColor map, and creates one material instance per texture.
    ///
    /// `meshes` must already be the library the renderer resolves handles
    /// against (Renderer3D::set_mesh_library), because the handles handed out
    /// here are only meaningful to that library. Returns false and fills
    /// `out_error` when the manifest is missing or nothing loaded; individual
    /// piece failures are collected in report().failures and do not abort the
    /// load — a village missing one prop still beats no village.
    bool load(const std::filesystem::path& content_dir,
              rhi::IGraphicsDevice& device,
              rendering::Renderer3D& renderer,
              rendering::MeshLibrary& meshes,
              std::string& out_error);

    const KitPiece* piece(std::string_view name) const;
    const std::vector<KitPiece>& pieces() const { return m_pieces; }
    const LoadReport& report() const { return m_report; }

private:
    struct TextureEntry {
        std::unique_ptr<rhi::Texture> texture;
        std::unique_ptr<rhi::TextureView> view;
        std::unique_ptr<rhi::Sampler> sampler;
    };

    std::vector<KitPiece> m_pieces;
    std::unordered_map<std::string, usize> m_index;
    std::unordered_map<std::string, TextureEntry> m_textures;
    LoadReport m_report;
};

} // namespace nf::sample::medieval
