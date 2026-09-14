#include <NF/Rendering/Shader.hpp>
#include <NF/Core/Logger.hpp>

namespace nf::rendering {

Shader::Shader(ShaderId id, std::string path, rhi::ShaderStage stage,
               std::vector<u8> spirv, std::unique_ptr<rhi::ShaderModule> module,
               ReflectionData reflection, uint64_t version)
    : m_id(id), m_path(std::move(path)), m_stage(stage),
      m_spirv(std::move(spirv)), m_module(std::move(module)),
      m_reflection(std::move(reflection)), m_version(version) {}

bool Shader::update(std::vector<u8> new_spirv, std::unique_ptr<rhi::ShaderModule> new_module, ReflectionData new_reflection) {
    if (!new_module || new_spirv.empty() || !new_reflection.valid) {
        NF_LOG_WARN(LogCategory::Core, "Shader::update: rejected invalid update for '{}'", m_path);
        return false;
    }
    m_spirv = std::move(new_spirv);
    m_module = std::move(new_module);
    m_reflection = std::move(new_reflection);
    ++m_version;
    NF_LOG_INFO(LogCategory::Core, "Shader '{}' updated to version {}", m_path, m_version);
    return true;
}

} // namespace nf::rendering
