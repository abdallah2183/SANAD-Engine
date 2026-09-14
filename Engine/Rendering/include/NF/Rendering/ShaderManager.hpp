#pragma once

// NF/Rendering/ShaderManager.hpp — Shader lifecycle + hot reload + pipeline invalidation
//
// Flow:
//   Shader Source → File Watcher → Change Detected → Recompile (Job) → Validation → SPIR-V → Reflection → Pipeline Cache Invalidation → New Pipeline → Material refresh
//
// The manager owns Shader assets and coordinates with PipelineCache so that only
// pipelines depending on the changed shader are invalidated.

#include <NF/Core/Types.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/Shader.hpp>
#include <NF/Rendering/PipelineCache.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nf::rendering {

class ShaderManager {
public:
    ShaderManager(rhi::IGraphicsDevice& device, PipelineCache& pipeline_cache);
    ~ShaderManager();

    ShaderManager(const ShaderManager&) = delete;
    ShaderManager& operator=(const ShaderManager&) = delete;

    // Loads a shader from a GLSL file (e.g. "shaders/textured_quad.frag").
    // Compiles via glslc, reflects, creates RHI module, caches.
    // Returns nullptr on failure (keeps no entry).
    Shader* load(const std::string& path, rhi::ShaderStage stage);

    // Force reload of a shader by path (synchronous, for tests).
    // Returns true if reload succeeded (or shader was up to date), false if compile failed (old kept).
    bool reload(const std::string& path);

    // Async reload: enqueues a job; call update() on main thread to commit.
    void reload_async(const std::string& path);

    Shader* get(const std::string& path) const;
    Shader* get_by_id(ShaderId id) const;

    // File watcher: call periodically (e.g. each frame) to detect changes.
    // Enqueues async reloads for changed files.
    void poll();

    // Must be called on main/render thread to commit completed async compiles.
    // Creates new ShaderModules and invalidates pipelines. Returns number of shaders updated.
    size_t update();

    // For tests: directly inject new SPIR-V for a shader (bypasses file + glslc).
    // Simulates a successful hot reload with already-compiled SPIR-V.
    bool inject_spirv(const std::string& path, std::vector<u8> spirv, rhi::ShaderStage stage);

    // Compile a GLSL file to SPIR-V via glslc. Exposed for tests.
    static bool compile_glsl_to_spirv(const std::string& glsl_path, rhi::ShaderStage stage,
                                      std::vector<u8>& out_spirv, std::string& out_error);

private:
    struct PendingResult {
        std::string path;
        rhi::ShaderStage stage;
        std::vector<u8> spirv;
        ReflectionData reflection;
        bool success = false;
        std::string error;
    };

    Shader* load_internal(const std::string& path, rhi::ShaderStage stage, bool is_reload);
    bool compile_and_reflect(const std::string& path, rhi::ShaderStage stage,
                             std::vector<u8>& out_spirv, ReflectionData& out_refl, std::string& out_error);

    rhi::IGraphicsDevice& m_device;
    PipelineCache& m_pipeline_cache;

    std::map<std::string, std::unique_ptr<Shader>> m_shaders;
    std::map<std::string, std::filesystem::file_time_type> m_file_times;
    std::map<ShaderId, std::string> m_id_to_path;

    // Pending async results (produced by worker jobs, consumed by update() on main thread)
    std::vector<PendingResult> m_pending;
    std::mutex m_pending_mutex;

    ShaderId m_next_id = 1;
};

} // namespace nf::rendering
