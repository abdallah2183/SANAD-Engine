#include <NF/Core/BVH.hpp>

#include <algorithm>
#include <cmath>

namespace nf::core {

namespace {

static inline f32 aabb_surface_area(const AABB& box) {
    const Vec3 d = box.max - box.min;
    return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
}

static inline f32 aabb_sq_dist(const AABB& box, const Vec3& pt) {
    f32 sq_dist = 0.0f;
    const f32* pt_arr = &pt.x;
    const f32* min_arr = &box.min.x;
    const f32* max_arr = &box.max.x;
    for (int i = 0; i < 3; ++i) {
        const f32 v = pt_arr[i];
        const f32 min_v = min_arr[i];
        const f32 max_v = max_arr[i];
        if (v < min_v) {
            sq_dist += (min_v - v) * (min_v - v);
        } else if (v > max_v) {
            sq_dist += (v - max_v) * (v - max_v);
        }
    }
    return sq_dist;
}

static inline Vec3 aabb_normal_at(const AABB& box, const Vec3& pt) {
    Vec3 normal{0.0f, 1.0f, 0.0f};
    f32 min_dist = 1e30f;

    const f32 d_min_x = std::abs(pt.x - box.min.x);
    if (d_min_x < min_dist) { min_dist = d_min_x; normal = Vec3{-1.0f, 0.0f, 0.0f}; }
    const f32 d_max_x = std::abs(pt.x - box.max.x);
    if (d_max_x < min_dist) { min_dist = d_max_x; normal = Vec3{1.0f, 0.0f, 0.0f}; }

    const f32 d_min_y = std::abs(pt.y - box.min.y);
    if (d_min_y < min_dist) { min_dist = d_min_y; normal = Vec3{0.0f, -1.0f, 0.0f}; }
    const f32 d_max_y = std::abs(pt.y - box.max.y);
    if (d_max_y < min_dist) { min_dist = d_max_y; normal = Vec3{0.0f, 1.0f, 0.0f}; }

    const f32 d_min_z = std::abs(pt.z - box.min.z);
    if (d_min_z < min_dist) { min_dist = d_min_z; normal = Vec3{0.0f, 0.0f, -1.0f}; }
    const f32 d_max_z = std::abs(pt.z - box.max.z);
    if (d_max_z < min_dist) { normal = Vec3{0.0f, 0.0f, 1.0f}; }

    return normal;
}

} // namespace

void BVH::clear() noexcept {
    m_nodes.clear();
    m_ordered_primitives.clear();
}

const AABB& BVH::root_bounds() const noexcept {
    static const AABB s_empty;
    return m_nodes.empty() ? s_empty : m_nodes[0].bounds;
}

void BVH::build(std::vector<BVHPrimitive> primitives,
                BVHSplitMethod method,
                u32 max_prims_in_node)
{
    clear();
    if (primitives.empty()) {
        return;
    }

    max_prims_in_node = std::clamp(max_prims_in_node, 1u, 64u);

    std::vector<PrimInfo> prim_info(primitives.size());
    for (size_t i = 0; i < primitives.size(); ++i) {
        prim_info[i].prim_index = static_cast<u32>(i);
        prim_info[i].bounds = primitives[i].bounds;
        prim_info[i].centroid = primitives[i].centroid();
    }

    u32 total_nodes = 0;
    std::vector<BVHPrimitive> ordered_prims;
    ordered_prims.reserve(primitives.size());

    std::unique_ptr<BuildNode> root = recursive_build(
        prim_info,
        0,
        static_cast<u32>(primitives.size()),
        &total_nodes,
        ordered_prims,
        primitives,
        method,
        max_prims_in_node
    );

    m_ordered_primitives = std::move(ordered_prims);
    m_nodes.resize(total_nodes);
    u32 offset = 0;
    flatten_tree(root.get(), &offset);
}

std::unique_ptr<BVH::BuildNode> BVH::recursive_build(
    std::vector<PrimInfo>& prim_info,
    u32 start,
    u32 end,
    u32* total_nodes,
    std::vector<BVHPrimitive>& ordered_prims,
    const std::vector<BVHPrimitive>& src_prims,
    BVHSplitMethod method,
    u32 max_prims_in_node)
{
    (*total_nodes)++;
    auto node = std::make_unique<BuildNode>();

    // Compute total bounds and centroid bounds for this subset
    AABB total_bounds;
    AABB centroid_bounds;
    for (u32 i = start; i < end; ++i) {
        total_bounds.expand(prim_info[i].bounds);
        centroid_bounds.expand(prim_info[i].centroid);
    }

    const u32 count = end - start;
    if (count <= max_prims_in_node) {
        // Create leaf node
        const u32 first_prim_offset = static_cast<u32>(ordered_prims.size());
        for (u32 i = start; i < end; ++i) {
            ordered_prims.push_back(src_prims[prim_info[i].prim_index]);
        }
        node->init_leaf(first_prim_offset, count, total_bounds);
        return node;
    }

    // Choose split axis as the longest axis of centroid_bounds
    const Vec3 centroid_diag = centroid_bounds.size();
    u32 dim = 0;
    if (centroid_diag.y > centroid_diag.x && centroid_diag.y > centroid_diag.z) {
        dim = 1;
    } else if (centroid_diag.z > centroid_diag.x) {
        dim = 2;
    }

    // Degenerate case: all centroids coincide
    const f32 dim_extent = (&centroid_bounds.max.x)[dim] - (&centroid_bounds.min.x)[dim];
    if (dim_extent <= 1e-6f) {
        const u32 first_prim_offset = static_cast<u32>(ordered_prims.size());
        for (u32 i = start; i < end; ++i) {
            ordered_prims.push_back(src_prims[prim_info[i].prim_index]);
        }
        node->init_leaf(first_prim_offset, count, total_bounds);
        return node;
    }

    u32 mid = (start + end) / 2;

    if (method == BVHSplitMethod::Middle) {
        const f32 pmid = 0.5f * ((&centroid_bounds.min.x)[dim] + (&centroid_bounds.max.x)[dim]);
        auto mid_iter = std::partition(
            prim_info.begin() + start,
            prim_info.begin() + end,
            [dim, pmid](const PrimInfo& pi) {
                return (&pi.centroid.x)[dim] < pmid;
            }
        );
        mid = static_cast<u32>(mid_iter - prim_info.begin());
        if (mid == start || mid == end) {
            mid = (start + end) / 2;
            std::nth_element(
                prim_info.begin() + start,
                prim_info.begin() + mid,
                prim_info.begin() + end,
                [dim](const PrimInfo& a, const PrimInfo& b) {
                    return (&a.centroid.x)[dim] < (&b.centroid.x)[dim];
                }
            );
        }
    } else if (method == BVHSplitMethod::EqualCounts) {
        mid = (start + end) / 2;
        std::nth_element(
            prim_info.begin() + start,
            prim_info.begin() + mid,
            prim_info.begin() + end,
            [dim](const PrimInfo& a, const PrimInfo& b) {
                return (&a.centroid.x)[dim] < (&b.centroid.x)[dim];
            }
        );
    } else { // SAH
        if (count <= 4) {
            mid = (start + end) / 2;
            std::nth_element(
                prim_info.begin() + start,
                prim_info.begin() + mid,
                prim_info.begin() + end,
                [dim](const PrimInfo& a, const PrimInfo& b) {
                    return (&a.centroid.x)[dim] < (&b.centroid.x)[dim];
                }
            );
        } else {
            constexpr u32 kNumBins = 12;
            struct Bin {
                u32 count = 0;
                AABB bounds;
            } bins[kNumBins];

            const f32 min_val = (&centroid_bounds.min.x)[dim];
            const f32 max_val = (&centroid_bounds.max.x)[dim];
            const f32 inv_range = (max_val > min_val) ? (static_cast<f32>(kNumBins) / (max_val - min_val)) : 0.0f;

            for (u32 i = start; i < end; ++i) {
                u32 b = static_cast<u32>(((&prim_info[i].centroid.x)[dim] - min_val) * inv_range);
                if (b >= kNumBins) b = kNumBins - 1;
                bins[b].count++;
                bins[b].bounds.expand(prim_info[i].bounds);
            }

            f32 cost[kNumBins - 1];
            for (u32 i = 0; i < kNumBins - 1; ++i) {
                AABB b0, b1;
                u32 count0 = 0, count1 = 0;
                for (u32 j = 0; j <= i; ++j) {
                    b0.expand(bins[j].bounds);
                    count0 += bins[j].count;
                }
                for (u32 j = i + 1; j < kNumBins; ++j) {
                    b1.expand(bins[j].bounds);
                    count1 += bins[j].count;
                }
                const f32 total_sa = aabb_surface_area(total_bounds);
                cost[i] = 1.0f + (static_cast<f32>(count0) * aabb_surface_area(b0) +
                                  static_cast<f32>(count1) * aabb_surface_area(b1)) / (total_sa > 1e-6f ? total_sa : 1.0f);
            }

            f32 min_cost = cost[0];
            u32 min_cost_split_bin = 0;
            for (u32 i = 1; i < kNumBins - 1; ++i) {
                if (cost[i] < min_cost) {
                    min_cost = cost[i];
                    min_cost_split_bin = i;
                }
            }

            const f32 leaf_cost = static_cast<f32>(count);
            if (count <= max_prims_in_node && min_cost > leaf_cost) {
                const u32 first_prim_offset = static_cast<u32>(ordered_prims.size());
                for (u32 i = start; i < end; ++i) {
                    ordered_prims.push_back(src_prims[prim_info[i].prim_index]);
                }
                node->init_leaf(first_prim_offset, count, total_bounds);
                return node;
            }

            auto mid_iter = std::partition(
                prim_info.begin() + start,
                prim_info.begin() + end,
                [dim, min_val, inv_range, min_cost_split_bin](const PrimInfo& pi) {
                    u32 b = static_cast<u32>(((&pi.centroid.x)[dim] - min_val) * inv_range);
                    if (b >= kNumBins) b = kNumBins - 1;
                    return b <= min_cost_split_bin;
                }
            );
            mid = static_cast<u32>(mid_iter - prim_info.begin());
            if (mid == start || mid == end) {
                mid = (start + end) / 2;
                std::nth_element(
                    prim_info.begin() + start,
                    prim_info.begin() + mid,
                    prim_info.begin() + end,
                    [dim](const PrimInfo& a, const PrimInfo& b) {
                        return (&a.centroid.x)[dim] < (&b.centroid.x)[dim];
                    }
                );
            }
        }
    }

    auto c0 = recursive_build(prim_info, start, mid, total_nodes, ordered_prims, src_prims, method, max_prims_in_node);
    auto c1 = recursive_build(prim_info, mid, end, total_nodes, ordered_prims, src_prims, method, max_prims_in_node);
    node->init_interior(dim, std::move(c0), std::move(c1));

    return node;
}

u32 BVH::flatten_tree(const BuildNode* node, u32* offset) {
    const u32 my_offset = (*offset)++;
    BVHLinearNode& linear_node = m_nodes[my_offset];
    linear_node.bounds = node->bounds;

    if (node->is_leaf()) {
        linear_node.primitives_offset_or_second_child = node->first_prim_offset;
        linear_node.primitive_count = static_cast<u16>(node->prim_count);
    } else {
        linear_node.split_axis = static_cast<u8>(node->split_axis);
        linear_node.primitive_count = 0;
        flatten_tree(node->children[0].get(), offset);
        linear_node.primitives_offset_or_second_child = flatten_tree(node->children[1].get(), offset);
    }

    return my_offset;
}

bool BVH::raycast(const Ray& ray, BVHRayHit& out_hit, f32 max_distance) const {
    if (m_nodes.empty()) {
        return false;
    }

    f32 closest_t = max_distance;
    bool hit_found = false;

    u32 stack[64];
    u32 stack_ptr = 0;
    stack[stack_ptr++] = 0;

    const bool dir_is_neg[3] = {
        ray.direction.x < 0.0f,
        ray.direction.y < 0.0f,
        ray.direction.z < 0.0f
    };

    while (stack_ptr > 0) {
        const u32 node_idx = stack[--stack_ptr];
        const BVHLinearNode& node = m_nodes[node_idx];

        f32 node_t = 0.0f;
        if (!ray.intersects(node.bounds, node_t) || node_t >= closest_t) {
            continue;
        }

        if (node.is_leaf()) {
            for (u16 i = 0; i < node.primitive_count; ++i) {
                const BVHPrimitive& prim = m_ordered_primitives[node.primitives_offset_or_second_child + i];
                f32 prim_t = 0.0f;
                if (ray.intersects(prim.bounds, prim_t) && prim_t < closest_t) {
                    closest_t = prim_t;
                    out_hit.distance = prim_t;
                    out_hit.primitive_id = prim.id;
                    out_hit.point = ray.at(prim_t);
                    out_hit.normal = aabb_normal_at(prim.bounds, out_hit.point);
                    hit_found = true;
                }
            }
        } else {
            // Push farther node first so nearer node is popped and visited first
            if (dir_is_neg[node.split_axis]) {
                stack[stack_ptr++] = node_idx + 1;
                stack[stack_ptr++] = node.primitives_offset_or_second_child;
            } else {
                stack[stack_ptr++] = node.primitives_offset_or_second_child;
                stack[stack_ptr++] = node_idx + 1;
            }
        }
    }

    return hit_found;
}

bool BVH::raycast_all(const Ray& ray, std::vector<BVHRayHit>& out_hits, f32 max_distance) const {
    if (m_nodes.empty()) {
        return false;
    }

    u32 stack[64];
    u32 stack_ptr = 0;
    stack[stack_ptr++] = 0;

    while (stack_ptr > 0) {
        const u32 node_idx = stack[--stack_ptr];
        const BVHLinearNode& node = m_nodes[node_idx];

        f32 node_t = 0.0f;
        if (!ray.intersects(node.bounds, node_t) || node_t >= max_distance) {
            continue;
        }

        if (node.is_leaf()) {
            for (u16 i = 0; i < node.primitive_count; ++i) {
                const BVHPrimitive& prim = m_ordered_primitives[node.primitives_offset_or_second_child + i];
                f32 prim_t = 0.0f;
                if (ray.intersects(prim.bounds, prim_t) && prim_t <= max_distance) {
                    BVHRayHit hit;
                    hit.distance = prim_t;
                    hit.primitive_id = prim.id;
                    hit.point = ray.at(prim_t);
                    hit.normal = aabb_normal_at(prim.bounds, hit.point);
                    out_hits.push_back(hit);
                }
            }
        } else {
            stack[stack_ptr++] = node_idx + 1;
            stack[stack_ptr++] = node.primitives_offset_or_second_child;
        }
    }

    std::sort(out_hits.begin(), out_hits.end(), [](const BVHRayHit& a, const BVHRayHit& b) {
        return a.distance < b.distance;
    });

    return !out_hits.empty();
}

void BVH::query_aabb(const AABB& box, std::vector<u32>& out_primitive_ids) const {
    if (m_nodes.empty()) {
        return;
    }

    u32 stack[64];
    u32 stack_ptr = 0;
    stack[stack_ptr++] = 0;

    while (stack_ptr > 0) {
        const u32 node_idx = stack[--stack_ptr];
        const BVHLinearNode& node = m_nodes[node_idx];

        if (!node.bounds.intersects(box)) {
            continue;
        }

        if (node.is_leaf()) {
            for (u16 i = 0; i < node.primitive_count; ++i) {
                const BVHPrimitive& prim = m_ordered_primitives[node.primitives_offset_or_second_child + i];
                if (prim.bounds.intersects(box)) {
                    out_primitive_ids.push_back(prim.id);
                }
            }
        } else {
            stack[stack_ptr++] = node_idx + 1;
            stack[stack_ptr++] = node.primitives_offset_or_second_child;
        }
    }
}

void BVH::query_frustum(const Frustum& frustum, std::vector<u32>& out_primitive_ids) const {
    if (m_nodes.empty()) {
        return;
    }

    u32 stack[64];
    u32 stack_ptr = 0;
    stack[stack_ptr++] = 0;

    while (stack_ptr > 0) {
        const u32 node_idx = stack[--stack_ptr];
        const BVHLinearNode& node = m_nodes[node_idx];

        if (!frustum.intersects_aabb(node.bounds)) {
            continue;
        }

        if (node.is_leaf()) {
            for (u16 i = 0; i < node.primitive_count; ++i) {
                const BVHPrimitive& prim = m_ordered_primitives[node.primitives_offset_or_second_child + i];
                if (frustum.intersects_aabb(prim.bounds)) {
                    out_primitive_ids.push_back(prim.id);
                }
            }
        } else {
            stack[stack_ptr++] = node_idx + 1;
            stack[stack_ptr++] = node.primitives_offset_or_second_child;
        }
    }
}

bool BVH::query_nearest(const Vec3& point, u32& out_primitive_id, f32& out_distance) const {
    if (m_nodes.empty()) {
        return false;
    }

    f32 closest_sq = 1e30f;
    bool found = false;

    u32 stack[64];
    u32 stack_ptr = 0;
    stack[stack_ptr++] = 0;

    while (stack_ptr > 0) {
        const u32 node_idx = stack[--stack_ptr];
        const BVHLinearNode& node = m_nodes[node_idx];

        const f32 node_dist_sq = aabb_sq_dist(node.bounds, point);
        if (node_dist_sq >= closest_sq) {
            continue;
        }

        if (node.is_leaf()) {
            for (u16 i = 0; i < node.primitive_count; ++i) {
                const BVHPrimitive& prim = m_ordered_primitives[node.primitives_offset_or_second_child + i];
                const f32 d_sq = aabb_sq_dist(prim.bounds, point);
                if (d_sq < closest_sq) {
                    closest_sq = d_sq;
                    out_primitive_id = prim.id;
                    found = true;
                }
            }
        } else {
            stack[stack_ptr++] = node_idx + 1;
            stack[stack_ptr++] = node.primitives_offset_or_second_child;
        }
    }

    if (found) {
        out_distance = std::sqrt(closest_sq);
    }
    return found;
}

void BVH::refit() {
    if (m_nodes.empty()) {
        return;
    }

    // Post-order bottom-up update of bounding boxes
    for (i32 i = static_cast<i32>(m_nodes.size()) - 1; i >= 0; --i) {
        BVHLinearNode& node = m_nodes[static_cast<size_t>(i)];
        if (node.is_leaf()) {
            node.bounds.reset();
            for (u16 p = 0; p < node.primitive_count; ++p) {
                node.bounds.expand(m_ordered_primitives[node.primitives_offset_or_second_child + p].bounds);
            }
        } else {
            node.bounds = m_nodes[static_cast<size_t>(i) + 1].bounds;
            node.bounds.expand(m_nodes[node.primitives_offset_or_second_child].bounds);
        }
    }
}

} // namespace nf::core
