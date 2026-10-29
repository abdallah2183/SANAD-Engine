#include <NF/Physics/PhysicsMaterial.hpp>
#include <algorithm>

namespace nf::physics {

PhysicsMaterial PhysicsMaterial::standard() {
    PhysicsMaterial m;
    m.name = "Standard";
    m.static_friction = 0.5f;
    m.dynamic_friction = 0.4f;
    m.restitution = 0.1f;
    m.density = 1000.0f;
    return m;
}

PhysicsMaterial PhysicsMaterial::ice() {
    PhysicsMaterial m;
    m.name = "Ice";
    m.static_friction = 0.05f;
    m.dynamic_friction = 0.03f;
    m.restitution = 0.05f;
    m.density = 917.0f;
    m.friction_combine = CombineMode::Minimum;
    return m;
}

PhysicsMaterial PhysicsMaterial::rubber() {
    PhysicsMaterial m;
    m.name = "Rubber";
    m.static_friction = 0.9f;
    m.dynamic_friction = 0.8f;
    m.restitution = 0.85f;
    m.density = 1100.0f;
    m.restitution_combine = CombineMode::Maximum;
    return m;
}

PhysicsMaterial PhysicsMaterial::metal() {
    PhysicsMaterial m;
    m.name = "Metal";
    m.static_friction = 0.4f;
    m.dynamic_friction = 0.3f;
    m.restitution = 0.2f;
    m.density = 7800.0f;
    return m;
}

PhysicsMaterial PhysicsMaterial::wood() {
    PhysicsMaterial m;
    m.name = "Wood";
    m.static_friction = 0.6f;
    m.dynamic_friction = 0.5f;
    m.restitution = 0.15f;
    m.density = 700.0f;
    return m;
}

static f32 combine_scalar(f32 val_a, f32 val_b, CombineMode mode_a, CombineMode mode_b) {
    // Mode priority: Maximum > Multiply > Minimum > Average
    const CombineMode effective_mode = std::max(mode_a, mode_b);
    switch (effective_mode) {
    case CombineMode::Average:
        return (val_a + val_b) * 0.5f;
    case CombineMode::Minimum:
        return std::min(val_a, val_b);
    case CombineMode::Multiply:
        return val_a * val_b;
    case CombineMode::Maximum:
        return std::max(val_a, val_b);
    }
    return (val_a + val_b) * 0.5f;
}

CombinedMaterialProperties combine_materials(const PhysicsMaterial& a, const PhysicsMaterial& b) {
    CombinedMaterialProperties res;
    res.static_friction = std::max(0.0f, combine_scalar(a.static_friction, b.static_friction, a.friction_combine, b.friction_combine));
    res.dynamic_friction = std::max(0.0f, combine_scalar(a.dynamic_friction, b.dynamic_friction, a.friction_combine, b.friction_combine));
    res.restitution = std::clamp(combine_scalar(a.restitution, b.restitution, a.restitution_combine, b.restitution_combine), 0.0f, 1.0f);
    return res;
}

} // namespace nf::physics
