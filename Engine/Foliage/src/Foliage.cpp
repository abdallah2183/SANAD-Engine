// NF/Foliage/Foliage.cpp — deterministic foliage scatter (design doc 61).

#include <NF/Foliage/Foliage.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace nf::foliage {

namespace {

/// SplitMix-style finaliser: one input word to one well-distributed output word.
u64 mix64(u64 h) {
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebull;
    h ^= h >> 31;
    return h;
}

/// A [0, 1) value that depends on the cell, the species, and the salt — the one
/// random source the scatter has. No RNG state, no floats in the hash, so a
/// replay is bit-identical and the same cell in a different rect answers the
/// same. `salt` picks which random number the cell yields, so the jitter, the
/// acceptance, the scale, the yaw, and the phase are five independent draws
/// from one well instead of five correlated views of one value.
f32 hash01(i64 cx, i64 cz, u32 type_index, u64 salt, u64 seed) {
    u64 h = seed * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<u64>(cx) * 0xBF58476D1CE4E5B9ull;
    h ^= static_cast<u64>(cz) * 0xC4CEB9FE1A85EC53ull;
    h ^= static_cast<u64>(type_index) * 0x94D049BB133111EBull;
    h ^= salt * 0xCBF29CE484222325ull;
    // Top 24 bits: the low bits of an xorshift-style mix are the weak ones, and
    // 24 bits is finer than the scatter's accept/reject resolution needs.
    return static_cast<f32>(mix64(h) >> 40u) / static_cast<f32>(0x1000000u);
}

// Which random draw a hash is used for.
enum HashSalt : u64 {
    kSaltJitterX = 1u,
    kSaltJitterZ = 2u,
    kSaltAccept = 3u,
    kSaltScale = 4u,
    kSaltYaw = 5u,
    kSaltPhase = 6u,
    kSaltFade = 7u,
};

/// The instance's LOD bucket from its distance to the reference point. Beyond
/// the last distance the bucket is still the last one — the fade in the caller
/// decides whether it survives, so a type whose outermost ring is thinned still
/// reports the bucket its mesh LOD would have drawn.
u32 lod_bucket(f32 dist, const f32 (&lod_distances)[4]) {
    for (u32 i = 0u; i < 4u; ++i) {
        if (dist < lod_distances[i]) return i;
    }
    return 3u;
}

/// Odds an instance in the ring past the last LOD distance survives at `dist`:
/// one at the ring's inner edge, falling linearly to none at `cull_radius`. A
/// hard cut would delete an entire distant field at one radius; the fade takes
/// a fraction of it per unit of distance, so the edge walks in instead of
/// blinking out.
f32 fade_odds(f32 dist, f32 last, f32 cull_radius) {
    if (cull_radius <= last) return 0.0f;
    const f32 span = cull_radius - last;
    const f32 t = (dist - last) / span;
    return std::clamp(1.0f - t, 0.0f, 1.0f);
}

/// True when the candidate falls inside every gate the species sets.
bool passes_gates(const FoliageType& type, const GroundSample& ground) {
    if (ground.height < type.min_height || ground.height > type.max_height) return false;
    if (ground.slope < type.min_slope || ground.slope > type.max_slope) return false;
    return true;
}

} // namespace

Vec3 wind_displacement(const FoliageInstance& instance,
                       const FoliageType& type,
                       const WindState& wind) {
    // A rigid species (wind_amount <= 0) has no displacement, and neither does
    // a zero gust — checked here rather than by the caller, so every query
    // site agrees about what "no wind" means.
    if (!(type.wind_amount > 0.0f) || !(wind.gust > 0.0f)) return Vec3{0.0f, 0.0f, 0.0f};
    Vec3 dir = wind.direction;
    const f32 len = dir.length();
    if (len <= EPSILON) return Vec3{0.0f, 0.0f, 0.0f};
    dir = dir / len;
    const f32 phase = instance.wind_phase * TWO_PI;
    // Two sinusoids of incommensurate frequency: the sum never quite repeats,
    // so the sway reads as organic while staying a pure function of time.
    const f32 wave = std::sin(wind.time * type.wind_speed * TWO_PI + phase) * 0.7f +
                     std::sin(wind.time * type.wind_speed * 1.9f * TWO_PI + phase * 1.37f) * 0.3f;
    // The envelope builds and ebbs slowly across the whole field, so a gust
    // crosses it rather than every blade oscillating at fixed amplitude.
    const f32 envelope = wind.gust * (0.75f + 0.25f * std::sin(wind.time * 0.35f));
    const f32 amount = type.wind_amount * instance.scale * wave * envelope;
    return dir * amount;
}

std::vector<FoliageInstance> scatter_foliage(const std::vector<FoliageType>& types,
                                             const GroundSampler& ground,
                                             const DensityMask& mask,
                                             const f32 min_x, const f32 min_z,
                                             const f32 max_x, const f32 max_z,
                                             const ScatterOptions& opts,
                                             ScatterStats* stats) {
    std::vector<FoliageInstance> out;
    if (stats != nullptr) *stats = ScatterStats{};
    // A rect with no area has no cells, and an empty ground is a wiring bug the
    // first call would throw on — reported as no instances, not as a crash.
    if (max_x <= min_x || max_z <= min_z) return out;
    if (!ground) return out;
    if (types.empty()) return out;

    const f32 fade_start = std::max({opts.lod_distances[0u], opts.lod_distances[1u],
                                     opts.lod_distances[2u], opts.lod_distances[3u]});
    const bool have_cull = opts.cull_radius > 0.0f;

    for (u32 type_index = 0u; type_index < static_cast<u32>(types.size()); ++type_index) {
        const FoliageType& type = types[type_index];
        // Density is the only knob that says "none of this here": zero or
        // negative, or a degenerate scale range, emits nothing rather than a
        // divide-by-zero cell grid.
        if (!(type.density > 0.0f)) continue;
        if (!(type.max_scale >= type.min_scale)) continue;
        if (!(type.weight > 0.0f)) continue;

        // One candidate per cell of side 1/sqrt(density), so the expected count
        // per cell is ~1 and the density means what it says at any rect size.
        const f64 cell = 1.0 / std::sqrt(static_cast<f64>(type.density));
        const f64 cell_f = static_cast<f64>(cell);
        // Cell origins are integer multiples of `cell`, so which cells a rect
        // covers is a property of the rect alone and a shared cell answers
        // identically in any rect that contains it — the property streaming
        // (design doc 60) needs.
        const i64 x0 = static_cast<i64>(std::floor(static_cast<f64>(min_x) / cell_f));
        const i64 z0 = static_cast<i64>(std::floor(static_cast<f64>(min_z) / cell_f));
        for (i64 cx = x0; static_cast<f64>(cx) * cell_f < static_cast<f64>(max_x); ++cx) {
            for (i64 cz = z0; static_cast<f64>(cz) * cell_f < static_cast<f64>(max_z); ++cz) {
                if (stats != nullptr) ++stats->candidates;

                const f64 ox = static_cast<f64>(cx) * cell_f;
                const f64 oz = static_cast<f64>(cz) * cell_f;
                // The jitter keeps the candidate inside its own cell: a jittered
                // grid is the cheap stand-in for a Poisson disk, and staying in
                // the cell is what makes the count predictable.
                const f64 jx = static_cast<f64>(hash01(cx, cz, type_index, kSaltJitterX, opts.seed));
                const f64 jz = static_cast<f64>(hash01(cx, cz, type_index, kSaltJitterZ, opts.seed));
                const f32 px = static_cast<f32>(ox + jx * cell_f);
                const f32 pz = static_cast<f32>(oz + jz * cell_f);

                const GroundSample g = ground(px, pz);
                if (!passes_gates(type, g)) {
                    if (stats != nullptr) ++stats->rejected;
                    continue;
                }

                // Weight, then the mask: the mask is the artist's brush and the
                // weight is the species' share of the ground it could take, so
                // the two multiply — a masked-down field at weight 1 is sparse
                // the same way an unmasked one at weight 0.2 is.
                const f32 odds = type.weight *
                                 (mask ? std::clamp(mask(px, pz), 0.0f, 1.0f) : 1.0f);
                if (!(odds > 0.0f)) {
                    if (stats != nullptr) ++stats->rejected;
                    continue;
                }
                if (hash01(cx, cz, type_index, kSaltAccept, opts.seed) >= odds) {
                    if (stats != nullptr) ++stats->rejected;
                    continue;
                }

                const f32 dx = static_cast<f32>(px - opts.center_x);
                const f32 dz = static_cast<f32>(pz - opts.center_z);
                const f32 dist = std::sqrt(dx * dx + dz * dz);
                // The cull radius is the hard edge and is checked first, so a
                // cull_radius inside the last LOD ring still cuts there — the
                // fade is what happens *between* the last ring and the radius,
                // and an empty or inverted interval simply means no fade.
                if (have_cull && dist >= opts.cull_radius) {
                    if (stats != nullptr) ++stats->culled;
                    continue;
                }
                if (have_cull && dist >= fade_start) {
                    // The ring past the last LOD: keep a falling fraction. A
                    // hash draw, not a deterministic stride, so the survivors
                    // stay scattered rather than becoming a regular lattice as
                    // the field thins. Its own salt: reusing the accept draw
                    // would tie thinning to acceptance — a field admitted on a
                    // low odds already drew a low number, so it would keep its
                    // distant instances at a higher rate than a dense one.
                    const f32 keep = fade_odds(dist, fade_start, opts.cull_radius);
                    if (hash01(cx, cz, type_index, kSaltFade, opts.seed) >= keep) {
                        if (stats != nullptr) ++stats->culled;
                        continue;
                    }
                }

                FoliageInstance inst{};
                inst.position[0u] = px;
                inst.position[1u] = g.height; // on the ground it passed
                inst.position[2u] = pz;
                const f32 st = hash01(cx, cz, type_index, kSaltScale, opts.seed);
                inst.scale = type.min_scale + st * (type.max_scale - type.min_scale);
                const f32 yt = hash01(cx, cz, type_index, kSaltYaw, opts.seed) - 0.5f;
                inst.yaw = yt * type.yaw_spread;
                inst.wind_phase = hash01(cx, cz, type_index, kSaltPhase, opts.seed);
                inst.type_index = type_index;
                inst.lod = lod_bucket(dist, opts.lod_distances);
                out.push_back(inst);
                if (stats != nullptr) ++stats->placed;
            }
        }
    }

    // The instance order is the species order, then the cell order — the order
    // the loops produced, which is what makes a replay identical. Sorting by
    // distance would read better for a renderer's front-to-back pass, but it
    // would make the buffer depend on the centre, and the centre moves.
    return out;
}

} // namespace nf::foliage
