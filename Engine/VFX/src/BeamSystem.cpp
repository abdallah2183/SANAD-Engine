#include <NF/Vfx/BeamSystem.hpp>
#include <algorithm>
#include <cmath>

namespace nf::vfx {

namespace {

inline f32 next_float(u64& state) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    u64 val = state * 0x2545F4914F6CDD1Dull;
    return static_cast<f32>(val & 0xFFFFFF) / 16777216.0f; // [0, 1)
}

inline f32 next_float_range(u64& state, f32 min_val, f32 max_val) {
    return min_val + next_float(state) * (max_val - min_val);
}

void get_orthonormal_basis(const Vec3& dir, Vec3& out_u, Vec3& out_v) {
    Vec3 norm_dir = dir;
    f32 len = norm_dir.length();
    if (len > 1e-5f) {
        norm_dir = norm_dir * (1.0f / len);
    } else {
        norm_dir = Vec3{0.0f, 1.0f, 0.0f};
    }

    Vec3 up = (std::abs(norm_dir.x) < 0.9f) ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
    out_u = norm_dir.cross(up);
    f32 u_len = out_u.length();
    if (u_len > 1e-5f) {
        out_u = out_u * (1.0f / u_len);
    } else {
        out_u = Vec3{0.0f, 0.0f, 1.0f};
    }
    out_v = norm_dir.cross(out_u);
}

void append_quad(std::vector<BeamVertex>& out_vertices,
                 std::vector<uint32_t>& out_indices,
                 const Vec3& a, const Vec3& b,
                 const Vec3& normal,
                 f32 half_thickness,
                 const Vec4& color,
                 f32 u_start, f32 u_end) {
    uint32_t base_idx = static_cast<uint32_t>(out_vertices.size());

    Vec3 offset = normal * half_thickness;

    BeamVertex v0{a - offset, Vec2{u_start, 0.0f}, color, half_thickness * 2.0f};
    BeamVertex v1{a + offset, Vec2{u_start, 1.0f}, color, half_thickness * 2.0f};
    BeamVertex v2{b + offset, Vec2{u_end, 1.0f}, color, half_thickness * 2.0f};
    BeamVertex v3{b - offset, Vec2{u_end, 0.0f}, color, half_thickness * 2.0f};

    out_vertices.push_back(v0);
    out_vertices.push_back(v1);
    out_vertices.push_back(v2);
    out_vertices.push_back(v3);

    out_indices.push_back(base_idx);
    out_indices.push_back(base_idx + 1);
    out_indices.push_back(base_idx + 2);

    out_indices.push_back(base_idx);
    out_indices.push_back(base_idx + 2);
    out_indices.push_back(base_idx + 3);
}

} // namespace

void LightningArc::generate(const Vec3& start, const Vec3& end, const LightningArcDescriptor& desc) {
    clear();
    m_start = start;
    m_end = end;

    u64 seed = desc.jitter_seed;
    u32 max_level = 1u;
    u32 divs = (desc.subdivisions > 1u) ? desc.subdivisions : 1u;
    while ((1u << max_level) < divs && max_level < 8u) {
        max_level++;
    }

    subdivide(start, end, 0u, max_level, desc.roughness, 0u, desc, seed);
}

void LightningArc::subdivide(const Vec3& a, const Vec3& b, u32 level, u32 max_level,
                            f32 roughness, u32 branch_depth, const LightningArcDescriptor& desc, u64& seed) {
    if (level >= max_level) {
        BeamSegment seg;
        seg.start = a;
        seg.end = b;
        seg.branch_level = branch_depth;
        seg.thickness = (branch_depth == 0) ? desc.core_thickness : (desc.core_thickness * 0.6f);
        seg.color = (branch_depth == 0) ? desc.core_color : desc.glow_color;
        m_segments.push_back(seg);
        if (branch_depth == 0) {
            m_trunk_count++;
        }
        return;
    }

    Vec3 delta = b - a;
    f32 len = delta.length();

    Vec3 u_axis, v_axis;
    get_orthonormal_basis(delta, u_axis, v_axis);

    f32 angle = next_float_range(seed, 0.0f, 6.2831853f);
    f32 disp_scale = roughness * (len * 0.35f);
    f32 disp_mag = next_float_range(seed, -disp_scale, disp_scale);

    Vec3 offset = (u_axis * std::cos(angle) + v_axis * std::sin(angle)) * disp_mag;
    Vec3 mid = (a + b) * 0.5f + offset;

    // Recurse first half and second half
    subdivide(a, mid, level + 1, max_level, roughness * 0.7f, branch_depth, desc, seed);
    subdivide(mid, b, level + 1, max_level, roughness * 0.7f, branch_depth, desc, seed);

    // Optional secondary branch off midpoint
    if (branch_depth < desc.max_branch_depth && next_float(seed) < desc.branch_chance) {
        f32 branch_len = len * desc.branch_length_decay;
        f32 b_angle = next_float_range(seed, 0.0f, 6.2831853f);
        Vec3 b_dir = (delta.normalized() * 0.6f + (u_axis * std::cos(b_angle) + v_axis * std::sin(b_angle)) * 0.8f).normalized();
        Vec3 branch_end = mid + b_dir * branch_len;

        u32 b_max_level = (max_level > 2u) ? (max_level - 2u) : 1u;
        subdivide(mid, branch_end, 0u, b_max_level, roughness * 0.6f, branch_depth + 1, desc, seed);
    }
}

void LightningArc::generate_mesh(std::vector<BeamVertex>& out_vertices,
                                std::vector<uint32_t>& out_indices,
                                const Vec3& camera_pos,
                                bool include_glow) const {
    out_vertices.clear();
    out_indices.clear();

    if (m_segments.empty()) {
        return;
    }

    size_t est_quads = m_segments.size() * (include_glow ? 2 : 1);
    out_vertices.reserve(est_quads * 4);
    out_indices.reserve(est_quads * 6);

    for (size_t i = 0; i < m_segments.size(); ++i) {
        const auto& seg = m_segments[i];
        Vec3 dir = seg.end - seg.start;
        f32 len = dir.length();
        if (len < 1e-5f) {
            continue;
        }

        Vec3 mid = (seg.start + seg.end) * 0.5f;
        Vec3 view = mid - camera_pos;
        Vec3 normal = dir.cross(view);
        f32 n_len = normal.length();
        if (n_len > 1e-5f) {
            normal = normal * (1.0f / n_len);
        } else {
            normal = Vec3{0.0f, 1.0f, 0.0f};
        }

        f32 u0 = static_cast<f32>(i) / static_cast<f32>(m_segments.size());
        f32 u1 = static_cast<f32>(i + 1) / static_cast<f32>(m_segments.size());

        // Emit core quad
        append_quad(out_vertices, out_indices, seg.start, seg.end, normal,
                    seg.thickness * 0.5f, seg.color, u0, u1);

        // Emit outer glow quad
        if (include_glow) {
            Vec4 halo_col = seg.color;
            halo_col.w *= 0.35f;
            append_quad(out_vertices, out_indices, seg.start, seg.end, normal,
                        seg.thickness * 2.5f, halo_col, u0, u1);
        }
    }
}

void LightningArc::clear() {
    m_segments.clear();
    m_trunk_count = 0;
}

std::vector<ChainJump> ChainLightningSolver::solve(
    const Vec3& source_pos,
    const std::vector<ChainTarget>& candidates,
    u32 max_bounces,
    f32 max_jump_distance,
    f32 damage_decay_per_bounce,
    f32 jump_delay) {

    std::vector<ChainJump> jumps;
    if (candidates.empty() || max_bounces == 0) {
        return jumps;
    }

    std::vector<bool> visited(candidates.size(), false);
    Vec3 current_pos = source_pos;
    f32 current_mult = 1.0f;
    f32 current_delay = 0.0f;

    for (u32 bounce = 0; bounce < max_bounces; ++bounce) {
        int best_idx = -1;
        f32 best_score = -1e9f;

        for (size_t i = 0; i < candidates.size(); ++i) {
            if (visited[i]) {
                continue;
            }

            f32 dist = (candidates[i].position - current_pos).length();
            if (dist <= max_jump_distance) {
                // Closer targets and higher priority score better
                f32 score = candidates[i].priority / (dist + 0.1f);
                if (score > best_score) {
                    best_score = score;
                    best_idx = static_cast<int>(i);
                }
            }
        }

        if (best_idx < 0) {
            break; // No more eligible targets within reach
        }

        visited[static_cast<size_t>(best_idx)] = true;
        const auto& target = candidates[static_cast<size_t>(best_idx)];

        ChainJump jump;
        jump.target_id = target.id;
        jump.from_position = current_pos;
        jump.to_position = target.position;
        jump.damage_multiplier = current_mult;
        jump.delay = current_delay;
        jump.bounce_index = bounce;

        jumps.push_back(jump);

        current_pos = target.position;
        current_mult *= damage_decay_per_bounce;
        current_delay += jump_delay;
    }

    return jumps;
}

void LaserBeam::set_endpoints(const Vec3& start, const Vec3& end) {
    m_start = start;
    Vec3 delta = end - start;
    f32 len = delta.length();
    if (len > 1e-4f) {
        m_direction = delta * (1.0f / len);
        m_max_length = len;
        m_hit_distance = len;
    } else {
        m_direction = Vec3{0.0f, 0.0f, 1.0f};
        m_max_length = 1.0f;
        m_hit_distance = 1.0f;
    }
}

void LaserBeam::set_direction(const Vec3& start, const Vec3& direction, f32 max_length) {
    m_start = start;
    f32 len = direction.length();
    m_direction = (len > 1e-4f) ? direction * (1.0f / len) : Vec3{0.0f, 0.0f, 1.0f};
    m_max_length = (max_length > 0.01f) ? max_length : 0.01f;
    m_hit_distance = m_max_length;
}

void LaserBeam::set_hit_distance(f32 dist) {
    m_hit_distance = std::clamp(dist, 0.0f, m_max_length);
}

void LaserBeam::set_thickness(f32 core_thickness, f32 glow_thickness) {
    m_core_thickness = (core_thickness > 0.001f) ? core_thickness : 0.001f;
    m_glow_thickness = (glow_thickness > m_core_thickness) ? glow_thickness : m_core_thickness;
}

void LaserBeam::set_colors(const Vec4& core, const Vec4& glow) {
    m_core_color = core;
    m_glow_color = glow;
}

void LaserBeam::set_pulse(f32 frequency, f32 min_intensity, f32 max_intensity) {
    m_pulse_freq = (frequency >= 0.0f) ? frequency : 0.0f;
    m_min_intensity = min_intensity;
    m_max_intensity = max_intensity;
}

void LaserBeam::update(f32 dt) {
    if (dt > 0.0f) {
        m_time += dt;
    }
}

Vec3 LaserBeam::current_hit_point() const {
    return m_start + m_direction * m_hit_distance;
}

f32 LaserBeam::length() const {
    return m_hit_distance;
}

void LaserBeam::generate_mesh(std::vector<BeamVertex>& out_vertices,
                              std::vector<uint32_t>& out_indices,
                              const Vec3& camera_pos) const {
    out_vertices.clear();
    out_indices.clear();

    if (m_hit_distance < 1e-4f) {
        return;
    }

    Vec3 hit_pt = current_hit_point();
    Vec3 mid = (m_start + hit_pt) * 0.5f;
    Vec3 view = mid - camera_pos;
    Vec3 normal = m_direction.cross(view);
    f32 n_len = normal.length();
    if (n_len > 1e-5f) {
        normal = normal * (1.0f / n_len);
    } else {
        normal = Vec3{0.0f, 1.0f, 0.0f};
    }

    f32 pulse = 1.0f;
    if (m_pulse_freq > 0.0f) {
        f32 wave = 0.5f * (1.0f + std::sin(m_time * m_pulse_freq * 6.2831853f));
        pulse = m_min_intensity + wave * (m_max_intensity - m_min_intensity);
    }

    Vec4 core_c = m_core_color * pulse;
    Vec4 glow_c = m_glow_color * pulse;

    // Core quad
    append_quad(out_vertices, out_indices, m_start, hit_pt, normal,
                m_core_thickness * 0.5f, core_c, 0.0f, 1.0f);

    // Glow quad
    append_quad(out_vertices, out_indices, m_start, hit_pt, normal,
                m_glow_thickness * 0.5f, glow_c, 0.0f, 1.0f);
}

} // namespace nf::vfx
