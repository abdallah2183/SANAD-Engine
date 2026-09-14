#pragma once

// NF/Rendering/Shader.hpp — Shader asset with reflection and hot-reload support

#include <NF/Core/Types.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/ShaderReflection.hpp>

#include <memory>
#include <string>
#include <vector>

namespace nf::rendering {

using ShaderId = u32;
static constexpr ShaderId kInvalidShaderId = u32_max;

class Shader {
public:
    Shader(ShaderId id, std::string path, rhi::ShaderStage stage,
           std::vector<u8> spirv, std::unique_ptr<rhi::ShaderModule> module,
           ReflectionData reflection, uint64_t version = 1);

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    ShaderId id() const { return m_id; }
    const std::string& path() const { return m_path; }
    rhi::ShaderStage stage() const { return m_stage; }
    const std::vector<u8>& spirv() const { return m_spirv; }
    rhi::ShaderModule* module() const { return m_module.get(); }
    const ReflectionData& reflection() const { return m_reflection; }
    uint64_t version() const { return m_version; }

    // Hot-reload: replace spirv/module/reflection with new version.
    // Returns true on success, false if the new spirv is invalid (old kept).
    bool update(std::vector<u8> new_spirv, std::unique_ptr<rhi::ShaderModule> new_module, ReflectionData new_reflection);

private:
    ShaderId m_id = kInvalidShaderId;
    std::string m_path;
    rhi::ShaderStage m_stage = rhi::ShaderStage::Vertex;
    std::vector<u8> m_spirv;
    std::unique_ptr<rhi::ShaderModule> m_module;
    ReflectionData m_reflection;
    uint64_t m_version = 0;
};

} // namespace nf::rendering
