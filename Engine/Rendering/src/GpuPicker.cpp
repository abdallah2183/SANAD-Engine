#include <NF/Rendering/GpuPicker.hpp>

#include <NF/Core/Logger.hpp>
#include <NF/Rendering/StaticMesh.hpp>

#include <array>
#include <cstring>
#include <fstream>

namespace nf::rendering {
namespace {

/// Loads a compiled SPIR-V blob. Mirrors Renderer3D's helper: the picker owns
/// its shaders so it can be constructed and torn down independently.
std::vector<u8> load_spirv_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto size = static_cast<std::streamsize>(f.tellg());
    if (size <= 0) return {};
    f.seekg(0, std::ios::beg);
    std::vector<u8> data(static_cast<usize>(size));
    f.read(reinterpret_cast<char*>(data.data()), size);
    if (f.gcount() != size) return {};
    return data;
}

/// Push-constant block shared by both pick stages. Layout must match pick.vert
/// and pick.frag exactly: two mat4s then the packed id.
struct PickPush {
    float view_proj[16];
    float model[16];
    float pick_id[4];
};
static_assert(sizeof(PickPush) == 144, "pick push constants must be 144 bytes");

/// 24-bit ceiling on pickable ids (see pick.frag).
constexpr u32 kMaxPickId = 0x00FFFFFFu;

} // namespace

void pack_pick_id(u32 object_id, float out_rgb[3]) {
    // 1-based so 0 stays the clear value; saturate rather than wrap so an
    // out-of-range id can never alias a real entity.
    const u32 id = (object_id >= kMaxPickId) ? kMaxPickId : object_id + 1u;
    out_rgb[0] = static_cast<float>(id & 0xFFu) / 255.0f;
    out_rgb[1] = static_cast<float>((id >> 8) & 0xFFu) / 255.0f;
    out_rgb[2] = static_cast<float>((id >> 16) & 0xFFu) / 255.0f;
}

u32 unpack_pick_id(u8 r, u8 g, u8 b, u8 a) {
    if (a == 0) return 0; // cleared pixel: nothing drawn here
    const u32 id = static_cast<u32>(r) | (static_cast<u32>(g) << 8) |
                   (static_cast<u32>(b) << 16);
    if (id == 0) return 0;
    return id - 1u;
}

GpuPicker::~GpuPicker() { shutdown(); }

bool GpuPicker::init(rhi::IGraphicsDevice& device, const std::filesystem::path& shader_dir,
                     u32 width, u32 height) {
    shutdown();
    m_device = &device;

    if (width == 0 || height == 0) {
        NF_LOG_ERROR(LogCategory::RHI, "GpuPicker: invalid size {}x{}", width, height);
        m_device = nullptr;
        return false;
    }
    if (shader_dir.empty() || !std::filesystem::exists(shader_dir)) {
        NF_LOG_WARN(LogCategory::RHI, "GpuPicker: shader dir not found: '{}'", shader_dir.string());
        m_device = nullptr;
        return false;
    }

    const auto vs_code = load_spirv_file(shader_dir / "pick_vert.spv");
    const auto fs_code = load_spirv_file(shader_dir / "pick_frag.spv");
    if (vs_code.empty() || fs_code.empty()) {
        // Not fatal: the caller falls back to CPU picking.
        NF_LOG_WARN(LogCategory::RHI, "GpuPicker: pick shaders missing in '{}'", shader_dir.string());
        m_device = nullptr;
        return false;
    }

    m_vs = device.create_shader_module({vs_code, rhi::ShaderStage::Vertex});
    m_fs = device.create_shader_module({fs_code, rhi::ShaderStage::Fragment});
    if (!m_vs || !m_fs) {
        NF_LOG_ERROR(LogCategory::RHI, "GpuPicker: shader module creation failed");
        shutdown();
        return false;
    }

    rhi::ColorAttachment color{};
    color.format = rhi::Format::R8G8B8A8_UNorm;
    color.blend_enabled = false; // ids must be written verbatim, never blended
    const std::array<rhi::ColorAttachment, 1> colors{color};

    rhi::RenderPassDesc rpd{};
    rpd.color_attachments = std::span<const rhi::ColorAttachment>(colors);
    rpd.depth_format = rhi::Format::D32_SFloat;
    rpd.has_depth = true;
    // Offscreen target that is read back, never presented.
    rpd.present_source = false;
    m_pass = device.create_render_pass(rpd);
    if (!m_pass) {
        NF_LOG_ERROR(LogCategory::RHI, "GpuPicker: render pass creation failed");
        shutdown();
        return false;
    }

    static const std::array<rhi::VertexAttrib, 3> kMeshAttribs{{
        {0, offsetof(Vertex, position), rhi::Format::R32G32B32_SFloat},
        {1, offsetof(Vertex, normal), rhi::Format::R32G32B32_SFloat},
        {2, offsetof(Vertex, uv0), rhi::Format::R32G32_SFloat},
    }};

    rhi::PipelineDesc pd{};
    pd.vs = m_vs.get();
    pd.fs = m_fs.get();
    pd.topology = rhi::PrimitiveTopology::TriangleList;
    pd.vertex_layout.stride = sizeof(Vertex);
    pd.vertex_layout.attributes = std::span<const rhi::VertexAttrib>(kMeshAttribs);
    pd.rasterizer.cull_mode = rhi::CullMode::Back;
    // Depth test + write so the nearest surface at each pixel owns the id.
    pd.depth.test_enabled = true;
    pd.depth.write_enabled = true;
    pd.depth.compare = rhi::CompareOp::Less;
    pd.render_pass = m_pass.get();
    pd.push_constant_size = sizeof(PickPush);
    pd.push_constant_stages = rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment;
    m_pipeline = device.create_pipeline(pd);
    if (!m_pipeline) {
        NF_LOG_ERROR(LogCategory::RHI, "GpuPicker: pipeline creation failed");
        shutdown();
        return false;
    }

    if (!create_targets(width, height)) {
        shutdown();
        return false;
    }

    m_ready = true;
    NF_LOG_INFO(LogCategory::RHI, "GpuPicker: ready at {}x{}", m_width, m_height);
    return true;
}

bool GpuPicker::create_targets(u32 width, u32 height) {
    rhi::TextureDesc color_desc{};
    color_desc.width = width;
    color_desc.height = height;
    color_desc.format = rhi::Format::R8G8B8A8_UNorm;
    color_desc.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::TransferSrc;
    m_color = m_device->create_texture(color_desc);
    if (!m_color) return false;

    rhi::TextureDesc depth_desc{};
    depth_desc.width = width;
    depth_desc.height = height;
    depth_desc.format = rhi::Format::D32_SFloat;
    depth_desc.usage = rhi::ImageUsage::DepthAtt;
    m_depth = m_device->create_texture(depth_desc);
    if (!m_depth) return false;

    const std::array<rhi::Texture*, 1> fb_colors{m_color.get()};
    m_fb = m_device->create_framebuffer(*m_pass, std::span<rhi::Texture* const>(fb_colors),
                                        m_depth.get());
    if (!m_fb) return false;

    // One pixel is all a pick ever reads back.
    rhi::BufferDesc rb{};
    rb.size = 4;
    rb.usage = rhi::BufferUsage::TransferDst;
    rb.memory = rhi::MemoryUsage::GPUToCPU;
    m_readback = m_device->create_buffer(rb);
    if (!m_readback) return false;

    m_width = width;
    m_height = height;
    return true;
}

void GpuPicker::destroy_targets() {
    m_readback.reset();
    m_fb.reset();
    m_depth.reset();
    m_color.reset();
    m_width = 0;
    m_height = 0;
}

bool GpuPicker::resize(u32 width, u32 height) {
    if (!m_ready || !m_device) return false;
    if (width == m_width && height == m_height) return true;
    if (width == 0 || height == 0) return false;
    // Caller contract: the GPU is idle, so dropping the old targets is safe.
    destroy_targets();
    if (!create_targets(width, height)) {
        m_ready = false;
        return false;
    }
    return true;
}

void GpuPicker::shutdown() {
    if (m_device) {
        // Targets are referenced by the framebuffer, so it goes first.
        destroy_targets();
        m_pipeline.reset();
        m_pass.reset();
        m_fs.reset();
        m_vs.reset();
    }
    m_device = nullptr;
    m_ready = false;
}

PickHit GpuPicker::pick(rhi::CommandBuffer& cmd, std::span<const RenderObject> objects,
                        const Camera& camera, MeshLibrary& meshes, u32 x, u32 y) {
    PickHit result{};
    if (!m_ready || !m_device) return result;
    if (m_width == 0 || m_height == 0) return result;
    if (x >= m_width || y >= m_height) return result; // out of range: no hit

    // Start from a clean slate: every pixel reads "nothing here" unless an
    // object actually covers it.
    const std::array<rhi::ClearValue, 1> clears{rhi::ClearValue{0.0f, 0.0f, 0.0f, 0.0f}};

    PickPush push{};
    std::memcpy(push.view_proj, camera.view_projection.m, sizeof(push.view_proj));

    // One command buffer for the whole pick: the id pass first, then the
    // single-pixel copy. Recording the copy after end_render_pass is what
    // orders it behind the draws — a second submission would need an extra
    // fence to achieve the same thing.
    cmd.begin();
    cmd.begin_render_pass(*m_pass, *m_fb, std::span<const rhi::ClearValue>(clears), 1.0f, 0);
    cmd.bind_pipeline(*m_pipeline);
    cmd.set_viewport(0, 0, m_width, m_height);
    cmd.set_scissor(0, 0, m_width, m_height);

    u32 drawn = 0;
    for (const RenderObject& ro : objects) {
        if (!ro.visible || !ro.mesh_handle.valid()) continue;
        const StaticMesh* mesh = meshes.get(ro.mesh_handle);
        if (mesh == nullptr || !mesh->is_uploaded()) continue;
        if (ro.lod >= mesh->lods().size()) continue;
        const rhi::Buffer* vb = mesh->vertex_buffer(ro.lod);
        const rhi::Buffer* ib = mesh->index_buffer(ro.lod);
        if (vb == nullptr || ib == nullptr) continue;

        // A mesh is drawn submesh by submesh, matching Renderer3D exactly: if
        // the id pass drew a different range the two images would disagree.
        const MeshLOD& lod = mesh->lods()[ro.lod];
        if (lod.submeshes.empty()) continue;

        std::memcpy(push.model, ro.world.m, sizeof(push.model));
        pack_pick_id(ro.id, push.pick_id);

        // Push constants come after the pipeline bind: the command buffer
        // resolves the layout from the bound pipeline.
        cmd.push_constants(rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
                           sizeof(PickPush), &push);

        const std::array<const rhi::Buffer*, 1> vbs{vb};
        cmd.bind_vertex_buffers(std::span<const rhi::Buffer* const>(vbs));
        cmd.bind_index_buffer(*ib, 0);
        for (const SubMesh& sm : lod.submeshes) {
            cmd.draw_indexed(sm.index_count, 1, sm.index_offset,
                             static_cast<i32>(sm.vertex_offset), 0);
        }
        ++drawn;
    }
    cmd.end_render_pass();

    if (drawn == 0) {
        // Nothing was submitted, so there is nothing to read back.
        cmd.end();
        return result;
    }

    // Exactly one pixel: the readback buffer is 4 bytes, and copying the whole
    // image into it would overrun it.
    cmd.copy_texture_to_buffer(*m_color, *m_readback, x, y, 1, 1, 0);
    cmd.end();

    auto fence = m_device->create_fence(false);
    if (!fence) return result;
    m_device->submit(cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    if (!fence->wait(5'000'000'000ull)) {
        NF_LOG_ERROR(LogCategory::RHI, "GpuPicker: readback fence timed out");
        return result;
    }

    const auto* px = static_cast<const u8*>(m_readback->map());
    if (px == nullptr) return result;
    const u32 id = unpack_pick_id(px[0], px[1], px[2], px[3]);
    m_readback->unmap();

    if (id != 0) {
        result.hit = true;
        result.object_id = id;
    }
    return result;
}

} // namespace nf::rendering
