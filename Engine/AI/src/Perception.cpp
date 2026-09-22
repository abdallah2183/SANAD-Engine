#include <NF/AI/Perception.hpp>

#include <algorithm>
#include <cmath>

namespace nf::ai {
namespace {

/// Detection is compared against 1.0 after an accumulate-and-clamp. A guard
/// whose meter sits at 0.99999994 because the final increment rounded down
/// must still count as aware; without this slack awareness would flip on at
/// one dt and off at the next for byte-identical input, which would break the
/// replay contract the whole AI module is written under.
inline constexpr float kAwareEpsilon = 1e-5f;

/// Distance falloff: 1 at contact, 0 at `reach`. Linear rather than inverse
/// because a perception meter is an authoring knob — a designer asks for
/// "detected at 15 metres", not "detected at 1/15th intensity".
float falloff(float distance, float reach) {
    if (reach <= 0.0f) return 0.0f;
    const float f = 1.0f - distance / reach;
    return f < 0.0f ? 0.0f : f;
}

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

/// Sight stimulus (0..1) or 0 when the target is not visible.
float sight_strength(Vec3 forward, float sensor_radius,
                     const PerceptionTarget& t, const SenseConfig& c,
                     const Vec3& delta, float d2) {
    if (c.range <= 0.0f) return 0.0f; // sense disabled

    const float reach = c.range + t.radius + sensor_radius;
    if (d2 > reach * reach) return 0.0f; // too far even edge-to-edge

    const float d = std::sqrt(d2);
    // A target on the sensor's exact position has no direction; treat it as
    // dead ahead so a stacked pair still sees each other.
    const Vec3 dir = (d > EPSILON) ? delta * (1.0f / d) : forward;

    // The cone test only runs when the sense is not omnidirectional. Compared
    // against cos(half_fov) rather than the angle against half_fov to stay off
    // the acos branch, which is slow and loses precision near zero.
    if (c.half_fov_rad < PI - 1e-6f) {
        if (dir.dot(forward) < std::cos(c.half_fov_rad)) return 0.0f;
    }

    return t.visibility * falloff(d, reach);
}

/// Continuous noise a target emits (0..1) or 0 when inaudible. Discrete noises
/// — a gunshot — are Stimuli and handled with the target ones in the other
/// loop; this is the "it is walking" channel.
float hearing_strength(float sensor_radius,
                       const PerceptionTarget& t, const SenseConfig& c,
                       float d2) {
    if (c.range <= 0.0f || t.noise <= 0.0f) return 0.0f;

    const float reach = c.range + t.radius + sensor_radius;
    if (d2 > reach * reach) return 0.0f;

    const float d = std::sqrt(d2);
    return t.noise * falloff(d, reach);
}

/// Touch: contact is contact, there is no falloff to compute.
float touch_strength(float sensor_radius,
                     const PerceptionTarget& t, const SenseConfig& c,
                     float d2) {
    if (c.range <= 0.0f) return 0.0f;
    const float reach = c.range + t.radius + sensor_radius;
    return d2 <= reach * reach ? 1.0f : 0.0f;
}

/// Extends the entry's search window to `linger` if the sense is more patient
/// than anything that noticed the target before. Keeping the max rather than
/// overwriting means a target seen (sight, 3 s) then hit (damage, 5 s) keeps the
/// longer window, while one only ever seen never inherits damage's patience.
void extend_linger(PerceivedTarget& pt, float linger) {
    if (linger > pt.linger_deadline) pt.linger_deadline = linger;
}

/// Adds one sense's contribution to a meter. A disabled sense (range <= 0)
/// never gets here: its strength function already returned 0. Also records that
/// the sense fired this tick — see `stimulated` for why decay is skipped for it.
void integrate_sense(PerceivedTarget& pt, StimulusKind kind, float stim,
                     const SenseConfig& c, float dt) {
    if (stim <= 0.0f) return;
    // A threshold of 0 means "no uncertainty at all", so it is clamped to a
    // epsilon: the step becomes huge, the clamp turns it into an instant 1.0,
    // which is what was asked for.
    const float threshold = c.threshold > 0.0f ? c.threshold : 1e-6f;
    const float step = (c.gain * stim / threshold) * dt;
    const u32 k = static_cast<u32>(kind);
    pt.detection[k] = clamp01(pt.detection[k] + step);
    pt.stimulated |= static_cast<u8>(1u << k);
    extend_linger(pt, c.linger);
}

} // namespace

// ---------------------------------------------------------------------------
// PerceptionScene
// ---------------------------------------------------------------------------

u32 PerceptionScene::add_target(PerceptionTarget target) {
    target.id = m_next_id++;
    m_targets.push_back(target);
    return target.id;
}

void PerceptionScene::remove_target(u32 id) {
    const auto end = m_targets.end();
    m_targets.erase(std::remove_if(m_targets.begin(), end,
                                   [id](const PerceptionTarget& t) { return t.id == id; }),
                    end);
}

void PerceptionScene::clear() {
    m_targets.clear();
    m_stimuli.clear();
}

void PerceptionScene::age(float dt) {
    if (dt < 0.0f) dt = 0.0f;
    // Decrement in place then drop the dead: keeps emission order, which the
    // sensor's own determinism relies on, and allocates nothing while every
    // stimulus survives.
    for (Stimulus& s : m_stimuli) s.remaining -= dt;
    m_stimuli.erase(
        std::remove_if(m_stimuli.begin(), m_stimuli.end(),
                       [](const Stimulus& s) { return s.remaining <= 0.0f; }),
        m_stimuli.end());
}

void PerceptionScene::emit(Stimulus s) {
    m_stimuli.push_back(s); // append: emission order is the visit order
}

void PerceptionScene::clear_stimuli() { m_stimuli.clear(); }

// ---------------------------------------------------------------------------
// Perception
// ---------------------------------------------------------------------------

Perception::Perception() {
    m_senses[static_cast<u32>(StimulusKind::Sight)]  = SenseConfig{20.0f, 1.05f, 1.0f, 1.0f, 0.5f, 3.0f};
    m_senses[static_cast<u32>(StimulusKind::Sound)]  = SenseConfig{15.0f, PI,     1.0f, 1.0f, 0.5f, 3.0f};
    m_senses[static_cast<u32>(StimulusKind::Touch)]  = SenseConfig{1.5f,  PI,     1.0f, 4.0f, 0.5f, 1.0f};
    m_senses[static_cast<u32>(StimulusKind::Damage)] = SenseConfig{1e9f,  PI,     1.0f, 1.0f, 0.5f, 5.0f};
}

Perception::Perception(const SenseConfig& sight, const SenseConfig& hearing,
                       const SenseConfig& touch, const SenseConfig& damage) {
    m_senses[static_cast<u32>(StimulusKind::Sight)]  = sight;
    m_senses[static_cast<u32>(StimulusKind::Sound)]  = hearing;
    m_senses[static_cast<u32>(StimulusKind::Touch)]  = touch;
    m_senses[static_cast<u32>(StimulusKind::Damage)] = damage;
}

void Perception::set_forward(Vec3 f) {
    // normalized() returns the input unchanged on a zero vector; a T-posing
    // NPC would then "look" down a degenerate axis and every dot product would
    // be 0, which passes no cone test. +Z is the engine's default forward, so
    // the failure mode is "blind" rather than "sees everything".
    const float len = f.length();
    m_forward = (len > EPSILON) ? f * (1.0f / len) : Vec3{0.0f, 0.0f, 1.0f};
}

void Perception::update(const PerceptionScene& scene, float dt) {
    if (dt < 0.0f) dt = 0.0f;

    // Pass 0 — clear the per-tick flags. Detection is NOT decayed here: decaying
    // before the stimulus passes run would make a continuously-visible target's
    // net rate (gain*stim - loss), and any stimulus below loss/gain could then
    // never fill no matter how long it lasted. Decay happens in the final pass,
    // and only for senses that went quiet this tick.
    for (PerceivedTarget& pt : m_perceived) {
        pt.sensed = false;
    }

    // Pass 1 — transient events. Runs before the target pass so that a target
    // both heard and seen ends up recorded where it *is*, not where its last
    // noise came from: sight overwrites, noise does not.
    for (const Stimulus& s : scene.stimuli()) {
        if (s.kind == StimulusKind::Sight) continue; // sight is target-derived
        if (s.source_id == 0) continue;              // unattributed, by design

        const u32 k = static_cast<u32>(s.kind);
        const SenseConfig& c = m_senses[k];
        if (c.range <= 0.0f) continue;               // sense disabled

        PerceivedTarget* pt = find_or_add(s.source_id);

        if (s.kind == StimulusKind::Damage) {
            // Pain is neither distance-attenuated nor gradual. A hit from a
            // sniper the sensor never spotted still makes it aware — and the
            // position recorded is where the hit landed, the only thing the
            // sensor actually knows.
            pt->detection[k] = 1.0f;
            pt->stimulated |= static_cast<u8>(1u << k);
            extend_linger(*pt, c.linger);
            pt->last_known_position = s.position;
        } else {
            const float d = (s.position - m_position).length();
            const float stim = s.strength * falloff(d, c.range + m_radius);
            if (stim <= 0.0f) continue;
            integrate_sense(*pt, s.kind, stim, c, dt);
            pt->last_known_position = s.position;
        }

        pt->silence = 0.0f;
        pt->sensed = true;
    }

    // Pass 2 — the persistent cast. One linear pass over the scene; each target
    // is looked up in m_perceived, which is O(sensors x cast) and deliberately
    // not a map: AI casts are tens of actors, and a hash would make iteration
    // order implementation-defined, breaking the replay contract.
    for (const PerceptionTarget& t : scene.targets()) {
        PerceivedTarget* pt = find_or_add(t.id);

        const Vec3 delta = t.position - m_position;
        const float d2 = delta.length_sq();

        float sight_stim = sight_strength(m_forward, m_radius, t,
                                          m_senses[static_cast<u32>(StimulusKind::Sight)],
                                          delta, d2);
        // The one place occlusion is consulted. Hearing goes around corners and
        // a touch is by definition unobstructed, so only sight can be blocked.
        if (sight_stim > 0.0f && m_blocked && m_blocked(m_position, t.position)) {
            sight_stim = 0.0f;
        }
        const float sound_stim = hearing_strength(m_radius, t,
                                                  m_senses[static_cast<u32>(StimulusKind::Sound)],
                                                  d2);
        const float touch_stim = touch_strength(m_radius, t,
                                                m_senses[static_cast<u32>(StimulusKind::Touch)],
                                                d2);

        if (sight_stim > 0.0f) {
            integrate_sense(*pt, StimulusKind::Sight, sight_stim,
                            m_senses[static_cast<u32>(StimulusKind::Sight)], dt);
            pt->last_known_position = t.position; // the truth, not a rumour
            pt->silence = 0.0f;
            pt->sensed = true;
        }
        if (sound_stim > 0.0f) {
            integrate_sense(*pt, StimulusKind::Sound, sound_stim,
                            m_senses[static_cast<u32>(StimulusKind::Sound)], dt);
            // A target heard but not seen does NOT move its recorded position:
            // a noise tells you where it was, and a walking NPC has already
            // left. Only a discrete Stimulus pins a position.
            pt->silence = 0.0f;
            pt->sensed = true;
        }
        if (touch_stim > 0.0f) {
            integrate_sense(*pt, StimulusKind::Touch, touch_stim,
                            m_senses[static_cast<u32>(StimulusKind::Touch)], dt);
            pt->last_known_position = t.position; // contact is certain
            pt->silence = 0.0f;
            pt->sensed = true;
        }
    }

    // Pass 3 — decay, derive the outputs and forget the stale. `aware` latches:
    // once a sense was sure, the sensor stays sure until the entry is pruned,
    // even as the certainty meter drains — that is the whole point of `linger`.
    //
    // `silence` is aged here rather than in pass 0 because an entry created this
    // tick by passes 1 or 2 would otherwise carry a silence of 0 past the prune
    // test and survive a tick in which nothing whatsoever reached it. Aging here
    // means a target sensed this tick sits at 0 and one ignored sits at dt.
    for (size_t i = 0; i < m_perceived.size();) {
        PerceivedTarget& pt = m_perceived[i];

        // Only the quiet senses drain. See `stimulated`: this is what makes a
        // weak-but-persistent stimulus eventually register.
        for (u32 k = 0; k < kStimulusKindCount; ++k) {
            if ((pt.stimulated >> k) & 1u) continue;
            pt.detection[k] = clamp01(pt.detection[k] - m_senses[k].loss * dt);
        }
        pt.stimulated = 0;

        if (!pt.sensed) pt.silence += dt;

        if (!pt.aware && pt.strongest_detection() >= 1.0f - kAwareEpsilon) {
            pt.aware = true;
        }

        // last_sense is the strongest reading this tick; strict `>` so a tie
        // falls to the earlier kind in StimulusKind order (sight before sound).
        u32 best = 0;
        for (u32 k = 1; k < kStimulusKindCount; ++k) {
            if (pt.detection[k] > pt.detection[best]) best = k;
        }
        pt.last_sense = static_cast<StimulusKind>(best);

        if (pt.silence > pt.linger_deadline) {
            // Swap-and-pop would be faster, but it reorders surviving entries
            // and m_perceived's order IS the contract (first-seen). Erase
            // preserves it; the cast is small.
            m_perceived.erase(m_perceived.begin() + static_cast<ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

const PerceivedTarget* Perception::find(u32 target_id) const {
    for (const PerceivedTarget& pt : m_perceived) {
        if (pt.target_id == target_id) return &pt;
    }
    return nullptr;
}

bool Perception::is_aware_of(u32 target_id) const {
    const PerceivedTarget* pt = find(target_id);
    return pt != nullptr && pt->aware;
}

bool Perception::is_sensing(u32 target_id) const {
    const PerceivedTarget* pt = find(target_id);
    return pt != nullptr && pt->sensed;
}

const PerceivedTarget* Perception::most_threatening() const {
    const PerceivedTarget* best = nullptr;
    float best_detection = -1.0f;
    for (const PerceivedTarget& pt : m_perceived) {
        if (!pt.aware) continue;
        const float d = pt.strongest_detection();
        if (d > best_detection) { // strict: ties keep the earlier target
            best_detection = d;
            best = &pt;
        }
    }
    return best;
}

bool Perception::forget(u32 target_id) {
    const auto end = m_perceived.end();
    const auto it = std::remove_if(m_perceived.begin(), end,
                                   [target_id](const PerceivedTarget& pt) {
                                       return pt.target_id == target_id;
                                   });
    const bool removed = it != end;
    m_perceived.erase(it, end);
    return removed;
}

void Perception::reset() { m_perceived.clear(); }

PerceivedTarget* Perception::find_or_add(u32 id) {
    for (PerceivedTarget& pt : m_perceived) {
        if (pt.target_id == id) return &pt;
    }
    PerceivedTarget pt;
    pt.target_id = id;
    m_perceived.push_back(pt); // append: first-seen order, never insert by id
    return &m_perceived.back();
}

} // namespace nf::ai
