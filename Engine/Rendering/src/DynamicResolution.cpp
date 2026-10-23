#include <NF/Rendering/DynamicResolution.hpp>

#include <NF/Rendering/RenderScale.hpp>

#include <algorithm>
#include <cmath>

namespace nf::rendering {

namespace {

constexpr f32 kSmoothing = 0.2f;

f32 finite_or(f32 value, f32 fallback) {
    return std::isfinite(value) ? value : fallback;
}

} // namespace

void DynamicResolution::configure(const DynamicResolutionConfig& config) {
    DynamicResolutionConfig clean{};
    clean.enabled = config.enabled;
    clean.target_frame_ms =
        std::clamp(finite_or(config.target_frame_ms, kDefaultTargetFrameMs), 1.0f, 1000.0f);
    clean.min_scale = sanitize_render_scale(config.min_scale);
    clean.max_scale = sanitize_render_scale(config.max_scale);
    if (clean.min_scale > clean.max_scale) {
        std::swap(clean.min_scale, clean.max_scale);
    }
    clean.step = std::clamp(finite_or(config.step, kDefaultStep), 0.01f, 0.5f);
    clean.cooldown_s =
        std::clamp(finite_or(config.cooldown_s, kDefaultCooldownSeconds), 0.0f, 10.0f);
    clean.tolerance = std::clamp(finite_or(config.tolerance, kDefaultTolerance), 0.0f, 1.0f);

    const bool was_enabled = m_config.enabled;
    m_config = clean;

    if (!m_has_scale) {
        m_scale = clean.max_scale;
        m_has_scale = true;
    } else if (clean.enabled && !was_enabled) {
        // A fresh enable starts at the authored ceiling, not at whatever scale
        // the previous session happened to leave behind.
        m_scale = clean.max_scale;
        m_average_frame_s = 0.0f;
        m_time_since_change_s = 0.0f;
        m_has_sample = false;
    } else {
        // Bounds can change under a live governor (the editor can move the
        // static scale, for example). Keep the current value if it still fits.
        m_scale = std::clamp(m_scale, clean.min_scale, clean.max_scale);
    }
}

void DynamicResolution::reset(f32 scale) {
    const f32 clean = sanitize_render_scale(scale);
    m_scale = std::clamp(clean, m_config.min_scale, m_config.max_scale);
    m_average_frame_s = 0.0f;
    m_time_since_change_s = 0.0f;
    m_has_sample = false;
    m_has_scale = true;
}

f32 DynamicResolution::update(f32 frame_time_s) {
    if (!m_config.enabled) {
        return m_scale;
    }
    if (!std::isfinite(frame_time_s) || frame_time_s <= 0.0f) {
        return m_scale;
    }

    const f32 target_s = m_config.target_frame_ms / 1000.0f;
    // A load hitch is not a steady-state frame. Clamping the sample keeps a
    // one-second stall from counting as two seconds of "this resolution is too
    // expensive", while still letting the average learn from ordinary spikes.
    const f32 hitch_cap = std::max(target_s * 4.0f, 0.25f);
    const f32 sample = std::min(frame_time_s, hitch_cap);

    if (!m_has_sample) {
        m_average_frame_s = sample;
        m_has_sample = true;
    } else {
        m_average_frame_s = m_average_frame_s * (1.0f - kSmoothing) + sample * kSmoothing;
    }
    m_time_since_change_s += sample;

    if (m_time_since_change_s < m_config.cooldown_s) {
        return m_scale;
    }

    const f32 upper = target_s * (1.0f + m_config.tolerance);
    const f32 lower = target_s * (1.0f - m_config.tolerance);

    f32 next = m_scale;
    if (m_average_frame_s > upper) {
        next = m_scale - m_config.step;
    } else if (m_average_frame_s < lower) {
        next = m_scale + m_config.step;
    } else {
        // Inside the hysteresis band: no change. The timer keeps running, so
        // the next real miss is handled immediately rather than after another
        // full cooldown.
        return m_scale;
    }

    next = std::clamp(next, m_config.min_scale, m_config.max_scale);
    if (next == m_scale) {
        // Already at a bound in the direction the load suggests. Do not reset
        // the timer: when the load changes, the governor should react at once.
        return m_scale;
    }

    m_scale = next;
    m_time_since_change_s = 0.0f;
    // The average that justified this change described the old resolution.
    m_average_frame_s = target_s;
    return m_scale;
}

} // namespace nf::rendering
