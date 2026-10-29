#include <NF/Core/Curve.hpp>
#include <cmath>

namespace nf {

size_t FloatCurve::add_key(float time, float value, TangentMode mode) {
    return add_key(Keyframe(time, value, mode));
}

size_t FloatCurve::add_key(const Keyframe& key) {
    auto it = std::lower_bound(m_keys.begin(), m_keys.end(), key.time,
        [](const Keyframe& k, float t) {
            return k.time < t;
        });

    if (it != m_keys.end() && std::abs(it->time - key.time) < 1e-6f) {
        *it = key;
        return static_cast<size_t>(std::distance(m_keys.begin(), it));
    }

    auto inserted = m_keys.insert(it, key);
    return static_cast<size_t>(std::distance(m_keys.begin(), inserted));
}

bool FloatCurve::remove_key(size_t index) {
    if (index >= m_keys.size()) return false;
    m_keys.erase(m_keys.begin() + index);
    return true;
}

void FloatCurve::clear() {
    m_keys.clear();
}

float FloatCurve::apply_extrapolation(float time) const {
    if (m_keys.size() <= 1) return time;

    float t_start = m_keys.front().time;
    float t_end = m_keys.back().time;
    float d = t_end - t_start;
    if (d <= 0.0f) return t_start;

    if (time < t_start) {
        switch (pre_extrapolation) {
            case ExtrapolationMode::Clamp:
                return t_start;
            case ExtrapolationMode::Loop: {
                float offset = std::fmod(t_start - time, d);
                return (offset == 0.0f) ? t_start : (t_end - offset);
            }
            case ExtrapolationMode::PingPong: {
                float seg = (t_start - time) / d;
                int seg_idx = static_cast<int>(std::floor(seg));
                float frac = seg - static_cast<float>(seg_idx);
                return (seg_idx % 2 == 0) ? (t_start + frac * d) : (t_end - frac * d);
            }
        }
    } else if (time > t_end) {
        switch (post_extrapolation) {
            case ExtrapolationMode::Clamp:
                return t_end;
            case ExtrapolationMode::Loop: {
                float offset = std::fmod(time - t_start, d);
                return t_start + offset;
            }
            case ExtrapolationMode::PingPong: {
                float seg = (time - t_start) / d;
                int seg_idx = static_cast<int>(std::floor(seg));
                float frac = seg - static_cast<float>(seg_idx);
                return (seg_idx % 2 == 0) ? (t_start + frac * d) : (t_end - frac * d);
            }
        }
    }

    return time;
}

float FloatCurve::evaluate(float time) const {
    if (m_keys.empty()) return 0.0f;
    if (m_keys.size() == 1) return m_keys[0].value;

    time = apply_extrapolation(time);

    if (time <= m_keys.front().time) return m_keys.front().value;
    if (time >= m_keys.back().time) return m_keys.back().value;

    // Binary search for the interval [k0, k1]
    auto it = std::upper_bound(m_keys.begin(), m_keys.end(), time,
        [](float t, const Keyframe& k) {
            return t < k.time;
        });

    size_t idx1 = static_cast<size_t>(std::distance(m_keys.begin(), it));
    size_t idx0 = idx1 - 1;

    const Keyframe& k0 = m_keys[idx0];
    const Keyframe& k1 = m_keys[idx1];

    float dt = k1.time - k0.time;
    if (dt <= 1e-7f) return k0.value;

    float u = (time - k0.time) / dt;

    if (k0.mode == TangentMode::Constant) {
        return k0.value;
    } else if (k0.mode == TangentMode::Linear) {
        return k0.value + u * (k1.value - k0.value);
    }

    // Cubic Hermite Spline
    float u2 = u * u;
    float u3 = u2 * u;

    float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
    float h10 = u3 - 2.0f * u2 + u;
    float h01 = -2.0f * u3 + 3.0f * u2;
    float h11 = u3 - u2;

    float m0 = k0.out_tangent * dt;
    float m1 = k1.in_tangent * dt;

    return h00 * k0.value + h10 * m0 + h01 * k1.value + h11 * m1;
}

void FloatCurve::auto_smooth_tangents() {
    if (m_keys.size() <= 1) return;

    for (size_t i = 0; i < m_keys.size(); ++i) {
        if (m_keys[i].mode != TangentMode::Smooth) continue;

        if (i == 0) {
            float dt = m_keys[1].time - m_keys[0].time;
            float slope = (dt > 1e-6f) ? (m_keys[1].value - m_keys[0].value) / dt : 0.0f;
            m_keys[i].in_tangent = slope;
            m_keys[i].out_tangent = slope;
        } else if (i == m_keys.size() - 1) {
            float dt = m_keys[i].time - m_keys[i - 1].time;
            float slope = (dt > 1e-6f) ? (m_keys[i].value - m_keys[i - 1].value) / dt : 0.0f;
            m_keys[i].in_tangent = slope;
            m_keys[i].out_tangent = slope;
        } else {
            float dt = m_keys[i + 1].time - m_keys[i - 1].time;
            float slope = (dt > 1e-6f) ? (m_keys[i + 1].value - m_keys[i - 1].value) / dt : 0.0f;
            m_keys[i].in_tangent = slope;
            m_keys[i].out_tangent = slope;
        }
    }
}

FloatCurve FloatCurve::linear(float start_val, float end_val, float duration) {
    FloatCurve curve;
    curve.add_key(0.0f, start_val, TangentMode::Linear);
    curve.add_key(duration, end_val, TangentMode::Linear);
    return curve;
}

FloatCurve FloatCurve::ease_in_out(float start_val, float end_val, float duration) {
    FloatCurve curve;
    curve.add_key(Keyframe(0.0f, start_val, 0.0f, 0.0f, TangentMode::Smooth));
    curve.add_key(Keyframe(duration, end_val, 0.0f, 0.0f, TangentMode::Smooth));
    return curve;
}

} // namespace nf
