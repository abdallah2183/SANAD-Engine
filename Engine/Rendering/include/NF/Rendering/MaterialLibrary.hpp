#pragma once

// NF/Rendering/MaterialLibrary.hpp — PBR parameters + Material instances
//
// Material → MaterialInstance → Parameters:
//
//   A Material owns the pipeline configuration (shaders, layouts, render
//   pass) — one rhi::Pipeline per unique configuration, shared via the
//   PipelineCache. A MaterialInstance (an entry in this library) references a
//   Material and owns only per-instance data: its PBR parameter block, stored
//   in a small uniform buffer.
//
// Changing BaseColor / Metallic / Roughness / AO / Emission rewrites the
// instance's parameter buffer. It never touches the pipeline, never touches
// the PipelineCache, and never allocates GPU memory beyond the initial
// 48-byte block — that is the contract the tests enforce.
//
// The parameter block is backend-neutral (plain floats); the descriptor
// plumbing lives in Material/MaterialInstance above the RHI abstraction.

#include <NF/Core/Types.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/Handles.hpp>
#include <NF/Rendering/Material.hpp>

#include <memory>
#include <string>
#include <vector>

namespace nf::rendering {

/// PBR surface parameters, exposed by the gbuffer shader. Kept as a plain
/// 12-float block so it maps 1:1 onto the shader's std140 uniform:
///   vec4 baseColor;  vec4(metallic, roughness, ao, emissionStrength);  vec4 emission.rgb, useBaseColorTex
struct PBRMaterialParams {
    float base_color[4] = {0.8f, 0.2f, 0.2f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.8f;
    float ao = 1.0f;
    float emission[3] = {0.0f, 0.0f, 0.0f};
    float emission_strength = 0.0f;
    float use_base_color_texture = 0.0f;

    /// Packs into the exact 12-float uniform layout (std140-safe order).
    void pack(float out[12]) const {
        out[0] = base_color[0]; out[1] = base_color[1];
        out[2] = base_color[2]; out[3] = base_color[3];
        out[4] = metallic; out[5] = roughness; out[6] = ao; out[7] = emission_strength;
        out[8] = emission[0]; out[9] = emission[1]; out[10] = emission[2];
        out[11] = use_base_color_texture;
    }
};

static_assert(sizeof(PBRMaterialParams) == 12 * sizeof(float),
              "PBRMaterialParams must stay a dense 12-float block");

class Material;

/// One registered material instance: a shared pipeline + private parameters.
struct MaterialEntry {
    Material* material = nullptr;          // non-owning; pipeline is shared
    PBRMaterialParams params;
    std::unique_ptr<rhi::Buffer> params_ubo; // persistent 48-byte uniform block
    const rhi::TextureView* albedo_view = nullptr; // optional texture binding
    const rhi::Sampler* sampler = nullptr;
    std::string name;

    /// Descriptor set binding (params_ubo, albedo_view), cached across frames.
    ///
    /// Material instances are shared by every object that references them, so
    /// building this set per object per frame meant N allocations and N
    /// vkUpdateDescriptorSets for what is a handful of distinct materials.
    /// The set is built by the renderer (which owns the layout) and only
    /// rebuilt when the binding changes.
    ///
    /// `set_dirty` is set here rather than destroying the set directly, because
    /// a material edit can happen mid-frame while the previous frame's command
    /// buffer still references it. The renderer rebuilds at a point where it
    /// has already waited on the frame fence.
    std::unique_ptr<rhi::DescriptorSet> cached_set;
    bool set_dirty = true;
};

class MaterialLibrary {
public:
    explicit MaterialLibrary(rhi::IGraphicsDevice& device);
    ~MaterialLibrary() = default;

    MaterialLibrary(const MaterialLibrary&) = delete;
    MaterialLibrary& operator=(const MaterialLibrary&) = delete;

    /// Registers an instance sharing `material`'s pipeline. Allocates only the
    /// 48-byte parameter buffer — never a pipeline, never a descriptor set at
    /// creation time (the renderer builds `cached_set` lazily, on first draw).
    MaterialHandle create_instance(Material& material, const PBRMaterialParams& params,
                                   std::string name = "material");

    MaterialEntry* get(MaterialHandle handle);
    const MaterialEntry* get(MaterialHandle handle) const;

    /// Updates parameters in place. Cheap by design: one host-visible buffer
    /// write. Changing parameters must never create or invalidate a pipeline.
    void set_params(MaterialHandle handle, const PBRMaterialParams& params);

    const PBRMaterialParams* params(MaterialHandle handle) const;

    /// Optional per-instance albedo texture (binding 1 of the material layout).
    /// The view/sampler must outlive the library entry.
    void set_albedo_texture(MaterialHandle handle, const rhi::TextureView& view,
                             const rhi::Sampler& sampler);

    /// Removes the albedo binding (scalar base color again). Needed so texture
    /// reload/unassign never leaves a dangling view behind.
    void clear_albedo_texture(MaterialHandle handle);

    size_t size() const { return m_entries.size(); }

private:
    rhi::IGraphicsDevice* m_device = nullptr;
    std::vector<std::unique_ptr<MaterialEntry>> m_entries;
};

} // namespace nf::rendering
