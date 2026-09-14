#pragma once

// NF/Rendering/Handles.hpp — stable, backend-neutral handles for render assets
//
// Handles decouple the ECS/gameplay layer from asset storage: components carry
// a handle, libraries own the actual objects. Handles are indices into a
// library, so they stay valid as long as the library entry does and never
// dangle like raw pointers would when assets stream in and out.

#include <NF/Core/Types.hpp>

namespace nf::rendering {

struct StaticMeshHandle {
    u32 id = u32_max;
    bool valid() const { return id != u32_max; }
    bool operator==(const StaticMeshHandle& o) const { return id == o.id; }
    bool operator!=(const StaticMeshHandle& o) const { return !(*this == o); }
};

struct MaterialHandle {
    u32 id = u32_max;
    bool valid() const { return id != u32_max; }
    bool operator==(const MaterialHandle& o) const { return id == o.id; }
    bool operator!=(const MaterialHandle& o) const { return !(*this == o); }
};

constexpr StaticMeshHandle kInvalidMeshHandle{};
constexpr MaterialHandle kInvalidMaterialHandle{};

} // namespace nf::rendering
