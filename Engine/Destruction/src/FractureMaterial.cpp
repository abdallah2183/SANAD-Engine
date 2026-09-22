// NF/Destruction/src/FractureMaterial.cpp — named material preset lookup.

#include <NF/Destruction/FractureMaterial.hpp>
#include <NF/Destruction/DestructibleComponent.hpp>

#include <cctype>

namespace nf::destruction {

namespace {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0u; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

const FractureMaterial* find_material(std::string_view name) {
    if (name.empty()) return nullptr;
    for (const FractureMaterial& material : kFractureMaterials) {
        if (iequals(name, material.name)) return &material;
    }
    return nullptr;
}

void apply_material(DestructibleComponent& component, const FractureMaterial& material) {
    component.strength_scale = material.strength_scale;
    component.density = material.density;
    component.shard_lifetime = material.shard_lifetime;
}

} // namespace nf::destruction
