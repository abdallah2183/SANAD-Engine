// NF/Rendering/LodGenerator.cpp — grid-clustering mesh simplification.

#include <NF/Rendering/LodGenerator.hpp>
#include <NF/Core/Logger.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <vector>

namespace nf::rendering {

namespace {

// Ordered cell key: std::map keeps emission order deterministic across
// platforms and runs (no hash seed, no bucket order).
using CellKey = std::tuple<i32, i32, i32>;

struct CellAccum {
    double px = 0, py = 0, pz = 0;
    double nx = 0, ny = 0, nz = 0;
    float uv0[2] = {0, 0};
    float uv1[2] = {0, 0};
    float tangent[4] = {1, 0, 0, 1};
    u32 count = 0;
};

float lod_diagonal(const MeshLOD& lod) {
    const float dx = lod.bounds.max_x - lod.bounds.min_x;
    const float dy = lod.bounds.max_y - lod.bounds.min_y;
    const float dz = lod.bounds.max_z - lod.bounds.min_z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

u32 count_triangles(const MeshLOD& lod) {
    u32 tris = 0;
    for (const auto& sm : lod.submeshes) tris += sm.index_count / 3;
    return tris;
}

} // namespace

MeshLOD simplify_lod(const MeshLOD& src, float cell_size) {
    MeshLOD out;
    out.material_slots = src.material_slots;
    if (src.vertices.empty() || src.indices.empty() || src.submeshes.empty()) {
        return out; // degenerate input -> empty level, never a crash
    }
    if (!(cell_size > 0.0f) || !std::isfinite(cell_size)) {
        out = src; // documented no-op (copy keeps every field)
        return out;
    }

    // Clustering origin at the LOD minimum keeps cell coordinates small.
    const float ox = src.bounds.min_x;
    const float oy = src.bounds.min_y;
    const float oz = src.bounds.min_z;

    for (const auto& sm : src.submeshes) {
        if (sm.index_count == 0 || sm.index_count % 3 != 0) continue;
        const usize idx_begin = sm.index_offset;
        const usize idx_end = idx_begin + sm.index_count;
        if (idx_end > src.indices.size()) continue;

        std::map<CellKey, u32> cell_to_new; // cell -> local vertex id
        std::vector<CellAccum> cells;
        // Global old vertex -> local new vertex. Vertices referenced outside
        // any declared window still resolve (keyed here), so corrupt windows
        // degrade instead of crashing.
        std::map<u32, u32> sparse_remap;

        // Seed pass: every referenced vertex opens (or finds) its cell.
        // Cells start empty (count 0); the fold pass below counts each
        // referenced vertex exactly once.
        for (usize i = idx_begin; i < idx_end; ++i) {
            const u32 old = src.indices[i];
            if (old >= src.vertices.size()) continue;
            if (sparse_remap.find(old) != sparse_remap.end()) continue;
            const Vertex& v = src.vertices[old];
            const CellKey key{
                static_cast<i32>(std::floor((v.position[0] - ox) / cell_size)),
                static_cast<i32>(std::floor((v.position[1] - oy) / cell_size)),
                static_cast<i32>(std::floor((v.position[2] - oz) / cell_size)),
            };
            auto cit = cell_to_new.find(key);
            if (cit != cell_to_new.end()) {
                sparse_remap[old] = cit->second;
                continue;
            }
            const u32 local = static_cast<u32>(cells.size());
            cell_to_new[key] = local;
            sparse_remap[old] = local;
            CellAccum acc;
            acc.uv0[0] = v.uv0[0];
            acc.uv0[1] = v.uv0[1];
            acc.uv1[0] = v.uv1[0];
            acc.uv1[1] = v.uv1[1];
            for (int k = 0; k < 4; ++k) acc.tangent[k] = v.tangent[k];
            acc.count = 0;
            cells.push_back(acc);
        }
        // Fold pass: accumulate every referenced vertex into its cell.
        for (usize i = idx_begin; i < idx_end; ++i) {
            const u32 old = src.indices[i];
            if (old >= src.vertices.size()) continue;
            const u32 local = sparse_remap[old];
            const Vertex& v = src.vertices[old];
            cells[local].px += v.position[0];
            cells[local].py += v.position[1];
            cells[local].pz += v.position[2];
            cells[local].nx += v.normal[0];
            cells[local].ny += v.normal[1];
            cells[local].nz += v.normal[2];
            cells[local].count += 1;
        }
        // Emit merged vertices in deterministic (ordered-map) cell order.
        const u32 base = static_cast<u32>(out.vertices.size());
        for (const auto& [key, local] : cell_to_new) {
            const CellAccum& acc = cells[local];
            Vertex v;
            const float inv = 1.0f / static_cast<float>(acc.count);
            v.position[0] = static_cast<float>(acc.px * inv);
            v.position[1] = static_cast<float>(acc.py * inv);
            v.position[2] = static_cast<float>(acc.pz * inv);
            float nx = static_cast<float>(acc.nx * inv);
            float ny = static_cast<float>(acc.ny * inv);
            float nz = static_cast<float>(acc.nz * inv);
            const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (nl > 1e-12f) {
                nx /= nl; ny /= nl; nz /= nl;
            } else {
                nx = 0; ny = 0; nz = 1;
            }
            v.normal[0] = nx;
            v.normal[1] = ny;
            v.normal[2] = nz;
            v.uv0[0] = acc.uv0[0];
            v.uv0[1] = acc.uv0[1];
            v.uv1[0] = acc.uv1[0];
            v.uv1[1] = acc.uv1[1];
            for (int k = 0; k < 4; ++k) v.tangent[k] = acc.tangent[k];
            out.vertices.push_back(v);
        }
        // Rewrite indices, dropping out-of-range refs and degenerate tris.
        const usize out_idx_begin = out.indices.size();
        for (usize i = idx_begin; i + 2 < idx_end; i += 3) {
            const u32 o0 = src.indices[i], o1 = src.indices[i + 1], o2 = src.indices[i + 2];
            if (o0 >= src.vertices.size() || o1 >= src.vertices.size() ||
                o2 >= src.vertices.size()) {
                continue;
            }
            const u32 n0 = base + sparse_remap[o0];
            const u32 n1 = base + sparse_remap[o1];
            const u32 n2 = base + sparse_remap[o2];
            if (n0 == n1 || n1 == n2 || n0 == n2) continue; // collapsed
            out.indices.push_back(n0);
            out.indices.push_back(n1);
            out.indices.push_back(n2);
        }
        const usize out_idx_end = out.indices.size();
        if (out_idx_end > out_idx_begin) {
            SubMesh nsm;
            nsm.index_offset = static_cast<u32>(out_idx_begin);
            nsm.index_count = static_cast<u32>(out_idx_end - out_idx_begin);
            nsm.vertex_offset = base;
            nsm.vertex_count = static_cast<u32>(cell_to_new.size());
            nsm.material_slot = sm.material_slot;
            out.submeshes.push_back(nsm);
        }
    }

    StaticMesh::compute_lod_bounds(out);
    return out;
}

LodGenerateStats build_lods(StaticMesh& mesh, const LodGenerateOptions& options) {
    LodGenerateStats stats;
    if (mesh.lods().empty()) return stats;
    const MeshLOD& lod0 = mesh.lods()[0];
    if (lod0.vertices.empty() || lod0.indices.empty()) return stats;
    const float ratio = std::clamp(options.target_ratio, 0.05f, 0.95f);
    const u32 max_levels = std::min(options.max_levels, 8u);
    const float diag = lod_diagonal(lod0);
    if (!(diag > 0.0f)) return stats; // degenerate bounds: nothing to scale
    stats.triangles.push_back(count_triangles(lod0));

    const MeshLOD* prev = &mesh.lods()[0];
    for (u32 level = 0; level < max_levels; ++level) {
        const u32 prev_tris = count_triangles(*prev);
        if (prev_tris <= options.min_triangles) break;
        const u32 target = static_cast<u32>(static_cast<float>(prev_tris) * ratio);
        // Binary search the finest cell that meets the target: 10 steps from
        // a no-op cell (diag/1024) up to whole-bounds (diag). Finer cells
        // keep more triangles; total collapse (0 tris) means too coarse.
        float lo = diag / 1024.0f;
        float hi = diag;
        MeshLOD best_below; // most detailed level at/below target
        u32 best_below_tris = 0;
        MeshLOD best_above; // finest reduction still above target (fallback)
        u32 best_above_tris = 0;
        for (int step = 0; step < 10; ++step) {
            const float mid = (lo + hi) * 0.5f;
            MeshLOD cand = simplify_lod(*prev, mid);
            const u32 tris = count_triangles(cand);
            if (tris == 0) {
                hi = mid; // collapsed everything: refine toward finer cells
                continue;
            }
            if (tris >= prev_tris) {
                lo = mid; // no reduction yet: coarsen
                continue;
            }
            if (tris <= target) {
                if (tris > best_below_tris) {
                    best_below = std::move(cand);
                    best_below_tris = tris;
                }
                hi = mid; // meets target: try finer for more detail
            } else {
                if (tris > best_above_tris) {
                    best_above = std::move(cand);
                    best_above_tris = tris;
                }
                lo = mid; // above target: coarsen
            }
        }
        // Prefer the most detailed level at/below target; fall back to the
        // finest reduction above it (all-or-nothing meshes still progress).
        // Neither existing means no cell size reduces: stop.
        MeshLOD best;
        u32 best_tris = 0;
        if (best_below_tris > 0) {
            best = std::move(best_below);
            best_tris = best_below_tris;
        } else if (best_above_tris > 0) {
            best = std::move(best_above);
            best_tris = best_above_tris;
        } else {
            break;
        }
        stats.cell_sizes.push_back(hi);
        stats.triangles.push_back(best_tris);
        mesh.lods().push_back(std::move(best));
        prev = &mesh.lods().back();
        ++stats.levels_built;
        NF_LOG_INFO(LogCategory::Core, "LodGenerator: level {}: {} -> {} tris (cell {:.4f})",
                    stats.levels_built, prev_tris, best_tris, hi);
    }
    return stats;
}

} // namespace nf::rendering
