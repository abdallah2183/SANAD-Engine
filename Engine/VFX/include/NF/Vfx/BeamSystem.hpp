#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <cstdint>
#include <vector>

namespace nf::vfx {

/// Vertex data generated for rendering camera-facing beam and electric arc quads.
struct BeamVertex {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec2 uv{0.0f, 0.0f};
    Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    f32 thickness = 0.1f;
};

/// A single segment in an arc (trunk or branch).
struct BeamSegment {
    Vec3 start{0.0f, 0.0f, 0.0f};
    Vec3 end{0.0f, 0.0f, 0.0f};
    f32 thickness = 0.1f;
    Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    u32 branch_level = 0u; // 0 = main trunk, 1+ = secondary branches
};

/// Authoring parameters for procedural electric arcs and beams.
struct LightningArcDescriptor {
    u32 subdivisions = 16u;               // Number of segments along main trunk
    f32 roughness = 0.5f;                 // Amplitude of lateral midpoint displacement
    f32 branch_chance = 0.25f;            // Probability [0, 1] of branching at each division
    f32 branch_length_decay = 0.5f;       // Length of branch relative to parent trunk segment
    u32 max_branch_depth = 2u;            // Maximum recursive branch generation depth
    f32 core_thickness = 0.05f;           // Inner bright core beam width
    f32 glow_thickness = 0.25f;           // Outer atmospheric/energy glow width
    Vec4 core_color{1.0f, 1.0f, 1.0f, 1.0f};       // Bright core color
    Vec4 glow_color{0.2f, 0.6f, 1.0f, 0.8f};       // Energy halo color
    u64 jitter_seed = 1337ull;            // Deterministic hash seed
};

/// Procedural lightning and electrical discharge generator.
class LightningArc {
public:
    LightningArc() = default;

    /// Builds a procedural electric arc between start and end.
    void generate(const Vec3& start, const Vec3& end, const LightningArcDescriptor& desc);

    /// Generates triangulated camera-facing quad strips for all segments (trunk and branches).
    void generate_mesh(std::vector<BeamVertex>& out_vertices,
                       std::vector<uint32_t>& out_indices,
                       const Vec3& camera_pos = Vec3(0.0f, 0.0f, -10.0f),
                       bool include_glow = true) const;

    const std::vector<BeamSegment>& segments() const { return m_segments; }
    size_t segment_count() const { return m_segments.size(); }
    size_t trunk_segment_count() const { return m_trunk_count; }

    const Vec3& start_point() const { return m_start; }
    const Vec3& end_point() const { return m_end; }

    void clear();

private:
    void subdivide(const Vec3& a, const Vec3& b, u32 level, u32 max_level,
                   f32 roughness, u32 branch_depth, const LightningArcDescriptor& desc, u64& seed);

    Vec3 m_start{0.0f, 0.0f, 0.0f};
    Vec3 m_end{0.0f, 0.0f, 0.0f};
    std::vector<BeamSegment> m_segments;
    size_t m_trunk_count = 0;
};

/// Target candidate for chain lightning propagation.
struct ChainTarget {
    u32 id = 0u;
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 priority = 1.0f;
};

/// Result step in a solved chain lightning jump.
struct ChainJump {
    u32 target_id = 0u;
    Vec3 from_position{0.0f, 0.0f, 0.0f};
    Vec3 to_position{0.0f, 0.0f, 0.0f};
    f32 damage_multiplier = 1.0f;
    f32 delay = 0.0f;
    u32 bounce_index = 0u;
};

/// Solves chaining paths across groups of targets (e.g. Chain Lightning spells).
class ChainLightningSolver {
public:
    static std::vector<ChainJump> solve(
        const Vec3& source_pos,
        const std::vector<ChainTarget>& candidates,
        u32 max_bounces = 4u,
        f32 max_jump_distance = 15.0f,
        f32 damage_decay_per_bounce = 0.8f,
        f32 jump_delay = 0.05f);
};

/// Continuous collimated laser / energy beam with pulse modulation.
class LaserBeam {
public:
    LaserBeam() = default;

    void set_endpoints(const Vec3& start, const Vec3& end);
    void set_direction(const Vec3& start, const Vec3& direction, f32 max_length);
    void set_hit_distance(f32 dist);

    void set_thickness(f32 core_thickness, f32 glow_thickness);
    void set_colors(const Vec4& core, const Vec4& glow);
    void set_pulse(f32 frequency, f32 min_intensity = 0.7f, f32 max_intensity = 1.0f);

    void update(f32 dt);

    Vec3 current_hit_point() const;
    f32 length() const;

    void generate_mesh(std::vector<BeamVertex>& out_vertices,
                       std::vector<uint32_t>& out_indices,
                       const Vec3& camera_pos = Vec3(0.0f, 0.0f, -10.0f)) const;

private:
    Vec3 m_start{0.0f, 0.0f, 0.0f};
    Vec3 m_direction{0.0f, 0.0f, 1.0f};
    f32 m_max_length = 50.0f;
    f32 m_hit_distance = 50.0f;
    f32 m_core_thickness = 0.04f;
    f32 m_glow_thickness = 0.16f;
    Vec4 m_core_color{1.0f, 0.3f, 0.3f, 1.0f}; // Red laser default
    Vec4 m_glow_color{1.0f, 0.1f, 0.1f, 0.6f};
    f32 m_pulse_freq = 0.0f;
    f32 m_min_intensity = 0.8f;
    f32 m_max_intensity = 1.0f;
    f32 m_time = 0.0f;
};

} // namespace nf::vfx
