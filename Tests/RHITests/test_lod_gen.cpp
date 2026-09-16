// RHITests — LOD generation: grid-clustering simplification (Phase 12).
//
// Pure CPU (no device): pins determinism, validity (indices in range,
// submesh windows consistent), reduction behavior, and edge cases.

#include <NF/Test/TestFramework.hpp>
#include <NF/Rendering/LodGenerator.hpp>
#include <NF/Rendering/StaticMesh.hpp>

#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::rendering;

namespace {

u32 tri_count(const MeshLOD& lod) {
    u32 tris = 0;
    for (const auto& sm : lod.submeshes) tris += sm.index_count / 3;
    return tris;
}

// Structural validity: every index resolves, every submesh window is inside
// the arrays, every triangle is non-degenerate, bounds contain the vertices.
bool lod_is_valid(const MeshLOD& lod) {
    for (u32 idx : lod.indices) {
        if (idx >= lod.vertices.size()) return false;
    }
    for (const auto& sm : lod.submeshes) {
        if (sm.index_count % 3 != 0) return false;
        if (sm.index_offset + sm.index_count > lod.indices.size()) return false;
        if (sm.vertex_offset + sm.vertex_count > lod.vertices.size()) return false;
        for (u32 i = 0; i < sm.index_count; i += 3) {
            const u32 a = lod.indices[sm.index_offset + i];
            const u32 b = lod.indices[sm.index_offset + i + 1];
            const u32 c = lod.indices[sm.index_offset + i + 2];
            if (a == b || b == c || a == c) return false;
        }
    }
    for (const auto& v : lod.vertices) {
        if (v.position[0] < lod.bounds.min_x || v.position[0] > lod.bounds.max_x) return false;
        if (v.position[1] < lod.bounds.min_y || v.position[1] > lod.bounds.max_y) return false;
        if (v.position[2] < lod.bounds.min_z || v.position[2] > lod.bounds.max_z) return false;
        const float nl = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] +
                                   v.normal[2] * v.normal[2]);
        if (std::abs(nl - 1.0f) > 1e-3f) return false;
    }
    return true;
}

} // namespace

NF_TEST(lod_simplify_cube_welds_split_corners) {
    auto mesh = StaticMesh::create_cube(2.0f);
    const MeshLOD& src = mesh->lods()[0];
    NF_CHECK(src.vertices.size() == 24); // split per face
    NF_CHECK(tri_count(src) == 12);

    // One cell per corner octant (cell 1.0 on a [-1,1] cube): 8 corners
    // survive, all 12 triangles stay valid.
    MeshLOD simple = simplify_lod(src, 1.0f);
    NF_CHECK(simple.vertices.size() == 8);
    NF_CHECK(tri_count(simple) == 12);
    NF_CHECK(lod_is_valid(simple));
    // Welded corner is the average of its 3 face copies: still a corner.
    for (const auto& v : simple.vertices) {
        NF_CHECK_NEAR(std::abs(v.position[0]), 1.0f, 1e-4f);
        NF_CHECK_NEAR(std::abs(v.position[1]), 1.0f, 1e-4f);
        NF_CHECK_NEAR(std::abs(v.position[2]), 1.0f, 1e-4f);
    }
    NF_CHECK(simple.submeshes.size() == 1);
    NF_CHECK(simple.submeshes[0].material_slot == 0);
}

NF_TEST(lod_simplify_single_cell_collapses_cleanly) {
    auto mesh = StaticMesh::create_cube(2.0f);
    MeshLOD simple = simplify_lod(mesh->lods()[0], 1000.0f);
    // Everything merges: every triangle degenerates, no submesh survives,
    // but the level stays well-formed (1 averaged vertex, valid bounds).
    NF_CHECK(tri_count(simple) == 0);
    NF_CHECK(simple.submeshes.empty());
    NF_CHECK(simple.vertices.size() == 1);
    NF_CHECK_NEAR(simple.vertices[0].position[0], 0.0f, 1e-4f);
}

NF_TEST(lod_simplify_degenerate_cell_size_is_noop_copy) {
    auto mesh = StaticMesh::create_cube(2.0f);
    for (float bad : {0.0f, -1.0f}) {
        MeshLOD copy = simplify_lod(mesh->lods()[0], bad);
        NF_CHECK(copy.vertices.size() == 24);
        NF_CHECK(tri_count(copy) == 12);
    }
    MeshLOD empty_out = simplify_lod(MeshLOD{}, 1.0f);
    NF_CHECK(empty_out.vertices.empty());
    NF_CHECK(empty_out.indices.empty());
}

NF_TEST(lod_simplify_sphere_reduces_and_stays_valid) {
    auto mesh = StaticMesh::create_sphere(1.0f, 24);
    const MeshLOD& src = mesh->lods()[0];
    const u32 src_tris = tri_count(src);
    NF_CHECK(src_tris == 24 * 24 * 2);

    MeshLOD simple = simplify_lod(src, 0.5f);
    const u32 tris = tri_count(simple);
    NF_CHECK(tris < src_tris);
    NF_CHECK(tris > 0);
    NF_CHECK(simple.vertices.size() < src.vertices.size());
    NF_CHECK(lod_is_valid(simple));
    // Bounds survive simplification (within one cell of the source).
    NF_CHECK(simple.bounds.min_x >= src.bounds.min_x - 0.5f);
    NF_CHECK(simple.bounds.max_x <= src.bounds.max_x + 0.5f);
}

NF_TEST(lod_build_levels_decrease_monotonically) {
    auto mesh = StaticMesh::create_sphere(1.0f, 24);
    LodGenerateOptions opt;
    opt.target_ratio = 0.5f;
    opt.max_levels = 3;
    LodGenerateStats stats = build_lods(*mesh, opt);

    NF_CHECK(stats.levels_built >= 1);
    NF_CHECK(mesh->lods().size() == 1 + stats.levels_built);
    NF_CHECK(stats.triangles.size() == mesh->lods().size());
    // LOD 0 untouched.
    NF_CHECK(tri_count(mesh->lods()[0]) == 24 * 24 * 2);
    for (usize i = 1; i < mesh->lods().size(); ++i) {
        NF_CHECK(tri_count(mesh->lods()[i]) < tri_count(mesh->lods()[i - 1]));
        NF_CHECK(lod_is_valid(mesh->lods()[i]));
    }
    // Material slots survive every level.
    for (const auto& lod : mesh->lods()) {
        NF_CHECK(!lod.material_slots.empty());
    }
}

NF_TEST(lod_build_is_deterministic) {
    auto a = StaticMesh::create_sphere(1.0f, 16);
    auto b = StaticMesh::create_sphere(1.0f, 16);
    LodGenerateOptions opt;
    opt.max_levels = 2;
    build_lods(*a, opt);
    build_lods(*b, opt);
    NF_CHECK(a->lods().size() == b->lods().size());
    for (usize l = 0; l < a->lods().size(); ++l) {
        const MeshLOD& la = a->lods()[l];
        const MeshLOD& lb = b->lods()[l];
        NF_CHECK(la.vertices.size() == lb.vertices.size());
        NF_CHECK(la.indices.size() == lb.indices.size());
        for (usize i = 0; i < la.vertices.size(); ++i) {
            NF_CHECK_NEAR(la.vertices[i].position[0], lb.vertices[i].position[0], 1e-6f);
            NF_CHECK_NEAR(la.vertices[i].position[2], lb.vertices[i].position[2], 1e-6f);
        }
        NF_CHECK(la.indices == lb.indices);
    }
}

NF_TEST(lod_build_stops_on_tiny_meshes) {
    auto mesh = StaticMesh::create_cube(1.0f); // 12 tris <= default min
    LodGenerateOptions opt;
    opt.min_triangles = 12;
    LodGenerateStats stats = build_lods(*mesh, opt);
    NF_CHECK(stats.levels_built == 0);
    NF_CHECK(mesh->lods().size() == 1);
}
