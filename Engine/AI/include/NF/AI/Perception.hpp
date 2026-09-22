#pragma once

// NF/AI/Perception.hpp — sight, hearing, touch and damage sensing (design doc
// Section 45, "AI Core: perception").
//
// Behaviour trees decide from a Blackboard; this module is what puts anything
// *in* it. A guard's "is the player visible" is a stimulus here long before it
// is a BT condition, and the distinction matters: perception is stateful and
// time-integrated. A target half-seen for half a second is not "seen", and a
// target glimpsed then lost stays known for a while — the last known position
// is what a search behaviour walks to.
//
// Two halves
// ----------
// PerceptionScene is what the world *is*: the actors that can be perceived and
// the transient events (gunshots, hits) they caused. It is shared — twenty
// NPCs read one scene rather than each maintaining its own copy of the cast.
//
// Perception is one NPC's senses. It owns no targets and no occlusion:
// line-of-sight arrives through an injected query so this module stays pure
// CPU (NavGrid::has_line_of_sight is one valid implementation; a physics
// raycast another) and a test needs no Vulkan, no physics, no clock.
//
// Determinism
// -----------
// The engine replays AI bit-identically (NavGrid documents the same contract),
// so every choice that could vary between two runs is pinned here:
//   - targets are visited in scene order, which is insertion order — never
//     pointer or hash order;
//   - PerceivedTarget entries are kept in first-seen order and are never
//     sorted in place, so the whole run emits the same sequence;
//   - "most threatening" uses a strict `>`, so ties go to the earlier target;
//   - sense evaluation order is Sight, Sound, Touch, Damage, and that order is
//     what resolves a tie for last_sense;
//   - there is no RNG and no wall clock. `dt` is the only time input, and a
//     negative dt is clamped to zero rather than running time backwards.
//
// Units: distances are world metres, angles are radians, dt is seconds.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <array>
#include <functional>
#include <vector>

namespace nf::ai {

/// What a sensor can pick up. Kept as an enum rather than flags because a
/// target can be perceived by several senses at once but "which sense" is a
/// single answer the game asks for — heard-but-not-seen means "investigate",
/// seen means "engage".
enum class StimulusKind : u8 {
    Sight  = 0,
    Sound  = 1,
    Touch  = 2,
    Damage = 3,
};

inline constexpr u32 kStimulusKindCount = 4;

/// One thing a sensor can notice. Registered on the scene, not on a sensor, so
/// the cast is authored once. `id` comes from the scene.
struct PerceptionTarget {
    u32 id = 0;
    Vec3 position{};

    /// Body radius, added to the sensor's own for range tests. A target is
    /// visible a bit before its centre clears the horizon and touchable before
    /// the two centres coincide.
    float radius = 0.5f;

    /// Multiplier on sight strength: camouflage, darkness, a small target.
    /// Slows detection rather than preventing it — a staring guard still
    /// accumulates it, which is the behaviour a player expects from a
    /// partially-concealed flank.
    float visibility = 1.0f;

    /// Continuous noise the target emits per second (0 = silent). Footsteps
    /// live here. A discrete event — a gunshot, a thrown bottle — is a Stimulus
    /// instead, because it has a moment and a loudness, not a rate.
    float noise = 0.0f;

    /// Whatever the game filters on (friend/foe). Uninterpreted here; the BT
    /// reads it. Deliberately not a bool so a game can have more than two.
    u32 team = 0;
};

/// A transient event the scene noticed: a gunshot, an explosion, a hit landed.
/// Unlike a target's fields this has a lifetime — `remaining` counts down while
/// the scene ages, and once it expires the event is gone for every sensor.
struct Stimulus {
    StimulusKind kind = StimulusKind::Sound;
    Vec3 position{};
    /// Loudness / brightness / damage amount. Scaled by the matching sense's
    /// distance falloff, so a distant shout is weaker than a near one.
    float strength = 1.0f;
    /// PerceptionTarget that caused it, so a sensor can blame the right actor
    /// for a noise it never saw. 0 means "unattributed": such a stimulus ages
    /// out of the scene like any other but no sensor records it. Pointing one
    /// at an actor the scene no longer lists is fine — the sensor still
    /// records the position, which is the point of an unattributable noise.
    u32 source_id = 0;
    /// Seconds left. The scene owns this; sensors never mutate it.
    float remaining = 0.0f;
};

/// Tuning for one sense. Defaults are "everything, forever" — a real NPC
/// overrides at least sight. All fields must be finite; a NaN range would
/// make every distance test fail and silently blind the NPC.
struct SenseConfig {
    /// Max distance the sense reaches, in metres. 0 disables the sense
    /// outright: a deaf or blind NPC is expressed here, not as a flag on every
    /// target.
    float range = 20.0f;

    /// Half the field of view, in radians. PI means omnidirectional, which is
    /// the right default for hearing and touch. Sight usually wants about 1.05
    /// (120 degrees total); a suspicion cone is narrower.
    float half_fov_rad = PI;

    /// Detection needed (in normalised units, 1.0 = perceived) before the sense
    /// reports the target. Separate from gain: gain is *how fast*, threshold is
    /// *how sure*.
    float threshold = 1.0f;

    /// Normalised detection gained per second of unobstructed stimulus. A
    /// threshold of 1 and gain of 1 means "0.7 seconds of clear line of sight".
    float gain = 1.0f;

    /// Normalised detection lost per second once the stimulus stops. Slower
    /// than gain models an NPC that stays suspicious.
    float loss = 0.5f;

    /// Seconds a target stays known after *every* stimulus ends, with its last
    /// known position retained. This is the search window.
    float linger = 3.0f;
};

/// One actor's awareness of one target. Detection is per-sense, so "I can hear
/// it but I cannot see it" is a number the game can read rather than something
/// it has to infer.
struct PerceivedTarget {
    u32 target_id = 0;
    Vec3 last_known_position{};

    /// Normalised 0..1 per sense: 1.0 means that sense is sure. Decays under
    /// that sense's `loss` once its stimulus stops.
    std::array<float, kStimulusKindCount> detection{};

    /// Seconds since any stimulus reached this sensor from the target. 0 this
    /// tick. Drives the linger countdown.
    float silence = 0.0f;

    /// Longest `linger` of any sense that has ever noticed this target. The entry
    /// survives `silence` seconds past that sense's last stimulus, so a target
    /// only ever *seen* expires on sight's linger even though the damage sense is
    /// more patient. 0 means nothing has ever reached this sensor, and the entry
    /// is pruned on its first quiet tick.
    float linger_deadline = 0.0f;

    /// Bit per StimulusKind that received a stimulus this tick. Detection decays
    /// only under senses that went quiet: `loss` is documented as "once the
    /// stimulus stops", and draining a meter *while* the stimulus arrives makes
    /// the net rate (gain*stim - loss), so any stimulus below loss/gain could
    /// never fill — a camouflaged target at range would be undetectable no matter
    /// how long the guard stared. Set wherever a stimulus integrates, cleared in
    /// the final pass.
    u8 stimulated = 0;

    /// True if any sense received a stimulus this tick. Cleared the moment the
    /// target leaves every sense; `aware` outlives it by `linger`.
    bool sensed = false;

    /// True once any sense's detection filled, and stays true until the target
    /// has been quiet past the longest `linger` of any sense that ever noticed
    /// it. Latched rather than recomputed each tick: certainty drains out of the
    /// meter while the sensor still *knows* — that gap is exactly the search
    /// window, and flipping `aware` off the moment detection dipped would make
    /// `linger` unreachable. Cleared by forget()/reset() or when the entry is
    /// pruned.
    bool aware = false;

    /// The sense with the strongest detection this tick, or the earliest on a
    /// tie — see the determinism note. Meaningful while `sensed`.
    StimulusKind last_sense = StimulusKind::Sight;

    float detection_of(StimulusKind kind) const {
        return detection[static_cast<u32>(kind)];
    }

    /// Strongest sense's reading. What a "spotted!" meter shows.
    float strongest_detection() const {
        float best = detection[0];
        for (u32 i = 1; i < kStimulusKindCount; ++i) {
            if (detection[i] > best) best = detection[i];
        }
        return best;
    }
};

class PerceptionScene {
public:
    PerceptionScene() = default;

    /// Registers a target and returns the id it was given. Ids strictly
    /// increase and are never recycled while the scene lives: a recycled id
    /// would make a lingering PerceivedTarget point at a *new* actor and
    /// believe it had been watching it for the last three seconds.
    u32 add_target(PerceptionTarget target);

    /// Removes the target with this id, if present. Targets past it shift down,
    /// which changes iteration order — that is fine, order only has to be fixed
    /// *between* scene edits, and a removal is a game decision, not a
    /// per-tick occurrence.
    void remove_target(u32 id);

    /// Wipes targets and stimuli. Does not reset the id counter, so a clear
    /// followed by adds cannot collide with ids issued before the clear.
    void clear();

    const std::vector<PerceptionTarget>& targets() const { return m_targets; }
    const std::vector<Stimulus>& stimuli() const { return m_stimuli; }

    /// Adds a transient event. A stimulus is visible to every sensor reading
    /// the scene and lasts `remaining` seconds; hand the same event to twenty
    /// NPCs by emitting once, not once each.
    void emit(Stimulus s);

    /// Ages every transient event by `dt` and drops the expired ones. The scene
    /// owns stimulus lifetimes so that N sensors see the *same* event for the
    /// *same* duration; a sensor aging the scene itself would double-count.
    /// Call once per tick, before sensors read. Negative dt is clamped.
    void age(float dt);

    /// Drops all stimuli without touching targets. For a client that only
    /// cares about the persistent cast.
    void clear_stimuli();

private:
    std::vector<PerceptionTarget> m_targets;
    std::vector<Stimulus> m_stimuli;
    u32 m_next_id = 1; // 0 is reserved for "unattributed"
};

/// One NPC's senses. Owns no targets (it reads a PerceptionScene) and no
/// occlusion (it asks an injected query), so it is deterministic and testable
/// with no device.
class Perception {
public:
    Perception();
    Perception(const SenseConfig& sight, const SenseConfig& hearing,
               const SenseConfig& touch, const SenseConfig& damage);

    SenseConfig& sense(StimulusKind kind) { return m_senses[static_cast<u32>(kind)]; }
    const SenseConfig& sense(StimulusKind kind) const {
        return m_senses[static_cast<u32>(kind)];
    }

    /// Where the sensor's eyes are. Updated by the game before each update.
    void set_position(Vec3 p) { m_position = p; }
    Vec3 position() const { return m_position; }

    /// Where the sensor looks. Need not be normalised; it is stored normalised,
    /// and a zero vector (a T-posing NPC) is treated as +Z so a degenerate aim
    /// blinds rather than seeing through the back of its own head.
    void set_forward(Vec3 f);

    /// The sensor's own body radius, added to each target's for range tests.
    void set_radius(float r) { m_radius = r; }

    /// `blocked(a, b)` returns true when sight from a to b is obstructed.
    /// Empty means no occlusion (an open field). Only sight consults it:
    /// hearing goes around corners, and a touch is by definition unobstructed.
    void set_sight_blocker(std::function<bool(Vec3, Vec3)> blocked) {
        m_blocked = std::move(blocked);
    }

    /// Evaluates the scene, integrating detection over `dt`. Reads only; it
    /// never mutates the scene. Negative dt clamps to zero.
    void update(const PerceptionScene& scene, float dt);

    /// Every target this sensor currently knows about, in first-seen order.
    const std::vector<PerceivedTarget>& perceived() const { return m_perceived; }

    const PerceivedTarget* find(u32 target_id) const;

    /// Any sense is sure of the target. Includes the linger window after loss.
    bool is_aware_of(u32 target_id) const;

    /// A stimulus from the target arrived this tick. Strictly stronger than
    /// `is_aware_of`: a half-seen target is neither.
    bool is_sensing(u32 target_id) const;

    /// The aware target with the strongest detection, or null when the sensor
    /// is unaware of anything. Ties go to the earlier target in first-seen
    /// order, so the choice is reproducible.
    const PerceivedTarget* most_threatening() const;

    /// Drops one target's memory immediately (the target died, or the sensor
    /// was told to forget). Returns whether there was anything to forget.
    bool forget(u32 target_id);

    /// Clears all memory, keeping the sense configs. For a respawn or a
    /// patrol-reset; the sensor starts the next life as though it never saw
    /// anything.
    void reset();

private:
    /// Finds the entry for `id` or appends a fresh one. Appending is what keeps
    /// first-seen order — never insert at a position derived from the id.
    PerceivedTarget* find_or_add(u32 id);

    std::array<SenseConfig, kStimulusKindCount> m_senses;
    std::vector<PerceivedTarget> m_perceived;
    std::function<bool(Vec3, Vec3)> m_blocked;

    Vec3 m_position{};
    Vec3 m_forward{0.0f, 0.0f, 1.0f};
    float m_radius = 0.5f;
};

} // namespace nf::ai
