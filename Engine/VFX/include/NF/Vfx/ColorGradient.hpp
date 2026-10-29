#pragma once

#include <NF/Core/Math.hpp>
#include <vector>
#include <algorithm>

namespace nf::vfx {

struct ColorStop {
    float time = 0.0f; // normalized [0.0, 1.0]
    Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
};

/// High-performance multi-stop color and alpha gradient evaluator over particle lifetime.
class ColorGradient {
public:
    ColorGradient() = default;

    void add_stop(float time, const Vec4& color) {
        m_stops.push_back({std::clamp(time, 0.0f, 1.0f), color});
        std::sort(m_stops.begin(), m_stops.end(), [](const ColorStop& a, const ColorStop& b) {
            return a.time < b.time;
        });
    }

    size_t stop_count() const { return m_stops.size(); }
    void clear() { m_stops.clear(); }

    Vec4 evaluate(float t) const {
        if (m_stops.empty()) {
            return Vec4(1.0f, 1.0f, 1.0f, 1.0f);
        }

        t = std::clamp(t, 0.0f, 1.0f);

        if (t <= m_stops.front().time) {
            return m_stops.front().color;
        }
        if (t >= m_stops.back().time) {
            return m_stops.back().color;
        }

        for (size_t i = 0; i + 1 < m_stops.size(); ++i) {
            if (t >= m_stops[i].time && t <= m_stops[i + 1].time) {
                float span = m_stops[i + 1].time - m_stops[i].time;
                float factor = (span > 0.0001f) ? (t - m_stops[i].time) / span : 0.0f;
                return lerp(m_stops[i].color, m_stops[i + 1].color, factor);
            }
        }
        return m_stops.back().color;
    }

    // Built-in presets
    static ColorGradient fire() {
        ColorGradient cg;
        cg.add_stop(0.0f, Vec4(1.0f, 1.0f, 0.8f, 1.0f)); // Bright white/yellow
        cg.add_stop(0.3f, Vec4(1.0f, 0.6f, 0.1f, 0.9f)); // Intense orange
        cg.add_stop(0.7f, Vec4(0.8f, 0.1f, 0.0f, 0.6f)); // Deep red
        cg.add_stop(1.0f, Vec4(0.1f, 0.1f, 0.1f, 0.0f)); // Fading ash smoke
        return cg;
    }

    static ColorGradient smoke() {
        ColorGradient cg;
        cg.add_stop(0.0f, Vec4(0.7f, 0.7f, 0.7f, 0.8f));
        cg.add_stop(0.5f, Vec4(0.4f, 0.4f, 0.4f, 0.5f));
        cg.add_stop(1.0f, Vec4(0.2f, 0.2f, 0.2f, 0.0f));
        return cg;
    }

    static ColorGradient electric_blue() {
        ColorGradient cg;
        cg.add_stop(0.0f, Vec4(0.8f, 0.9f, 1.0f, 1.0f));
        cg.add_stop(0.4f, Vec4(0.2f, 0.6f, 1.0f, 0.8f));
        cg.add_stop(1.0f, Vec4(0.0f, 0.1f, 0.5f, 0.0f));
        return cg;
    }

private:
    static Vec4 lerp(const Vec4& a, const Vec4& b, float factor) {
        return Vec4(
            a.x + (b.x - a.x) * factor,
            a.y + (b.y - a.y) * factor,
            a.z + (b.z - a.z) * factor,
            a.w + (b.w - a.w) * factor
        );
    }

    std::vector<ColorStop> m_stops;
};

} // namespace nf::vfx
