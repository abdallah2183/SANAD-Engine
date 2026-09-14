// NF/Editor/ImportQueue.cpp — sync + async external import (see header).

#include <NF/Editor/ImportQueue.hpp>
#include <NF/Assets/MeshAsset.hpp>
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

} // namespace

assets::AssetType ImportQueue::type_for_extension(const std::string& ext_lower) {
    if (ext_lower == ".nfmesh") {
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
    const assets::AssetType type = type_for_extension(lower_ext(src_absolute));
    if (type == assets::AssetType::Unknown) {
        out_err = "Unsupported extension for '" + src_absolute +
                  "' (expected .nfmesh/.png/.jpg/.bmp/.tga/.nfmat/.nfscene)";
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
    ImportJob job;
    job.id = m_next_id++;
    job.src_absolute = src_absolute;
    job.dst_logical = dir + "/" + file_name_of(src_absolute);
    job.overwrite = overwrite;
    job.type = type;
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
void ImportQueue::run_cpu_stages(const std::string& src_absolute, assets::AssetType type,
                                 size_t job_id, const std::shared_ptr<ImportStaged>& staged) {
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
        assets::AssetId file_id;
        if (type == assets::AssetType::Mesh) {
            std::string perr;
            auto asset = assets::MeshAsset::load_from_bytes(std::span<const uint8_t>(bytes), perr);
            if (!asset) {
                finish_staged(staged, false, "Invalid .nfmesh: " + perr, 0.3f);
                return;
            }
            file_id = asset->id;
        } else if (type == assets::AssetType::Texture) {
            std::string derr;
            rendering::DecodedImage img =
                rendering::decode_image_memory(bytes.data(), bytes.size(), derr);
            if (!img.ok()) {
                finish_staged(staged, false, "Undecodable image: " + derr, 0.3f);
                return;
            }
        } else if (type == assets::AssetType::Material) {
            rendering::MaterialAsset mat;
            std::string merr;
            const std::string text(bytes.begin(), bytes.end());
            if (!rendering::MaterialAsset::load_from_text(text, mat, merr)) {
                finish_staged(staged, false, "Invalid .nfmat: " + merr, 0.3f);
                return;
            }
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
        }
        const std::string fp = fingerprint_hex(bytes);
        {
            std::lock_guard<std::mutex> lock(staged->mutex);
            staged->bytes = std::move(bytes);
            staged->file_id = file_id;
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
    std::vector<uint8_t> bytes;
    assets::AssetId file_id;
    std::string fingerprint;
    {
        std::lock_guard<std::mutex> lock(staged->mutex);
        bytes = staged->bytes;
        file_id = staged->file_id;
        fingerprint = staged->fingerprint;
    }
    job.progress = 0.75f;

    // Copy into content:// (siblings may have landed: re-check).
    {
        auto ex = vfs.exists(job.dst_logical);
        if (ex.ok && ex.value) {
            if (!job.overwrite) {
                job.error = "Destination already exists: '" + job.dst_logical + "'";
                job.state = ImportJob::State::Failed;
                job.staged.reset();
                return true;
            }
            // Overwrite: drop the previous content file, its cooked copy and
            // its registry entry (last writer wins, nothing dangles).
            if (auto prev = reg.find_by_path(job.dst_logical)) {
                const std::string prev_cooked = prev->cooked_path;
                reg.remove(prev->id);
                if (!prev_cooked.empty()) {
                    if (auto rc = vfs.resolve(prev_cooked); rc.ok) {
                        std::error_code ec;
                        std::filesystem::remove(rc.value, ec);
                    }
                }
            }
            if (auto rd = vfs.resolve(job.dst_logical); rd.ok) {
                std::error_code ec;
                std::filesystem::remove(rd.value, ec);
            }
        }
    }
    if (!vfs.write_bytes(job.dst_logical, std::span<const uint8_t>(bytes)).ok) {
        job.error = "Failed to write '" + job.dst_logical + "'";
        job.state = ImportJob::State::Failed;
        job.staged.reset();
        return true;
    }
    job.progress = 0.75f;

    // Cooked copy + registry (Mesh/Texture only).
    if (job.type == assets::AssetType::Mesh || job.type == assets::AssetType::Texture) {
        const std::string rel = job.dst_logical.substr(std::string("content://").size());
        const std::string cooked = "cache://" + rel;
        if (!vfs.write_bytes(cooked, std::span<const uint8_t>(bytes)).ok) {
            job.error = "Failed to write cooked '" + cooked + "'";
            job.state = ImportJob::State::Failed;
            job.staged.reset();
            return true;
        }
        assets::AssetMetadata meta;
        // Fresh id per import (matches the cooker): repeated imports of one
        // file never collide; the assigned id is reported on the job.
        meta.id = assets::AssetId::generate();
        if (file_id.valid() && !reg.contains(file_id) && job.type == assets::AssetType::Mesh) {
            // Prefer the file's own id when free: keeps hand-built references working.
            meta.id = file_id;
        }
        meta.type = job.type;
        meta.logical_path = job.dst_logical;
        meta.cooked_path = cooked;
        meta.fingerprint = fingerprint;
        meta.format = (job.type == assets::AssetType::Mesh) ? "nfmesh-v1" : "img-v1";
        meta.version = 1;
        std::string aerr;
        if (!reg.add(meta, aerr)) {
            job.error = "Registry rejected import: " + aerr;
            job.state = ImportJob::State::Failed;
            job.staged.reset();
            return true;
        }
        job.assigned_id = meta.id;
    }
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
    run_cpu_stages(job->src_absolute, job->type, job->id, staged);
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
            JobSystem::instance().enqueue([staged = j.staged, src = j.src_absolute, type = j.type,
                                           id = j.id] {
                run_cpu_stages(src, type, id, staged);
            });
        } else {
            run_cpu_stages(j.src_absolute, j.type, j.id, j.staged);
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
