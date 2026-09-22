// NF/Water/Water.cpp — water surface (design doc 62: ocean, lakes, rivers).

#include <NF/Water/Water.hpp>

#include <NF/Rendering/StaticMesh.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace nf::water {

namespace {

/// A `Wave` with everything the field needs per evaluation, computed once
/// instead of once per vertex: wavenumber from the wavelength, phase rate from
/// the speed, and the unit direction (a caller's `direction` need not be
/// normalised and normalising it per vertex would be 64x the work).
struct WaveParams {
    f32 k = 0.0f;      // wavenumber, 2*pi/wavelength
    f32 omega = 0.0f;  // phase rate, k*speed (per second)
    f32 sharp = 0.0f;  // pinch per unit phase: steepness*amplitude*k
    f32 horiz = 0.0f;  // horizontal displacement magnitude: steepness*amplitude
    f32 amp = 0.0f;    // vertical amplitude
    Vec2 dir;          // unit
};

/// Drops the waves that cannot contribute: a zero amplitude, a non-positive
/// wavelength, or a zero direction is a flat surface, and a flat surface is
/// what `build_water_mesh` returns nullptr for rather than a mesh that lies
/// about being water.
std::vector<WaveParams> prepare(const std::vector<Wave>& waves) {
    std::vector<WaveParams> out;
    out.reserve(waves.size());
    for (const Wave& w : waves) {
        const f32 len = w.direction.length();
        if (!(w.amplitude > 0.0f) || !(w.wavelength > 0.0f) || !(len > 0.0f)) continue;
        WaveParams p;
        p.k = TWO_PI / w.wavelength;
        p.omega = p.k * w.speed;
        // `steepness` is clamped rather than trusted: above 1 the wave loops
        // over itself and the surface stops being a function of position, so
        // the field (and the mesh built from it) would self-intersect.
        p.horiz = clamp(w.steepness, 0.0f, 1.0f) * w.amplitude;
        p.sharp = p.horiz * p.k;
        p.amp = w.amplitude;
        p.dir = w.direction / len;
        out.push_back(p);
    }
    return out;
}

/// Everything the surface does at one point, in one pass — height, horizontal
/// displacement, the two Jacobian columns the normal needs, and the pinch the
/// foam reads. The Gerstner sum is
///   P(x,z) = (x,z) + Σ horiz·dir·cos θ,   y = Σ amp·sin θ,   θ = k·(dir·p) − ωt
/// so the partials are exact closed forms of the same sinusoids, which is why
/// a vertex normal and a CPU query here agree to the last bit at the same t.
struct FieldSample {
    f32 height = 0.0f;   // above the rest level
    Vec2 disp;           // horizontal displacement of the rest position
    f32 pinch = 0.0f;    // 1 − det of the horizontal map: how crested this is
    f32 total_sharp = 0.0f;  // Σ sharp, the pinch this wave set can produce
    f32 dydx = 0.0f;     // ∂y/∂x
    f32 dydz = 0.0f;     // ∂y/∂z
    f32 dxx = 1.0f;      // ∂X/∂x, starts at identity
    f32 dxz = 0.0f;      // ∂X/∂z == ∂Z/∂x, the map is symmetric
    f32 dzz = 1.0f;      // ∂Z/∂z
};

FieldSample sample_field(f32 x, f32 z, f32 t, const std::vector<WaveParams>& waves) {
    FieldSample s;
    for (const WaveParams& w : waves) {
        const f32 phase = w.k * (w.dir.x * x + w.dir.y * z) - w.omega * t;
        const f32 c = static_cast<f32>(std::cos(static_cast<double>(phase)));
        const f32 sn = static_cast<f32>(std::sin(static_cast<double>(phase)));
        s.height += w.amp * sn;
        s.disp.x += w.horiz * w.dir.x * c;
        s.disp.y += w.horiz * w.dir.y * c;
        s.total_sharp += w.sharp;
        s.dydx += w.amp * w.k * w.dir.x * c;
        s.dydz += w.amp * w.k * w.dir.y * c;
        s.dxx -= w.sharp * w.dir.x * w.dir.x * sn;
        s.dzz -= w.sharp * w.dir.y * w.dir.y * sn;
        s.dxz -= w.sharp * w.dir.x * w.dir.y * sn;
    }
    // det [[dxx dxz],[dxz dzz]]: how much the horizontal map compresses. A
    // single axis-aligned wave gives 1 − sharp·sin θ, so this is the surface's
    // own pinching, and the −1 end is the trough, the +1 end the crest.
    s.pinch = 1.0f - (s.dxx * s.dzz - s.dxz * s.dxz);
    return s;
}

/// The crest in [0, 1]: pinch normalised by everything this wave set can
/// pinch, so 0.6 means the same kind of crest for a 0.2u ripple and a 3u
/// swell. Crossing crests genuinely pinch more than any one wave, and the
/// clamp keeps the knob's meaning at the corners of the space.
f32 crest01(const FieldSample& s) {
    if (!(s.total_sharp > 0.0f)) return 0.5f;
    const f32 ratio = clamp(s.pinch / s.total_sharp, -1.0f, 1.0f);
    return (ratio + 1.0f) * 0.5f;
}

/// Unit normal of the displaced surface, from the exact tangents above. Order
/// matters: at a flat point ∂P/∂z × ∂P/∂x is (0,1,0), so the same cross gives
/// up on every grid and there is no per-vertex sign fix-up to get wrong.
Vec3 field_normal(const FieldSample& s) {
    const Vec3 tangent{s.dxx, s.dydx, s.dxz};   // ∂P/∂x
    const Vec3 bitangent{s.dxz, s.dydz, s.dzz}; // ∂P/∂z
    const Vec3 n = bitangent.cross(tangent);
    if (n.length_sq() < EPSILON * EPSILON) return Vec3{0.0f, 1.0f, 0.0f};
    return n.normalized();
}

/// SplitMix-style finaliser: one input word to one well-distributed output word.
u64 mix64(u64 h) {
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebull;
    h ^= h >> 31;
    return h;
}

/// A [0, 1) value that depends only on the integer cell, the salt, and the
/// seed — the one random source the foam has. No RNG state and no float in the
/// hash, so the same crest in a different patch, a different process, or a
/// replay is the same foam. `salt` picks which random number the cell yields.
f32 hash01(i64 cx, i64 cz, u64 salt, u64 seed) {
    u64 h = seed * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<u64>(cx) * 0xBF58476D1CE4E5B9ull;
    h ^= static_cast<u64>(cz) * 0xC4CEB9FE1A85EC53ull;
    h ^= salt * 0xCBF29CE484222325ull;
    return static_cast<f32>(mix64(h) >> 40u) / static_cast<f32>(0x1000000u);
}

// Which random draw a hash is used for.
enum HashSalt : u64 {
    kSaltFoam = 1u,
};

/// World units of one foam patch. Fixed rather than derived from a wavelength
/// so the texture does not tighten on a short sea until it aliases.
constexpr f32 kFoamCell = 2.0f;

/// Foam from a sampled field, at crest and at shore. Split out so a mesh
/// vertex and a standalone query answer the same value — the mesh samples the
/// field once and reuses it instead of re-deriving the wave set per channel.
f32 foam_from_field(const FieldSample& s, f32 x, f32 z, const WaterOptions& options,
                    const ShoreSampler& shore) {
    // Crest foam: a band above the steepness threshold, `softness` wide.
    const f32 crest = crest01(s);
    const f32 band = (options.foam_softness > 0.0f) ? options.foam_softness : 1e-4f;
    f32 foam = clamp((crest - options.foam_steepness) / band, 0.0f, 1.0f);

    // Shore surf: a wave over a floor at the break depth throws more than one
    // in open water. It is a tent over the break, not a ramp — shallower has no
    // water to throw, deeper passes over as swell, and a floor far below the
    // surface is the open ocean, which foams at its crests and not its shore.
    if (shore) {
        const f32 depth = shore(x, z);
        const f32 break_depth =
            (options.shore_break_depth > 0.0f) ? options.shore_break_depth : 1e-4f;
        const f32 surf = clamp(1.0f - std::abs(depth - break_depth) / break_depth,
                               0.0f, 1.0f);
        foam = std::max(foam, surf);
    }

    // The seeded texture: a per-cell multiplier whose mean over the lattice is
    // 1, so the foam is patchy — crest, gap, crest — while the amount a crest
    // holds is a function of the waves, not of the seed.
    const i64 cx = static_cast<i64>(std::floor(x / kFoamCell));
    const i64 cz = static_cast<i64>(std::floor(z / kFoamCell));
    foam *= 0.6f + 0.8f * hash01(cx, cz, kSaltFoam, options.seed);
    return clamp(foam * options.foam_amount, 0.0f, 1.0f);
}

/// Cheap reflection from a sampled field (see `water_reflection`).
f32 reflection_from_field(const FieldSample& s, f32 viewer_elevation,
                          const WaterOptions& options) {
    const Vec3 n = field_normal(s);
    // A viewer `elevation` radians above the horizon sees a flat facet at
    // (90° − elevation) from its normal, so sin(e) is the cosine of incidence:
    // a low viewer is at grazing, where water is a mirror.
    const f32 e = clamp(viewer_elevation, 0.0f, HALF_PI);
    const f32 cos_inc = static_cast<f32>(std::sin(static_cast<double>(e)));
    const f32 f0 = clamp(options.fresnel_normal, 0.0f, 1.0f);
    f32 r = f0 + (1.0f - f0) *
                     static_cast<f32>(std::pow(1.0 - static_cast<double>(cos_inc), 5.0));

    // A tilted facet is that much closer to grazing than a flat one would be,
    // and the tilt is the only part of the normal a distant viewer resolves —
    // the horizontal direction of the tilt is below this model's resolution, so
    // it raises the estimate rather than signing it.
    const f32 tilt_sq = clamp(n.x * n.x + n.z * n.z, 0.0f, 1.0f);
    r += std::sqrt(tilt_sq) * (1.0f - r);
    return clamp(r * options.reflection_strength, 0.0f, 1.0f);
}

} // namespace

ShoreSampler constant_shore(f32 depth) {
    return [depth](f32, f32) { return depth; };
}

f32 water_height(f32 x, f32 z, const std::vector<Wave>& waves,
                 const WaterOptions& options) {
    const std::vector<WaveParams> params = prepare(waves);
    const FieldSample s = sample_field(x, z, options.time, params);
    return options.level + s.height;
}

Vec2 water_displacement(f32 x, f32 z, const std::vector<Wave>& waves,
                        const WaterOptions& options) {
    const std::vector<WaveParams> params = prepare(waves);
    const FieldSample s = sample_field(x, z, options.time, params);
    return s.disp;
}

Vec3 water_normal(f32 x, f32 z, const std::vector<Wave>& waves,
                  const WaterOptions& options) {
    const std::vector<WaveParams> params = prepare(waves);
    const FieldSample s = sample_field(x, z, options.time, params);
    return field_normal(s);
}

f32 water_foam(f32 x, f32 z, const std::vector<Wave>& waves,
               const WaterOptions& options, const ShoreSampler& shore) {
    const std::vector<WaveParams> params = prepare(waves);
    if (params.empty() || !(options.foam_amount > 0.0f)) return 0.0f;
    const FieldSample s = sample_field(x, z, options.time, params);
    return foam_from_field(s, x, z, options, shore);
}

f32 water_reflection(f32 x, f32 z, f32 viewer_elevation,
                     const std::vector<Wave>& waves, const WaterOptions& options) {
    const std::vector<WaveParams> params = prepare(waves);
    const FieldSample s = sample_field(x, z, options.time, params);
    return reflection_from_field(s, viewer_elevation, options);
}

f32 water_surge(const std::vector<Wave>& waves, const WaterOptions& options) {
    const std::vector<WaveParams> params = prepare(waves);
    if (params.empty() || !(options.size > 0.0f)) return 0.0f;
    // A fixed grid over the patch: the fraction above rest is a property of
    // the wave set, not of the mesh, so this does not read `resolution`.
    constexpr u32 kSurgeGrid = 8u;
    const f32 half = options.size * 0.5f;
    const f32 step = options.size / static_cast<f32>(kSurgeGrid - 1u);
    u32 above = 0u;
    u32 total = 0u;
    for (u32 j = 0u; j < kSurgeGrid; ++j) {
        for (u32 i = 0u; i < kSurgeGrid; ++i) {
            const f32 x = -half + static_cast<f32>(i) * step;
            const f32 z = -half + static_cast<f32>(j) * step;
            const FieldSample s = sample_field(x, z, options.time, params);
            if (s.height > 0.0f) ++above;
            ++total;
        }
    }
    return static_cast<f32>(above) / static_cast<f32>(total);
}

std::unique_ptr<nf::rendering::StaticMesh>
build_water_mesh(const std::vector<Wave>& waves,
                 const WaterOptions& options,
                 const ShoreSampler& shore,
                 WaterMeshStats* stats) {
    if (options.resolution < 2u || !(options.size > 0.0f) || waves.empty()) return nullptr;
    const std::vector<WaveParams> params = prepare(waves);
    if (params.empty()) return nullptr;

    // The viewer a baked grid shades for: 45° above the horizon, where the
    // cheap Fresnel is halfway between the mirror it becomes at grazing and
    // the diffuse it becomes from straight overhead.
    const f32 elevation = HALF_PI * 0.25f;

    auto mesh = std::make_unique<nf::rendering::StaticMesh>("water");
    nf::rendering::MeshLOD& lod = mesh->lod(0);
    const u32 res = std::min(options.resolution, 1024u); // absurd-input guard
    const f32 half = options.size * 0.5f;
    const f32 step = options.size / static_cast<f32>(res - 1u);
    lod.vertices.reserve(static_cast<usize>(res) * res);
    lod.indices.reserve(static_cast<usize>(res - 1u) * (res - 1u) * 6u);

    for (u32 j = 0u; j < res; ++j) {
        for (u32 i = 0u; i < res; ++i) {
            const f32 x = -half + static_cast<f32>(i) * step;
            const f32 z = -half + static_cast<f32>(j) * step;
            const FieldSample s = sample_field(x, z, options.time, params);
            const f32 foam = foam_from_field(s, x, z, options, shore);
            const f32 reflection = reflection_from_field(s, elevation, options);

            nf::rendering::Vertex v{};
            // The visual vertex sits where the surface point *went*: the rest
            // position plus the horizontal displacement, and the height on top.
            // `water_height` alone is the physics query and the two are
            // deliberately not the same point.
            v.position[0] = x + s.disp.x;
            v.position[1] = options.level + s.height;
            v.position[2] = z + s.disp.y;
            const Vec3 n = field_normal(s);
            v.normal[0] = n.x;
            v.normal[1] = n.y;
            v.normal[2] = n.z;
            v.tangent[0] = 1.0f;
            v.tangent[1] = 0.0f;
            v.tangent[2] = 0.0f;
            v.tangent[3] = 1.0f;
            // World coordinates, not grid fractions: a tiling normal map stays
            // put in the world while the mesh's own resolution changes.
            v.uv0[0] = x;
            v.uv0[1] = z;
            v.uv1[0] = foam;
            v.uv1[1] = reflection;
            lod.vertices.push_back(v);
            if (stats != nullptr) {
                stats->max_foam = std::max(stats->max_foam, foam);
                stats->max_height = std::max(stats->max_height, s.height);
            }
        }
    }

    for (u32 j = 0u; j + 1u < res; ++j) {
        for (u32 i = 0u; i + 1u < res; ++i) {
            const u32 i0 = j * res + i;
            const u32 i1 = i0 + 1u;
            const u32 i2 = i0 + res;
            const u32 i3 = i2 + 1u;
            // Counter-clockwise from above (+Y), the same winding as the
            // terrain grid, so one back-face cull state serves both.
            lod.indices.push_back(i0);
            lod.indices.push_back(i2);
            lod.indices.push_back(i1);
            lod.indices.push_back(i1);
            lod.indices.push_back(i2);
            lod.indices.push_back(i3);
        }
    }

    nf::rendering::SubMesh sm;
    sm.index_offset = 0u;
    sm.index_count = static_cast<u32>(lod.indices.size());
    sm.vertex_offset = 0u;
    sm.vertex_count = static_cast<u32>(lod.vertices.size());
    sm.material_slot = 0u;
    lod.submeshes.push_back(sm);
    lod.material_slots.push_back(nf::rendering::MaterialSlot{"water"});
    nf::rendering::StaticMesh::compute_lod_bounds(lod);

    if (stats != nullptr) {
        stats->vertices = static_cast<u32>(lod.vertices.size());
        stats->indices = static_cast<u32>(lod.indices.size());
    }
    return mesh;
}

} // namespace nf::water
