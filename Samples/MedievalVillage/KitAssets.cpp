// Samples/MedievalVillage/KitAssets.cpp — see KitAssets.hpp.

#include "KitAssets.hpp"

#include <NF/Assets/MeshAsset.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Rendering/ImageDecode.hpp>
#include <NF/Rendering/MaterialLibrary.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/MeshUpload.hpp>
#include <NF/Rendering/Renderer3D.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

namespace nf::sample::medieval {

namespace {

/// Parses "x,y,z" (the manifest writes the importer's numbers verbatim, spaces
/// already stripped by import_kit.sh). Returns false on anything malformed, so a
/// hand-edited manifest fails visibly instead of silently becoming a zero-size
/// piece the layout would place at the origin. from_chars, not sscanf: the
/// build treats CRT deprecation warnings as errors.
bool parse_vec3(std::string_view text, Vec3& out) {
    float v[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 3; ++i) {
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v[i]);
        if (ec != std::errc{}) {
            return false;
        }
        // NOTE: ptr == end is only an error for the first two components (a ','
        // must follow); for z it is the expected success case. Hitting end
        // early on i<2 falls through to the ',' check below and fails there.
        text = text.substr(static_cast<usize>(ptr - text.data()));
        if (i < 2) {
            if (text.empty() || text.front() != ',') {
                return false;
            }
            text = text.substr(1);
        }
    }
    if (!text.empty()) {
        return false; // trailing junk after z
    }
    out = Vec3{v[0], v[1], v[2]};
    return true;
}

/// Decodes a PNG and uploads it as a sampled R8G8B8A8 texture with a linear
/// clamp sampler. The staging -> upload -> transition sequence is the engine's
/// standard one-shot upload (same shape as Editor/src/TexturePreviewCache).
bool upload_texture(rhi::IGraphicsDevice& device, const std::filesystem::path& png,
                    std::unique_ptr<rhi::Texture>& out_tex,
                    std::unique_ptr<rhi::TextureView>& out_view,
                    std::unique_ptr<rhi::Sampler>& out_sampler, std::string& out_err) {
    std::string dec_err;
    const rendering::DecodedImage img = rendering::decode_image_file(png.string(), dec_err);
    if (!img.ok()) {
        out_err = "decode failed: " + dec_err;
        return false;
    }

    rhi::TextureDesc td{};
    td.width = static_cast<u32>(img.width);
    td.height = static_cast<u32>(img.height);
    td.format = rhi::Format::R8G8B8A8_UNorm;
    td.usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst;
    out_tex = device.create_texture(td);
    if (!out_tex) {
        out_err = "create_texture failed";
        return false;
    }

    const usize bytes = img.rgba.size();
    rhi::BufferDesc staging_desc{};
    staging_desc.size = bytes;
    staging_desc.usage = rhi::BufferUsage::TransferSrc;
    staging_desc.memory = rhi::MemoryUsage::CPUToGPU;
    auto staging = device.create_buffer(staging_desc);
    if (!staging) {
        out_err = "staging buffer failed";
        return false;
    }
    staging->update(img.rgba.data(), 0, bytes);

    auto upload = device.create_upload_context();
    if (!upload) {
        out_err = "upload context failed";
        return false;
    }
    upload->copy_buffer_to_texture(*staging, *out_tex, 0, 0, 0, td.width, td.height);
    if (auto fence = upload->submit()) {
        fence->wait();
    }

    // Sampling needs the image out of TransferDst and into ShaderReadOnly.
    if (auto cmd = device.create_command_buffer()) {
        if (auto fence = device.create_fence(false)) {
            cmd->begin();
            cmd->transition_texture_for_sampling(*out_tex);
            cmd->end();
            device.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
            fence->wait();
        }
    }

    rhi::TextureViewDesc vd{};
    vd.texture = out_tex.get();
    out_view = device.create_texture_view(vd);
    if (!out_view) {
        out_err = "texture view failed";
        return false;
    }

    rhi::SamplerDesc sd{};
    sd.mag = rhi::Filter::Linear;
    sd.min = rhi::Filter::Linear;
    sd.mip = rhi::MipMapMode::None;
    sd.address_u = rhi::AddressMode::Repeat;
    sd.address_v = rhi::AddressMode::Repeat;
    out_sampler = device.create_sampler(sd);
    if (!out_sampler) {
        out_err = "sampler failed";
        return false;
    }
    return true;
}

} // namespace

bool Kit::load(const std::filesystem::path& content_dir, rhi::IGraphicsDevice& device,
               rendering::Renderer3D& renderer, rendering::MeshLibrary& meshes,
               std::string& out_error) {
    m_pieces.clear();
    m_index.clear();
    m_textures.clear();
    m_report = LoadReport{};

    const std::filesystem::path manifest = content_dir / "MedievalKit.manifest";
    std::ifstream in(manifest);
    if (!in) {
        out_error = "kit manifest not found: " + manifest.string() +
                    " (run Samples/MedievalVillage/Tools/import_kit.sh)";
        return false;
    }

    // --- 1. Parse ------------------------------------------------------------
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream ls(line);
        std::string keyword;
        ls >> keyword;
        if (keyword != "piece") {
            continue;
        }
        KitPiece p;
        if (!(ls >> p.name)) {
            continue;
        }
        bool have_min = false, have_max = false;
        std::string field;
        while (ls >> field) {
            const auto eq = field.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string key = field.substr(0, eq);
            const std::string value = field.substr(eq + 1);
            if (key == "mesh") {
                p.mesh_rel = value;
            } else if (key == "tex") {
                p.texture = value;
            } else if (key == "min") {
                have_min = parse_vec3(value, p.bounds_min);
            } else if (key == "max") {
                have_max = parse_vec3(value, p.bounds_max);
            } else if (key == "tris") {
                p.triangles = static_cast<u32>(std::strtoul(value.c_str(), nullptr, 10));
            }
        }
        if (!have_min || !have_max) {
            m_report.failures.push_back(p.name + ": malformed bounds in manifest");
            continue;
        }
        m_index[p.name] = m_pieces.size();
        m_pieces.push_back(std::move(p));
    }
    if (m_pieces.empty()) {
        out_error = "kit manifest contained no pieces: " + manifest.string();
        return false;
    }

    // --- 2. Textures + one material per texture ------------------------------
    // Done before the meshes so a material handle exists the moment a piece does.
    for (KitPiece& p : m_pieces) {
        if (m_textures.find(p.texture) != m_textures.end()) {
            continue;
        }
        TextureEntry entry;
        std::string err;
        const std::filesystem::path png = content_dir / "Textures" / (p.texture + ".png");
        if (!upload_texture(device, png, entry.texture, entry.view, entry.sampler, err)) {
            m_report.failures.push_back(p.texture + ": " + err);
            continue;
        }
        ++m_report.textures_uploaded;
        m_textures.emplace(p.texture, std::move(entry));
    }
    for (const auto& [stem, entry] : m_textures) {
        rendering::PBRMaterialParams params{};
        params.base_color[0] = 1.0f;
        params.base_color[1] = 1.0f;
        params.base_color[2] = 1.0f;
        params.base_color[3] = 1.0f;
        params.metallic = 0.0f;
        params.roughness = 0.75f;
        params.ao = 1.0f;
        params.use_base_color_texture = 1.0f;
        const rendering::MaterialHandle handle =
            renderer.materials().create_instance(*renderer.gbuffer_material(), params, stem);
        renderer.materials().set_albedo_texture(handle, *entry.view, *entry.sampler);
        ++m_report.materials_created;
        for (KitPiece& p : m_pieces) {
            if (p.texture == stem) {
                p.material = handle;
            }
        }
    }

    // --- 3. Meshes -----------------------------------------------------------
    for (KitPiece& p : m_pieces) {
        const std::filesystem::path nfmesh = content_dir / p.mesh_rel;
        std::string err;
        auto asset = assets::MeshAsset::load_from_file(nfmesh.string(), err);
        if (!asset) {
            m_report.failures.push_back(p.name + ": " + err);
            continue;
        }
        auto mesh = rendering::make_static_mesh(*asset, p.name);
        if (!mesh) {
            m_report.failures.push_back(p.name + ": make_static_mesh failed");
            continue;
        }
        p.mesh = meshes.add(std::move(mesh));
    }

    if (!meshes.upload_all(device)) {
        // upload_all returns false if ANY mesh failed; the pieces that did upload
        // are still usable, so this is a warning and not a load failure.
        m_report.failures.push_back("MeshLibrary::upload_all reported a failed upload");
    }
    for (const KitPiece& p : m_pieces) {
        if (p.valid()) {
            ++m_report.meshes_uploaded;
        }
    }

    m_report.pieces = m_pieces.size();
    NF_LOG_INFO(LogCategory::Core,
                "MedievalVillage kit: {} pieces, {} meshes uploaded, {} textures, {} materials, {} failures",
                m_report.pieces, m_report.meshes_uploaded, m_report.textures_uploaded,
                m_report.materials_created, m_report.failures.size());
    for (const std::string& f : m_report.failures) {
        NF_LOG_WARN(LogCategory::Core, "MedievalVillage kit: {}", f);
    }
    return m_report.meshes_uploaded > 0;
}

const KitPiece* Kit::piece(std::string_view name) const {
    const auto it = m_index.find(std::string(name));
    if (it == m_index.end()) {
        return nullptr;
    }
    return &m_pieces[it->second];
}

} // namespace nf::sample::medieval
