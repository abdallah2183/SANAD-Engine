// Samples/MedievalVillage/UiOverlay.cpp — see UiOverlay.hpp.

#include "UiOverlay.hpp"

#include <NF/Core/Logger.hpp>
#include <NF/UI/ArabicShaper.hpp>
#include <NF/UI/Localization.hpp>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>

#include <array>
#include <cstring>
#include <fstream>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// Declared here on purpose: the backend header keeps it inside `#if 0` so that
// including it does not drag <windows.h> into every translation unit.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);
#endif

namespace nf::sample::medieval {

namespace {

constexpr uintptr_t kFontTextureId = 1;

// ImDrawVert must be the packed 20-byte vertex the pipeline declares.
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

// Latin + Arabic blocks + presentation forms (the shaper emits FE70-FEFF), plus
// General Punctuation for the dashes and quotes a mixed line uses. Same ranges
// the editor loads, so a HUD string shapes identically in both.
const ImWchar* arabic_glyph_ranges() {
    static const ImWchar kRanges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin-1
        0x0600, 0x06FF, // Arabic
        0x0750, 0x077F, // Arabic Supplement
        0x08A0, 0x08FF, // Arabic Extended-A
        0x2010, 0x202F, // General Punctuation
        0xFB50, 0xFDFF, // Arabic Presentation Forms-A
        0xFE70, 0xFEFF, // Arabic Presentation Forms-B
        0,
    };
    return kRanges;
}

std::string find_bundled_font(const char* file) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = fs::current_path(ec);
    if (ec) {
        return {};
    }
    for (int i = 0; i < 6; ++i) {
        const fs::path cand = dir / "Resources" / "fonts" / file;
        if (fs::exists(cand, ec) && !ec) {
            return cand.string();
        }
        if (!dir.has_parent_path()) {
            break;
        }
        dir = dir.parent_path();
    }
    return {};
}

ImU32 to_u32(const ui::UiColor& c) {
    const auto ch = [](float v) -> unsigned {
        const float clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<unsigned>(clamped * 255.0f + 0.5f);
    };
    return IM_COL32(ch(c.r), ch(c.g), ch(c.b), ch(c.a));
}

} // namespace

UiOverlay::~UiOverlay() {
    shutdown();
}

bool UiOverlay::init(rhi::IGraphicsDevice& device, void* hwnd,
                     const std::filesystem::path& shader_dir) {
    shutdown();
    m_device = &device;

    // --- ImGui context, fonts, style ----------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    m_context_created = (ImGui::GetCurrentContext() != nullptr);
    if (!m_context_created) {
        NF_LOG_ERROR(LogCategory::Core, "UiOverlay: ImGui::CreateContext failed");
        shutdown();
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // deterministic layout; a game writes no imgui.ini

    if (ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 20.0f)) {
        (void)font;
        m_font_used = "Segoe UI";
    } else {
        io.Fonts->AddFontDefault();
        m_font_used = "ImGui default";
    }
    // Arabic companion font, merged into the same atlas: the shaper emits
    // presentation forms (FE70-FEFF) and they must resolve in the SAME font
    // ImGui draws the line with, or an Arabic HUD line comes out as tofu.
    if (const std::string path = find_bundled_font("Amiri-Regular.ttf"); !path.empty()) {
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.PixelSnapH = true;
        if (io.Fonts->AddFontFromFileTTF(path.c_str(), 20.0f, &cfg, arabic_glyph_ranges()) !=
            nullptr) {
            m_arabic_font = true;
            m_font_used += " + Amiri (AR)";
        }
    }

    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0.0f;
    st.FrameRounding = 2.0f;
    st.WindowBorderSize = 1.0f;
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.12f, 0.94f);
    c[ImGuiCol_Border] = ImVec4(0.25f, 0.28f, 0.33f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.298f, 0.553f, 1.000f, 0.35f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.298f, 0.553f, 1.000f, 0.55f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.298f, 0.553f, 1.000f, 0.75f);
    c[ImGuiCol_Button] = ImVec4(0.298f, 0.553f, 1.000f, 0.40f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.298f, 0.553f, 1.000f, 0.60f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.298f, 0.553f, 1.000f, 0.85f);

    if (hwnd != nullptr) {
        if (!ImGui_ImplWin32_Init(hwnd)) {
            NF_LOG_ERROR(LogCategory::Core, "UiOverlay: ImGui_ImplWin32_Init failed");
            shutdown();
            return false;
        }
    }

    // --- RHI pipeline --------------------------------------------------------
    const auto vs_code = load_spirv_file(shader_dir / "imgui_vert.spv");
    const auto fs_code = load_spirv_file(shader_dir / "imgui_frag.spv");
    if (vs_code.empty() || fs_code.empty()) {
        NF_LOG_ERROR(LogCategory::Core, "UiOverlay: missing imgui SPIR-V in '{}'",
                     shader_dir.string());
        shutdown();
        return false;
    }
    m_vs = device.create_shader_module({std::span<const u8>(vs_code), rhi::ShaderStage::Vertex});
    m_fs = device.create_shader_module({std::span<const u8>(fs_code), rhi::ShaderStage::Fragment});
    if (!m_vs || !m_fs) {
        NF_LOG_ERROR(LogCategory::Core, "UiOverlay: shader module creation failed");
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
        NF_LOG_ERROR(LogCategory::Core, "UiOverlay: descriptor layout failed");
        shutdown();
        return false;
    }

    rhi::SamplerDesc samp{};
    samp.mag = rhi::Filter::Linear;
    samp.min = rhi::Filter::Linear;
    samp.address_u = rhi::AddressMode::ClampToEdge;
    samp.address_v = rhi::AddressMode::ClampToEdge;
    m_sampler = device.create_sampler(samp);
    m_allocator = device.create_descriptor_allocator(8);
    if (!m_sampler || !m_allocator) {
        NF_LOG_ERROR(LogCategory::Core, "UiOverlay: sampler/allocator failed");
        shutdown();
        return false;
    }

    // Font atlas upload (staging + UploadContext + transition, the engine's
    // one-shot upload path).
    unsigned char* px = nullptr;
    int fw = 0, fh = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
    if (px == nullptr || fw <= 0 || fh <= 0) {
        NF_LOG_ERROR(LogCategory::Core, "UiOverlay: font atlas unavailable");
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
        NF_LOG_ERROR(LogCategory::Core, "UiOverlay: font staging failed");
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
        NF_LOG_ERROR(LogCategory::Core, "UiOverlay: font view failed");
        shutdown();
        return false;
    }
    io.Fonts->SetTexID(static_cast<ImTextureID>(kFontTextureId));

    m_font_set = m_allocator->allocate(*m_layout);
    if (!m_font_set) {
        NF_LOG_ERROR(LogCategory::Core, "UiOverlay: font descriptor set failed");
        shutdown();
        return false;
    }
    const std::array<rhi::DescriptorWrite, 1> w{{
        {0, rhi::DescriptorType::SampledImage, nullptr, 0, 0, m_font_view.get(), m_sampler.get()},
    }};
    device.update_descriptor_set(*m_font_set, std::span<const rhi::DescriptorWrite>(w));

    NF_LOG_INFO(LogCategory::Core, "UiOverlay ready (font {}x{}, {})", fw, fh, m_font_used);
    return true;
}

void UiOverlay::shutdown() {
    detach_swapchain();
    m_pipeline.reset();
    if (m_device != nullptr) {
        m_device->wait_idle();
    }
    if (m_allocator) {
        m_allocator->reset();
    }
    m_font_set.reset();
    m_allocator.reset();
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
    m_draw_data = nullptr;
    if (m_context_created) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        m_context_created = false;
    }
    m_device = nullptr;
}

bool UiOverlay::valid() const {
    return m_device != nullptr && m_context_created && m_vs && m_fs && m_layout && m_sampler &&
           m_font_view && m_font_set && m_allocator;
}

void UiOverlay::detach_swapchain() {
    if (m_device != nullptr) {
        m_device->wait_idle();
    }
    m_framebuffers.clear();
    m_pass.reset();
    m_swapchain = nullptr;
}

bool UiOverlay::attach_swapchain(rhi::Swapchain& swapchain) {
    detach_swapchain();
    if (!valid()) {
        return false;
    }
    m_swapchain = &swapchain;

    // Color Load: the scene is already in the image and must survive. The pass
    // blends (SrcAlpha / OneMinusSrcAlpha) so a HUD panel's own alpha works, and
    // present_source makes the final layout presentation-ready — the same
    // contract the editor's overlay pass uses.
    rhi::ColorAttachment ca{};
    ca.format = swapchain.format();
    ca.blend_enabled = true;
    ca.src_color = rhi::BlendFactor::SrcAlpha;
    ca.dst_color = rhi::BlendFactor::OneMinusSrcAlpha;
    const std::array<rhi::ColorAttachment, 1> atts{ca};
    rhi::RenderPassDesc rpd{};
    rpd.color_attachments = std::span<const rhi::ColorAttachment>(atts);
    rpd.color_load = rhi::RenderPassDesc::ColorLoad::Load;
    rpd.present_source = true;
    m_pass = m_device->create_render_pass(rpd);
    if (!m_pass) {
        NF_LOG_ERROR(LogCategory::Core, "UiOverlay: overlay render pass failed");
        return false;
    }
    for (u32 i = 0; i < swapchain.image_count(); ++i) {
        rhi::Texture* tex = swapchain.get_texture(i);
        if (tex == nullptr) {
            NF_LOG_ERROR(LogCategory::Core, "UiOverlay: swapchain image {} is null", i);
            return false;
        }
        const std::array<rhi::Texture*, 1> cols{tex};
        auto fb = m_device->create_framebuffer(
            *m_pass, std::span<rhi::Texture* const>(cols), nullptr);
        if (!fb) {
            NF_LOG_ERROR(LogCategory::Core, "UiOverlay: framebuffer {} failed", i);
            return false;
        }
        m_framebuffers.push_back(std::move(fb));
    }
    return m_framebuffers.size() == swapchain.image_count();
}

void UiOverlay::begin_frame(f32 dt) {
    if (!m_context_created) {
        return;
    }
    ImGui::GetIO().DeltaTime = (dt > 0.0f) ? dt : (1.0f / 60.0f);
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void UiOverlay::draw(const ui::DisplayList& list, f32 width, f32 height) {
    if (!m_context_created || width <= 0.0f || height <= 0.0f) {
        return;
    }
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (dl == nullptr) {
        return;
    }
    for (const ui::DlRect& r : list.rects) {
        dl->AddRectFilled(ImVec2(r.rect.x, r.rect.y), ImVec2(r.rect.x + r.rect.w, r.rect.y + r.rect.h),
                          to_u32(r.color));
    }
    for (const ui::DlLine& l : list.lines) {
        dl->AddLine(ImVec2(l.x1, l.y1), ImVec2(l.x2, l.y2), to_u32(l.color), l.width);
    }
    // Text: the list carries logical-order UTF-8 and shaping belongs to the
    // backend, so an Arabic run is shaped (joining + bidi) before it is drawn.
    ImFont* font = ImGui::GetFont();
    const float font_size_default = ImGui::GetFontSize();
    for (const ui::DlText& t : list.texts) {
        if (t.text.empty()) {
            continue;
        }
        const std::string visual =
            ui::needs_shaping(t.text) ? ui::shape_arabic(t.text) : t.text;
        ImFont* use_font = (font != nullptr) ? font : ImGui::GetFont();
        const float size = (t.size > 0.0f) ? t.size : font_size_default;
        ImVec2 at(t.x, t.y);
        if (use_font != nullptr) {
            const ImVec2 measured =
                use_font->CalcTextSizeA(size, FLT_MAX, 0.0f, visual.c_str(), visual.c_str() + visual.size());
            switch (t.align) {
            case ui::TextAlign::Center: at.x -= measured.x * 0.5f; break;
            case ui::TextAlign::Right:  at.x -= measured.x; break;
            case ui::TextAlign::Left:   break;
            }
            dl->AddText(use_font, size, at, to_u32(t.color), visual.c_str(),
                        visual.c_str() + visual.size());
        }
    }
}

void UiOverlay::draw_canvas(const ui::Canvas& canvas, f32 width, f32 height) {
    draw(canvas.snapshot(width, height), width, height);
}

void UiOverlay::end_frame() {
    if (!m_context_created) {
        return;
    }
    ImGui::Render();
    m_draw_data = ImGui::GetDrawData();
}

rhi::Pipeline* UiOverlay::pipeline() {
    if (!valid() || !m_pass) {
        return nullptr;
    }
    if (m_pipeline) {
        return m_pipeline.get();
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
    pd.render_pass = m_pass.get();
    pd.descriptor_set_layout = m_layout.get();
    pd.push_constant_size = 16; // scale.xy + translate.xy
    pd.push_constant_stages = rhi::ShaderStage::Vertex;
    m_pipeline = m_device->create_pipeline(pd);
    if (!m_pipeline) {
        NF_LOG_ERROR(LogCategory::Core, "UiOverlay: pipeline creation failed");
    }
    return m_pipeline.get();
}

bool UiOverlay::ensure_buffers(usize vtx_count, usize idx_count) {
    if (m_device == nullptr) {
        return false;
    }
    if (vtx_count > m_vb_cap) {
        usize cap = (m_vb_cap == 0) ? vtx_count : m_vb_cap;
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
        m_vb = std::move(nb); // the caller has already waited on the frame fence
        m_vb_cap = cap;
    }
    if (idx_count > m_ib_cap) {
        usize cap = (m_ib_cap == 0) ? idx_count : m_ib_cap;
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

bool UiOverlay::render(rhi::CommandBuffer& cmd, rhi::Texture& target, u32 image_index, u32 width,
                       u32 height) {
    const ImDrawData* draw_data = m_draw_data;
    if (!valid() || !m_pass || image_index >= m_framebuffers.size()) {
        return false;
    }
    if (draw_data == nullptr || draw_data->CmdListsCount == 0 || width == 0 || height == 0) {
        return true; // an empty frame is a successful no-op
    }
    rhi::Pipeline* pipe = pipeline();
    if (pipe == nullptr) {
        return false;
    }

    usize vtx_total = 0, idx_total = 0;
    for (int n = 0; n < draw_data->CmdListsCount; ++n) {
        const ImDrawList* list = draw_data->CmdLists[n];
        vtx_total += static_cast<usize>(list->VtxBuffer.Size);
        idx_total += static_cast<usize>(list->IdxBuffer.Size);
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
        vtx.insert(vtx.end(), list->VtxBuffer.Data, list->VtxBuffer.Data + list->VtxBuffer.Size);
        for (int i = 0; i < list->IdxBuffer.Size; ++i) {
            idx.push_back(static_cast<u32>(list->IdxBuffer.Data[i]));
        }
    }
    m_vb->update(vtx.data(), 0, vtx.size() * sizeof(ImDrawVert));
    m_ib->update(idx.data(), 0, idx.size() * sizeof(u32));

    // The scene wrote this image as a color attachment; the Load pass reads it
    // back as one, so the layout stays ColorAtt and only a barrier is needed.
    cmd.barrier_texture(target, rhi::ImageUsage::ColorAtt, rhi::ImageUsage::ColorAtt);
    cmd.begin_render_pass(*m_pass, *m_framebuffers[image_index],
                          std::span<const rhi::ClearValue>{});
    cmd.bind_pipeline(*pipe);
    const std::array<const rhi::Buffer*, 1> vbs{m_vb.get()};
    cmd.bind_vertex_buffers(std::span<const rhi::Buffer* const>(vbs));
    cmd.bind_index_buffer(*m_ib, 0);
    const std::array<const rhi::DescriptorSet*, 1> sets{m_font_set.get()};
    cmd.bind_descriptor_sets(*m_layout, std::span<const rhi::DescriptorSet* const>(sets), 0);
    cmd.set_viewport(0, 0, width, height);
    cmd.set_scissor(0, 0, width, height);

    const float fw = static_cast<float>(width);
    const float fh = static_cast<float>(height);
    const float pc[4] = {
        2.0f / fw,
        2.0f / fh,
        -1.0f - draw_data->DisplayPos.x * (2.0f / fw),
        -1.0f - draw_data->DisplayPos.y * (2.0f / fh),
    };
    cmd.push_constants(rhi::ShaderStage::Vertex, 0, sizeof(pc), pc);

    const float fb_scale_x = draw_data->FramebufferScale.x;
    const float fb_scale_y = draw_data->FramebufferScale.y;
    usize vtx_base = 0, idx_base = 0;
    for (int n = 0; n < draw_data->CmdListsCount; ++n) {
        const ImDrawList* list = draw_data->CmdLists[n];
        for (int ci = 0; ci < list->CmdBuffer.Size; ++ci) {
            const ImDrawCmd* pcmd = &list->CmdBuffer[ci];
            if (pcmd->UserCallback != nullptr || pcmd->ElemCount == 0) {
                continue;
            }
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
            if (ex > width) {
                ex = width;
            }
            if (ey > height) {
                ey = height;
            }
            if (ex <= sx || ey <= sy) {
                continue;
            }
            cmd.set_scissor(sx, sy, ex - sx, ey - sy);
            cmd.draw_indexed(pcmd->ElemCount, 1,
                             static_cast<u32>(idx_base + static_cast<usize>(pcmd->IdxOffset)),
                             static_cast<i32>(vtx_base + static_cast<usize>(pcmd->VtxOffset)), 0);
        }
        vtx_base += static_cast<usize>(list->VtxBuffer.Size);
        idx_base += static_cast<usize>(list->IdxBuffer.Size);
    }
    cmd.end_render_pass();
    return true;
}

bool UiOverlay::handle_win32_message(void* hwnd, u32 msg, u64 wparam, i64 lparam) {
#ifdef _WIN32
    if (ImGui::GetCurrentContext() == nullptr) {
        return false;
    }
    ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(hwnd), static_cast<UINT>(msg),
                                   static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));
#else
    (void)hwnd;
    (void)msg;
    (void)wparam;
    (void)lparam;
#endif
    return false; // never consumes: engine input and DefWindowProc always run
}

} // namespace nf::sample::medieval
