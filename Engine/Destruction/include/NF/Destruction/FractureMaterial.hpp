#pragma once

// NF/Destruction/FractureMaterial.hpp — named material presets for breakables.
//
// DestructibleComponent exposes the knobs an object's shards actually obey —
// bond strength, shard mass, shard lifetime — but nothing names them. A caller
// that wants a crate of glass should not have to re-derive "weak bonds, light
// shards, short life" from first principles each time, and two callers who do
// should agree, because a pile of debris mixes in one world and its budget is
// shared.
//
// The presets are data, not an enum the engine switches on: strength_scale
// multiplies every bond strength at apply_damage time (so the same fracture
// asset serves glass and stone — the *asset* is the shape of the break, the
// material is how hard it resists), density sets shard mass, and
// shard_lifetime overrides the world budget per shard.
//
// No preset is a substitute for tuning a specific object: these are starting
// points whose ratios are chosen so a glass object comes apart under an
// impulse a steel one shrugs off in the same scene.

#include <NF/Core/Types.hpp>

#include <cstddef>
#include <string_view>

namespace nf::destruction {

struct DestructibleComponent;

/// How one material breaks. Every field lands on a DestructibleComponent that
/// apply_damage / emit_body / tick already read, so a preset is not a parallel
/// set of semantics — it is a named bundle of the existing knobs.
struct FractureMaterial {
    /// Lookup key, matched case-insensitively by find_material().
    const char* name;
    /// Multiplies every bond strength. < 1 = brittle (glass), > 1 = stubborn
    /// (steel). 0 is rejected in find_material so a typo cannot make an object
    /// weightless-fragile.
    f32 strength_scale;
    /// Shard mass = chunk volume * this. Stone sinks a pile, wood scatters.
    f32 density;
    /// Seconds a shard lives, 0 = the world budget's lifetime.
    f32 shard_lifetime;
};

/// The shipped materials, ordered by strength: glass, wood, stone, steel.
/// Exposed (not an implementation detail) so a test can walk the table and a
/// tool can list the options for an author.
inline constexpr FractureMaterial kFractureMaterials[] = {
    // Shatters at a touch, light shards that fade fast — the pile is busy but
    // never permanent.
    {"glass", 0.20f, 0.4f, 4.0f},
    // The reference crate: comes apart under a real slam, shards linger at the
    // budget's pace.
    {"wood", 1.00f, 1.0f, 0.0f},
    // Takes a beating; heavy pieces that stay where they land.
    {"stone", 3.00f, 3.0f, 0.0f},
    // Shrugs off small arms; when it does break the chunks are dense enough to
    // matter in the budget.
    {"steel", 8.00f, 8.0f, 0.0f},
};

/// The material named `name`, or nullptr when there is none (including the
/// empty name). Case-insensitive; "Wood" and "wood" are the same material so a
/// scene author's casing does not silently opt out of breaking.
const FractureMaterial* find_material(std::string_view name);

/// Writes `material`'s knobs onto `component`. A null material is a no-op
/// rather than a reset, so an unknown name leaves the object as it was — the
/// caller decides whether to warn.
void apply_material(DestructibleComponent& component, const FractureMaterial& material);

} // namespace nf::destruction
