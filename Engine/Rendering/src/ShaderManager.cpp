#include <NF/Rendering/ShaderManager.hpp>
#include <NF/Rendering/ShaderReflection.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Jobs/JobSystem.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace nf::rendering {

ShaderManager::ShaderManager(rhi::IGraphicsDevice& device, PipelineCache& pipeline_cache)
    : m_device(device), m_pipeline_cache(pipeline_cache) {}

ShaderManager::~ShaderManager() = default;

bool ShaderManager::compile_glsl_to_spirv(const std::string& glsl_path, rhi::ShaderStage stage,
                                          std::vector<u8>& out_spirv, std::string& out_error) {
    out_spirv.clear();
    out_error.clear();

    std::string glslc;
    std::string vulkan_sdk_str;
#ifdef _MSC_VER
    char* buf = nullptr; size_t sz = 0;
    if (_dupenv_s(&buf, &sz, "VULKAN_SDK") == 0 && buf) { vulkan_sdk_str = buf; std::free(buf); }
#else
    if (const char* env = std::getenv("VULKAN_SDK")) vulkan_sdk_str = env;
#endif
    if (!vulkan_sdk_str.empty()) {
        std::filesystem::path p = std::filesystem::path(vulkan_sdk_str) / "Bin" / "glslc.exe";
        if (std::filesystem::exists(p)) glslc = p.string();
        else {
            p = std::filesystem::path(vulkan_sdk_str) / "bin" / "glslc";
            if (std::filesystem::exists(p)) glslc = p.string();
        }
    }
    if (glslc.empty()) {
        // Try common Windows install
        std::filesystem::path p = "C:/VulkanSDK/1.4.357.0/Bin/glslc.exe";
        if (std::filesystem::exists(p)) glslc = p.string();
    }
    if (glslc.empty() || !std::filesystem::exists(glslc)) {
        out_error = "glslc not found (VULKAN_SDK not set)";
        return false;
    }

    std::string stage_flag;
    switch (stage) {
        case rhi::ShaderStage::Vertex: stage_flag = "vert"; break;
        case rhi::ShaderStage::Fragment: stage_flag = "frag"; break;
        case rhi::ShaderStage::Compute: stage_flag = "comp"; break;
        default: stage_flag = "frag"; break;
    }

    // Output to temp file
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "nf_shader_compile.spv";
    std::string inner = "\"" + glslc + "\" -fshader-stage=" + stage_flag + " \"" + glsl_path + "\" -o \"" + tmp.string() + "\" 2>&1";
#ifdef _WIN32
    // _popen runs the string via `cmd /c`, which strips the FIRST and LAST
    // quote of a quoted command line. Wrapping the whole command in one
    // extra pair of quotes keeps the inner quoting intact; without it the
    // quoted exe path degenerates and cmd reports "syntax is incorrect".
    std::string cmd = "\"" + inner + "\"";
#else
    std::string cmd = inner;
#endif

    // Use _popen to capture output
    std::string compile_log;
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) {
        out_error = "Failed to run glslc";
        return false;
    }
    char line_buf[512];
    while (fgets(line_buf, sizeof(line_buf), pipe) != nullptr) compile_log += line_buf;
#ifdef _WIN32
    int ret = _pclose(pipe);
#else
    int ret = pclose(pipe);
#endif

    if (ret != 0) {
        out_error = compile_log.empty() ? "glslc failed" : compile_log;
        std::error_code remove_ec;
        std::filesystem::remove(tmp, remove_ec);
        return false;
    }

    if (!std::filesystem::exists(tmp)) {
        out_error = "glslc produced no output";
        return false;
    }

    std::ifstream file(tmp, std::ios::binary | std::ios::ate);
    if (!file) {
        out_error = "Failed to read compiled SPIR-V";
        std::error_code remove_ec;
        std::filesystem::remove(tmp, remove_ec);
        return false;
    }
    auto size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    out_spirv.resize(size);
    file.read(reinterpret_cast<char*>(out_spirv.data()), static_cast<std::streamsize>(size));
    std::error_code remove_ec;
    std::filesystem::remove(tmp, remove_ec);

    if (out_spirv.empty()) {
        out_error = "Compiled SPIR-V is empty";
        return false;
    }

    return true;
}

bool ShaderManager::compile_and_reflect(const std::string& path, rhi::ShaderStage stage,
                                        std::vector<u8>& out_spirv, ReflectionData& out_refl, std::string& out_error) {
    if (!compile_glsl_to_spirv(path, stage, out_spirv, out_error)) {
        return false;
    }
    out_refl = reflect_spirv(std::span<const u8>(out_spirv), stage);
    if (!out_refl.valid) {
        out_error = "Reflection failed";
        return false;
    }
    return true;
}

Shader* ShaderManager::load_internal(const std::string& path, rhi::ShaderStage stage, bool is_reload) {
    std::vector<u8> spirv;
    ReflectionData refl;
    std::string error;
    if (!compile_and_reflect(path, stage, spirv, refl, error)) {
        NF_LOG_ERROR(LogCategory::Core, "Shader compile failed '{}': {}", path, error);
        return nullptr;
    }

    rhi::ShaderModuleDesc mod_desc{ std::span<const u8>(spirv), stage };
    auto module = m_device.create_shader_module(mod_desc);
    if (!module) {
        NF_LOG_ERROR(LogCategory::Core, "Failed to create ShaderModule for '{}'", path);
        return nullptr;
    }

    if (is_reload) {
        auto it = m_shaders.find(path);
        if (it != m_shaders.end()) {
            Shader* shader = it->second.get();
            rhi::ShaderModule* old_module = shader->module();
            bool ok = shader->update(std::move(spirv), std::move(module), std::move(refl));
            if (ok && old_module) {
                size_t invalidated = m_pipeline_cache.invalidate_shader(old_module);
                NF_LOG_INFO(LogCategory::Core, "Hot reload '{}' invalidated {} pipelines", path, invalidated);
            }
            // Update file time
            std::error_code ec;
            m_file_times[path] = std::filesystem::last_write_time(path, ec);
            return ok ? shader : nullptr;
        }
        return nullptr;
    } else {
        ShaderId id = m_next_id++;
        auto shader = std::make_unique<Shader>(id, path, stage, std::move(spirv), std::move(module), std::move(refl), 1);
        Shader* raw = shader.get();
        m_shaders[path] = std::move(shader);
        m_id_to_path[id] = path;
        std::error_code ec;
        m_file_times[path] = std::filesystem::last_write_time(path, ec);
        NF_LOG_INFO(LogCategory::Core, "Shader loaded '{}' id={}", path, id);
        return raw;
    }
}

Shader* ShaderManager::load(const std::string& path, rhi::ShaderStage stage) {
    auto it = m_shaders.find(path);
    if (it != m_shaders.end()) return it->second.get();
    return load_internal(path, stage, false);
}

Shader* ShaderManager::get(const std::string& path) const {
    auto it = m_shaders.find(path);
    return it != m_shaders.end() ? it->second.get() : nullptr;
}

Shader* ShaderManager::get_by_id(ShaderId id) const {
    auto it = m_id_to_path.find(id);
    if (it == m_id_to_path.end()) return nullptr;
    return get(it->second);
}

bool ShaderManager::reload(const std::string& path) {
    auto it = m_shaders.find(path);
    if (it == m_shaders.end()) {
        NF_LOG_WARN(LogCategory::Core, "reload: shader '{}' not loaded", path);
        return false;
    }
    rhi::ShaderStage stage = it->second->stage();
    Shader* result = load_internal(path, stage, true);
    return result != nullptr;
}

void ShaderManager::reload_async(const std::string& path) {
    auto it = m_shaders.find(path);
    if (it == m_shaders.end()) return;
    rhi::ShaderStage stage = it->second->stage();
    // Enqueue compile job
    JobSystem::instance().enqueue([this, path, stage]() {
        std::vector<u8> spirv;
        ReflectionData refl;
        std::string error;
        bool ok = compile_and_reflect(path, stage, spirv, refl, error);
        PendingResult pr;
        pr.path = path;
        pr.stage = stage;
        pr.spirv = std::move(spirv);
        pr.reflection = std::move(refl);
        pr.success = ok;
        pr.error = error;
        {
            std::lock_guard<std::mutex> lock(m_pending_mutex);
            m_pending.push_back(std::move(pr));
        }
        if (!ok) {
            NF_LOG_WARN(LogCategory::Core, "Async compile failed '{}': {}", path, error);
        }
    });
}

void ShaderManager::poll() {
    for (auto& [path, shader] : m_shaders) {
        std::error_code ec;
        auto current_time = std::filesystem::last_write_time(path, ec);
        if (ec) continue;
        auto it = m_file_times.find(path);
        if (it == m_file_times.end() || current_time != it->second) {
            NF_LOG_INFO(LogCategory::Core, "File watcher detected change '{}'", path);
            reload_async(path);
            m_file_times[path] = current_time;
        }
    }
}

size_t ShaderManager::update() {
    std::vector<PendingResult> pending_copy;
    {
        std::lock_guard<std::mutex> lock(m_pending_mutex);
        pending_copy.swap(m_pending);
    }
    size_t updated = 0;
    for (auto& pr : pending_copy) {
        if (!pr.success) {
            NF_LOG_WARN(LogCategory::Core, "Shader update skipped (compile failed) '{}': {}", pr.path, pr.error);
            continue;
        }
        auto it = m_shaders.find(pr.path);
        if (it == m_shaders.end()) continue;
        Shader* shader = it->second.get();
        rhi::ShaderModule* old_module = shader->module();
        rhi::ShaderModuleDesc desc{ std::span<const u8>(pr.spirv), pr.stage };
        auto new_module = m_device.create_shader_module(desc);
        if (!new_module) {
            NF_LOG_ERROR(LogCategory::Core, "Failed to create ShaderModule for '{}' on update", pr.path);
            continue;
        }
        bool ok = shader->update(std::move(pr.spirv), std::move(new_module), std::move(pr.reflection));
        if (ok && old_module) {
            m_pipeline_cache.invalidate_shader(old_module);
            ++updated;
        }
    }
    return updated;
}

bool ShaderManager::inject_spirv(const std::string& path, std::vector<u8> spirv, rhi::ShaderStage stage) {
    auto it = m_shaders.find(path);
    if (it == m_shaders.end()) {
        // Create new shader via inject
        ReflectionData refl = reflect_spirv(std::span<const u8>(spirv), stage);
        if (!refl.valid) {
            NF_LOG_ERROR(LogCategory::Core, "inject_spirv: reflection failed for '{}'", path);
            return false;
        }
        rhi::ShaderModuleDesc desc{ std::span<const u8>(spirv), stage };
        auto module = m_device.create_shader_module(desc);
        if (!module) return false;
        ShaderId id = m_next_id++;
        auto shader = std::make_unique<Shader>(id, path, stage, std::move(spirv), std::move(module), std::move(refl), 1);
        m_shaders[path] = std::move(shader);
        m_id_to_path[id] = path;
        return true;
    } else {
        Shader* shader = it->second.get();
        ReflectionData refl = reflect_spirv(std::span<const u8>(spirv), stage);
        if (!refl.valid) return false;
        rhi::ShaderModuleDesc desc{ std::span<const u8>(spirv), stage };
        auto new_module = m_device.create_shader_module(desc);
        if (!new_module) return false;
        rhi::ShaderModule* old_module = shader->module();
        bool ok = shader->update(std::move(spirv), std::move(new_module), std::move(refl));
        if (ok && old_module) {
            m_pipeline_cache.invalidate_shader(old_module);
        }
        return ok;
    }
}

} // namespace nf::rendering
