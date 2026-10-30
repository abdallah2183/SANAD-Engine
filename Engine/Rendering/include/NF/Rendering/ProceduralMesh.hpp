#pragma once

// NF/Rendering/ProceduralMesh.hpp — Procedural 3D Mesh Generation & Geometry Engine

#include <NF/Core/Types.hpp>
#include <NF/Core/Math.hpp>
#include <NF/Core/Noise.hpp>
#include <NF/Rendering/StaticMesh.hpp>

#include <memory>
#include <vector>

namespace nf::rendering {

/// High-performance procedural mesh generators and geometry processing utilities.
/// Generates ready-to-render StaticMesh assets (LOD 0 with positions, normals, tangents, UVs, and bounds).
class ProceduralMesh {
public:
    // --- 1. Geometric Primitive Builders ---

    /// Creates a 3D box/cuboid centered at origin with width (X), height (Y), depth (Z).
    /// Supports grid subdivisions per face with independent face normals and UV coordinates.
    static std::unique_ptr<StaticMesh> build_box(
        float width = 1.0f, float height = 1.0f, float depth = 1.0f,
        u32 sub_x = 1, u32 sub_y = 1, u32 sub_z = 1
    );

    /// Creates a flat grid plane centered at origin in the XZ plane with normals facing +Y.
    static std::unique_ptr<StaticMesh> build_plane(
        float width = 10.0f, float depth = 10.0f,
        u32 sub_x = 10, u32 sub_z = 10
    );

    /// Creates a 2D circular disc centered at origin in the XZ plane with normals facing +Y.
    static std::unique_ptr<StaticMesh> build_disc(
        float radius = 1.0f, u32 segments = 24
    );

    /// Creates a UV sphere with latitude rings and longitude sectors.
    static std::unique_ptr<StaticMesh> build_uv_sphere(
        float radius = 1.0f, u32 rings = 16, u32 sectors = 32
    );

    /// Creates a geodesic icosphere with uniform triangle distribution (no polar distortion).
    /// Starts from a regular icosahedron and recursively subdivides triangular faces.
    static std::unique_ptr<StaticMesh> build_icosphere(
        float radius = 1.0f, u32 subdivisions = 2
    );

    /// Creates a cylinder centered at origin along the Y-axis.
    static std::unique_ptr<StaticMesh> build_cylinder(
        float radius = 0.5f, float height = 2.0f,
        u32 radial_segments = 24, bool capped = true
    );

    /// Creates a cone centered along the Y-axis with apex at +height/2 and circular base at -height/2.
    static std::unique_ptr<StaticMesh> build_cone(
        float radius = 0.5f, float height = 2.0f,
        u32 radial_segments = 24, bool capped = true
    );

    /// Creates a capsule centered along the Y-axis with cylinder body and two hemisphere end-caps.
    static std::unique_ptr<StaticMesh> build_capsule(
        float radius = 0.5f, float cylinder_height = 1.0f,
        u32 rings = 8, u32 sectors = 24
    );

    /// Creates a 3D torus (donut) centered at origin in the XZ plane.
    static std::unique_ptr<StaticMesh> build_torus(
        float major_radius = 1.0f, float minor_radius = 0.25f,
        u32 radial_segments = 24, u32 tubular_segments = 16
    );

    // --- 2. Advanced Extrusion & Path Sweeping ---

    /// Sweeps an arbitrary 2D cross-section polygon along a 3D path curve.
    /// Generates smooth pipes, tubes, rails, tunnels, or extruded architectural profiles.
    static std::unique_ptr<StaticMesh> extrude_polygon_along_path(
        const std::vector<Vec2>& cross_section,
        const std::vector<Vec3>& path_points,
        bool closed_path = false,
        bool cap_ends = true
    );

    // --- 3. Geometric Modifiers & Operations ---

    /// Transforms all vertex positions, normals, and tangents by an affine 4x4 matrix.
    static void transform_mesh(StaticMesh& mesh, const Mat4& transform);

    /// Combines two StaticMeshes into a single unified StaticMesh, merging submeshes and material slots.
    static std::unique_ptr<StaticMesh> merge_meshes(
        const StaticMesh& a, const StaticMesh& b
    );

    /// Recomputes vertex normals from polygon topology.
    /// If smooth is true, computes area/angle-weighted smooth vertex normals;
    /// if false, splits shared vertices to produce flat-shaded face normals.
    static void recompute_normals(StaticMesh& mesh, bool smooth = true);

    /// Computes orthonormal tangent basis (Tx, Ty, Tz, Handedness) using UV0 coordinates and normals.
    static void recompute_tangents(StaticMesh& mesh);

    /// Inverts all triangle face windings and flips normal vectors (for skyboxes or inverted hulls).
    static void invert_faces(StaticMesh& mesh);

    // --- 4. Procedural Noise Deformation & Organic Surfaces ---

    /// Displaces vertices along their normals according to a coherent procedural noise function.
    static void displace_mesh(
        StaticMesh& mesh,
        const core::Noise& noise,
        float amplitude,
        float frequency,
        core::NoiseType type = core::NoiseType::Perlin,
        u32 octaves = 3
    );

    /// Generates a procedural asteroid or organic rock from a subdivided icosphere deformed by 3D fractal noise.
    static std::unique_ptr<StaticMesh> build_asteroid(
        float radius,
        const core::Noise& noise,
        float roughness = 0.35f,
        u32 subdivisions = 3
    );

    /// Generates a procedural terrain heightfield mesh patch using coherent fractal noise.
    static std::unique_ptr<StaticMesh> build_terrain_patch(
        float width, float depth,
        u32 sub_x, u32 sub_z,
        const core::Noise& noise,
        float height_scale = 5.0f,
        float frequency = 0.05f,
        u32 octaves = 4
    );
};

} // namespace nf::rendering
