#pragma once

#include <NF/Core/Types.hpp>
#include <vector>
#include <algorithm>

namespace nf {

enum class TangentMode : u8 {
    Linear = 0,
    Constant,
    Smooth
};

enum class ExtrapolationMode : u8 {
    Clamp = 0,
    Loop,
    PingPong
};

struct Keyframe {
    float time = 0.0f;
    float value = 0.0f;
    float in_tangent = 0.0f;
    float out_tangent = 0.0f;
    TangentMode mode = TangentMode::Smooth;

    Keyframe() = default;
    Keyframe(float t, float v, TangentMode m = TangentMode::Smooth)
        : time(t), value(v), mode(m) {}
    Keyframe(float t, float v, float in_tan, float out_tan, TangentMode m = TangentMode::Smooth)
        : time(t), value(v), in_tangent(in_tan), out_tangent(out_tan), mode(m) {}
};

/// Animation curve supporting Hermite cubic splines, constant stepping, and linear interpolation.
class FloatCurve {
public:
    FloatCurve() = default;

    /// Adds a keyframe at the specified time, maintaining sorted order.
    size_t add_key(float time, float value, TangentMode mode = TangentMode::Smooth);
    size_t add_key(const Keyframe& key);

    /// Removes a keyframe by index.
    bool remove_key(size_t index);

    /// Clears all keyframes.
    void clear();

    /// Evaluates the curve value at the given time.
    float evaluate(float time) const;

    /// Number of keyframes in the curve.
    size_t key_count() const { return m_keys.size(); }

    /// Access keyframe by index.
    const Keyframe& key(size_t index) const { return m_keys[index]; }
    Keyframe& key(size_t index) { return m_keys[index]; }

    /// Computes and assigns smooth tangents across all keyframes based on Catmull-Rom slopes.
    void auto_smooth_tangents();

    float start_time() const { return m_keys.empty() ? 0.0f : m_keys.front().time; }
    float end_time() const { return m_keys.empty() ? 0.0f : m_keys.back().time; }
    float duration() const { return std::max(0.0f, end_time() - start_time()); }

    ExtrapolationMode pre_extrapolation = ExtrapolationMode::Clamp;
    ExtrapolationMode post_extrapolation = ExtrapolationMode::Clamp;

    /// Preset curve generators
    static FloatCurve linear(float start_val, float end_val, float duration = 1.0f);
    static FloatCurve ease_in_out(float start_val, float end_val, float duration = 1.0f);

private:
    float apply_extrapolation(float time) const;

    std::vector<Keyframe> m_keys;
};

} // namespace nf
