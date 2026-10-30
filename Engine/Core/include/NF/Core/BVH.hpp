#pragma once

#include <NF/Core/Geometry.hpp>
#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <memory>
#include <vector>

namespace nf::core {

/// Partitioning strategy for BVH building
enum class BVHSplitMethod : u8 {
    Middle = 0,     // Midpoint along the largest axis
    EqualCounts,    // Median / equal object counts
    SAH             // Binned Surface Area Heuristic
};

/// An input primitive to the BVH
struct BVHPrimitive {
    u32 id = 0;
    AABB bounds;

    [[nodiscard]] Vec3 centroid() const noexcept { return bounds.center(); }
};

/// Hit result from a BVH raycast query
struct BVHRayHit {
    u32 primitive_id = 0;
    f32 distance = 1e30f;
    Vec3 point{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
};

/// Linear flattened node stored in contiguous memory
struct alignas(32) BVHLinearNode {
    AABB bounds;
    u32  primitives_offset_or_second_child = 0; // If leaf: offset in m_ordered_primitives. If interior: index of second child.
    u16  primitive_count = 0;                   // 0 for interior node, >0 for leaf
    u8   split_axis = 0;                        // 0=X, 1=Y, 2=Z
    u8   pad = 0;

    [[nodiscard]] bool is_leaf() const noexcept { return primitive_count > 0; }
};

/// High-performance Linear Bounding Volume Hierarchy (BVH)
/// Uses binned Surface Area Heuristic (SAH) and a cache-friendly flattened linear node layout.
/// Traversal uses a fixed stack without dynamic allocations or recursion.
class BVH {
public:
    BVH() = default;
    ~BVH() = default;

    /// Build BVH from a collection of primitives
    void build(std::vector<BVHPrimitive> primitives,
               BVHSplitMethod method = BVHSplitMethod::SAH,
               u32 max_prims_in_node = 4);

    /// Clear all nodes and primitive data
    void clear() noexcept;

    /// Fast closest-hit raycast
    bool raycast(const Ray& ray, BVHRayHit& out_hit, f32 max_distance = 1e30f) const;

    /// Collect all intersecting primitives along the ray, sorted by distance
    bool raycast_all(const Ray& ray, std::vector<BVHRayHit>& out_hits, f32 max_distance = 1e30f) const;

    /// Query all primitives whose bounding boxes intersect the given box
    void query_aabb(const AABB& box, std::vector<u32>& out_primitive_ids) const;

    /// Query all primitives intersecting the view frustum (frustum culling)
    void query_frustum(const Frustum& frustum, std::vector<u32>& out_primitive_ids) const;

    /// Find the nearest primitive to a 3D point (by AABB distance)
    bool query_nearest(const Vec3& point, u32& out_primitive_id, f32& out_distance) const;

    /// Fast refitting of bounding boxes when primitives deform without changing topology
    void refit();

    [[nodiscard]] bool is_valid() const noexcept { return !m_nodes.empty(); }
    [[nodiscard]] size_t node_count() const noexcept { return m_nodes.size(); }
    [[nodiscard]] size_t primitive_count() const noexcept { return m_ordered_primitives.size(); }
    [[nodiscard]] const AABB& root_bounds() const noexcept;
    [[nodiscard]] const std::vector<BVHPrimitive>& ordered_primitives() const noexcept { return m_ordered_primitives; }
    [[nodiscard]] const std::vector<BVHLinearNode>& nodes() const noexcept { return m_nodes; }

private:
    struct BuildNode {
        AABB bounds;
        std::unique_ptr<BuildNode> children[2];
        u32 split_axis = 0;
        u32 first_prim_offset = 0;
        u32 prim_count = 0;

        void init_leaf(u32 first, u32 count, const AABB& b) {
            first_prim_offset = first;
            prim_count = count;
            bounds = b;
            children[0] = nullptr;
            children[1] = nullptr;
        }

        void init_interior(u32 axis, std::unique_ptr<BuildNode> c0, std::unique_ptr<BuildNode> c1) {
            split_axis = axis;
            children[0] = std::move(c0);
            children[1] = std::move(c1);
            bounds = children[0]->bounds;
            bounds.expand(children[1]->bounds);
            prim_count = 0;
        }

        [[nodiscard]] bool is_leaf() const noexcept { return prim_count > 0; }
    };

    struct PrimInfo {
        u32 prim_index = 0;
        AABB bounds;
        Vec3 centroid{0.0f, 0.0f, 0.0f};
    };

    std::unique_ptr<BuildNode> recursive_build(
        std::vector<PrimInfo>& prim_info,
        u32 start,
        u32 end,
        u32* total_nodes,
        std::vector<BVHPrimitive>& ordered_prims,
        const std::vector<BVHPrimitive>& src_prims,
        BVHSplitMethod method,
        u32 max_prims_in_node);

    u32 flatten_tree(const BuildNode* node, u32* offset);

    std::vector<BVHLinearNode> m_nodes;
    std::vector<BVHPrimitive>  m_ordered_primitives;
};

} // namespace nf::core
