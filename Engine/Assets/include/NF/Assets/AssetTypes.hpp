#pragma once

#include <NF/Assets/AssetId.hpp>

#include <string>

namespace nf::assets {

enum class AssetType : uint8_t {
    Unknown = 0,
    Mesh = 1,
    Shader = 2,
    Scene = 3,
    Texture = 4,
    Material = 5,
};

inline std::string to_string(AssetType type) {
    switch (type) {
        case AssetType::Mesh: return "Mesh";
        case AssetType::Shader: return "Shader";
        case AssetType::Scene: return "Scene";
        case AssetType::Texture: return "Texture";
        case AssetType::Material: return "Material";
        default: return "Unknown";
    }
}

inline AssetType asset_type_from_string(const std::string& s) {
    if (s=="Mesh") return AssetType::Mesh;
    if (s=="Shader") return AssetType::Shader;
    if (s=="Scene") return AssetType::Scene;
    if (s=="Texture") return AssetType::Texture;
    if (s=="Material") return AssetType::Material;
    return AssetType::Unknown;
}

struct AssetMetadata {
    AssetId id;
    AssetType type = AssetType::Unknown;
    std::string logical_path;  // e.g. content://Meshes/cube.nfmesh
    std::string cooked_path;   // e.g. cache://Meshes/cube.nfmesh (logical)
    std::string fingerprint;   // hex hash of source content
    std::string format = "nfmesh-v1"; // or nfscene-v1, spv-v1
    uint32_t version = 1;
};

} // namespace nf::assets
