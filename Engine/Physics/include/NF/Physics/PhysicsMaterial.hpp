#pragma once

#include <NF/Core/Types.hpp>
#include <string>

namespace nf::physics {

enum class CombineMode : u8 {
    Average = 0,
    Minimum = 1,
    Multiply = 2,
    Maximum = 3
};

struct PhysicsMaterial {
    std::string name = "Default";
    f32 static_friction = 0.5f;
    f32 dynamic_friction = 0.4f;
    f32 restitution = 0.1f;
    f32 density = 1000.0f; // kg/m^3 (water = 1000)
    CombineMode friction_combine = CombineMode::Average;
    CombineMode restitution_combine = CombineMode::Average;

    static PhysicsMaterial standard();
    static PhysicsMaterial ice();
    static PhysicsMaterial rubber();
    static PhysicsMaterial metal();
    static PhysicsMaterial wood();
};

struct CombinedMaterialProperties {
    f32 static_friction = 0.5f;
    f32 dynamic_friction = 0.4f;
    f32 restitution = 0.1f;
};

/// Combine two materials according to their configured combine modes
CombinedMaterialProperties combine_materials(const PhysicsMaterial& a, const PhysicsMaterial& b);

} // namespace nf::physics
