// UiRenderer.cpp — ImGui draw submission through the RHI (see header).

#include <NF/Editor/UiRenderer.hpp>
#include <NF/Core/Logger.hpp>

#include <imgui.h>

#include <array>
#include <cstring>
#include <fstream>
#include <unordered_set>
#include <vector>

namespace nf::editor {

namespace {

// ImDrawVert must be the packed 20-byte vertex our pipeline declares.
static_assert(sizeof(ImDrawVert) == 20, "ImDrawVert layout changed; update the vertex layout");
static_assert(offsetof(ImDrawVert, pos) == 0, "ImDrawVert.pos moved");
static_assert(offsetof(ImDrawVert, uv) == 8, "ImDrawVert.uv moved");
static_assert(offsetof(ImDrawVert, col) == 16, "ImDrawVert.col moved");

std::vector<u8> load_spirv_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        return {};
    }
    const auto size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<u8> data(size);
    if (size > 0) {
        f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
        if (static_cast<size_t>(f.gcount()) != size) {
            return {};
        }
    }
    return data;
}

} // namespace

UiRenderer::~UiRenderer() {
    shutdown();
}

bool UiRenderer::init(rhi::IGraphicsDevice& device, const std::filesystem::path& shader_dir) {
    shutdown();
    m_device = &device;

    const auto vs_code = load_spirv_file(shader_dir / "imgui_vert.spv");
    const auto fs_code = load_spirv_file(shader_dir / "imgui_frag.spv");
    if (vs_code.empty() || fs_code.empty()) {
        NF_LOG_ERROR(LogCategory::Editor, "UiRenderer: missing imgui SPIR-V in '{}'", shader_dir.string());
        shutdown();
        return false;
    }
    m_vs = device.create_shader_module({std::span<const u8>(vs_code), rhi::ShaderStage::Vertex});
    m_fs = device.create_shader_module({std::span<const u8>(fs_code), rhi::ShaderStage::Fragment});
    if (!m_vs || !m_fs) {
        NF_LOG_ERROR(LogCategory::Editor, "UiRenderer: shader module creation failed");
        shutdown();
        return false;
    }

    const std::array<rhi::DescriptorBinding, 1> binds{{
        {0, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1},
    }};
    rhi::DescriptorSetLayoutDesc ld{};
    ld.bindings = std::span<const rhi::DescriptorBinding>(binds);
    m_layout = device.create_descriptor_set_layout(ld);
    if (!m_layout) {
        NF_LOG_ERROR(LogCategory::Editor, "UiRenderer: descriptor layout failed");
        shutdown();
        return false;
    }

    rhi::SamplerDesc samp{};
    samp.mag = rhi::Filter::Linear;
    samp.min = rhi::Filter::Linear;
    samp.address_u = rhi::AddressMode::ClampToEdge;
    samp.address_v = rhi::AddressMode::ClampToEdge;
    m_sampler = device.create_sampler(samp);
    // Font + viewport plus one set per live preview thumbnail. Sets are only
    // allocated for ids the recorded draw data actually references, so a tree
    // with hundreds of previews never comes close to this cap.
    m_allocator = device.create_descriptor_allocator(256);
    if (!m_sampler || !m_allocator) {
        NF_LOG_ERROR(LogCategory::Editor, "UiRenderer: sampler/allocator failed");
        shutdown();
        return false;
    }

    // Font atlas upload (proven pattern: staging + UploadContext + transition).
    unsigned char* px = nullptr;
    int fw = 0, fh = 0;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
    if (px == nullptr || fw <= 0 || fh <= 0) {
        NF_LOG_ERROR(LogCategory::Editor, "UiRenderer: font atlas unavailable");
        shutdown();
        return false;
    }
    const auto byte_count = static_cast<usize>(fw) * static_cast<usize>(fh) * 4;
    rhi::TextureDesc td{};
    td.width = static_cast<u32>(fw);
    td.height = static_cast<u32>(fh);
    td.format = rhi::Format::R8G8B8A8_UNorm;
    td.usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst;
    m_font_texture = device.create_texture(td);
    rhi::BufferDesc staging_desc{};
    staging_desc.size = byte_count;
    staging_desc.usage = rhi::BufferUsage::TransferSrc;
    staging_desc.memory = rhi::MemoryUsage::CPUToGPU;
    auto staging = device.create_buffer(staging_desc);
    if (!m_font_texture || !staging) {
        NF_LOG_ERROR(LogCategory::Editor, "UiRenderer: font staging failed");
        shutdown();
        return false;
    }
    staging->update(px, 0, byte_count);
    if (auto upload = device.create_upload_context()) {
        upload->copy_buffer_to_texture(*staging, *m_font_texture, 0, 0, 0, static_cast<u32>(fw),
                                       static_cast<u32>(fh));
        if (auto fence = upload->submit()) {
            fence->wait();
        }
    }
    if (auto trans_cmd = device.create_command_buffer()) {
        if (auto trans_fence = device.create_fence(false)) {
            trans_cmd->begin();
            trans_cmd->transition_texture_for_sampling(*m_font_texture);
            trans_cmd->end();
            device.submit(*trans_cmd, rhi::SubmitInfo{.signal_fence = trans_fence.get()});
            trans_fence->wait();
        }
    }
    device.wait_idle();
    rhi::TextureViewDesc vd{};
    vd.texture = m_font_texture.get();
    m_font_view = device.create_texture_view(vd);
    if (!m_font_view) {
        NF_LOG_ERROR(LogCategory::Editor, "UiRenderer: font view failed");
        shutdown();
        return false;
    }
    io.Fonts->SetTexID(static_cast<ImTextureID>(kFontTextureId));

    NF_LOG_INFO(LogCategory::Editor, "UiRenderer initialized (font {}x{})", fw, fh);
    return true;
}

void UiRenderer::shutdown() {
    if (m_device != nullptr) {
        m_device->wait_idle();
    }
    m_pipelines.clear();
    if (m_allocator) {
        m_allocator->reset();
        m_allocator.reset();
    }
    m_vb.reset();
    m_ib.reset();
    m_vb_cap = 0;
    m_ib_cap = 0;
    m_font_view.reset();
    m_font_texture.reset();
    m_sampler.reset();
    m_layout.reset();
    m_fs.reset();
    m_vs.reset();
    m_viewport_view = nullptr;
    m_viewport_sampler = nullptr;
    m_device = nullptr;
}

bool UiRenderer::valid() const {
    return m_device != nullptr && m_vs && m_fs && m_layout && m_sampler && m_font_view &&
           m_allocator;
}

void UiRenderer::set_viewport_texture(const rhi::TextureView* view, const rhi::Sampler* sampler) {
    m_viewport_view = view;
    m_viewport_sampler = sampler;
}

void UiRenderer::set_content_textures(const TextureBinding* bindings, size_t count) {
    m_content_bindings.clear();
    if (bindings != nullptr && count > 0) {
        m_content_bindings.assign(bindings, bindings + count);
    }
}

rhi::Pipeline* UiRenderer::pipeline_for(const rhi::RenderPass& pass) {
    if (m_device == nullptr || !valid()) {
        return nullptr;
    }
    auto it = m_pipelines.find(&pass);
    if (it != m_pipelines.end()) {
        return it->second.get();
    }
    static const std::array<rhi::VertexAttrib, 3> attribs{{
        {0, 0, rhi::Format::R32G32_SFloat},
        {1, 8, rhi::Format::R32G32_SFloat},
        {2, 16, rhi::Format::R8G8B8A8_UNorm},
    }};
    rhi::VertexLayout vl{};
    vl.binding = 0;
    vl.stride = sizeof(ImDrawVert);
    vl.attributes = std::span<const rhi::VertexAttrib>(attribs);
    rhi::PipelineDesc pd{};
    pd.vs = m_vs.get();
    pd.fs = m_fs.get();
    pd.vertex_layout = vl;
    pd.topology = rhi::PrimitiveTopology::TriangleList;
    pd.rasterizer.cull_mode = rhi::CullMode::None;
    pd.depth.test_enabled = false;
    pd.depth.write_enabled = false;
    pd.render_pass = &pass;
    pd.descriptor_set_layout = m_layout.get();
    pd.push_constant_size = 16; // scale.xy + translate.xy
    pd.push_constant_stages = rhi::ShaderStage::Vertex;
    auto pipe = m_device->create_pipeline(pd);
    if (!pipe) {
        NF_LOG_ERROR(LogCategory::Editor, "UiRenderer: pipeline creation failed");
        return nullptr;
    }
    rhi::Pipeline* raw = pipe.get();
    m_pipelines.emplace(&pass, std::move(pipe));
    return raw;
}

bool UiRenderer::ensure_buffers(size_t vtx_count, size_t idx_count) {
    if (m_device == nullptr) {
        return false;
    }
    if (vtx_count > m_vb_cap) {
        const size_t want = (m_vb_cap == 0) ? vtx_count : m_vb_cap;
        size_t cap = want;
        while (cap < vtx_count) {
            cap *= 2;
        }
        rhi::BufferDesc bd{};
        bd.size = cap * sizeof(ImDrawVert);
        bd.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::TransferDst;
        bd.memory = rhi::MemoryUsage::CPUToGPU;
        auto nb = m_device->create_buffer(bd);
        if (!nb) {
            return false;
        }
        m_vb = std::move(nb); // caller guarantees the old buffer is idle
        m_vb_cap = cap;
    }
    if (idx_count > m_ib_cap) {
        size_t cap = (m_ib_cap == 0) ? idx_count : m_ib_cap;
        while (cap < idx_count) {
            cap *= 2;
        }
        rhi::BufferDesc bd{};
        bd.size = cap * sizeof(u32);
        bd.usage = rhi::BufferUsage::Index | rhi::BufferUsage::TransferDst;
        bd.memory = rhi::MemoryUsage::CPUToGPU;
        auto nb = m_device->create_buffer(bd);
        if (!nb) {
            return false;
        }
        m_ib = std::move(nb);
        m_ib_cap = cap;
    }
    return m_vb != nullptr && m_ib != nullptr;
}

bool UiRenderer::render(rhi::CommandBuffer& cmd, const rhi::RenderPass& pass,
                        const ImDrawData* draw_data, uint32_t fb_width, uint32_t fb_height) {
    if (!valid()) {
        return false;
    }
    if (draw_data == nullptr || draw_data->CmdListsCount == 0 || fb_width == 0 || fb_height == 0) {
        return true;
    }
    rhi::Pipeline* pipe = pipeline_for(pass);
    if (pipe == nullptr) {
        return false;
    }

    size_t vtx_total = 0, idx_total = 0;
    for (int n = 0; n < draw_data->CmdListsCount; ++n) {
        const ImDrawList* list = draw_data->CmdLists[n];
        vtx_total += static_cast<size_t>(list->VtxBuffer.Size);
        idx_total += static_cast<size_t>(list->IdxBuffer.Size);
    }
    if (vtx_total == 0 || idx_total == 0) {
        return true;
    }
    if (!ensure_buffers(vtx_total, idx_total)) {
        return false;
    }

    // Pack + upconvert (RHI index buffers are always u32; ImGui uses u16).
    std::vector<ImDrawVert> vtx;
    std::vector<u32> idx;
    vtx.reserve(vtx_total);
    idx.reserve(idx_total);
    for (int n = 0; n < draw_data->CmdListsCount; ++n) {
        const ImDrawList* list = draw_data->CmdLists[n];
        vtx.insert(vtx.end(), list->VtxBuffer.Data,
                   list->VtxBuffer.Data + list->VtxBuffer.Size);
        for (int i = 0; i < list->IdxBuffer.Size; ++i) {
            idx.push_back(static_cast<u32>(list->IdxBuffer.Data[i]));
        }
    }
    m_vb->update(vtx.data(), 0, vtx.size() * sizeof(ImDrawVert));
    m_ib->update(idx.data(), 0, idx.size() * sizeof(u32));

    // Previous frame's GPU work is done by contract: recycle descriptors.
    m_allocator->reset();

    // Build exactly one descriptor set per texture id the recorded draw data
    // references. Scanning the commands first (rather than binding every
    // preview the cache holds) is what keeps a large content tree free: an id
    // that no panel drew this frame allocates nothing.
    std::vector<uintptr_t> used_ids;
    {
        std::unordered_set<uintptr_t> seen;
        seen.reserve(16);
        for (int n = 0; n < draw_data->CmdListsCount; ++n) {
            const ImDrawList* list = draw_data->CmdLists[n];
            for (int ci = 0; ci < list->CmdBuffer.Size; ++ci) {
                const uintptr_t id = static_cast<uintptr_t>(list->CmdBuffer[ci].GetTexID());
                if (seen.insert(id).second) {
                    used_ids.push_back(id);
                }
            }
        }
    }
    std::vector<std::unique_ptr<rhi::DescriptorSet>> id_sets;
    id_sets.reserve(used_ids.size());
    for (uintptr_t id : used_ids) {
        const rhi::TextureView* view = nullptr;
        const rhi::Sampler* samp = nullptr;
        if (id == kFontTextureId) {
            view = m_font_view.get();
            samp = m_sampler.get();
        } else if (id == kViewportTextureId) {
            // The viewport target is optional: when the shell has none, the
            // font atlas is a defined (if useless) sample rather than a crash.
            view = (m_viewport_view != nullptr) ? m_viewport_view : m_font_view.get();
            samp = (m_viewport_sampler != nullptr) ? m_viewport_sampler : m_sampler.get();
        } else {
            for (const TextureBinding& b : m_content_bindings) {
                if (b.id == id && b.view != nullptr) {
                    view = b.view;
                    samp = (b.sampler != nullptr) ? b.sampler : m_sampler.get();
                    break;
                }
            }
        }
        if (view == nullptr) {
            NF_LOG_WARN(LogCategory::Editor, "UiRenderer: unresolvable texture id {}, skipping draw",
                        id);
            continue;
        }
        auto set = m_allocator->allocate(*m_layout);
        if (!set) {
            NF_LOG_ERROR(LogCategory::Editor, "UiRenderer: descriptor allocation failed");
            return false;
        }
        const std::array<rhi::DescriptorWrite, 1> w{{
            {0, rhi::DescriptorType::SampledImage, nullptr, 0, 0, view, samp},
        }};
        m_device->update_descriptor_set(*set, std::span<const rhi::DescriptorWrite>(w));
        id_sets.push_back(std::move(set));
    }
    // id_sets[i] serves used_ids[i]; the draw loop looks up by id.
    auto set_for = [used_ids, &id_sets](uintptr_t id) -> const rhi::DescriptorSet* {
        for (size_t i = 0; i < used_ids.size(); ++i) {
            if (used_ids[i] == id) {
                return id_sets[i].get();
            }
        }
        return nullptr;
    };

    cmd.bind_pipeline(*pipe);
    const std::array<const rhi::Buffer*, 1> vbs{m_vb.get()};
    cmd.bind_vertex_buffers(std::span<const rhi::Buffer* const>(vbs));
    cmd.bind_index_buffer(*m_ib, 0);
    cmd.set_viewport(0, 0, fb_width, fb_height);
    cmd.set_scissor(0, 0, fb_width, fb_height);

    const float fw = static_cast<float>(fb_width);
    const float fh = static_cast<float>(fb_height);
    const float pc[4] = {
        2.0f / fw,
        2.0f / fh,
        -1.0f - draw_data->DisplayPos.x * (2.0f / fw),
        -1.0f - draw_data->DisplayPos.y * (2.0f / fh),
    };
    cmd.push_constants(rhi::ShaderStage::Vertex, 0, sizeof(pc), pc);

    const float fb_scale_x = draw_data->FramebufferScale.x;
    const float fb_scale_y = draw_data->FramebufferScale.y;
    size_t vtx_base = 0, idx_base = 0;
    for (int n = 0; n < draw_data->CmdListsCount; ++n) {
        const ImDrawList* list = draw_data->CmdLists[n];
        for (int ci = 0; ci < list->CmdBuffer.Size; ++ci) {
            const ImDrawCmd* pcmd = &list->CmdBuffer[ci];
            if (pcmd->UserCallback != nullptr) {
                NF_LOG_WARN(LogCategory::Editor, "UiRenderer: skipping UserCallback draw command");
                continue;
            }
            if (pcmd->ElemCount == 0) {
                continue;
            }
            // ClipRect is in framebuffer pixels already scaled by the backend
            // contract (DisplayPos/scale applied above for vertices; clip
            // needs the framebuffer scale factor).
            float cx0 = (pcmd->ClipRect.x - draw_data->DisplayPos.x) * fb_scale_x;
            float cy0 = (pcmd->ClipRect.y - draw_data->DisplayPos.y) * fb_scale_y;
            float cx1 = (pcmd->ClipRect.z - draw_data->DisplayPos.x) * fb_scale_x;
            float cy1 = (pcmd->ClipRect.w - draw_data->DisplayPos.y) * fb_scale_y;
            if (cx1 <= cx0 || cy1 <= cy0) {
                continue;
            }
            const u32 sx = static_cast<u32>(cx0 > 0.0f ? cx0 : 0.0f);
            const u32 sy = static_cast<u32>(cy0 > 0.0f ? cy0 : 0.0f);
            u32 ex = static_cast<u32>(cx1);
            u32 ey = static_cast<u32>(cy1);
            if (ex > fb_width) {
                ex = fb_width;
            }
            if (ey > fb_height) {
                ey = fb_height;
            }
            if (ex <= sx || ey <= sy) {
                continue;
            }
            const ImTextureID tex_id = pcmd->GetTexID();
            const rhi::DescriptorSet* set = set_for(static_cast<uintptr_t>(tex_id));
            if (set == nullptr) {
                continue; // unresolved above and already logged
            }
            const std::array<const rhi::DescriptorSet*, 1> sets{set};
            cmd.bind_descriptor_sets(*m_layout, std::span<const rhi::DescriptorSet* const>(sets), 0);
            cmd.set_scissor(sx, sy, ex - sx, ey - sy);
            cmd.draw_indexed(pcmd->ElemCount, 1,
                             static_cast<u32>(idx_base + static_cast<size_t>(pcmd->IdxOffset)),
                             static_cast<i32>(vtx_base + static_cast<size_t>(pcmd->VtxOffset)), 0);
        }
        vtx_base += static_cast<size_t>(list->VtxBuffer.Size);
        idx_base += static_cast<size_t>(list->IdxBuffer.Size);
    }
    return true;
}

} // namespace nf::editor
