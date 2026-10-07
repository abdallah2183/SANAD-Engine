// NF/Editor/ImportQueue.cpp — sync + async external import (see header).

#include <NF/Editor/ImportQueue.hpp>
#include <NF/Assets/MeshAsset.hpp>
#include <NF/Assets/MeshImport.hpp>
#include <NF/Core/Hash.hpp>
#include <NF/Jobs/JobSystem.hpp>
#include <NF/Rendering/ImageDecode.hpp>
#include <NF/Rendering/MaterialAsset.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace nf::editor {

namespace {

std::string lower_ext(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return {};
    }
    std::string ext = path.substr(dot);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

std::string file_name_of(const std::string& abs) {
    const size_t slash = abs.find_last_of("/\\");
    return (slash == std::string::npos) ? abs : abs.substr(slash + 1);
}

std::string stem_of(const std::string& abs) {
    std::string name = file_name_of(abs);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) {
        name = name.substr(0, dot);
    }
    if (name.empty()) {
        name = "mesh";
    }
    return name;
}

/// A name safe to use as a file stem: model sources are written by other tools,
/// so an image or material name may contain spaces, slashes or dots. Everything
/// outside [A-Za-z0-9_-] becomes '_' — a deterministic mapping, never a silent
/// rename of one asset onto another.
std::string sanitize_stem_part(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) != 0 || c == '_' || c == '-') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    while (!out.empty() && out.front() == '_') out.erase(out.begin());
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? std::string("asset") : out;
}

/// The sibling directory of a content:// directory, e.g.
/// ("content://Meshes", "Textures") -> "content://Textures". A model import
/// writes its meshes under the chosen directory but its textures and materials
/// under the project's own conventions, so the two cannot drift apart.
std::string sibling_dir(const std::string& dir, const char* name) {
    const size_t slash = dir.find_last_of('/');
    const std::string prefix =
        (slash == std::string::npos) ? std::string("content://") : dir.substr(0, slash + 1);
    return prefix + name;
}

std::string fingerprint_hex(const std::vector<uint8_t>& bytes) {
    const u64 h = fnv1a_64(bytes.data(), bytes.size());
    char buf[17]{};
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

bool read_file_bytes(const std::string& abs, std::vector<uint8_t>& out, std::string& err) {
    std::ifstream in(abs, std::ios::binary | std::ios::ate);
    if (!in) {
        err = "Cannot open source file '" + abs + "'";
        return false;
    }
    const auto size = static_cast<size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    out.resize(size);
    if (size > 0) {
        in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
        if (!in) {
            err = "Cannot read source file '" + abs + "'";
            return false;
        }
    }
    return true;
}

void finish_staged(const std::shared_ptr<ImportStaged>& staged, bool ok, std::string error,
                   float progress) {
    std::lock_guard<std::mutex> lock(staged->mutex);
    staged->ok = ok;
    staged->error = std::move(error);
    staged->progress = progress;
    staged->finished = true;
}

/// A source the importer has to COOK into .nfmesh. Everything the mesh reader
/// understands except .nfmesh itself, which is already cooked.
bool is_model_source(assets::MeshImportFormat format) {
    return format != assets::MeshImportFormat::Unknown &&
           format != assets::MeshImportFormat::NfMesh;
}

/// Drops a previously imported asset completely: registry entry, cooked copy,
/// content copy. Used by overwrite so a re-import never leaves a stale entry
/// pointing at bytes that are about to be replaced.
void remove_previous_import(assets::VirtualFileSystem& vfs, assets::AssetRegistry& reg,
                            const std::string& logical) {
    if (auto prev = reg.find_by_path(logical)) {
        const std::string prev_cooked = prev->cooked_path;
        reg.remove(prev->id);
        if (!prev_cooked.empty()) {
            if (auto rc = vfs.resolve(prev_cooked); rc.ok) {
                std::error_code ec;
                std::filesystem::remove(rc.value, ec);
            }
        }
    }
    if (auto rd = vfs.resolve(logical); rd.ok) {
        std::error_code ec;
        std::filesystem::remove(rd.value, ec);
    }
}

} // namespace

assets::AssetType ImportQueue::type_for_extension(const std::string& ext_lower) {
    // Mesh sources: the cooked container plus every format the reader accepts.
    assets::MeshImportFormat mesh_format = assets::MeshImportFormat::Unknown;
    if (assets::mesh_import_format_from_extension(ext_lower, mesh_format)) {
        return assets::AssetType::Mesh;
    }
    if (ext_lower == ".png" || ext_lower == ".jpg" || ext_lower == ".jpeg" || ext_lower == ".bmp" ||
        ext_lower == ".tga") {
        return assets::AssetType::Texture;
    }
    if (ext_lower == ".nfmat") {
        return assets::AssetType::Material;
    }
    if (ext_lower == ".nfscene") {
        return assets::AssetType::Scene;
    }
    return assets::AssetType::Unknown;
}

std::string ImportQueue::accepted_extension_glob() {
    std::string glob;
    for (assets::MeshImportFormat format : assets::mesh_import_formats()) {
        if (!glob.empty()) {
            glob += ";";
        }
        glob += "*.";
        glob += assets::mesh_import_format_extension(format);
    }
    glob += ";*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.nfmat;*.nfscene";
    return glob;
}

size_t ImportQueue::submit(const std::string& src_absolute, const std::string& dst_dir_logical,
                           bool overwrite, std::string& out_err) {
    if (src_absolute.empty()) {
        out_err = "Source path is empty";
        return 0;
    }
    if (!std::filesystem::exists(src_absolute)) {
        out_err = "Source file does not exist: '" + src_absolute + "'";
        return 0;
    }
    const std::string ext = lower_ext(src_absolute);
    const assets::AssetType type = type_for_extension(ext);
    if (type == assets::AssetType::Unknown) {
        out_err = "Unsupported extension for '" + src_absolute + "' (accepted: " +
                  accepted_extension_glob() + ")";
        return 0;
    }
    std::string dir = dst_dir_logical;
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) {
        dir.pop_back();
    }
    if (dir.rfind("content://", 0) != 0) {
        out_err = "Destination must live under content:// (got '" + dst_dir_logical + "')";
        return 0;
    }

    assets::MeshImportFormat source_format = assets::MeshImportFormat::Unknown;
    if (type == assets::AssetType::Mesh) {
        assets::mesh_import_format_from_extension(ext, source_format);
    }
    const bool cook_needed = is_model_source(source_format);

    ImportJob job;
    job.id = m_next_id++;
    job.src_absolute = src_absolute;
    job.dst_dir = dir;
    // A model source lands as a cooked .nfmesh named after the source stem: the
    // cooked mesh IS the asset, and the uncooked file is not copied into the
    // project (a second copy is an invitation to edit the wrong one).
    job.dst_logical = dir + "/" + (cook_needed ? stem_of(src_absolute) + ".nfmesh"
                                               : file_name_of(src_absolute));
    job.overwrite = overwrite;
    job.type = type;
    job.source_format = source_format;
    m_jobs.push_back(std::move(job));
    return m_jobs.back().id;
}

bool ImportQueue::has_pending() const {
    for (const auto& j : m_jobs) {
        if (j.state == ImportJob::State::Queued || j.state == ImportJob::State::Working) {
            return true;
        }
    }
    return false;
}

void ImportQueue::clear_finished() {
    std::vector<ImportJob> keep;
    for (auto& j : m_jobs) {
        if (j.state == ImportJob::State::Queued || j.state == ImportJob::State::Working) {
            keep.push_back(std::move(j));
        }
    }
    m_jobs.swap(keep);
}

// Worker body: pure CPU, no shared state except the staged block (locked).
// Any exception is converted to failure — a worker must never terminate.
void ImportQueue::run_cpu_stages(const std::string& src_absolute, const std::string& dst_dir,
                                 assets::AssetType type,
                                 assets::MeshImportFormat source_format, size_t job_id,
                                 const std::shared_ptr<ImportStaged>& staged) {
    // The destination names are derived here rather than passed in, so a worker
    // needs nothing but the source path, the destination directory and the two
    // enums.
    const std::string dst_stem = stem_of(src_absolute);
    const std::string dst_name = file_name_of(src_absolute);
    try {
        std::vector<uint8_t> bytes;
        {
            std::string err;
            if (!read_file_bytes(src_absolute, bytes, err) || bytes.empty()) {
                finish_staged(staged, false,
                              bytes.empty() && err.empty()
                                  ? "Source file is empty: '" + src_absolute + "'"
                                  : err,
                              0.1f);
                return;
            }
        }
        {
            std::lock_guard<std::mutex> lock(staged->mutex);
            staged->progress = 0.3f;
        }

        std::vector<ImportStagedFile> files;
        std::vector<std::string> warnings;

        if (type == assets::AssetType::Mesh && is_model_source(source_format)) {
            // Cook the source into one .nfmesh per mesh it contains. The reader
            // is the engine's single mesh entry point, so the editor accepts
            // exactly the formats NFModelImporter and the cookers accept.
            const assets::MeshImportResult imported = assets::import_mesh_file(src_absolute);
            if (!imported.ok) {
                finish_staged(staged, false, "Model import failed: " + imported.error, 0.3f);
                return;
            }
            if (imported.meshes.empty()) {
                finish_staged(staged, false,
                              "Model import produced no meshes (source: '" + src_absolute + "')",
                              0.3f);
                return;
            }
            warnings = imported.warnings;

            const std::string textures_dir = sibling_dir(dst_dir, "Textures");
            const std::string materials_dir = sibling_dir(dst_dir, "Materials");

            // Textures and materials are staged into their own lists and
            // appended AFTER the meshes, so `written` keeps the job's primary
            // destination at index 0 — which is what the UI names and what a
            // caller opens. The texture paths still have to be computed first,
            // because a material's `albedo` must name a file that exists.
            std::vector<ImportStagedFile> texture_files;
            std::vector<ImportStagedFile> material_files;

            std::vector<std::string> image_paths(imported.images.size());
            for (usize i = 0; i < imported.images.size(); ++i) {
                const assets::MeshImportImage& image = imported.images[i];
                if (image.extension.empty()) {
                    // No magic number matched. Writing it with a made-up
                    // extension would be a guess; .bin is the honest name for
                    // "bytes whose container this engine does not recognise".
                    warnings.push_back("texture '" + image.name +
                                       "' is in a container this engine does not decode; it was "
                                       "written as .bin and will not load as a texture");
                }
                ImportStagedFile file;
                file.logical_path = textures_dir + "/" + dst_stem + "_" +
                                    sanitize_stem_part(image.name) +
                                    (image.extension.empty() ? ".bin" : image.extension);
                file.bytes = image.bytes;
                file.registered = true;
                file.type = assets::AssetType::Texture;
                image_paths[i] = file.logical_path;
                texture_files.push_back(std::move(file));
            }

            for (usize i = 0; i < imported.meshes.size(); ++i) {
                ImportStagedFile file;
                file.logical_path =
                    dst_dir + "/" + ((i == 0) ? dst_stem + ".nfmesh"
                                              : dst_stem + "_" + std::to_string(i) + ".nfmesh");
                if (!imported.meshes[i]->save_to_bytes(file.bytes)) {
                    finish_staged(staged, false, "Failed to serialize cooked mesh", 0.3f);
                    return;
                }
                file.file_id = imported.meshes[i]->id;
                file.registered = true;
                file.type = assets::AssetType::Mesh;
                files.push_back(std::move(file));
            }

            // Materials as .nfmat — the only route a material has into the
            // engine, because .nfmesh carries geometry and nothing else. The
            // submesh material_slot indexes this list in order.
            for (usize i = 0; i < imported.materials.size(); ++i) {
                const assets::GltfMaterialInfo& material = imported.materials[i];
                rendering::MaterialAsset asset;
                asset.name = material.name.empty()
                                 ? dst_stem + "_material" + std::to_string(i)
                                 : material.name;
                for (int c = 0; c < 4; ++c) {
                    asset.params.base_color[c] = material.base_color[c];
                }
                asset.params.metallic = material.metallic;
                asset.params.roughness = material.roughness;
                for (int c = 0; c < 3; ++c) {
                    asset.params.emission[c] = material.emissive[c];
                }
                asset.params.emission_strength = material.emissive_strength;
                if (material.albedo_image >= 0 &&
                    static_cast<usize>(material.albedo_image) < image_paths.size()) {
                    asset.albedo = image_paths[static_cast<usize>(material.albedo_image)];
                }
                // PBR maps ride the same staged images: each index is the same
                // list, only the .nfmat key differs.
                auto wire_map = [&](int image_index, std::string& slot) {
                    if (image_index >= 0 &&
                        static_cast<usize>(image_index) < image_paths.size()) {
                        slot = image_paths[static_cast<usize>(image_index)];
                    }
                };
                wire_map(material.normal_image, asset.normal);
                wire_map(material.metallic_roughness_image, asset.mrough);
                wire_map(material.occlusion_image, asset.occlusion);
                wire_map(material.emissive_image, asset.emissive);
                if (!material.dropped.empty()) {
                    // One readable line per material instead of one per slot: a
                    // full MTL declares four or five things this pipeline has no
                    // room for, and five separate warnings per material would
                    // bury the import result.
                    std::string joined;
                    for (const std::string& entry : material.dropped) {
                        if (!joined.empty()) joined += ", ";
                        joined += entry;
                    }
                    warnings.push_back("material '" + asset.name + "' declares " + joined);
                }

                ImportStagedFile file;
                file.logical_path = materials_dir + "/" + dst_stem + "_" +
                                    sanitize_stem_part(asset.name) + ".nfmat";
                const std::string text = asset.save_to_text();
                file.bytes.assign(text.begin(), text.end());
                file.registered = false; // text assets bypass the registry
                file.type = assets::AssetType::Material;
                material_files.push_back(std::move(file));
            }

            for (ImportStagedFile& file : texture_files) {
                files.push_back(std::move(file));
            }
            for (ImportStagedFile& file : material_files) {
                files.push_back(std::move(file));
            }
        } else if (type == assets::AssetType::Mesh) {
            std::string perr;
            auto asset = assets::MeshAsset::load_from_bytes(std::span<const uint8_t>(bytes), perr);
            if (!asset) {
                finish_staged(staged, false, "Invalid .nfmesh: " + perr, 0.3f);
                return;
            }
            ImportStagedFile file;
            file.logical_path = dst_dir + "/" + dst_name;
            file.bytes = bytes;
            file.file_id = asset->id;
            file.registered = true;
            file.type = assets::AssetType::Mesh;
            files.push_back(std::move(file));
        } else if (type == assets::AssetType::Texture) {
            std::string derr;
            rendering::DecodedImage img =
                rendering::decode_image_memory(bytes.data(), bytes.size(), derr);
            if (!img.ok()) {
                finish_staged(staged, false, "Undecodable image: " + derr, 0.3f);
                return;
            }
            ImportStagedFile file;
            file.logical_path = dst_dir + "/" + dst_name;
            file.bytes = bytes;
            file.registered = true;
            file.type = assets::AssetType::Texture;
            files.push_back(std::move(file));
        } else if (type == assets::AssetType::Material) {
            rendering::MaterialAsset mat;
            std::string merr;
            const std::string text(bytes.begin(), bytes.end());
            if (!rendering::MaterialAsset::load_from_text(text, mat, merr)) {
                finish_staged(staged, false, "Invalid .nfmat: " + merr, 0.3f);
                return;
            }
            ImportStagedFile file;
            file.logical_path = dst_dir + "/" + dst_name;
            file.bytes = bytes;
            file.type = assets::AssetType::Material;
            files.push_back(std::move(file));
        } else if (type == assets::AssetType::Scene) {
            // Scene validation needs a file for the loader but must not touch
            // VFS (shared): use a uniquely-named temp file, cleaned below.
            const std::string scratch =
                (std::filesystem::temp_directory_path() /
                 ("nf_import_validate_" + std::to_string(job_id) + ".tmp"))
                    .string();
            {
                std::ofstream o(scratch, std::ios::binary | std::ios::trunc);
                if (!o) {
                    finish_staged(staged, false, "Scratch write failed during validation", 0.3f);
                    return;
                }
                o.write(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
                if (!o) {
                    std::error_code ec;
                    std::filesystem::remove(scratch, ec);
                    finish_staged(staged, false, "Scratch write failed during validation", 0.3f);
                    return;
                }
            }
            auto lr = runtime::load_scene_from_physical(scratch);
            std::error_code ec;
            std::filesystem::remove(scratch, ec);
            if (!lr.success) {
                finish_staged(staged, false, "Invalid .nfscene: " + lr.error, 0.3f);
                return;
            }
            ImportStagedFile file;
            file.logical_path = dst_dir + "/" + dst_name;
            file.bytes = bytes;
            file.type = assets::AssetType::Scene;
            files.push_back(std::move(file));
        }

        if (files.empty()) {
            finish_staged(staged, false, "Import produced no files", 0.3f);
            return;
        }

        const std::string fp = fingerprint_hex(bytes);
        {
            std::lock_guard<std::mutex> lock(staged->mutex);
            staged->files = std::move(files);
            staged->warnings = std::move(warnings);
            staged->fingerprint = fp;
            staged->progress = 0.55f;
        }
        finish_staged(staged, true, {}, 0.55f);
    } catch (const std::exception& e) {
        finish_staged(staged, false, std::string("Import worker exception: ") + e.what(), 0.1f);
    } catch (...) {
        finish_staged(staged, false, "Import worker unknown exception", 0.1f);
    }
}

bool ImportQueue::commit_staged(assets::VirtualFileSystem& vfs, assets::AssetRegistry& reg,
                                ImportJob& job) {
    // Caller guarantees a finished staged block; single-threaded commit.
    const std::shared_ptr<ImportStaged> staged = job.staged;
    bool finished = false, ok = false;
    if (staged) {
        std::lock_guard<std::mutex> lock(staged->mutex);
        finished = staged->finished;
        ok = staged->ok;
    }
    if (!finished || !ok) {
        if (finished && staged) {
            job.error = staged->error;
        } else {
            job.error = "Commit before worker finished (caller bug)";
        }
        job.state = ImportJob::State::Failed;
        job.staged.reset();
        return true;
    }
    std::vector<ImportStagedFile> files;
    std::vector<std::string> warnings;
    std::string fingerprint;
    {
        std::lock_guard<std::mutex> lock(staged->mutex);
        files = staged->files;
        warnings = staged->warnings;
        fingerprint = staged->fingerprint;
    }
    job.progress = 0.75f;

    // Pass 1: refuse or clear every destination BEFORE writing any of them, so
    // a multi-file import (meshes + textures + materials) is all-or-nothing
    // instead of half-applied when one of its files already exists.
    for (const ImportStagedFile& file : files) {
        auto ex = vfs.exists(file.logical_path);
        if (ex.ok && ex.value) {
            if (!job.overwrite) {
                job.error = "Destination already exists: '" + file.logical_path + "'";
                job.state = ImportJob::State::Failed;
                job.staged.reset();
                return true;
            }
            remove_previous_import(vfs, reg, file.logical_path);
        }
    }

    // Pass 2: write, cook and register.
    for (const ImportStagedFile& file : files) {
        const std::string& logical = file.logical_path;
        if (!vfs.write_bytes(logical, std::span<const uint8_t>(file.bytes)).ok) {
            job.error = "Failed to write '" + logical + "'";
            job.state = ImportJob::State::Failed;
            job.staged.reset();
            return true;
        }
        job.written.push_back(ImportWritten{logical, file.type});

        if (!file.registered) {
            continue; // text assets bypass the registry (like scenes always have)
        }
        const std::string rel = logical.substr(std::string("content://").size());
        const std::string cooked = "cache://" + rel;
        if (!vfs.write_bytes(cooked, std::span<const uint8_t>(file.bytes)).ok) {
            job.error = "Failed to write cooked '" + cooked + "'";
            job.state = ImportJob::State::Failed;
            job.staged.reset();
            return true;
        }
        assets::AssetMetadata meta;
        // Fresh id per import (matches the cooker): repeated imports of one
        // file never collide; the assigned id is reported on the job.
        meta.id = assets::AssetId::generate();
        if (file.file_id.valid() && !reg.contains(file.file_id) &&
            file.type == assets::AssetType::Mesh) {
            // Prefer the file's own id when free: keeps hand-built references working.
            meta.id = file.file_id;
        }
        meta.type = file.type;
        meta.logical_path = logical;
        meta.cooked_path = cooked;
        meta.fingerprint = fingerprint;
        meta.format = (file.type == assets::AssetType::Mesh) ? "nfmesh-v1" : "img-v1";
        meta.version = 1;
        std::string aerr;
        if (!reg.add(meta, aerr)) {
            job.error = "Registry rejected import: " + aerr;
            job.state = ImportJob::State::Failed;
            job.staged.reset();
            return true;
        }
        if (!job.assigned_id.valid()) {
            job.assigned_id = meta.id;
        }
    }

    job.warnings = std::move(warnings);
    job.progress = 1.0f;
    job.state = ImportJob::State::Done;
    job.staged.reset(); // release staged bytes promptly
    return true;
}

bool ImportQueue::process_next(assets::VirtualFileSystem& vfs, assets::AssetRegistry& reg) {
    ImportJob* job = nullptr;
    for (auto& j : m_jobs) {
        if (j.state == ImportJob::State::Queued) {
            job = &j;
            break;
        }
    }
    if (job == nullptr) {
        return false;
    }
    job->state = ImportJob::State::Working;
    job->progress = 0.1f;
    auto staged = std::make_shared<ImportStaged>();
    run_cpu_stages(job->src_absolute, job->dst_dir, job->type, job->source_format, job->id,
                   staged);
    job->staged = staged;
    commit_staged(vfs, reg, *job);
    return true;
}

void ImportQueue::process_all(assets::VirtualFileSystem& vfs, assets::AssetRegistry& reg) {
    while (process_next(vfs, reg)) {
    }
}

const ImportJob* ImportQueue::pump_async(assets::VirtualFileSystem& vfs, assets::AssetRegistry& reg) {
    const bool threaded = JobSystem::instance().is_initialized();
    // Dispatch every queued job (workers only touch their staged block).
    for (auto& j : m_jobs) {
        if (j.state != ImportJob::State::Queued) {
            continue;
        }
        j.staged = std::make_shared<ImportStaged>();
        j.state = ImportJob::State::Working;
        j.progress = 0.1f;
        if (threaded) {
            JobSystem::instance().enqueue([staged = j.staged, src = j.src_absolute, dir = j.dst_dir,
                                           type = j.type, format = j.source_format, id = j.id] {
                run_cpu_stages(src, dir, type, format, id, staged);
            });
        } else {
            run_cpu_stages(j.src_absolute, j.dst_dir, j.type, j.source_format, j.id, j.staged);
        }
    }
    // Mirror worker progress for the UI, then commit in submit order,
    // stopping at the first unfinished job.
    const ImportJob* terminal = nullptr;
    for (auto& j : m_jobs) {
        if (j.state == ImportJob::State::Done || j.state == ImportJob::State::Failed) {
            continue;
        }
        if (j.state != ImportJob::State::Working || !j.staged) {
            break; // Queued can only follow dispatched work; keep order
        }
        bool finished = false;
        {
            std::lock_guard<std::mutex> lock(j.staged->mutex);
            finished = j.staged->finished;
            j.progress = j.staged->progress;
        }
        if (!finished) {
            break;
        }
        commit_staged(vfs, reg, j);
        if (terminal == nullptr) {
            terminal = &j;
        }
    }
    return terminal;
}

} // namespace nf::editor
