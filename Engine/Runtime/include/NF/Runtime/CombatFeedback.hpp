#pragma once

// NF/Runtime/CombatFeedback.hpp — shot/impact events turned into feedback.
//
// Phase 44-55 publish what happened: `ShotEvent` says a pellet left a weapon,
// `ImpactEvent` says what it hit and for how much. Until now both were only a
// list a game had to draw itself, so a scene with a working gun still fired
// silently and left no mark. This is the small deterministic director that
// turns those streams into the four things a player actually perceives:
//
//   * a muzzle flash      (one per volley, at the shot origin)
//   * a tracer beam       (one per pellet, along the traced direction)
//   * a scorch mark       (one per surface hit, lasting seconds)
//   * a one-shot sound cue (one shot per volley, one impact per surface hit)
//
// The director owns NO GPU resource, NO entity and NO audio device. It is pure
// arithmetic over events, which is what makes the cue timing and the caps
// testable without a window. The Runtime maps its lists to RenderWorld objects,
// projected decals and audio emitters; the scene opts in with a `Feedback:`
// line, so a scene that never asks for it behaves exactly as before.
//
// The frame contract mirrors the event buffers:
//
//   feedback.update(dt, shots.previous(), impacts.previous());
//
// — i.e. it consumes the completed frame at the start of the next update, so a
// `fire_weapon()` call made between updates (a host, a script, a test) is
// included and every event is consumed exactly once. Aging happens BEFORE the
// ingest, so an effect spawned this frame is at age 0 and cannot be retired by
// the same frame's dt; every effect is guaranteed at least one rendered frame.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Runtime/Impact.hpp>
#include <NF/Runtime/Shot.hpp>

#include <span>
#include <vector>

namespace nf::runtime {

/// Tuning for the whole feedback layer. Every value is clamped by configure(),
/// so a component built in C++ (or an inspector edit) cannot ask for a negative
/// duration, a zero-width beam or an unbounded list.
struct CombatFeedbackConfig {
    // Muzzle flash. `flash_seconds` is short on purpose: a flash that lingers
    // reads as a lamp, not as a shot.
    f32 flash_seconds = 0.055f;
    /// Edge length of the flash cube, in metres.
    f32 flash_size = 0.22f;

    // Tracer. The beam is drawn from just past the origin along the traced
    // direction; `tracer_length` is its visible run, not the bullet's range.
    f32 tracer_seconds = 0.09f;
    f32 tracer_length = 9.0f;
    f32 tracer_width = 0.025f;

    // Impact scorch. Marks persist far longer than flashes; this is the decal's
    // lifetime, and the runtime fades it over its last quarter.
    f32 impact_seconds = 6.0f;
    f32 impact_size = 0.14f;

    /// Linear volume for the procedural shot and impact one-shots. The bus and
    /// the master volume still multiply these.
    f32 shot_volume = 0.7f;
    f32 impact_volume = 0.45f;
    /// False keeps the visuals and drops every sound cue.
    bool audio = true;

    // Hard caps. A new effect at the cap drops the oldest, deterministically;
    // unfed static vectors are what a long fight would otherwise grow forever.
    u32 max_flashes = 48u;
    u32 max_tracers = 96u;
    u32 max_impacts = 192u;
    u32 max_sounds = 16u;
};

/// One muzzle flash, aged by the director. `direction` is the pellet's traced
/// direction so the renderer can orient the quad/cube with the barrel.
struct FeedbackFlash {
    Vec3 position{};
    Vec3 direction{0.0f, 0.0f, -1.0f};
    f32 age = 0.0f;
    f32 life = 0.055f;
    f32 size = 0.22f;
    /// 0..1, 1 at spawn. Lets a renderer shrink or dim without knowing the
    /// curve; the Runtime uses it for size.
    f32 strength = 1.0f;
};

/// One tracer beam segment.
struct FeedbackTracer {
    Vec3 start{};
    Vec3 end{};
    f32 age = 0.0f;
    f32 life = 0.09f;
    f32 width = 0.025f;
    f32 strength = 1.0f;
};

/// One scorch mark. `normal` is the surface normal the decal faces; `seed` is a
/// deterministic roll around it so two hits on the same wall do not look
/// identically stamped.
struct FeedbackImpact {
    Vec3 position{};
    Vec3 normal{0.0f, 0.0f, 1.0f};
    u32 seed = 0u;
    f32 age = 0.0f;
    f32 life = 6.0f;
    f32 size = 0.14f;
    f32 strength = 1.0f;
};

/// A one-shot sound the audio layer should start. `kind` selects the procedural
/// buffer; the position is where the player hears it come from.
struct FeedbackSound {
    enum class Kind : u8 {
        Shot,
        Impact,
    };

    Kind kind = Kind::Shot;
    Vec3 position{};
    f32 volume = 1.0f;
    /// Playback rate, deterministic per event (a small per-shot variation keeps
    /// a burst from sounding like one sample repeated).
    f32 pitch = 1.0f;
};

/// Ages, retires and ingests combat events. Single-threaded, like the rest of
/// the frame.
class CombatFeedback {
public:
    /// Applies `config`, clamping every value into its meaningful range. A
    /// second call keeps the live effects and only changes the tuning.
    void configure(const CombatFeedbackConfig& config);
    [[nodiscard]] const CombatFeedbackConfig& config() const { return m_config; }

    /// Drops every effect and cue. Called when a scene is adopted: a tracer
    /// from the previous level must not appear in the new one.
    void reset();

    /// Ages every effect by `dt`, retires the expired ones, clears the previous
    /// frame's sound cues, then ingests this frame's events.
    ///
    /// A non-positive `dt` still ingests (the events are real) but does not age
    /// anything, so a paused editor keeps the effects it has instead of
    /// flickering them away.
    void update(f32 dt, std::span<const ShotEvent> shots,
                std::span<const ImpactEvent> impacts);

    /// Cues are cleared by the next update(), so the audio layer drains them
    /// once per frame. A frame with no mix block yet keeps them until it does —
    /// the Runtime owns that handoff, not this class.
    void clear_sounds() { m_sounds.clear(); }

    [[nodiscard]] const std::vector<FeedbackFlash>& flashes() const { return m_flashes; }
    [[nodiscard]] const std::vector<FeedbackTracer>& tracers() const { return m_tracers; }
    [[nodiscard]] const std::vector<FeedbackImpact>& impacts() const { return m_impacts; }
    [[nodiscard]] const std::vector<FeedbackSound>& sounds() const { return m_sounds; }

    [[nodiscard]] bool empty() const {
        return m_flashes.empty() && m_tracers.empty() && m_impacts.empty() && m_sounds.empty();
    }

private:
    void add_flash(const ShotEvent& shot);
    void add_tracer(const ShotEvent& shot);
    void add_impact(const ImpactEvent& impact);
    void add_sound(FeedbackSound cue);

    CombatFeedbackConfig m_config;
    std::vector<FeedbackFlash> m_flashes;
    std::vector<FeedbackTracer> m_tracers;
    std::vector<FeedbackImpact> m_impacts;
    std::vector<FeedbackSound> m_sounds;
};

} // namespace nf::runtime
