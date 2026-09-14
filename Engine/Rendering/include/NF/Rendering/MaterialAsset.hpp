#pragma once

// NF/Rendering/MaterialAsset.hpp — text material asset (.nfmat, v1).
//
// A material asset is a NAMED set of PBR parameters shared by every entity
// assigned to it (MeshComponent::material holds the logical path, e.g.
// "content://Materials/Default"). Editing parameters rewrites the instance's
// 48-byte UBO only — never a pipeline, never the PipelineCache (the contract
// MaterialLibrary documents and its tests enforce).
//
// The format is line-based and tolerant: unknown lines are ignored, missing
// lines keep defaults, so older files always load. Pure parse/serialize here;
// file IO lives with the VFS owners (Runtime/Editor), keeping Rendering free
// of the Assets module.

#include <NF/Rendering/MaterialLibrary.hpp>

#include <string>

namespace nf::rendering {

struct MaterialAsset {
    std::string name = "Material";
    PBRMaterialParams params;
    // Optional albedo image (content:// logical path, "" = scalar only).
    std::string albedo;

    // Parses .nfmat text. Returns false with err only when the header is not
    // a material file at all; unknown/partial content loads with defaults.
    static bool load_from_text(const std::string& text, MaterialAsset& out, std::string& out_err);
    std::string save_to_text() const;
};

} // namespace nf::rendering
