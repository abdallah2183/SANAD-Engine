#include <NF/Editor/TexturePreviewCache.hpp>
#include <NF/Editor/PreviewPixels.hpp>

#include <NF/Core/Logger.hpp>
#include <NF/Rendering/ImageDecode.hpp>

namespace nf::editor {

TexturePreviewCache::~TexturePreviewCache() {
    shutdown();
}

bool TexturePreviewCache::init(rhi::IGraphicsDevice& device) {
    shutdown();
    m_device = &device;
    rhi::SamplerDesc samp{};
    samp.mag = rhi::Filter::Linear;
    samp.min = rhi::Filter::Linear;
    samp.address_u = rhi::AddressMode::ClampToEdge;
    samp.address_v = rhi::AddressMode::ClampToEdge;
    m_sampler = device.create_sampler(samp);
    if (!m_sampler) {
        NF_LOG_ERROR(nf::LogCategory::Editor, "TexturePreviewCache: sampler creation failed");
        m_device = nullptr;
        return false;
    }
    return true;
}

void TexturePreviewCache::shutdown() {
    if (m_device != nullptr) {
        m_device->wait_idle();
    }
    m_entries.clear(); // unique_ptr members destroy the GPU objects
    m_sampler.reset();
    m_device = nullptr;
    m_next_id = kFirstId;
}

uintptr_t TexturePreviewCache::acquire(assets::VirtualFileSystem& vfs,
                                       const std::string& logical_path) {
    if (m_device == nullptr || logical_path.empty()) {
        return kInvalidId;
    }
    auto found = m_entries.find(logical_path);
    if (found != m_entries.end()) {
        found->second.used = true;
        return found->second.id;
    }

    // Resolve + decode + shrink on the calling thread: this runs during panel
    // recording, but the preview is small and the decode is one-shot per path.
    // A worker would race with the frame's own texture work for no gain here.
    const auto resolved = vfs.resolve(logical_path);
    if (!resolved.ok) {
        return kInvalidId; // unresolvable paths are normal (not-yet-imported)
    }
    std::string dec_err;
    const nf::rendering::DecodedImage img =
        nf::rendering::decode_image_file(resolved.value.string(), dec_err);
    if (!img.ok()) {
        NF_LOG_WARN(nf::LogCategory::Editor, "Preview: cannot decode '{}': {}", logical_path,
                    dec_err);
        return kInvalidId;
    }
    const PreviewPixels small =
        downsample_rgba(img.rgba.data(), img.width, img.height, kMaxDim);
    if (!small.valid()) {
        return kInvalidId;
    }

    rhi::TextureDesc td{};
    td.width = static_cast<u32>(small.width);
    td.height = static_cast<u32>(small.height);
    td.format = rhi::Format::R8G8B8A8_UNorm;
    td.usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst;
    auto tex = m_device->create_texture(td);
    const size_t bytes = small.rgba.size();
    rhi::BufferDesc staging_desc{};
    staging_desc.size = bytes;
    staging_desc.usage = rhi::BufferUsage::TransferSrc;
    staging_desc.memory = rhi::MemoryUsage::CPUToGPU;
    auto staging = m_device->create_buffer(staging_desc);
    if (!tex || !staging) {
        NF_LOG_WARN(nf::LogCategory::Editor, "Preview: GPU allocation failed for '{}'", logical_path);
        return kInvalidId;
    }
    staging->update(small.rgba.data(), 0, bytes);
    // Upload + transition follow the exact sequence UiRenderer uses for the
    // font atlas (staging copy, fence-waited, then a sampling barrier).
    if (auto upload = m_device->create_upload_context()) {
        upload->copy_buffer_to_texture(*staging, *tex, 0, 0, 0,
                                       static_cast<u32>(small.width),
                                       static_cast<u32>(small.height));
        if (auto fence = upload->submit()) {
            fence->wait();
        }
    }
    if (auto trans_cmd = m_device->create_command_buffer()) {
        if (auto trans_fence = m_device->create_fence(false)) {
            trans_cmd->begin();
            trans_cmd->transition_texture_for_sampling(*tex);
            trans_cmd->end();
            m_device->submit(*trans_cmd, rhi::SubmitInfo{.signal_fence = trans_fence.get()});
            trans_fence->wait();
        }
    }
    rhi::TextureViewDesc vd{};
    vd.texture = tex.get();
    auto view = m_device->create_texture_view(vd);
    if (!view) {
        NF_LOG_WARN(nf::LogCategory::Editor, "Preview: view failed for '{}'", logical_path);
        return kInvalidId;
    }

    Entry e;
    e.id = m_next_id++;
    e.texture = std::move(tex);
    e.view = std::move(view);
    e.used = true;
    const uintptr_t id = e.id;
    m_entries.emplace(logical_path, std::move(e));
    NF_LOG_INFO(nf::LogCategory::Editor, "Preview: uploaded '{}' ({}x{} -> {}x{})", logical_path,
                img.width, img.height, small.width, small.height);
    return id;
}

void TexturePreviewCache::invalidate(const std::string& logical_path) {
    m_entries.erase(logical_path);
}

std::vector<TexturePreviewCache::Binding> TexturePreviewCache::drain_used() {
    std::vector<Binding> out;
    for (auto& [path, entry] : m_entries) {
        if (!entry.used || entry.view == nullptr) {
            continue;
        }
        entry.used = false;
        out.push_back(Binding{entry.id, entry.view.get(), m_sampler.get()});
    }
    return out;
}

} // namespace nf::editor
