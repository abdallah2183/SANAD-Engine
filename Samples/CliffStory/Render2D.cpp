// Samples/CliffStory/Render2D.cpp â€” GPU sprite renderer, texture pages, glyph atlas.

#include "Render2D.hpp"

#include <NF/Core/Assert.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Rendering/ImageDecode.hpp>
#include <NF/UI/ArabicShaper.hpp>

// imstb_truetype is vendored inside ImGui but is a standalone rasteriser: no
// ImGui types are involved. ImGui compiles the implementation inside
// imgui_draw.cpp with STBTT_STATIC, so here it must be asked for explicitly â€”
// without STB_TRUETYPE_IMPLEMENTATION this header only declares `static`
// functions that are defined nowhere in this translation unit.
//
// C4505 (unreferenced static function removed) fires on the ~30 entry points
// this sample never calls â€” SDF baking, the rect packer, SVG export. The build
// runs /WX, so they are suppressed for exactly this include rather than for the
// translation unit, and no engine code loses a warning it should be held to.
#ifdef _MSC_VER
    #pragma warning(push)
    #pragma warning(disable : 4505)
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>
#ifdef _MSC_VER
    #pragma warning(pop)
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>

// Only for GetModuleFileNameW, used to locate content relative to the
// executable. The engine keeps windows.h out of its public headers, so this is
// the one place the sample needs it.
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#endif

#ifndef NF_CLIFF_SHADER_DIR
    #define NF_CLIFF_SHADER_DIR ""
#endif

namespace nf::cliff {

namespace {

/// Ceiling on page count. Each page costs one descriptor set; the game uses a
/// handful, and the cap keeps a bad path from exhausting the allocator.
constexpr u32 kMaxPages = 32;
/// Uploads start here and grow on demand.
constexpr usize kInitialVertexBytes = 256 * 1024;
constexpr usize kInitialIndexBytes = 128 * 1024;

std::vector<u8> load_spirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to open shader file: {}", path.string());
        return {};
    }
    const auto size = static_cast<usize>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<u8> data(size);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (static_cast<usize>(file.gcount()) != size) {
        NF_LOG_ERROR(LogCategory::RHI, "Short read on shader file: {}", path.string());
        return {};
    }
    return data;
}

/// Uploads RGBA8 into an existing texture and leaves it readable by a sampler.
///
/// Split out of add_page because the glyph atlas needs the same transfer again:
/// it is created as an empty page and then refilled as glyphs are demanded.
bool upload_pixels(rhi::IGraphicsDevice& device, rhi::Texture& texture, u32 width, u32 height,
                   const u8* rgba) {
    const usize bytes = static_cast<usize>(width) * static_cast<usize>(height) * 4;

    rhi::BufferDesc staging_desc{};
    staging_desc.size = bytes;
    staging_desc.usage = rhi::BufferUsage::TransferSrc;
    staging_desc.memory = rhi::MemoryUsage::CPUToGPU;
    auto staging = device.create_buffer(staging_desc);
    if (!staging) return false;

    void* mapped = staging->map();
    if (!mapped) return false;
    std::memcpy(mapped, rgba, bytes);
    staging->unmap();

    auto upload = device.create_upload_context();
    if (!upload) return false;
    upload->copy_buffer_to_texture(*staging, texture, 0, 0, 0, width, height);
    auto fence = upload->submit();
    if (!fence || !fence->wait()) return false;
    device.wait_idle();

    // Undefined -> TransferDst -> ShaderRead, so a sampler may read it again.
    auto cmd = device.create_command_buffer();
    auto fence2 = device.create_fence(false);
    if (!cmd || !fence2) return false;
    cmd->begin();
    cmd->transition_texture_for_sampling(texture);
    cmd->end();
    device.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence2.get()});
    if (!fence2->wait()) return false;
    device.wait_idle();
    return true;
}

/// Unpacks the batcher's 0xAABBGGRR tint into four floats, because the RHI
/// exposes no integer vertex format to carry it.
inline void unpack_tint(u32 packed, f32& r, f32& g, f32& b, f32& a) {
    constexpr f32 kInv = 1.0f / 255.0f;
    r = static_cast<f32>(packed & 0xFFu) * kInv;
    g = static_cast<f32>((packed >> 8) & 0xFFu) * kInv;
    b = static_cast<f32>((packed >> 16) & 0xFFu) * kInv;
    a = static_cast<f32>((packed >> 24) & 0xFFu) * kInv;
}

} // namespace

u32 apply_alpha(u32 packed, f32 alpha) {
    const auto clamped = static_cast<u32>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    return (packed & 0x00FFFFFFu) | (clamped << 24);
}

// ---------------------------------------------------------------------------
// Asset lookup
// ---------------------------------------------------------------------------

std::vector<std::filesystem::path> ancestor_roots(const std::filesystem::path& start) {
    std::vector<std::filesystem::path> roots;
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::absolute(start, ec);
    if (ec) dir = start;
    for (int i = 0; i < 8; ++i) {
        if (dir.empty()) break;
        roots.push_back(dir);
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return roots;
}

std::vector<std::filesystem::path> content_roots() {
    std::vector<std::filesystem::path> roots;

    // The executable's own directory and its ancestors come first: a packaged
    // copy keeps Content next to itself, so that layout needs no source tree.
    wchar_t buffer[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) > 0) {
        const std::filesystem::path exe_dir = std::filesystem::path(buffer).parent_path();
        for (auto& root : ancestor_roots(exe_dir)) roots.push_back(std::move(root));
    }

    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    for (auto& root : ancestor_roots(cwd)) roots.push_back(std::move(root));

    return roots;
}

std::filesystem::path find_asset(const std::vector<std::filesystem::path>& roots,
                                 const std::string& relative) {
    for (const auto& root : roots) {
        if (root.empty()) continue;
        std::error_code ec;
        const std::filesystem::path candidate = root / relative;
        if (std::filesystem::exists(candidate, ec)) return candidate;
    }
    return {};
}

// ---------------------------------------------------------------------------
// SpriteRenderer
// ---------------------------------------------------------------------------

SpriteRenderer::~SpriteRenderer() { destroy(); }

bool SpriteRenderer::create(rhi::IGraphicsDevice& device, rhi::Swapchain& swapchain, u32 width,
                            u32 height) {
    m_device = &device;
    m_swapchain = &swapchain;
    m_viewport_w = width;
    m_viewport_h = height;

    std::filesystem::path shader_dir;
    if (const std::string_view configured = NF_CLIFF_SHADER_DIR; !configured.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(configured, ec)) {
            shader_dir = std::filesystem::path(configured);
        }
    }
    if (shader_dir.empty()) {
        shader_dir = find_asset(content_roots(), "Samples/CliffStory/shaders");
    }
    if (shader_dir.empty()) {
        NF_LOG_FATAL(LogCategory::RHI, "CliffStory: could not locate the shader directory");
        return false;
    }

    const auto vert_code = load_spirv(shader_dir / "sprite2d_vert.spv");
    const auto frag_code = load_spirv(shader_dir / "sprite2d_frag.spv");
    if (vert_code.empty() || frag_code.empty()) {
        NF_LOG_FATAL(LogCategory::RHI, "CliffStory: failed to load SPIR-V from {}",
                     shader_dir.string());
        return false;
    }

    m_vertex_shader =
        device.create_shader_module(rhi::ShaderModuleDesc{vert_code, rhi::ShaderStage::Vertex});
    m_fragment_shader =
        device.create_shader_module(rhi::ShaderModuleDesc{frag_code, rhi::ShaderStage::Fragment});
    if (!m_vertex_shader || !m_fragment_shader) {
        NF_LOG_FATAL(LogCategory::RHI, "CliffStory: failed to create shader modules");
        return false;
    }

    // Straight alpha, no depth attachment: SpriteBatcher emitted the sprites in
    // back-to-front order, so the painter's algorithm is the layering. That is
    // what makes overlapping ivy, lantern glow and mist bands correct.
    rhi::ColorAttachment color_attachment{};
    color_attachment.format = rhi::Format::B8G8R8A8_UNorm;
    color_attachment.blend_enabled = true;
    color_attachment.color_blend_op = rhi::BlendOp::Add;
    color_attachment.src_color = rhi::BlendFactor::SrcAlpha;
    color_attachment.dst_color = rhi::BlendFactor::OneMinusSrcAlpha;

    const std::array<rhi::ColorAttachment, 1> color_attachments{color_attachment};
    rhi::RenderPassDesc rp_desc{};
    rp_desc.color_attachments = std::span<const rhi::ColorAttachment>(color_attachments);
    rp_desc.has_depth = false;
    // The pass ends in the swapchain image, so it must end in PRESENT_SRC_KHR or
    // every present fails validation.
    rp_desc.present_source = true;
    m_render_pass = device.create_render_pass(rp_desc);
    if (!m_render_pass) {
        NF_LOG_FATAL(LogCategory::RHI, "CliffStory: failed to create the sprite render pass");
        return false;
    }

    m_pipeline_cache = std::make_unique<rendering::PipelineCache>(device);

    const std::array<rhi::DescriptorBinding, 1> bindings{
        {{0, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}}};
    rhi::DescriptorSetLayoutDesc layout_desc{};
    layout_desc.bindings = std::span<const rhi::DescriptorBinding>(bindings);
    m_set_layout = device.create_descriptor_set_layout(layout_desc);
    if (!m_set_layout) {
        NF_LOG_FATAL(LogCategory::RHI, "CliffStory: failed to create the descriptor set layout");
        return false;
    }

    // position + uv + colour, using only formats the RHI actually has: two
    // R32G32_SFloat and one R32G32B32A32_SFloat.
    const std::array<rhi::VertexAttrib, 3> attribs{
        {{0, offsetof(RenderVertex, x), rhi::Format::R32G32_SFloat},
         {1, offsetof(RenderVertex, u), rhi::Format::R32G32_SFloat},
         {2, offsetof(RenderVertex, r), rhi::Format::R32G32B32A32_SFloat}}};
    rhi::VertexLayout vl{};
    vl.binding = 0;
    vl.stride = sizeof(RenderVertex);
    vl.attributes = std::span<const rhi::VertexAttrib>(attribs);

    rendering::MaterialDesc mat_desc{};
    mat_desc.vs = m_vertex_shader.get();
    mat_desc.fs = m_fragment_shader.get();
    mat_desc.render_pass = m_render_pass.get();
    mat_desc.descriptor_set_layout = m_set_layout.get();
    mat_desc.vertex_layout = vl;
    mat_desc.topology = rhi::PrimitiveTopology::TriangleList;
    // CullMode::Back would cull every sprite: the batcher winds counter-clockwise
    // in y-down space, and the pixel->NDC flip inverts that to clockwise.
    mat_desc.rasterizer.cull_mode = rhi::CullMode::None;
    mat_desc.depth.test_enabled = false;
    mat_desc.depth.write_enabled = false;

    m_material = std::make_unique<rendering::Material>(device, *m_pipeline_cache, mat_desc);
    if (!m_material->valid()) {
        NF_LOG_FATAL(LogCategory::RHI, "CliffStory: failed to create the sprite pipeline");
        return false;
    }

    m_descriptor_allocator = device.create_descriptor_allocator(kMaxPages);
    if (!m_descriptor_allocator) {
        NF_LOG_FATAL(LogCategory::RHI, "CliffStory: failed to create the descriptor allocator");
        return false;
    }

    // Persistent host-visible storage. Buffer::update() writes straight into the
    // mapped range for CPUToGPU memory, so a per-frame sprite upload costs one
    // memcpy and no staging buffer.
    rhi::BufferDesc vb_desc{};
    vb_desc.size = kInitialVertexBytes;
    vb_desc.usage = rhi::BufferUsage::Vertex;
    vb_desc.memory = rhi::MemoryUsage::CPUToGPU;
    m_vertex_buffer = device.create_buffer(vb_desc);

    rhi::BufferDesc ib_desc{};
    ib_desc.size = kInitialIndexBytes;
    ib_desc.usage = rhi::BufferUsage::Index;
    ib_desc.memory = rhi::MemoryUsage::CPUToGPU;
    m_index_buffer = device.create_buffer(ib_desc);

    m_framebuffers.resize(swapchain.image_count());
    for (u32 i = 0; i < swapchain.image_count(); ++i) {
        rhi::Texture* tex = swapchain.get_texture(i);
        if (!tex) {
            NF_LOG_FATAL(LogCategory::RHI, "CliffStory: swapchain image {} has no texture", i);
            return false;
        }
        const std::array<rhi::Texture*, 1> attachments{tex};
        m_framebuffers[i] = device.create_framebuffer(
            *m_render_pass, std::span<rhi::Texture* const>(attachments), nullptr);
        if (!m_framebuffers[i]) {
            NF_LOG_FATAL(LogCategory::RHI, "CliffStory: failed to create framebuffer {}", i);
            return false;
        }
    }

    NF_LOG_INFO(LogCategory::RHI,
                "CliffStory: sprite pipeline ready ({}x{}, no depth, alpha blend)", width,
                height);
    return true;
}

void SpriteRenderer::destroy() {
    // Reverse construction order: instances reference the material and the
    // allocator, framebuffers reference the render pass, and every one of them
    // must go before the device shuts down.
    m_pages.clear();
    m_framebuffers.clear();
    // The capture framebuffer references the capture pass and its texture, so it
    // has to go before either.
    m_capture_framebuffer.reset();
    m_capture_pass.reset();
    m_capture_texture.reset();
    m_capture = CaptureTarget{};
    m_vertex_buffer.reset();
    m_index_buffer.reset();
    m_descriptor_allocator.reset();
    m_material.reset();
    m_pipeline_cache.reset();
    m_set_layout.reset();
    m_fragment_shader.reset();
    m_vertex_shader.reset();
    m_render_pass.reset();
    m_device = nullptr;
    m_swapchain = nullptr;
}

const TexturePage* SpriteRenderer::page(u32 index) const {
    if (index >= m_pages.size() || !m_pages[index].page) return nullptr;
    return m_pages[index].page.get();
}

u32 SpriteRenderer::add_page(const std::string& name, u32 width, u32 height, const u8* rgba,
                             PageWrap wrap, bool nearest) {
    if (!m_device || !m_material || !m_descriptor_allocator) {
        NF_LOG_ERROR(LogCategory::RHI, "CliffStory: add_page called before create()");
        return u32_max;
    }
    if (!rgba || width == 0 || height == 0) {
        NF_LOG_ERROR(LogCategory::RHI, "CliffStory: add_page('{}') got no pixels", name);
        return u32_max;
    }
    if (m_pages.size() >= kMaxPages) {
        NF_LOG_ERROR(LogCategory::RHI, "CliffStory: texture page limit ({}) reached", kMaxPages);
        return u32_max;
    }

    rhi::TextureDesc tex_desc{};
    tex_desc.width = width;
    tex_desc.height = height;
    tex_desc.format = rhi::Format::R8G8B8A8_UNorm;
    tex_desc.usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst;

    auto texture = m_device->create_texture(tex_desc);
    if (!texture) {
        NF_LOG_ERROR(LogCategory::RHI, "CliffStory: failed to create texture '{}'", name);
        return u32_max;
    }

    if (!upload_pixels(*m_device, *texture, width, height, rgba)) {
        NF_LOG_ERROR(LogCategory::RHI, "CliffStory: texture upload failed for '{}'", name);
        return u32_max;
    }

    const rhi::AddressMode mode =
        (wrap == PageWrap::Repeat) ? rhi::AddressMode::Repeat : rhi::AddressMode::ClampToEdge;
    rhi::SamplerDesc sampler_desc{};
    sampler_desc.mag = nearest ? rhi::Filter::Nearest : rhi::Filter::Linear;
    sampler_desc.min = nearest ? rhi::Filter::Nearest : rhi::Filter::Linear;
    sampler_desc.mip = rhi::MipMapMode::None;
    sampler_desc.address_u = mode;
    sampler_desc.address_v = mode;
    sampler_desc.address_w = mode;
    auto sampler = m_device->create_sampler(sampler_desc);

    rhi::TextureViewDesc view_desc{};
    view_desc.texture = texture.get();
    view_desc.dimension = rhi::ViewDimension::View2D;
    view_desc.aspect = rhi::ImageAspect::Color;
    view_desc.base_mip = 0;
    view_desc.mip_count = 1;
    view_desc.base_layer = 0;
    view_desc.layer_count = 1;
    auto view = m_device->create_texture_view(view_desc);

    if (!sampler || !view) {
        NF_LOG_ERROR(LogCategory::RHI, "CliffStory: no view/sampler for '{}'", name);
        return u32_max;
    }

    PageEntry entry;
    entry.page = std::make_unique<TexturePage>();
    entry.page->name = name;
    entry.page->texture = std::move(texture);
    entry.page->view = std::move(view);
    entry.page->sampler = std::move(sampler);
    entry.page->width = width;
    entry.page->height = height;

    // One instance per page, all sharing the single pipeline above: N pages, one
    // pipeline. This is the Material/instance split in NF/Rendering/Material.hpp
    // doing exactly what it was designed for.
    auto instance =
        std::make_unique<rendering::MaterialInstance>(*m_material, *m_descriptor_allocator);
    if (!instance->valid()) {
        NF_LOG_ERROR(LogCategory::RHI, "CliffStory: failed to allocate a descriptor set for '{}'",
                     name);
        return u32_max;
    }
    instance->set_texture(0, *entry.page->view, *entry.page->sampler);
    instance->update();
    entry.instance = std::move(instance);

    const u32 index = static_cast<u32>(m_pages.size());
    m_pages.push_back(std::move(entry));
    NF_LOG_INFO(LogCategory::RHI, "CliffStory: page {} '{}' ({}x{})", index, name, width, height);
    return index;
}

u32 SpriteRenderer::add_page_from_file(const std::filesystem::path& path, PageWrap wrap,
                                       bool nearest) {
    std::string err;
    const rendering::DecodedImage image = rendering::decode_image_file(path.string(), err);
    if (!image.ok()) {
        NF_LOG_ERROR(LogCategory::RHI, "CliffStory: cannot decode '{}': {}", path.string(),
                     err.empty() ? "decode failed" : err);
        return u32_max;
    }
    return add_page(path.stem().string(), static_cast<u32>(image.width),
                    static_cast<u32>(image.height), image.rgba.data(), wrap, nearest);
}

bool SpriteRenderer::ensure_capture_target(u32 width, u32 height) {
    if (!m_device || width == 0 || height == 0) return false;
    if (m_capture.ready) return true;

    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::Format::B8G8R8A8_UNorm;
    // TransferSrc is the whole point: without it the target can be rendered into
    // but never read back, which is what a screenshot is.
    desc.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::TransferSrc;
    m_capture_texture = m_device->create_texture(desc);
    if (!m_capture_texture) {
        NF_LOG_ERROR(LogCategory::RHI, "CliffStory: capture texture creation failed");
        return false;
    }

    // Same pass description as the swapchain one, minus present_source: this
    // target is never presented, so it must end in SHADER_READ_ONLY_OPTIMAL.
    rhi::ColorAttachment attachment{};
    attachment.format = rhi::Format::B8G8R8A8_UNorm;
    attachment.blend_enabled = true;
    attachment.color_blend_op = rhi::BlendOp::Add;
    attachment.src_color = rhi::BlendFactor::SrcAlpha;
    attachment.dst_color = rhi::BlendFactor::OneMinusSrcAlpha;

    const std::array<rhi::ColorAttachment, 1> attachments{attachment};
    rhi::RenderPassDesc rp{};
    rp.color_attachments = std::span<const rhi::ColorAttachment>(attachments);
    rp.has_depth = false;
    rp.present_source = false;
    m_capture_pass = m_device->create_render_pass(rp);
    if (!m_capture_pass) {
        NF_LOG_ERROR(LogCategory::RHI, "CliffStory: capture render pass failed");
        return false;
    }

    const std::array<rhi::Texture*, 1> tex{m_capture_texture.get()};
    m_capture_framebuffer =
        m_device->create_framebuffer(*m_capture_pass, std::span<rhi::Texture* const>(tex), nullptr);
    if (!m_capture_framebuffer) {
        NF_LOG_ERROR(LogCategory::RHI, "CliffStory: capture framebuffer failed");
        return false;
    }

    m_capture.pass = m_capture_pass.get();
    m_capture.framebuffer = m_capture_framebuffer.get();
    m_capture.ready = true;
    NF_LOG_INFO(LogCategory::RHI, "CliffStory: capture target ready ({}x{})", width, height);
    return true;
}

bool SpriteRenderer::read_back_capture(std::vector<u8>& rgba_out) {
    if (!m_device || !m_capture.ready || !m_capture_texture) return false;

    const u32 w = m_capture_texture->width();
    const u32 h = m_capture_texture->height();
    const usize bytes = static_cast<usize>(w) * h * 4;

    rhi::BufferDesc buf{};
    buf.size = bytes;
    buf.usage = rhi::BufferUsage::TransferDst;
    buf.memory = rhi::MemoryUsage::GPUToCPU;
    auto readback = m_device->create_buffer(buf);
    if (!readback) return false;

    auto cmd = m_device->create_command_buffer();
    auto fence = m_device->create_fence(false);
    if (!cmd || !fence) return false;

    cmd->begin();
    cmd->copy_texture_to_buffer(*m_capture_texture, *readback, 0, 0, w, h, 0);
    cmd->end();
    m_device->submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    if (!fence->wait()) return false;

    const void* mapped = readback->map();
    if (!mapped) return false;

    // BGRA on the GPU, RGBA for the PNG writer. Converted once, here, so no
    // consumer has to remember which order it is holding.
    const auto* src = static_cast<const u8*>(mapped);
    rgba_out.resize(bytes);
    for (usize i = 0; i < bytes; i += 4) {
        rgba_out[i + 0] = src[i + 2];
        rgba_out[i + 1] = src[i + 1];
        rgba_out[i + 2] = src[i + 0];
        rgba_out[i + 3] = 255;
    }
    readback->unmap();
    return true;
}

bool SpriteRenderer::refresh_page(u32 index, u32 width, u32 height, const u8* rgba) {
    if (!m_device || index >= m_pages.size() || !m_pages[index].page) return false;
    if (!rgba || width == 0 || height == 0) return false;
    return upload_pixels(*m_device, *m_pages[index].page->texture, width, height, rgba);
}

bool SpriteRenderer::ensure_capacity(usize vertex_bytes, usize index_bytes) {
    const bool grow_v = !m_vertex_buffer || m_vertex_buffer->size() < vertex_bytes;
    const bool grow_i = !m_index_buffer || m_index_buffer->size() < index_bytes;
    if (!grow_v && !grow_i) return true;

    rhi::BufferDesc vb_desc{};
    vb_desc.size = grow_v ? vertex_bytes : m_vertex_buffer->size();
    vb_desc.usage = rhi::BufferUsage::Vertex;
    vb_desc.memory = rhi::MemoryUsage::CPUToGPU;
    auto vb = m_device->create_buffer(vb_desc);

    rhi::BufferDesc ib_desc{};
    ib_desc.size = grow_i ? index_bytes : m_index_buffer->size();
    ib_desc.usage = rhi::BufferUsage::Index;
    ib_desc.memory = rhi::MemoryUsage::CPUToGPU;
    auto ib = m_device->create_buffer(ib_desc);

    if (!vb || !ib) {
        NF_LOG_ERROR(LogCategory::RHI, "CliffStory: failed to grow the sprite buffers");
        return false;
    }
    m_vertex_buffer = std::move(vb);
    m_index_buffer = std::move(ib);
    return true;
}

void SpriteRenderer::draw(const SpriteBatcher& batcher, rhi::CommandBuffer& cmd) {
    const auto& baked = batcher.vertices();
    const auto& indices = batcher.indices();
    const auto& batches = batcher.batches();

    // sprite_count() reads m_staging, which bake() has just cleared â€” the count
    // that matters after end() is what came out of the bake.
    m_last_sprites = static_cast<u32>(baked.size() / 4);
    m_last_draw_calls = 0;
    m_last_triangles = 0;
    if (baked.empty() || indices.empty() || batches.empty()) return;

    // Screen pixels (origin top-left, y down) -> NDC. This is the one place the
    // batcher's pixel space meets the pipeline's clip space.
    const f32 inv_w = 2.0f / static_cast<f32>(std::max<u32>(1, m_viewport_w));
    const f32 inv_h = 2.0f / static_cast<f32>(std::max<u32>(1, m_viewport_h));

    m_upload_vertices.resize(baked.size());
    for (usize i = 0; i < baked.size(); ++i) {
        const scene2d::SpriteVertex& src = baked[i];
        RenderVertex& dst = m_upload_vertices[i];
        dst.x = src.x * inv_w - 1.0f;
        // Vulkan's framebuffer Y runs DOWN while clip space Y runs UP, so NDC -1
    // is the top of the viewport and +1 the bottom. Screen row 0 therefore maps
        // to NDC -1. The OpenGL habit (1 - y) mirrors the whole frame instead —
      // and a cliff is nearly symmetric, so it still reads as a cliff while
        // every HUD element lands upside down.
        dst.y = src.y * inv_h - 1.0f;
        dst.u = src.u;
        dst.v = src.v;
        unpack_tint(src.color, dst.r, dst.g, dst.b, dst.a);
    }

    const usize vertex_bytes = m_upload_vertices.size() * sizeof(RenderVertex);
    const usize index_bytes = indices.size() * sizeof(u32);
    if (!ensure_capacity(vertex_bytes, index_bytes)) return;

    m_vertex_buffer->update(m_upload_vertices.data(), 0, vertex_bytes);
    m_index_buffer->update(indices.data(), 0, index_bytes);

    const std::array<const rhi::Buffer*, 1> vbs{m_vertex_buffer.get()};
    cmd.bind_vertex_buffers(std::span<const rhi::Buffer* const>(vbs));
    cmd.bind_index_buffer(*m_index_buffer, 0);

    for (const scene2d::SpriteBatch& batch : batches) {
        if (batch.page >= m_pages.size() || !m_pages[batch.page].instance) continue;
        m_pages[batch.page].instance->bind_pipeline_and_descriptors(cmd);
        // SpriteBatcher bakes absolute vertex indices (emit_quad adds the global
        // base), so the draw runs with vertex_offset 0 and seeks with first_index.
        cmd.draw_indexed(batch.index_count, 1, batch.index_offset, 0);
        ++m_last_draw_calls;
        m_last_triangles += batch.index_count / 3;
    }
}

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------

u32 utf8_next(const std::string& s, usize& index) {
    if (index >= s.size()) return 0;
    const auto lead = static_cast<u8>(s[index]);
    if (lead < 0x80) {
        ++index;
        return lead;
    }

    usize extra = 0;
    u32 cp = 0;
    if ((lead & 0xE0) == 0xC0) {
        extra = 1;
        cp = lead & 0x1Fu;
    } else if ((lead & 0xF0) == 0xE0) {
        extra = 2;
        cp = lead & 0x0Fu;
    } else if ((lead & 0xF8) == 0xF0) {
        extra = 3;
        cp = lead & 0x07u;
    } else {
        // A continuation or invalid lead byte: skip it and report a replacement.
        ++index;
        return 0xFFFD;
    }

    if (index + extra >= s.size()) {
        // Truncated sequence at the end of the string.
        index = s.size();
        return 0xFFFD;
    }

    ++index;
    for (usize i = 0; i < extra; ++i) {
        const auto cont = static_cast<u8>(s[index]);
        if ((cont & 0xC0) != 0x80) return 0xFFFD;  // malformed; index stays put
        cp = (cp << 6) | (cont & 0x3Fu);
        ++index;
    }
    return cp;
}

// ---------------------------------------------------------------------------
// GlyphAtlas
// ---------------------------------------------------------------------------

GlyphAtlas::~GlyphAtlas() {
    // stb_truetype needs both the TTF bytes and the parsed font structure freed;
    // the bitmaps it hands out are freed per glyph.
    if (m_font_info) {
        STBTT_free(m_font_info, nullptr);
        m_font_info = nullptr;
    }
    if (m_font_data) {
        STBTT_free(m_font_data, nullptr);
        m_font_data = nullptr;
    }
}

bool GlyphAtlas::create(SpriteRenderer& renderer, const std::filesystem::path& font_path,
                        u32 pixels_per_em, PageWrap wrap, u32 page_size) {
    m_renderer = &renderer;
    m_px = pixels_per_em;
    m_wrap = wrap;
    m_page_w = page_size;
    m_page_h = page_size;

    std::ifstream file(font_path, std::ios::binary | std::ios::ate);
    if (!file) {
        NF_LOG_ERROR(LogCategory::RHI, "CliffStory: cannot open font '{}'", font_path.string());
        return false;
    }
    const auto size = static_cast<usize>(file.tellg());
    file.seekg(0, std::ios::beg);
    m_font_bytes.resize(size);
    file.read(reinterpret_cast<char*>(m_font_bytes.data()),
              static_cast<std::streamsize>(size));
    if (static_cast<usize>(file.gcount()) != size) {
        NF_LOG_ERROR(LogCategory::RHI, "CliffStory: short read on font '{}'", font_path.string());
        return false;
    }

    m_font_data = STBTT_malloc(size, nullptr);
    if (!m_font_data) return false;
    std::memcpy(m_font_data, m_font_bytes.data(), size);

    m_font_info = STBTT_malloc(sizeof(stbtt_fontinfo), nullptr);
    if (!m_font_info) return false;
    const int offset = stbtt_GetFontOffsetForIndex(static_cast<const unsigned char*>(m_font_data), 0);
    if (stbtt_InitFont(static_cast<stbtt_fontinfo*>(m_font_info),
                       static_cast<const unsigned char*>(m_font_data), offset) == 0) {
        NF_LOG_ERROR(LogCategory::RHI, "CliffStory: '{}' is not a usable TTF",
                     font_path.string());
        return false;
    }

    auto* info = static_cast<stbtt_fontinfo*>(m_font_info);
    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    stbtt_GetFontVMetrics(info, &ascent, &descent, &line_gap);
    const f32 scale = stbtt_ScaleForPixelHeight(info, static_cast<f32>(m_px));
    m_ascender = static_cast<f32>(ascent) * scale;
    m_line_height = static_cast<f32>(ascent - descent + line_gap) * scale;

    m_page_pixels.assign(static_cast<usize>(page_size) * page_size * 4, 0u);
    m_shelves.clear();
    m_shelves.push_back(Shelf{0.0f, 0.0f, static_cast<f32>(page_size)});

    m_page = renderer.add_page("glyphs", m_page_w, m_page_h, m_page_pixels.data(), wrap, true);
    if (m_page == u32_max) return false;

    NF_LOG_INFO(LogCategory::RHI, "CliffStory: glyph atlas {}x{} from {} ({} px/em, line {} px)",
                m_page_w, m_page_h, font_path.filename().string(), m_px,
                static_cast<int>(m_line_height));
    return true;
}

bool GlyphAtlas::ensure_glyph(u32 codepoint) {
    if (m_glyphs.find(codepoint) != m_glyphs.end()) return true;
    if (!m_font_info || m_full) return false;

    auto* info = static_cast<stbtt_fontinfo*>(m_font_info);
    const f32 scale = stbtt_ScaleForPixelHeight(info, static_cast<f32>(m_px));

    int w = 0;
    int h = 0;
    int xoff = 0;
    int yoff = 0;
    unsigned char* bitmap = stbtt_GetCodepointBitmap(info, 0.0f, scale,
                                                    static_cast<int>(codepoint), &w, &h, &xoff,
                                                    &yoff);

    int advance_width = 0;
    int lsb = 0;
    stbtt_GetCodepointHMetrics(info, static_cast<int>(codepoint), &advance_width, &lsb);

    Glyph glyph;
    glyph.codepoint = codepoint;
    glyph.advance = static_cast<f32>(advance_width) * scale;

    if (!bitmap || w <= 0 || h <= 0) {
        // Space and other blank glyphs carry an advance and no pixels.
        if (bitmap) stbtt_FreeBitmap(bitmap, nullptr);
        m_glyphs[codepoint] = glyph;
        return true;
    }

    const f32 need_w = static_cast<f32>(w);
    const f32 need_h = static_cast<f32>(h);

    // Shelf-pack: the first shelf with vertical room and horizontal space left,
    // else open a new shelf at the bottom.
    int shelf_index = -1;
    for (usize i = 0; i < m_shelves.size(); ++i) {
        const Shelf& shelf = m_shelves[i];
        if (need_h <= shelf.height && shelf.x + need_w <= static_cast<f32>(m_page_w)) {
            shelf_index = static_cast<int>(i);
            break;
        }
    }
    if (shelf_index < 0) {
        f32 cursor = 0.0f;
        for (const Shelf& shelf : m_shelves) cursor += shelf.height;
        if (cursor + need_h <= static_cast<f32>(m_page_h)) {
            m_shelves.push_back(Shelf{0.0f, cursor, static_cast<f32>(m_page_h) - cursor});
            shelf_index = static_cast<int>(m_shelves.size()) - 1;
        } else {
            // Out of room. The atlas never grows (see the header), so the glyph
            // is recorded as advance-only and reported once.
            stbtt_FreeBitmap(bitmap, nullptr);
            m_glyphs[codepoint] = glyph;
            if (!m_full) {
                m_full = true;
                NF_LOG_WARN(LogCategory::RHI,
                            "CliffStory: glyph atlas full at {}x{}; text may render short",
                            m_page_w, m_page_h);
            }
            return true;
        }
    }

    Shelf& shelf = m_shelves[static_cast<usize>(shelf_index)];
    const f32 ax = shelf.x;
    const f32 ay = shelf.y;
    shelf.x += need_w + 1.0f;  // 1px gutter stops bilinear filtering bleeding

    // stb_truetype's coverage bitmap becomes white RGBA. Tinting belongs to the
    // sprite, so one colourless atlas serves every colour of text in the game.
    for (int y = 0; y < h; ++y) {
  // Straight copy. stb_truetype's bitmap is left-to-right, top-to-bottom and
        // v = 0 is the first row of texture data, so bitmap row 0 belongs at cell
        // row 0. An earlier build wrote these bottom-up to compensate for an
    // inverted pixel->clip conversion; now that the conversion is right, the
        // compensation renders every glyph upside down.
      const usize row = static_cast<usize>(ay) + static_cast<usize>(y);
        if (row >= m_page_h) break;
        for (int x = 0; x < w; ++x) {
            const usize col = static_cast<usize>(ax) + static_cast<usize>(x);
            if (col >= m_page_w) break;
            const u8 coverage = bitmap[static_cast<usize>(y) * w + x];
            const usize px = (row * m_page_w + col) * 4;
            m_page_pixels[px + 0] = 255;
            m_page_pixels[px + 1] = 255;
            m_page_pixels[px + 2] = 255;
            m_page_pixels[px + 3] = coverage;
        }
    }

    glyph.width = static_cast<u16>(w);
    glyph.height = static_cast<u16>(h);
    glyph.x_offset = static_cast<i16>(xoff);
    glyph.y_offset = static_cast<i16>(yoff);
    glyph.u0 = ax / static_cast<f32>(m_page_w);
    glyph.v0 = ay / static_cast<f32>(m_page_h);
    glyph.u1 = (ax + need_w) / static_cast<f32>(m_page_w);
    glyph.v1 = (ay + need_h) / static_cast<f32>(m_page_h);

    stbtt_FreeBitmap(bitmap, nullptr);
    m_glyphs[codepoint] = glyph;
    // The atlas texture was uploaded empty at create(); this flag is what tells
    // prepare() to push the freshly rasterised pixels to the GPU.
    m_pixels_dirty = true;
    return true;
}

std::string GlyphAtlas::prepare(const std::string& utf8) {
    // Logical Arabic rasterises as isolated letters in reverse order, so the
    // engine's own shaper converts to presentation forms and visual order first.
    // ASCII passes through byte-identical.
    const std::string visual = ui::shape_arabic(utf8);
    usize index = 0;
    while (index < visual.size()) {
        const u32 cp = utf8_next(visual, index);
        if (cp != 0) ensure_glyph(cp);
    }

    // One upload per string that introduced new glyphs. Without this the atlas
    // would reach the GPU once, blank, and no text would ever render â€” the
    // texture exists before there is anything in it.
    if (m_pixels_dirty && m_renderer && m_page != u32_max) {
        const bool ok =
            m_renderer->refresh_page(m_page, m_page_w, m_page_h, m_page_pixels.data());
        if (ok) m_pixels_dirty = false;
    }
    return visual;
}

f32 GlyphAtlas::measure(const std::string& visual) const {
    f32 total = 0.0f;
    usize index = 0;
    while (index < visual.size()) {
        const u32 cp = utf8_next(visual, index);
        const auto it = m_glyphs.find(cp);
        if (it != m_glyphs.end()) total += it->second.advance;
    }
    return total;
}

void GlyphAtlas::draw(SpriteBatcher& batcher, const std::string& visual, Vec2 pos, f32 size,
                      u32 color, f32 depth, f32 alpha, bool centre, f32 parallax) const {
    if (m_px == 0 || m_page == u32_max) return;
    const f32 scale = size / static_cast<f32>(m_px);

    f32 pen = pos.x;
    if (centre) pen -= measure(visual) * scale * 0.5f;

    const u32 tinted = apply_alpha(color, alpha);
    usize index = 0;
    while (index < visual.size()) {
        const u32 cp = utf8_next(visual, index);
        const auto it = m_glyphs.find(cp);
        if (it == m_glyphs.end()) continue;
        const Glyph& glyph = it->second;

        if (glyph.width > 0 && glyph.height > 0) {
            SpriteDraw d{};
            d.page = m_page;
            d.u0 = glyph.u0;
            d.v0 = glyph.v0;
            d.u1 = glyph.u1;
            d.v1 = glyph.v1;
            // stb_truetype reports yoff downward from the baseline, which is
            // already this space's convention, so pos.y IS the baseline.
            d.position = Vec2{pen + static_cast<f32>(glyph.x_offset) * scale,
                              pos.y + static_cast<f32>(glyph.y_offset) * scale};
            d.size = Vec2{static_cast<f32>(glyph.width) * scale,
                          static_cast<f32>(glyph.height) * scale};
            d.anchor = Vec2{0.0f, 0.0f};
            d.color = tinted;
            d.depth = depth;
            d.parallax = parallax;
            batcher.push(d);
        }
        pen += glyph.advance * scale;
    }
}

} // namespace nf::cliff
