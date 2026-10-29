#pragma once

#include <NF/Core/Types.hpp>
#include <cmath>
#include <algorithm>

namespace nf {

enum class EaseType : u8 {
    Linear = 0,
    InQuad, OutQuad, InOutQuad,
    InCubic, OutCubic, InOutCubic,
    InQuart, OutQuart, InOutQuart,
    InQuint, OutQuint, InOutQuint,
    InSine, OutSine, InOutSine,
    InExpo, OutExpo, InOutExpo,
    InCirc, OutCirc, InOutCirc,
    InElastic, OutElastic, InOutElastic,
    InBack, OutBack, InOutBack,
    InBounce, OutBounce, InOutBounce,
    Count
};

namespace easing {

static constexpr float kPi = 3.14159265358979323846f;
static constexpr float kHalfPi = kPi * 0.5f;

inline float linear(float t) {
    return t;
}

// Quadratic
inline float in_quad(float t) {
    return t * t;
}
inline float out_quad(float t) {
    return t * (2.0f - t);
}
inline float in_out_quad(float t) {
    return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
}

// Cubic
inline float in_cubic(float t) {
    return t * t * t;
}
inline float out_cubic(float t) {
    float f = t - 1.0f;
    return f * f * f + 1.0f;
}
inline float in_out_cubic(float t) {
    return t < 0.5f ? 4.0f * t * t * t : (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f;
}

// Quartic
inline float in_quart(float t) {
    return t * t * t * t;
}
inline float out_quart(float t) {
    float f = t - 1.0f;
    return 1.0f - f * f * f * f;
}
inline float in_out_quart(float t) {
    if (t < 0.5f) {
        return 8.0f * t * t * t * t;
    }
    float f = t - 1.0f;
    return 1.0f - 8.0f * f * f * f * f;
}

// Quintic
inline float in_quint(float t) {
    return t * t * t * t * t;
}
inline float out_quint(float t) {
    float f = t - 1.0f;
    return f * f * f * f * f + 1.0f;
}
inline float in_out_quint(float t) {
    if (t < 0.5f) {
        return 16.0f * t * t * t * t * t;
    }
    float f = 2.0f * t - 2.0f;
    return 0.5f * f * f * f * f * f + 1.0f;
}

// Sine
inline float in_sine(float t) {
    return 1.0f - std::cos(t * kHalfPi);
}
inline float out_sine(float t) {
    return std::sin(t * kHalfPi);
}
inline float in_out_sine(float t) {
    return 0.5f * (1.0f - std::cos(kPi * t));
}

// Exponential
inline float in_expo(float t) {
    return (t <= 0.0f) ? 0.0f : std::pow(2.0f, 10.0f * (t - 1.0f));
}
inline float out_expo(float t) {
    return (t >= 1.0f) ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
}
inline float in_out_expo(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    if (t < 0.5f) {
        return 0.5f * std::pow(2.0f, 20.0f * t - 10.0f);
    }
    return 1.0f - 0.5f * std::pow(2.0f, -20.0f * t + 10.0f);
}

// Circular
inline float in_circ(float t) {
    return 1.0f - std::sqrt(std::max(0.0f, 1.0f - t * t));
}
inline float out_circ(float t) {
    float f = t - 1.0f;
    return std::sqrt(std::max(0.0f, 1.0f - f * f));
}
inline float in_out_circ(float t) {
    if (t < 0.5f) {
        return 0.5f * (1.0f - std::sqrt(std::max(0.0f, 1.0f - 4.0f * t * t)));
    }
    float f = 2.0f * t - 2.0f;
    return 0.5f * (std::sqrt(std::max(0.0f, 1.0f - f * f)) + 1.0f);
}

// Elastic
inline float in_elastic(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return -std::pow(2.0f, 10.0f * (t - 1.0f)) * std::sin((t - 1.1f) * 5.0f * kPi);
}
inline float out_elastic(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return std::pow(2.0f, -10.0f * t) * std::sin((t - 0.1f) * 5.0f * kPi) + 1.0f;
}
inline float in_out_elastic(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    t *= 2.0f;
    if (t < 1.0f) {
        return -0.5f * std::pow(2.0f, 10.0f * (t - 1.0f)) * std::sin((t - 1.1f) * 5.0f * kPi);
    }
    return 0.5f * std::pow(2.0f, -10.0f * (t - 1.0f)) * std::sin((t - 1.1f) * 5.0f * kPi) + 1.0f;
}

// Back
inline float in_back(float t) {
    constexpr float s = 1.70158f;
    return t * t * ((s + 1.0f) * t - s);
}
inline float out_back(float t) {
    constexpr float s = 1.70158f;
    float f = t - 1.0f;
    return f * f * ((s + 1.0f) * f + s) + 1.0f;
}
inline float in_out_back(float t) {
    constexpr float s = 1.70158f * 1.525f;
    t *= 2.0f;
    if (t < 1.0f) {
        return 0.5f * (t * t * ((s + 1.0f) * t - s));
    }
    float f = t - 2.0f;
    return 0.5f * (f * f * ((s + 1.0f) * f + s) + 2.0f);
}

// Bounce
inline float out_bounce(float t) {
    if (t < (1.0f / 2.75f)) {
        return 7.5625f * t * t;
    } else if (t < (2.0f / 2.75f)) {
        float f = t - (1.5f / 2.75f);
        return 7.5625f * f * f + 0.75f;
    } else if (t < (2.5f / 2.75f)) {
        float f = t - (2.25f / 2.75f);
        return 7.5625f * f * f + 0.9375f;
    } else {
        float f = t - (2.625f / 2.75f);
        return 7.5625f * f * f + 0.984375f;
    }
}
inline float in_bounce(float t) {
    return 1.0f - out_bounce(1.0f - t);
}
inline float in_out_bounce(float t) {
    if (t < 0.5f) {
        return in_bounce(t * 2.0f) * 0.5f;
    }
    return out_bounce(t * 2.0f - 1.0f) * 0.5f + 0.5f;
}

} // namespace easing

/// Evaluates any easing equation by enum. Automatically clamps input to [0, 1].
inline float evaluate_ease(EaseType type, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    switch (type) {
        case EaseType::Linear:       return easing::linear(t);
        case EaseType::InQuad:       return easing::in_quad(t);
        case EaseType::OutQuad:      return easing::out_quad(t);
        case EaseType::InOutQuad:    return easing::in_out_quad(t);
        case EaseType::InCubic:      return easing::in_cubic(t);
        case EaseType::OutCubic:     return easing::out_cubic(t);
        case EaseType::InOutCubic:   return easing::in_out_cubic(t);
        case EaseType::InQuart:      return easing::in_quart(t);
        case EaseType::OutQuart:     return easing::out_quart(t);
        case EaseType::InOutQuart:   return easing::in_out_quart(t);
        case EaseType::InQuint:      return easing::in_quint(t);
        case EaseType::OutQuint:     return easing::out_quint(t);
        case EaseType::InOutQuint:   return easing::in_out_quint(t);
        case EaseType::InSine:       return easing::in_sine(t);
        case EaseType::OutSine:      return easing::out_sine(t);
        case EaseType::InOutSine:    return easing::in_out_sine(t);
        case EaseType::InExpo:       return easing::in_expo(t);
        case EaseType::OutExpo:      return easing::out_expo(t);
        case EaseType::InOutExpo:    return easing::in_out_expo(t);
        case EaseType::InCirc:       return easing::in_circ(t);
        case EaseType::OutCirc:      return easing::out_circ(t);
        case EaseType::InOutCirc:    return easing::in_out_circ(t);
        case EaseType::InElastic:    return easing::in_elastic(t);
        case EaseType::OutElastic:   return easing::out_elastic(t);
        case EaseType::InOutElastic: return easing::in_out_elastic(t);
        case EaseType::InBack:       return easing::in_back(t);
        case EaseType::OutBack:      return easing::out_back(t);
        case EaseType::InOutBack:    return easing::in_out_back(t);
        case EaseType::InBounce:     return easing::in_bounce(t);
        case EaseType::OutBounce:    return easing::out_bounce(t);
        case EaseType::InOutBounce:  return easing::in_out_bounce(t);
        default:                     return t;
    }
}

} // namespace nf
