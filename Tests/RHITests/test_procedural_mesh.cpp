// Tests/RHITests/test_procedural_mesh.cpp — Tests for ProceduralMesh primitive generation, extrusion & modifiers

#include <NF/Test/TestFramework.hpp>
#include <NF/Rendering/ProceduralMesh.hpp>
#include <NF/Core/Noise.hpp>

#include <cmath>
#include <algorithm>

using namespace nf;
using namespace nf::rendering;

NF_TEST(procedural_box_and_plane_generation) {
    // 1. Box with sub_x=2, sub_y=2, sub_z=2
    auto box = ProceduralMesh::build_box(2.0f, 4.0f, 6.0f, 2, 2, 2);
    NF_CHECK(box != nullptr);
    const MeshLOD& box_lod = box->lod(0);
    NF_CHECK(!box_lod.vertices.empty());
    NF_CHECK(!box_lod.indices.empty());

    // 6 faces * (2 * 2 quads) * 6 indices = 144 indices
    NF_CHECK_EQ(box_lod.indices.size(), 144u);

    // Verify bounds match exact dimensions: +/-1 on X, +/-2 on Y, +/-3 on Z
    NF_CHECK_NEAR(box_lod.bounds.min_x, -1.0f, 1e-5f);
    NF_CHECK_NEAR(box_lod.bounds.max_x,  1.0f, 1e-5f);
    NF_CHECK_NEAR(box_lod.bounds.min_y, -2.0f, 1e-5f);
    NF_CHECK_NEAR(box_lod.bounds.max_y,  2.0f, 1e-5f);
    NF_CHECK_NEAR(box_lod.bounds.min_z, -3.0f, 1e-5f);
    NF_CHECK_NEAR(box_lod.bounds.max_z,  3.0f, 1e-5f);

    // 2. Plane in XZ
    auto plane = ProceduralMesh::build_plane(10.0f, 20.0f, 4, 5);
    NF_CHECK(plane != nullptr);
    const MeshLOD& plane_lod = plane->lod(0);
    // (4 + 1) * (5 + 1) = 30 vertices
    NF_CHECK_EQ(plane_lod.vertices.size(), 30u);
    // 4 * 5 * 6 = 120 indices
    NF_CHECK_EQ(plane_lod.indices.size(), 120u);

    // Normals must all be (0, 1, 0)
    for (const auto& v : plane_lod.vertices) {
        NF_CHECK_NEAR(v.normal[0], 0.0f, 1e-5f);
        NF_CHECK_NEAR(v.normal[1], 1.0f, 1e-5f);
        NF_CHECK_NEAR(v.normal[2], 0.0f, 1e-5f);
    }
}

NF_TEST(procedural_spheres_uv_and_icosphere) {
    // 1. UV Sphere
    const float radius = 3.5f;
    auto uv_sphere = ProceduralMesh::build_uv_sphere(radius, 12, 24);
    NF_CHECK(uv_sphere != nullptr);
    const MeshLOD& uv_lod = uv_sphere->lod(0);
    NF_CHECK(!uv_lod.vertices.empty());

    for (const auto& v : uv_lod.vertices) {
        const float dist = std::sqrt(v.position[0]*v.position[0] + v.position[1]*v.position[1] + v.position[2]*v.position[2]);
        NF_CHECK_NEAR(dist, radius, 1e-3f);
    }

    // 2. Geodesic Icosphere
    auto icosphere = ProceduralMesh::build_icosphere(radius, 2);
    NF_CHECK(icosphere != nullptr);
    const MeshLOD& ico_lod = icosphere->lod(0);
    NF_CHECK(!ico_lod.vertices.empty());

    // Base icosahedron has 20 faces. Subdivided twice => 20 * 4^2 = 320 faces => 960 indices
    NF_CHECK_EQ(ico_lod.indices.size(), 960u);

    for (const auto& v : ico_lod.vertices) {
        const float dist = std::sqrt(v.position[0]*v.position[0] + v.position[1]*v.position[1] + v.position[2]*v.position[2]);
        NF_CHECK_NEAR(dist, radius, 1e-3f);
        // Normals should point outward with unit length
        const float n_len = std::sqrt(v.normal[0]*v.normal[0] + v.normal[1]*v.normal[1] + v.normal[2]*v.normal[2]);
        NF_CHECK_NEAR(n_len, 1.0f, 1e-3f);
    }
}

NF_TEST(procedural_cylinder_cone_capsule_torus) {
    // 1. Cylinder
    auto cyl = ProceduralMesh::build_cylinder(1.0f, 4.0f, 16, true);
    NF_CHECK(cyl != nullptr);
    const MeshLOD& cyl_lod = cyl->lod(0);
    NF_CHECK_NEAR(cyl_lod.bounds.min_y, -2.0f, 1e-4f);
    NF_CHECK_NEAR(cyl_lod.bounds.max_y,  2.0f, 1e-4f);

    // 2. Cone
    auto cone = ProceduralMesh::build_cone(2.0f, 6.0f, 16, true);
    NF_CHECK(cone != nullptr);
    const MeshLOD& cone_lod = cone->lod(0);
    NF_CHECK_NEAR(cone_lod.bounds.min_y, -3.0f, 1e-4f);
    NF_CHECK_NEAR(cone_lod.bounds.max_y,  3.0f, 1e-4f);

    // 3. Capsule
    auto capsule = ProceduralMesh::build_capsule(1.0f, 2.0f, 8, 16);
    NF_CHECK(capsule != nullptr);
    const MeshLOD& cap_lod = capsule->lod(0);
    // Cylinder height 2.0 ([-1, 1]) + radius 1.0 caps => bounds [-2, 2] on Y
    NF_CHECK_NEAR(cap_lod.bounds.min_y, -2.0f, 1e-3f);
    NF_CHECK_NEAR(cap_lod.bounds.max_y,  2.0f, 1e-3f);

    // 4. Torus
    auto torus = ProceduralMesh::build_torus(3.0f, 0.5f, 16, 12);
    NF_CHECK(torus != nullptr);
    const MeshLOD& tor_lod = torus->lod(0);
    // Major 3.0, minor 0.5 => extents [-3.5, 3.5] on X/Z, [-0.5, 0.5] on Y
    NF_CHECK_NEAR(tor_lod.bounds.min_x, -3.5f, 1e-3f);
    NF_CHECK_NEAR(tor_lod.bounds.max_x,  3.5f, 1e-3f);
    NF_CHECK_NEAR(tor_lod.bounds.min_y, -0.5f, 1e-3f);
    NF_CHECK_NEAR(tor_lod.bounds.max_y,  0.5f, 1e-3f);
}

NF_TEST(procedural_path_extrusion) {
    // 2D Square cross-section
    std::vector<Vec2> cross_section = {
        Vec2(-0.5f, -0.5f),
        Vec2( 0.5f, -0.5f),
        Vec2( 0.5f,  0.5f),
        Vec2(-0.5f,  0.5f)
    };

    // 3D Path
    std::vector<Vec3> path = {
        Vec3(0.0f, 0.0f, 0.0f),
        Vec3(0.0f, 2.0f, 0.0f),
        Vec3(2.0f, 4.0f, 0.0f),
        Vec3(4.0f, 4.0f, 2.0f)
    };

    auto pipe = ProceduralMesh::extrude_polygon_along_path(cross_section, path, false, true);
    NF_CHECK(pipe != nullptr);
    const MeshLOD& pipe_lod = pipe->lod(0);
    NF_CHECK(!pipe_lod.vertices.empty());
    NF_CHECK(!pipe_lod.indices.empty());

    // Verify submesh was created and bound calculation succeeded
    NF_CHECK_EQ(pipe_lod.submeshes.size(), 1u);
    NF_CHECK(pipe_lod.bounds.max_y >= 4.0f);
    NF_CHECK(pipe_lod.bounds.max_x >= 4.0f);
}

NF_TEST(procedural_mesh_modifiers_and_merge) {
    auto box_a = ProceduralMesh::build_box(1.0f, 1.0f, 1.0f);
    auto box_b = ProceduralMesh::build_box(1.0f, 1.0f, 1.0f);

    // Transform box_b by translating +10 along X
    Mat4 trans = Mat4::translate(Vec3(10.0f, 0.0f, 0.0f));
    ProceduralMesh::transform_mesh(*box_b, trans);

    NF_CHECK_NEAR(box_b->lod(0).bounds.min_x, 9.5f, 1e-4f);
    NF_CHECK_NEAR(box_b->lod(0).bounds.max_x, 10.5f, 1e-4f);

    // Merge box_a and box_b
    auto merged = ProceduralMesh::merge_meshes(*box_a, *box_b);
    NF_CHECK(merged != nullptr);
    const MeshLOD& m_lod = merged->lod(0);
    NF_CHECK_EQ(m_lod.vertices.size(), box_a->lod(0).vertices.size() + box_b->lod(0).vertices.size());
    NF_CHECK_EQ(m_lod.indices.size(), box_a->lod(0).indices.size() + box_b->lod(0).indices.size());

    // Invert faces
    const u32 old_i1 = m_lod.indices[1];
    const u32 old_i2 = m_lod.indices[2];
    ProceduralMesh::invert_faces(*merged);
    NF_CHECK_EQ(m_lod.indices[1], old_i2);
    NF_CHECK_EQ(m_lod.indices[2], old_i1);

    // Recompute normals and tangents
    ProceduralMesh::recompute_normals(*merged, true);
    ProceduralMesh::recompute_tangents(*merged);
    for (const auto& v : m_lod.vertices) {
        const float n_len = std::sqrt(v.normal[0]*v.normal[0] + v.normal[1]*v.normal[1] + v.normal[2]*v.normal[2]);
        NF_CHECK_NEAR(n_len, 1.0f, 1e-3f);
        const float t_len = std::sqrt(v.tangent[0]*v.tangent[0] + v.tangent[1]*v.tangent[1] + v.tangent[2]*v.tangent[2]);
        NF_CHECK_NEAR(t_len, 1.0f, 1e-3f);
    }
}

NF_TEST(procedural_noise_displacement_and_terrain_patch) {
    core::Noise noise(42);

    // 1. Procedural Asteroid
    auto asteroid = ProceduralMesh::build_asteroid(2.0f, noise, 0.25f, 2);
    NF_CHECK(asteroid != nullptr);
    const MeshLOD& ast_lod = asteroid->lod(0);
    NF_CHECK(!ast_lod.vertices.empty());

    // Ensure radii are varied around 2.0 (organic displacement)
    bool has_smaller = false;
    bool has_larger = false;
    for (const auto& v : ast_lod.vertices) {
        const float dist = std::sqrt(v.position[0]*v.position[0] + v.position[1]*v.position[1] + v.position[2]*v.position[2]);
        if (dist < 1.99f) has_smaller = true;
        if (dist > 2.01f) has_larger = true;
    }
    NF_CHECK(has_smaller && has_larger);

    // 2. Procedural Terrain Patch
    auto terrain = ProceduralMesh::build_terrain_patch(40.0f, 40.0f, 8, 8, noise, 10.0f, 0.05f, 3);
    NF_CHECK(terrain != nullptr);
    const MeshLOD& ter_lod = terrain->lod(0);
    NF_CHECK(!ter_lod.vertices.empty());

    // Y values must vary (not flat 0)
    bool y_varied = false;
    for (const auto& v : ter_lod.vertices) {
        if (std::abs(v.position[1]) > 0.01f) {
            y_varied = true;
            break;
        }
    }
    NF_CHECK(y_varied);
}
