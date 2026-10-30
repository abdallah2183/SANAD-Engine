// NF/Rendering/ProceduralMesh.cpp — Procedural 3D Mesh Generation & Geometry Engine

#include <NF/Rendering/ProceduralMesh.hpp>
#include <algorithm>
#include <cmath>
#include <map>

namespace nf::rendering {

namespace {

constexpr float PI = 3.14159265358979323846f;
constexpr float TWO_PI = 2.0f * PI;

void finalize_mesh(StaticMesh& mesh) {
    MeshLOD& lod = mesh.lod(0);
    lod.submeshes.clear();
    SubMesh sm;
    sm.index_offset = 0;
    sm.index_count = static_cast<u32>(lod.indices.size());
    sm.vertex_offset = 0;
    sm.vertex_count = static_cast<u32>(lod.vertices.size());
    sm.material_slot = 0;
    lod.submeshes.push_back(sm);

    if (lod.material_slots.empty()) {
        lod.material_slots.push_back(MaterialSlot{"default"});
    }
    StaticMesh::compute_lod_bounds(lod);
}

} // anonymous namespace

std::unique_ptr<StaticMesh> ProceduralMesh::build_box(
    float width, float height, float depth,
    u32 sub_x, u32 sub_y, u32 sub_z
) {
    sub_x = std::max(1u, sub_x);
    sub_y = std::max(1u, sub_y);
    sub_z = std::max(1u, sub_z);

    auto mesh = std::make_unique<StaticMesh>("ProceduralBox");
    MeshLOD& lod = mesh->lod(0);

    const float hw = width * 0.5f;
    const float hh = height * 0.5f;
    const float hd = depth * 0.5f;

    struct FaceDef {
        Vec3 origin;
        Vec3 u_axis;
        Vec3 v_axis;
        Vec3 normal;
        Vec4 tangent;
        u32 sub_u;
        u32 sub_v;
    };

    const FaceDef faces[6] = {
        // +Z face (Front)
        { Vec3(-hw, -hh,  hd), Vec3(width, 0.0f, 0.0f), Vec3(0.0f, height, 0.0f), Vec3(0.0f, 0.0f, 1.0f), Vec4(1.0f, 0.0f, 0.0f, 1.0f), sub_x, sub_y },
        // -Z face (Back)
        { Vec3( hw, -hh, -hd), Vec3(-width, 0.0f, 0.0f), Vec3(0.0f, height, 0.0f), Vec3(0.0f, 0.0f, -1.0f), Vec4(-1.0f, 0.0f, 0.0f, 1.0f), sub_x, sub_y },
        // +X face (Right)
        { Vec3( hw, -hh,  hd), Vec3(0.0f, 0.0f, -depth), Vec3(0.0f, height, 0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec4(0.0f, 0.0f, -1.0f, 1.0f), sub_z, sub_y },
        // -X face (Left)
        { Vec3(-hw, -hh, -hd), Vec3(0.0f, 0.0f,  depth), Vec3(0.0f, height, 0.0f), Vec3(-1.0f, 0.0f, 0.0f), Vec4(0.0f, 0.0f, 1.0f, 1.0f), sub_z, sub_y },
        // +Y face (Top)
        { Vec3(-hw,  hh,  hd), Vec3(width, 0.0f, 0.0f), Vec3(0.0f, 0.0f, -depth), Vec3(0.0f, 1.0f, 0.0f), Vec4(1.0f, 0.0f, 0.0f, 1.0f), sub_x, sub_z },
        // -Y face (Bottom)
        { Vec3(-hw, -hh, -hd), Vec3(width, 0.0f, 0.0f), Vec3(0.0f, 0.0f,  depth), Vec3(0.0f, -1.0f, 0.0f), Vec4(1.0f, 0.0f, 0.0f, 1.0f), sub_x, sub_z }
    };

    for (const auto& face : faces) {
        const u32 base_idx = static_cast<u32>(lod.vertices.size());
        for (u32 j = 0; j <= face.sub_v; ++j) {
            const float v = static_cast<float>(j) / static_cast<float>(face.sub_v);
            for (u32 i = 0; i <= face.sub_u; ++i) {
                const float u = static_cast<float>(i) / static_cast<float>(face.sub_u);
                const Vec3 pos = face.origin + face.u_axis * u + face.v_axis * v;

                Vertex vert{};
                vert.position[0] = pos.x;
                vert.position[1] = pos.y;
                vert.position[2] = pos.z;
                vert.normal[0] = face.normal.x;
                vert.normal[1] = face.normal.y;
                vert.normal[2] = face.normal.z;
                vert.tangent[0] = face.tangent.x;
                vert.tangent[1] = face.tangent.y;
                vert.tangent[2] = face.tangent.z;
                vert.tangent[3] = face.tangent.w;
                vert.uv0[0] = u;
                vert.uv0[1] = v;
                vert.uv1[0] = u;
                vert.uv1[1] = v;
                lod.vertices.push_back(vert);
            }
        }

        const u32 stride = face.sub_u + 1;
        for (u32 j = 0; j < face.sub_v; ++j) {
            for (u32 i = 0; i < face.sub_u; ++i) {
                const u32 i0 = base_idx + j * stride + i;
                const u32 i1 = i0 + 1;
                const u32 i2 = base_idx + (j + 1) * stride + i;
                const u32 i3 = i2 + 1;

                lod.indices.push_back(i0);
                lod.indices.push_back(i1);
                lod.indices.push_back(i3);

                lod.indices.push_back(i0);
                lod.indices.push_back(i3);
                lod.indices.push_back(i2);
            }
        }
    }

    finalize_mesh(*mesh);
    return mesh;
}

std::unique_ptr<StaticMesh> ProceduralMesh::build_plane(
    float width, float depth,
    u32 sub_x, u32 sub_z
) {
    sub_x = std::max(1u, sub_x);
    sub_z = std::max(1u, sub_z);

    auto mesh = std::make_unique<StaticMesh>("ProceduralPlane");
    MeshLOD& lod = mesh->lod(0);

    const float hw = width * 0.5f;
    const float hd = depth * 0.5f;

    for (u32 j = 0; j <= sub_z; ++j) {
        const float v = static_cast<float>(j) / static_cast<float>(sub_z);
        const float z = -hd + v * depth;
        for (u32 i = 0; i <= sub_x; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(sub_x);
            const float x = -hw + u * width;

            Vertex vert{};
            vert.position[0] = x;
            vert.position[1] = 0.0f;
            vert.position[2] = z;
            vert.normal[0] = 0.0f;
            vert.normal[1] = 1.0f;
            vert.normal[2] = 0.0f;
            vert.tangent[0] = 1.0f;
            vert.tangent[1] = 0.0f;
            vert.tangent[2] = 0.0f;
            vert.tangent[3] = 1.0f;
            vert.uv0[0] = u;
            vert.uv0[1] = v;
            vert.uv1[0] = u;
            vert.uv1[1] = v;
            lod.vertices.push_back(vert);
        }
    }

    const u32 stride = sub_x + 1;
    for (u32 j = 0; j < sub_z; ++j) {
        for (u32 i = 0; i < sub_x; ++i) {
            const u32 i0 = j * stride + i;
            const u32 i1 = i0 + 1;
            const u32 i2 = (j + 1) * stride + i;
            const u32 i3 = i2 + 1;

            lod.indices.push_back(i0);
            lod.indices.push_back(i2);
            lod.indices.push_back(i1);

            lod.indices.push_back(i1);
            lod.indices.push_back(i2);
            lod.indices.push_back(i3);
        }
    }

    finalize_mesh(*mesh);
    return mesh;
}

std::unique_ptr<StaticMesh> ProceduralMesh::build_disc(
    float radius, u32 segments
) {
    segments = std::max(3u, segments);

    auto mesh = std::make_unique<StaticMesh>("ProceduralDisc");
    MeshLOD& lod = mesh->lod(0);

    // Center vertex
    Vertex center{};
    center.position[0] = 0.0f;
    center.position[1] = 0.0f;
    center.position[2] = 0.0f;
    center.normal[0] = 0.0f;
    center.normal[1] = 1.0f;
    center.normal[2] = 0.0f;
    center.tangent[0] = 1.0f;
    center.tangent[1] = 0.0f;
    center.tangent[2] = 0.0f;
    center.tangent[3] = 1.0f;
    center.uv0[0] = 0.5f;
    center.uv0[1] = 0.5f;
    lod.vertices.push_back(center);

    for (u32 i = 0; i <= segments; ++i) {
        const float theta = static_cast<float>(i) * TWO_PI / static_cast<float>(segments);
        const float cos_t = std::cos(theta);
        const float sin_t = std::sin(theta);

        Vertex vert{};
        vert.position[0] = radius * cos_t;
        vert.position[1] = 0.0f;
        vert.position[2] = radius * sin_t;
        vert.normal[0] = 0.0f;
        vert.normal[1] = 1.0f;
        vert.normal[2] = 0.0f;
        vert.tangent[0] = -sin_t;
        vert.tangent[1] = 0.0f;
        vert.tangent[2] = cos_t;
        vert.tangent[3] = 1.0f;
        vert.uv0[0] = 0.5f + 0.5f * cos_t;
        vert.uv0[1] = 0.5f + 0.5f * sin_t;
        lod.vertices.push_back(vert);
    }

    for (u32 i = 0; i < segments; ++i) {
        lod.indices.push_back(0);
        lod.indices.push_back(i + 2);
        lod.indices.push_back(i + 1);
    }

    finalize_mesh(*mesh);
    return mesh;
}

std::unique_ptr<StaticMesh> ProceduralMesh::build_uv_sphere(
    float radius, u32 rings, u32 sectors
) {
    rings = std::max(2u, rings);
    sectors = std::max(3u, sectors);

    auto mesh = std::make_unique<StaticMesh>("ProceduralUVSphere");
    MeshLOD& lod = mesh->lod(0);

    for (u32 r = 0; r <= rings; ++r) {
        const float phi = PI * static_cast<float>(r) / static_cast<float>(rings);
        const float cos_phi = std::cos(phi);
        const float sin_phi = std::sin(phi);

        for (u32 s = 0; s <= sectors; ++s) {
            const float theta = TWO_PI * static_cast<float>(s) / static_cast<float>(sectors);
            const float cos_theta = std::cos(theta);
            const float sin_theta = std::sin(theta);

            const float x = radius * sin_phi * cos_theta;
            const float y = radius * cos_phi;
            const float z = radius * sin_phi * sin_theta;

            const float inv_r = (radius > 1e-6f) ? (1.0f / radius) : 1.0f;

            Vertex vert{};
            vert.position[0] = x;
            vert.position[1] = y;
            vert.position[2] = z;
            vert.normal[0] = x * inv_r;
            vert.normal[1] = y * inv_r;
            vert.normal[2] = z * inv_r;
            vert.tangent[0] = -sin_theta;
            vert.tangent[1] = 0.0f;
            vert.tangent[2] = cos_theta;
            vert.tangent[3] = 1.0f;
            vert.uv0[0] = static_cast<float>(s) / static_cast<float>(sectors);
            vert.uv0[1] = static_cast<float>(r) / static_cast<float>(rings);
            lod.vertices.push_back(vert);
        }
    }

    const u32 stride = sectors + 1;
    for (u32 r = 0; r < rings; ++r) {
        for (u32 s = 0; s < sectors; ++s) {
            const u32 cur = r * stride + s;
            const u32 next = (r + 1) * stride + s;

            if (r > 0) {
                lod.indices.push_back(cur);
                lod.indices.push_back(next);
                lod.indices.push_back(next + 1);
            }
            if (r < rings - 1) {
                lod.indices.push_back(cur);
                lod.indices.push_back(next + 1);
                lod.indices.push_back(cur + 1);
            }
        }
    }

    finalize_mesh(*mesh);
    return mesh;
}

std::unique_ptr<StaticMesh> ProceduralMesh::build_icosphere(
    float radius, u32 subdivisions
) {
    auto mesh = std::make_unique<StaticMesh>("ProceduralIcosphere");
    MeshLOD& lod = mesh->lod(0);

    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;

    std::vector<Vec3> base_verts = {
        Vec3(-1.0f,  t, 0.0f), Vec3( 1.0f,  t, 0.0f), Vec3(-1.0f, -t, 0.0f), Vec3( 1.0f, -t, 0.0f),
        Vec3( 0.0f, -1.0f,  t), Vec3( 0.0f,  1.0f,  t), Vec3( 0.0f, -1.0f, -t), Vec3( 0.0f,  1.0f, -t),
        Vec3( t, 0.0f, -1.0f), Vec3( t, 0.0f,  1.0f), Vec3(-t, 0.0f, -1.0f), Vec3(-t, 0.0f,  1.0f)
    };

    struct Tri { u32 v0, v1, v2; };
    std::vector<Tri> faces = {
        {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11},
        {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
        {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
        {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}
    };

    std::vector<Vec3> positions;
    for (auto& bv : base_verts) {
        positions.push_back(bv.normalized() * radius);
    }

    std::map<uint64_t, u32> midpoint_cache;
    auto get_midpoint = [&](u32 i1, u32 i2) -> u32 {
        const uint64_t small_idx = std::min(i1, i2);
        const uint64_t big_idx = std::max(i1, i2);
        const uint64_t key = (small_idx << 32) | big_idx;

        auto it = midpoint_cache.find(key);
        if (it != midpoint_cache.end()) {
            return it->second;
        }

        const Vec3 mid = ((positions[i1] + positions[i2]) * 0.5f).normalized() * radius;
        const u32 new_idx = static_cast<u32>(positions.size());
        positions.push_back(mid);
        midpoint_cache[key] = new_idx;
        return new_idx;
    };

    for (u32 s = 0; s < subdivisions; ++s) {
        std::vector<Tri> next_faces;
        next_faces.reserve(faces.size() * 4);
        for (const auto& tri : faces) {
            const u32 a = get_midpoint(tri.v0, tri.v1);
            const u32 b = get_midpoint(tri.v1, tri.v2);
            const u32 c = get_midpoint(tri.v2, tri.v0);

            next_faces.push_back({tri.v0, a, c});
            next_faces.push_back({tri.v1, b, a});
            next_faces.push_back({tri.v2, c, b});
            next_faces.push_back({a, b, c});
        }
        faces = std::move(next_faces);
    }

    for (const auto& p : positions) {
        const Vec3 n = p.normalized();
        const float u = 0.5f + std::atan2(n.z, n.x) / TWO_PI;
        const float v = 0.5f - std::asin(std::clamp(n.y, -1.0f, 1.0f)) / PI;

        Vertex vert{};
        vert.position[0] = p.x;
        vert.position[1] = p.y;
        vert.position[2] = p.z;
        vert.normal[0] = n.x;
        vert.normal[1] = n.y;
        vert.normal[2] = n.z;
        vert.tangent[0] = -n.z;
        vert.tangent[1] = 0.0f;
        vert.tangent[2] = n.x;
        vert.tangent[3] = 1.0f;
        vert.uv0[0] = u;
        vert.uv0[1] = v;
        lod.vertices.push_back(vert);
    }

    for (const auto& tri : faces) {
        lod.indices.push_back(tri.v0);
        lod.indices.push_back(tri.v1);
        lod.indices.push_back(tri.v2);
    }

    finalize_mesh(*mesh);
    return mesh;
}

std::unique_ptr<StaticMesh> ProceduralMesh::build_cylinder(
    float radius, float height,
    u32 radial_segments, bool capped
) {
    radial_segments = std::max(3u, radial_segments);

    auto mesh = std::make_unique<StaticMesh>("ProceduralCylinder");
    MeshLOD& lod = mesh->lod(0);

    const float hh = height * 0.5f;

    // Side wall vertices
    const u32 side_base = 0;
    for (u32 s = 0; s <= radial_segments; ++s) {
        const float u = static_cast<float>(s) / static_cast<float>(radial_segments);
        const float theta = u * TWO_PI;
        const float cos_t = std::cos(theta);
        const float sin_t = std::sin(theta);

        // Top vertex
        Vertex top_v{};
        top_v.position[0] = radius * cos_t;
        top_v.position[1] = hh;
        top_v.position[2] = radius * sin_t;
        top_v.normal[0] = cos_t;
        top_v.normal[1] = 0.0f;
        top_v.normal[2] = sin_t;
        top_v.tangent[0] = -sin_t;
        top_v.tangent[1] = 0.0f;
        top_v.tangent[2] = cos_t;
        top_v.tangent[3] = 1.0f;
        top_v.uv0[0] = u;
        top_v.uv0[1] = 0.0f;
        lod.vertices.push_back(top_v);

        // Bottom vertex
        Vertex bot_v{};
        bot_v.position[0] = radius * cos_t;
        bot_v.position[1] = -hh;
        bot_v.position[2] = radius * sin_t;
        bot_v.normal[0] = cos_t;
        bot_v.normal[1] = 0.0f;
        bot_v.normal[2] = sin_t;
        bot_v.tangent[0] = -sin_t;
        bot_v.tangent[1] = 0.0f;
        bot_v.tangent[2] = cos_t;
        bot_v.tangent[3] = 1.0f;
        bot_v.uv0[0] = u;
        bot_v.uv0[1] = 1.0f;
        lod.vertices.push_back(bot_v);
    }

    for (u32 s = 0; s < radial_segments; ++s) {
        const u32 i0 = side_base + s * 2;
        const u32 i1 = i0 + 1;
        const u32 i2 = i0 + 2;
        const u32 i3 = i0 + 3;

        lod.indices.push_back(i0);
        lod.indices.push_back(i1);
        lod.indices.push_back(i2);

        lod.indices.push_back(i2);
        lod.indices.push_back(i1);
        lod.indices.push_back(i3);
    }

    if (capped) {
        // Top cap
        const u32 top_center_idx = static_cast<u32>(lod.vertices.size());
        Vertex tc{};
        tc.position[0] = 0.0f; tc.position[1] = hh; tc.position[2] = 0.0f;
        tc.normal[0] = 0.0f; tc.normal[1] = 1.0f; tc.normal[2] = 0.0f;
        tc.tangent[0] = 1.0f; tc.tangent[1] = 0.0f; tc.tangent[2] = 0.0f; tc.tangent[3] = 1.0f;
        tc.uv0[0] = 0.5f; tc.uv0[1] = 0.5f;
        lod.vertices.push_back(tc);

        for (u32 s = 0; s <= radial_segments; ++s) {
            const float theta = static_cast<float>(s) * TWO_PI / static_cast<float>(radial_segments);
            const float cos_t = std::cos(theta);
            const float sin_t = std::sin(theta);
            Vertex tv{};
            tv.position[0] = radius * cos_t;
            tv.position[1] = hh;
            tv.position[2] = radius * sin_t;
            tv.normal[0] = 0.0f; tv.normal[1] = 1.0f; tv.normal[2] = 0.0f;
            tv.tangent[0] = 1.0f; tv.tangent[1] = 0.0f; tv.tangent[2] = 0.0f; tv.tangent[3] = 1.0f;
            tv.uv0[0] = 0.5f + 0.5f * cos_t;
            tv.uv0[1] = 0.5f + 0.5f * sin_t;
            lod.vertices.push_back(tv);
        }

        for (u32 s = 0; s < radial_segments; ++s) {
            lod.indices.push_back(top_center_idx);
            lod.indices.push_back(top_center_idx + s + 2);
            lod.indices.push_back(top_center_idx + s + 1);
        }

        // Bottom cap
        const u32 bot_center_idx = static_cast<u32>(lod.vertices.size());
        Vertex bc{};
        bc.position[0] = 0.0f; bc.position[1] = -hh; bc.position[2] = 0.0f;
        bc.normal[0] = 0.0f; bc.normal[1] = -1.0f; bc.normal[2] = 0.0f;
        bc.tangent[0] = 1.0f; bc.tangent[1] = 0.0f; bc.tangent[2] = 0.0f; bc.tangent[3] = 1.0f;
        bc.uv0[0] = 0.5f; bc.uv0[1] = 0.5f;
        lod.vertices.push_back(bc);

        for (u32 s = 0; s <= radial_segments; ++s) {
            const float theta = static_cast<float>(s) * TWO_PI / static_cast<float>(radial_segments);
            const float cos_t = std::cos(theta);
            const float sin_t = std::sin(theta);
            Vertex bv{};
            bv.position[0] = radius * cos_t;
            bv.position[1] = -hh;
            bv.position[2] = radius * sin_t;
            bv.normal[0] = 0.0f; bv.normal[1] = -1.0f; bv.normal[2] = 0.0f;
            bv.tangent[0] = 1.0f; bv.tangent[1] = 0.0f; bv.tangent[2] = 0.0f; bv.tangent[3] = 1.0f;
            bv.uv0[0] = 0.5f + 0.5f * cos_t;
            bv.uv0[1] = 0.5f - 0.5f * sin_t;
            lod.vertices.push_back(bv);
        }

        for (u32 s = 0; s < radial_segments; ++s) {
            lod.indices.push_back(bot_center_idx);
            lod.indices.push_back(bot_center_idx + s + 1);
            lod.indices.push_back(bot_center_idx + s + 2);
        }
    }

    finalize_mesh(*mesh);
    return mesh;
}

std::unique_ptr<StaticMesh> ProceduralMesh::build_cone(
    float radius, float height,
    u32 radial_segments, bool capped
) {
    radial_segments = std::max(3u, radial_segments);

    auto mesh = std::make_unique<StaticMesh>("ProceduralCone");
    MeshLOD& lod = mesh->lod(0);

    const float hh = height * 0.5f;
    const float slope = (height > 1e-6f) ? (radius / height) : 0.0f;

    // Side vertices
    for (u32 s = 0; s <= radial_segments; ++s) {
        const float u = static_cast<float>(s) / static_cast<float>(radial_segments);
        const float theta = u * TWO_PI;
        const float cos_t = std::cos(theta);
        const float sin_t = std::sin(theta);

        const Vec3 n = Vec3(cos_t, slope, sin_t).normalized();

        // Apex vertex
        Vertex apex{};
        apex.position[0] = 0.0f; apex.position[1] = hh; apex.position[2] = 0.0f;
        apex.normal[0] = n.x; apex.normal[1] = n.y; apex.normal[2] = n.z;
        apex.tangent[0] = -sin_t; apex.tangent[1] = 0.0f; apex.tangent[2] = cos_t; apex.tangent[3] = 1.0f;
        apex.uv0[0] = u; apex.uv0[1] = 0.0f;
        lod.vertices.push_back(apex);

        // Base rim vertex
        Vertex base_v{};
        base_v.position[0] = radius * cos_t; base_v.position[1] = -hh; base_v.position[2] = radius * sin_t;
        base_v.normal[0] = n.x; base_v.normal[1] = n.y; base_v.normal[2] = n.z;
        base_v.tangent[0] = -sin_t; base_v.tangent[1] = 0.0f; base_v.tangent[2] = cos_t; base_v.tangent[3] = 1.0f;
        base_v.uv0[0] = u; base_v.uv0[1] = 1.0f;
        lod.vertices.push_back(base_v);
    }

    for (u32 s = 0; s < radial_segments; ++s) {
        const u32 i0 = s * 2;
        const u32 i1 = i0 + 1;
        const u32 i3 = i0 + 3;

        lod.indices.push_back(i0);
        lod.indices.push_back(i1);
        lod.indices.push_back(i3);
    }

    if (capped) {
        const u32 bot_center_idx = static_cast<u32>(lod.vertices.size());
        Vertex bc{};
        bc.position[0] = 0.0f; bc.position[1] = -hh; bc.position[2] = 0.0f;
        bc.normal[0] = 0.0f; bc.normal[1] = -1.0f; bc.normal[2] = 0.0f;
        bc.tangent[0] = 1.0f; bc.tangent[1] = 0.0f; bc.tangent[2] = 0.0f; bc.tangent[3] = 1.0f;
        bc.uv0[0] = 0.5f; bc.uv0[1] = 0.5f;
        lod.vertices.push_back(bc);

        for (u32 s = 0; s <= radial_segments; ++s) {
            const float theta = static_cast<float>(s) * TWO_PI / static_cast<float>(radial_segments);
            const float cos_t = std::cos(theta);
            const float sin_t = std::sin(theta);
            Vertex bv{};
            bv.position[0] = radius * cos_t; bv.position[1] = -hh; bv.position[2] = radius * sin_t;
            bv.normal[0] = 0.0f; bv.normal[1] = -1.0f; bv.normal[2] = 0.0f;
            bv.tangent[0] = 1.0f; bv.tangent[1] = 0.0f; bv.tangent[2] = 0.0f; bv.tangent[3] = 1.0f;
            bv.uv0[0] = 0.5f + 0.5f * cos_t;
            bv.uv0[1] = 0.5f - 0.5f * sin_t;
            lod.vertices.push_back(bv);
        }

        for (u32 s = 0; s < radial_segments; ++s) {
            lod.indices.push_back(bot_center_idx);
            lod.indices.push_back(bot_center_idx + s + 1);
            lod.indices.push_back(bot_center_idx + s + 2);
        }
    }

    finalize_mesh(*mesh);
    return mesh;
}

std::unique_ptr<StaticMesh> ProceduralMesh::build_capsule(
    float radius, float cylinder_height,
    u32 rings, u32 sectors
) {
    rings = std::max(2u, rings);
    sectors = std::max(3u, sectors);

    auto mesh = std::make_unique<StaticMesh>("ProceduralCapsule");
    MeshLOD& lod = mesh->lod(0);

    const float hh = cylinder_height * 0.5f;
    const u32 total_rings = rings * 2 + 1; // Top cap + cylinder seam + bottom cap

    for (u32 r = 0; r <= total_rings; ++r) {
        float phi;
        float y_offset;

        if (r <= rings) {
            // Top hemisphere
            phi = (PI * 0.5f) * (static_cast<float>(r) / static_cast<float>(rings));
            y_offset = hh;
        } else if (r == rings + 1) {
            // Bottom of cylinder
            phi = PI * 0.5f;
            y_offset = -hh;
        } else {
            // Bottom hemisphere
            const u32 bot_r = r - 1;
            phi = (PI * 0.5f) + (PI * 0.5f) * (static_cast<float>(bot_r - rings) / static_cast<float>(rings));
            y_offset = -hh;
        }

        const float cos_phi = std::cos(phi);
        const float sin_phi = std::sin(phi);

        for (u32 s = 0; s <= sectors; ++s) {
            const float theta = TWO_PI * static_cast<float>(s) / static_cast<float>(sectors);
            const float cos_theta = std::cos(theta);
            const float sin_theta = std::sin(theta);

            const float x = radius * sin_phi * cos_theta;
            const float y = radius * cos_phi + y_offset;
            const float z = radius * sin_phi * sin_theta;

            const float nx = sin_phi * cos_theta;
            const float ny = cos_phi;
            const float nz = sin_phi * sin_theta;

            Vertex vert{};
            vert.position[0] = x;
            vert.position[1] = y;
            vert.position[2] = z;
            vert.normal[0] = nx;
            vert.normal[1] = ny;
            vert.normal[2] = nz;
            vert.tangent[0] = -sin_theta;
            vert.tangent[1] = 0.0f;
            vert.tangent[2] = cos_theta;
            vert.tangent[3] = 1.0f;
            vert.uv0[0] = static_cast<float>(s) / static_cast<float>(sectors);
            vert.uv0[1] = static_cast<float>(r) / static_cast<float>(total_rings);
            lod.vertices.push_back(vert);
        }
    }

    const u32 stride = sectors + 1;
    for (u32 r = 0; r < total_rings; ++r) {
        for (u32 s = 0; s < sectors; ++s) {
            const u32 cur = r * stride + s;
            const u32 next = (r + 1) * stride + s;

            lod.indices.push_back(cur);
            lod.indices.push_back(next);
            lod.indices.push_back(next + 1);

            lod.indices.push_back(cur);
            lod.indices.push_back(next + 1);
            lod.indices.push_back(cur + 1);
        }
    }

    finalize_mesh(*mesh);
    return mesh;
}

std::unique_ptr<StaticMesh> ProceduralMesh::build_torus(
    float major_radius, float minor_radius,
    u32 radial_segments, u32 tubular_segments
) {
    radial_segments = std::max(3u, radial_segments);
    tubular_segments = std::max(3u, tubular_segments);

    auto mesh = std::make_unique<StaticMesh>("ProceduralTorus");
    MeshLOD& lod = mesh->lod(0);

    for (u32 i = 0; i <= radial_segments; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(radial_segments);
        const float phi = u * TWO_PI;
        const float cos_phi = std::cos(phi);
        const float sin_phi = std::sin(phi);

        for (u32 j = 0; j <= tubular_segments; ++j) {
            const float v = static_cast<float>(j) / static_cast<float>(tubular_segments);
            const float theta = v * TWO_PI;
            const float cos_theta = std::cos(theta);
            const float sin_theta = std::sin(theta);

            const float x = (major_radius + minor_radius * cos_theta) * cos_phi;
            const float y = minor_radius * sin_theta;
            const float z = (major_radius + minor_radius * cos_theta) * sin_phi;

            const float nx = cos_theta * cos_phi;
            const float ny = sin_theta;
            const float nz = cos_theta * sin_phi;

            Vertex vert{};
            vert.position[0] = x;
            vert.position[1] = y;
            vert.position[2] = z;
            vert.normal[0] = nx;
            vert.normal[1] = ny;
            vert.normal[2] = nz;
            vert.tangent[0] = -sin_phi;
            vert.tangent[1] = 0.0f;
            vert.tangent[2] = cos_phi;
            vert.tangent[3] = 1.0f;
            vert.uv0[0] = u;
            vert.uv0[1] = v;
            lod.vertices.push_back(vert);
        }
    }

    const u32 stride = tubular_segments + 1;
    for (u32 i = 0; i < radial_segments; ++i) {
        for (u32 j = 0; j < tubular_segments; ++j) {
            const u32 i0 = i * stride + j;
            const u32 i1 = (i + 1) * stride + j;
            const u32 i2 = (i + 1) * stride + (j + 1);
            const u32 i3 = i * stride + (j + 1);

            lod.indices.push_back(i0);
            lod.indices.push_back(i1);
            lod.indices.push_back(i2);

            lod.indices.push_back(i0);
            lod.indices.push_back(i2);
            lod.indices.push_back(i3);
        }
    }

    finalize_mesh(*mesh);
    return mesh;
}

std::unique_ptr<StaticMesh> ProceduralMesh::extrude_polygon_along_path(
    const std::vector<Vec2>& cross_section,
    const std::vector<Vec3>& path_points,
    bool closed_path,
    bool cap_ends
) {
    if (cross_section.size() < 3 || path_points.size() < 2) {
        return std::make_unique<StaticMesh>("EmptyExtrusion");
    }

    auto mesh = std::make_unique<StaticMesh>("ExtrudedMesh");
    MeshLOD& lod = mesh->lod(0);

    const u32 num_path = static_cast<u32>(path_points.size());
    const u32 num_poly = static_cast<u32>(cross_section.size());

    // Precalculate cumulative path distances for UV mapping
    std::vector<float> path_dist(num_path, 0.0f);
    for (u32 k = 1; k < num_path; ++k) {
        path_dist[k] = path_dist[k - 1] + (path_points[k] - path_points[k - 1]).length();
    }
    const float total_length = (path_dist.back() > 1e-5f) ? path_dist.back() : 1.0f;

    // Parallel transport frame initialization
    Vec3 prev_tangent;
    if (closed_path) {
        prev_tangent = (path_points[1] - path_points.back()).normalized();
    } else {
        prev_tangent = (path_points[1] - path_points[0]).normalized();
    }

    Vec3 normal_frame = Vec3::up;
    if (std::abs(normal_frame.dot(prev_tangent)) > 0.95f) {
        normal_frame = Vec3::right;
    }
    Vec3 binormal_frame = prev_tangent.cross(normal_frame).normalized();
    normal_frame = binormal_frame.cross(prev_tangent).normalized();

    // Generate rings along path
    for (u32 k = 0; k < num_path; ++k) {
        Vec3 tangent;
        if (closed_path) {
            const u32 prev_idx = (k == 0) ? (num_path - 1) : (k - 1);
            const u32 next_idx = (k + 1) % num_path;
            tangent = (path_points[next_idx] - path_points[prev_idx]).normalized();
        } else {
            if (k == 0) {
                tangent = (path_points[1] - path_points[0]).normalized();
            } else if (k == num_path - 1) {
                tangent = (path_points[num_path - 1] - path_points[num_path - 2]).normalized();
            } else {
                tangent = (path_points[k + 1] - path_points[k - 1]).normalized();
            }
        }

        // Parallel transport rotation to new tangent
        const Vec3 rot_axis = prev_tangent.cross(tangent);
        const float dot_val = std::clamp(prev_tangent.dot(tangent), -1.0f, 1.0f);
        if (rot_axis.length_sq() > 1e-7f) {
            const float angle = std::acos(dot_val);
            const Quat q = Quat::from_axis_angle(rot_axis.normalized(), angle);
            const Mat4 rot_m = Mat4::rotation(rot_axis.normalized(), angle);
            binormal_frame = rot_m.transform_direction(binormal_frame).normalized();
            normal_frame = binormal_frame.cross(tangent).normalized();
        }
        prev_tangent = tangent;

        const float v_coord = path_dist[k] / total_length;

        for (u32 p = 0; p <= num_poly; ++p) {
            const u32 poly_idx = p % num_poly;
            const Vec2& c = cross_section[poly_idx];
            const Vec3 pos = path_points[k] + binormal_frame * c.x + normal_frame * c.y;

            const Vec2 norm2d = c.normalized();
            const Vec3 n = (binormal_frame * norm2d.x + normal_frame * norm2d.y).normalized();

            Vertex vert{};
            vert.position[0] = pos.x;
            vert.position[1] = pos.y;
            vert.position[2] = pos.z;
            vert.normal[0] = n.x;
            vert.normal[1] = n.y;
            vert.normal[2] = n.z;
            vert.tangent[0] = tangent.x;
            vert.tangent[1] = tangent.y;
            vert.tangent[2] = tangent.z;
            vert.tangent[3] = 1.0f;
            vert.uv0[0] = static_cast<float>(p) / static_cast<float>(num_poly);
            vert.uv0[1] = v_coord;
            lod.vertices.push_back(vert);
        }
    }

    const u32 stride = num_poly + 1;
    const u32 ring_count = closed_path ? num_path : (num_path - 1);
    for (u32 k = 0; k < ring_count; ++k) {
        const u32 cur_ring = k;
        const u32 next_ring = (k + 1) % num_path;

        for (u32 p = 0; p < num_poly; ++p) {
            const u32 i0 = cur_ring * stride + p;
            const u32 i1 = next_ring * stride + p;
            const u32 i2 = next_ring * stride + (p + 1);
            const u32 i3 = cur_ring * stride + (p + 1);

            lod.indices.push_back(i0);
            lod.indices.push_back(i1);
            lod.indices.push_back(i2);

            lod.indices.push_back(i0);
            lod.indices.push_back(i2);
            lod.indices.push_back(i3);
        }
    }

    if (cap_ends && !closed_path) {
        // Start cap (fan triangulation)
        const u32 start_center_idx = static_cast<u32>(lod.vertices.size());
        const Vec3 start_tan = (path_points[1] - path_points[0]).normalized();
        const Vec3 start_n = -start_tan;

        Vertex sc{};
        sc.position[0] = path_points[0].x; sc.position[1] = path_points[0].y; sc.position[2] = path_points[0].z;
        sc.normal[0] = start_n.x; sc.normal[1] = start_n.y; sc.normal[2] = start_n.z;
        sc.uv0[0] = 0.5f; sc.uv0[1] = 0.5f;
        lod.vertices.push_back(sc);

        for (u32 p = 0; p < num_poly; ++p) {
            const Vec2& c = cross_section[p];
            const Vec3 pos = path_points[0] + binormal_frame * c.x + normal_frame * c.y;
            Vertex sv{};
            sv.position[0] = pos.x; sv.position[1] = pos.y; sv.position[2] = pos.z;
            sv.normal[0] = start_n.x; sv.normal[1] = start_n.y; sv.normal[2] = start_n.z;
            sv.uv0[0] = 0.5f + c.x * 0.5f;
            sv.uv0[1] = 0.5f + c.y * 0.5f;
            lod.vertices.push_back(sv);
        }

        for (u32 p = 0; p < num_poly; ++p) {
            const u32 next_p = (p + 1) % num_poly;
            lod.indices.push_back(start_center_idx);
            lod.indices.push_back(start_center_idx + 1 + p);
            lod.indices.push_back(start_center_idx + 1 + next_p);
        }

        // End cap
        const u32 end_center_idx = static_cast<u32>(lod.vertices.size());
        const Vec3 end_tan = (path_points[num_path - 1] - path_points[num_path - 2]).normalized();

        Vertex ec{};
        ec.position[0] = path_points.back().x; ec.position[1] = path_points.back().y; ec.position[2] = path_points.back().z;
        ec.normal[0] = end_tan.x; ec.normal[1] = end_tan.y; ec.normal[2] = end_tan.z;
        ec.uv0[0] = 0.5f; ec.uv0[1] = 0.5f;
        lod.vertices.push_back(ec);

        for (u32 p = 0; p < num_poly; ++p) {
            const Vec2& c = cross_section[p];
            const Vec3 pos = path_points.back() + binormal_frame * c.x + normal_frame * c.y;
            Vertex ev{};
            ev.position[0] = pos.x; ev.position[1] = pos.y; ev.position[2] = pos.z;
            ev.normal[0] = end_tan.x; ev.normal[1] = end_tan.y; ev.normal[2] = end_tan.z;
            ev.uv0[0] = 0.5f + c.x * 0.5f;
            ev.uv0[1] = 0.5f + c.y * 0.5f;
            lod.vertices.push_back(ev);
        }

        for (u32 p = 0; p < num_poly; ++p) {
            const u32 next_p = (p + 1) % num_poly;
            lod.indices.push_back(end_center_idx);
            lod.indices.push_back(end_center_idx + 1 + next_p);
            lod.indices.push_back(end_center_idx + 1 + p);
        }
    }

    finalize_mesh(*mesh);
    return mesh;
}

void ProceduralMesh::transform_mesh(StaticMesh& mesh, const Mat4& transform) {
    const Mat4 norm_mat = transform.inverse().transposed();

    for (auto& lod : mesh.lods()) {
        for (auto& v : lod.vertices) {
            const Vec3 p = transform.transform_point(Vec3(v.position[0], v.position[1], v.position[2]));
            v.position[0] = p.x;
            v.position[1] = p.y;
            v.position[2] = p.z;

            const Vec3 n = norm_mat.transform_direction(Vec3(v.normal[0], v.normal[1], v.normal[2])).normalized();
            v.normal[0] = n.x;
            v.normal[1] = n.y;
            v.normal[2] = n.z;

            const Vec3 t = transform.transform_direction(Vec3(v.tangent[0], v.tangent[1], v.tangent[2])).normalized();
            v.tangent[0] = t.x;
            v.tangent[1] = t.y;
            v.tangent[2] = t.z;
        }
        StaticMesh::compute_lod_bounds(lod);
    }
}

std::unique_ptr<StaticMesh> ProceduralMesh::merge_meshes(
    const StaticMesh& a, const StaticMesh& b
) {
    auto result = std::make_unique<StaticMesh>("MergedMesh");
    MeshLOD& lod = result->lod(0);

    const auto& lod_a = a.lod(0);
    const auto& lod_b = b.lod(0);

    lod.vertices = lod_a.vertices;
    lod.indices = lod_a.indices;

    const u32 offset = static_cast<u32>(lod_a.vertices.size());
    for (const auto& v : lod_b.vertices) {
        lod.vertices.push_back(v);
    }
    for (const auto& idx : lod_b.indices) {
        lod.indices.push_back(idx + offset);
    }

    finalize_mesh(*result);
    return result;
}

void ProceduralMesh::recompute_normals(StaticMesh& mesh, bool smooth) {
    for (auto& lod : mesh.lods()) {
        if (lod.indices.empty() || lod.vertices.empty()) continue;

        if (smooth) {
            std::vector<Vec3> accumulated(lod.vertices.size(), Vec3::zero);

            for (usize i = 0; i + 2 < lod.indices.size(); i += 3) {
                const u32 i0 = lod.indices[i];
                const u32 i1 = lod.indices[i + 1];
                const u32 i2 = lod.indices[i + 2];

                const Vec3 p0(lod.vertices[i0].position[0], lod.vertices[i0].position[1], lod.vertices[i0].position[2]);
                const Vec3 p1(lod.vertices[i1].position[0], lod.vertices[i1].position[1], lod.vertices[i1].position[2]);
                const Vec3 p2(lod.vertices[i2].position[0], lod.vertices[i2].position[1], lod.vertices[i2].position[2]);

                const Vec3 fn = (p1 - p0).cross(p2 - p0);
                accumulated[i0] += fn;
                accumulated[i1] += fn;
                accumulated[i2] += fn;
            }

            for (usize i = 0; i < lod.vertices.size(); ++i) {
                Vec3 n = accumulated[i].normalized();
                if (n.length_sq() < 1e-6f) n = Vec3::up;
                lod.vertices[i].normal[0] = n.x;
                lod.vertices[i].normal[1] = n.y;
                lod.vertices[i].normal[2] = n.z;
            }
        } else {
            // Flat shading: unshare vertices per triangle
            std::vector<Vertex> flat_vertices;
            std::vector<u32> flat_indices;
            flat_vertices.reserve(lod.indices.size());
            flat_indices.reserve(lod.indices.size());

            for (usize i = 0; i + 2 < lod.indices.size(); i += 3) {
                Vertex v0 = lod.vertices[lod.indices[i]];
                Vertex v1 = lod.vertices[lod.indices[i + 1]];
                Vertex v2 = lod.vertices[lod.indices[i + 2]];

                const Vec3 p0(v0.position[0], v0.position[1], v0.position[2]);
                const Vec3 p1(v1.position[0], v1.position[1], v1.position[2]);
                const Vec3 p2(v2.position[0], v2.position[1], v2.position[2]);

                Vec3 fn = (p1 - p0).cross(p2 - p0).normalized();
                if (fn.length_sq() < 1e-6f) fn = Vec3::up;

                v0.normal[0] = fn.x; v0.normal[1] = fn.y; v0.normal[2] = fn.z;
                v1.normal[0] = fn.x; v1.normal[1] = fn.y; v1.normal[2] = fn.z;
                v2.normal[0] = fn.x; v2.normal[1] = fn.y; v2.normal[2] = fn.z;

                const u32 base = static_cast<u32>(flat_vertices.size());
                flat_vertices.push_back(v0);
                flat_vertices.push_back(v1);
                flat_vertices.push_back(v2);

                flat_indices.push_back(base);
                flat_indices.push_back(base + 1);
                flat_indices.push_back(base + 2);
            }

            lod.vertices = std::move(flat_vertices);
            lod.indices = std::move(flat_indices);
        }
        StaticMesh::compute_lod_bounds(lod);
    }
}

void ProceduralMesh::recompute_tangents(StaticMesh& mesh) {
    for (auto& lod : mesh.lods()) {
        if (lod.indices.empty() || lod.vertices.empty()) continue;

        std::vector<Vec3> tan1(lod.vertices.size(), Vec3::zero);
        std::vector<Vec3> tan2(lod.vertices.size(), Vec3::zero);

        for (usize i = 0; i + 2 < lod.indices.size(); i += 3) {
            const u32 i0 = lod.indices[i];
            const u32 i1 = lod.indices[i + 1];
            const u32 i2 = lod.indices[i + 2];

            const Vertex& v0 = lod.vertices[i0];
            const Vertex& v1 = lod.vertices[i1];
            const Vertex& v2 = lod.vertices[i2];

            const Vec3 p0(v0.position[0], v0.position[1], v0.position[2]);
            const Vec3 p1(v1.position[0], v1.position[1], v1.position[2]);
            const Vec3 p2(v2.position[0], v2.position[1], v2.position[2]);

            const Vec3 edge1 = p1 - p0;
            const Vec3 edge2 = p2 - p0;

            const float delta_u1 = v1.uv0[0] - v0.uv0[0];
            const float delta_v1 = v1.uv0[1] - v0.uv0[1];
            const float delta_u2 = v2.uv0[0] - v0.uv0[0];
            const float delta_v2 = v2.uv0[1] - v0.uv0[1];

            const float det = delta_u1 * delta_v2 - delta_u2 * delta_v1;
            const float r = (std::abs(det) > 1e-7f) ? (1.0f / det) : 0.0f;

            const Vec3 sdir(
                (delta_v2 * edge1.x - delta_v1 * edge2.x) * r,
                (delta_v2 * edge1.y - delta_v1 * edge2.y) * r,
                (delta_v2 * edge1.z - delta_v1 * edge2.z) * r
            );
            const Vec3 tdir(
                (delta_u1 * edge2.x - delta_u2 * edge1.x) * r,
                (delta_u1 * edge2.y - delta_u2 * edge1.y) * r,
                (delta_u1 * edge2.z - delta_u2 * edge1.z) * r
            );

            tan1[i0] += sdir;
            tan1[i1] += sdir;
            tan1[i2] += sdir;

            tan2[i0] += tdir;
            tan2[i1] += tdir;
            tan2[i2] += tdir;
        }

        for (usize i = 0; i < lod.vertices.size(); ++i) {
            const Vec3 n(lod.vertices[i].normal[0], lod.vertices[i].normal[1], lod.vertices[i].normal[2]);
            const Vec3 t = tan1[i];

            // Gram-Schmidt orthogonalize
            Vec3 ortho = (t - n * n.dot(t)).normalized();
            if (ortho.length_sq() < 1e-6f) {
                ortho = (std::abs(n.x) > 0.9f) ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
            }

            const float handedness = (n.cross(t).dot(tan2[i]) < 0.0f) ? -1.0f : 1.0f;

            lod.vertices[i].tangent[0] = ortho.x;
            lod.vertices[i].tangent[1] = ortho.y;
            lod.vertices[i].tangent[2] = ortho.z;
            lod.vertices[i].tangent[3] = handedness;
        }
    }
}

void ProceduralMesh::invert_faces(StaticMesh& mesh) {
    for (auto& lod : mesh.lods()) {
        for (usize i = 0; i + 2 < lod.indices.size(); i += 3) {
            std::swap(lod.indices[i + 1], lod.indices[i + 2]);
        }
        for (auto& v : lod.vertices) {
            v.normal[0] = -v.normal[0];
            v.normal[1] = -v.normal[1];
            v.normal[2] = -v.normal[2];
        }
        StaticMesh::compute_lod_bounds(lod);
    }
}

void ProceduralMesh::displace_mesh(
    StaticMesh& mesh,
    const core::Noise& noise,
    float amplitude,
    float frequency,
    core::NoiseType type,
    u32 octaves
) {
    (void)type;
    (void)octaves;
    for (auto& lod : mesh.lods()) {
        for (auto& v : lod.vertices) {
            const float val = noise.sample_3d(
                v.position[0] * frequency,
                v.position[1] * frequency,
                v.position[2] * frequency
            );

            v.position[0] += v.normal[0] * (val * amplitude);
            v.position[1] += v.normal[1] * (val * amplitude);
            v.position[2] += v.normal[2] * (val * amplitude);
        }
    }

    recompute_normals(mesh, true);
    recompute_tangents(mesh);
    finalize_mesh(mesh);
}

std::unique_ptr<StaticMesh> ProceduralMesh::build_asteroid(
    float radius,
    const core::Noise& noise,
    float roughness,
    u32 subdivisions
) {
    auto mesh = build_icosphere(radius, subdivisions);
    displace_mesh(*mesh, noise, radius * roughness, 1.0f / radius, core::NoiseType::Perlin, 3);
    return mesh;
}

std::unique_ptr<StaticMesh> ProceduralMesh::build_terrain_patch(
    float width, float depth,
    u32 sub_x, u32 sub_z,
    const core::Noise& noise,
    float height_scale,
    float frequency,
    u32 octaves
) {
    (void)octaves;
    auto mesh = build_plane(width, depth, sub_x, sub_z);
    MeshLOD& lod = mesh->lod(0);

    for (auto& v : lod.vertices) {
        const float val = noise.sample_2d(
            v.position[0] * frequency,
            v.position[2] * frequency
        );
        v.position[1] = val * height_scale;
    }

    recompute_normals(*mesh, true);
    recompute_tangents(*mesh);
    finalize_mesh(*mesh);
    return mesh;
}

} // namespace nf::rendering
