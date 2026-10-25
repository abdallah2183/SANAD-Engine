#include <NF/Runtime/CombatFeedback.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nf::runtime {
namespace {

constexpr f32 kMinimumLifetime = 0.002f;

f32 clamp_finite(f32 value, f32 lo, f32 hi, f32 fallback) {
    if (!std::isfinite(value)) {
        return fallback;
    }
    return std::clamp(value, lo, hi);
}

u32 clamp_cap(u32 value) {
    return std::clamp(value, 1u, 4096u);
}

/// FNV-1a over raw bytes. Good enough to roll a decal and pick a pitch; it is
/// not security-sensitive and must stay identical across runs, which a hash of
/// the double would not be on every compiler.
u32 hash_bytes(const void* data, usize size, u32 seed) {
    const auto* bytes = static_cast<const u8*>(data);
    u32 h = seed ? seed : 2166136261u;
    for (usize i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= 16777619u;
    }
    return h;
}

u32 hash_position(const Vec3& p, u32 seed = 0u) {
    u32 h = hash_bytes(&p, sizeof(p), seed);
    return h;
}

/// 0.94 .. 1.06. Small enough that a burst reads as one weapon, wide enough
/// that consecutive shots do not phase into a single tone.
f32 pitch_from(u32 hash) {
    return 0.94f + static_cast<f32>(hash % 13u) * 0.01f;
}

} // namespace

void CombatFeedback::configure(const CombatFeedbackConfig& config) {
    m_config = config;
    m_config.flash_seconds = clamp_finite(m_config.flash_seconds, kMinimumLifetime, 10.0f, 0.055f);
    m_config.flash_size = clamp_finite(m_config.flash_size, 0.001f, 10.0f, 0.22f);
    m_config.tracer_seconds = clamp_finite(m_config.tracer_seconds, kMinimumLifetime, 10.0f, 0.09f);
    m_config.tracer_length = clamp_finite(m_config.tracer_length, 0.01f, 1000.0f, 9.0f);
    m_config.tracer_width = clamp_finite(m_config.tracer_width, 0.0005f, 1.0f, 0.025f);
    m_config.impact_seconds = clamp_finite(m_config.impact_seconds, 0.01f, 600.0f, 6.0f);
    m_config.impact_size = clamp_finite(m_config.impact_size, 0.001f, 10.0f, 0.14f);
    m_config.shot_volume = clamp_finite(m_config.shot_volume, 0.0f, 4.0f, 0.7f);
    m_config.impact_volume = clamp_finite(m_config.impact_volume, 0.0f, 4.0f, 0.45f);
    m_config.max_flashes = clamp_cap(m_config.max_flashes);
    m_config.max_tracers = clamp_cap(m_config.max_tracers);
    m_config.max_impacts = clamp_cap(m_config.max_impacts);
    m_config.max_sounds = clamp_cap(m_config.max_sounds);
}

void CombatFeedback::reset() {
    m_flashes.clear();
    m_tracers.clear();
    m_impacts.clear();
    m_sounds.clear();
}

void CombatFeedback::update(f32 dt, std::span<const ShotEvent> shots,
                            std::span<const ImpactEvent> impacts) {
    // 1. Age and retire FIRST. An effect ingested below starts at age 0, so it
    //    survives this frame even when dt is longer than its whole lifetime.
    const bool age = std::isfinite(dt) && dt > 0.0f;
    if (age) {
        auto age_all = [dt](auto& list) {
            for (auto& effect : list) {
                effect.age += dt;
                effect.strength = effect.life > 0.0f
                                      ? std::clamp(1.0f - effect.age / effect.life, 0.0f, 1.0f)
                                      : 0.0f;
            }
            list.erase(std::remove_if(list.begin(), list.end(),
                                      [](const auto& effect) { return effect.age >= effect.life; }),
                       list.end());
        };
        age_all(m_flashes);
        age_all(m_tracers);
        age_all(m_impacts);
    }

    // 2. Cues belong to one frame. A frame with no mix block drains them on the
    //    next step_audio() (the Runtime owns that handoff), but a cue that was
    //    never drained must not reappear in a later frame's mix.
    m_sounds.clear();

    // 3. Ingest. Shots first: the muzzle flash and the shot sound are the start
    //    of the event the impacts complete.
    for (const ShotEvent& shot : shots) {
        if (shot.pellet <= 1u) {
            add_flash(shot);
            if (m_config.audio && m_config.shot_volume > 0.0f) {
                FeedbackSound cue;
                cue.kind = FeedbackSound::Kind::Shot;
                cue.position = shot.origin;
                cue.volume = m_config.shot_volume;
                cue.pitch = pitch_from(shot.seed != 0u ? shot.seed
                                                       : hash_position(shot.origin));
                add_sound(cue);
            }
        }
        add_tracer(shot);
    }

    for (const ImpactEvent& impact : impacts) {
        // A damage-over-time tick is not a surface; a splash victim is the
        // consequence of a blast, not a new round landing. Neither gets a
        // scorch mark or its own sound, or a burning crowd becomes a wall of
        // identical decals and clicks.
        if (impact.status || impact.splash) {
            continue;
        }
        add_impact(impact);
        if (m_config.audio && m_config.impact_volume > 0.0f) {
            FeedbackSound cue;
            cue.kind = FeedbackSound::Kind::Impact;
            cue.position = impact.position;
            cue.volume = m_config.impact_volume;
            cue.pitch = pitch_from(hash_position(impact.position));
            add_sound(cue);
        }
    }
}

void CombatFeedback::add_flash(const ShotEvent& shot) {
    if (m_config.flash_seconds <= 0.0f || m_config.flash_size <= 0.0f) {
        return;
    }
    if (m_flashes.size() >= m_config.max_flashes) {
        m_flashes.erase(m_flashes.begin());
    }
    FeedbackFlash flash;
    flash.position = shot.origin;
    flash.direction = shot.direction.normalized();
    if (flash.direction.length_sq() < 1.0e-8f) {
        flash.direction = Vec3{0.0f, 0.0f, -1.0f};
    }
    flash.age = 0.0f;
    flash.life = m_config.flash_seconds;
    flash.size = m_config.flash_size;
    flash.strength = 1.0f;
    m_flashes.push_back(flash);
}

void CombatFeedback::add_tracer(const ShotEvent& shot) {
    if (m_config.tracer_seconds <= 0.0f || m_config.tracer_length <= 0.0f) {
        return;
    }
    if (m_tracers.size() >= m_config.max_tracers) {
        m_tracers.erase(m_tracers.begin());
    }
    Vec3 dir = shot.direction.normalized();
    if (dir.length_sq() < 1.0e-8f) {
        dir = Vec3{0.0f, 0.0f, -1.0f};
    }
    FeedbackTracer tracer;
    // Start just past the muzzle/eye so a first-person camera is not looking
    // down the inside of its own beam.
    tracer.start = shot.origin + dir * 0.35f;
    tracer.end = tracer.start + dir * m_config.tracer_length;
    tracer.age = 0.0f;
    tracer.life = m_config.tracer_seconds;
    tracer.width = m_config.tracer_width;
    tracer.strength = 1.0f;
    m_tracers.push_back(tracer);
}

void CombatFeedback::add_impact(const ImpactEvent& impact) {
    if (m_config.impact_seconds <= 0.0f || m_config.impact_size <= 0.0f) {
        return;
    }
    if (m_impacts.size() >= m_config.max_impacts) {
        m_impacts.erase(m_impacts.begin());
    }
    FeedbackImpact mark;
    mark.position = impact.position;
    mark.normal = impact.normal.normalized();
    if (mark.normal.length_sq() < 1.0e-8f) {
        // A hit with no reported normal still happened; face the mark at the
        // camera-independent default rather than refusing to draw it.
        mark.normal = Vec3{0.0f, 0.0f, 1.0f};
    }
    mark.seed = hash_position(impact.position,
                              static_cast<u32>(impact.victim.id) * 2654435761u);
    mark.age = 0.0f;
    mark.life = m_config.impact_seconds;
    mark.size = m_config.impact_size;
    mark.strength = 1.0f;
    m_impacts.push_back(mark);
}

void CombatFeedback::add_sound(FeedbackSound cue) {
    if (m_sounds.size() >= m_config.max_sounds) {
        m_sounds.erase(m_sounds.begin());
    }
    m_sounds.push_back(cue);
}

} // namespace nf::runtime
