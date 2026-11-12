#include <NF/Runtime/TouchInput.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace nf::runtime {

bool TouchInputSource::add_control(TouchControl control) {
    if (control.action.empty() || !std::isfinite(control.x) || !std::isfinite(control.y) ||
        !std::isfinite(control.width) || !std::isfinite(control.height) || !std::isfinite(control.value) ||
        control.x < 0.0f || control.y < 0.0f || control.width <= 0.0f || control.height <= 0.0f ||
        control.x + control.width > 1.0f || control.y + control.height > 1.0f || control.value == 0.0f) return false;
    control.value = std::clamp(control.value, -1.0f, 1.0f);
    m_controls.push_back(std::move(control));
    return true;
}

void TouchInputSource::clear_controls() { m_controls.clear(); cancel_all(); }

bool TouchInputSource::touch_down(std::int64_t id, float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    for (auto& finger : m_fingers) {
        if (finger.id == id) { finger.x = x; finger.y = y; return true; }
    }
    if (m_fingers.size() >= 32) return false;
    m_fingers.push_back({id, x, y});
    return true;
}

bool TouchInputSource::touch_move(std::int64_t id, float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    for (auto& finger : m_fingers) {
        if (finger.id == id) { finger.x = x; finger.y = y; return true; }
    }
    return false;
}

void TouchInputSource::touch_up(std::int64_t id) {
    std::erase_if(m_fingers, [id](const Finger& finger) { return finger.id == id; });
}

void TouchInputSource::cancel_all() { m_fingers.clear(); }

bool TouchInputSource::active(const TouchControl& c) const {
    return std::any_of(m_fingers.begin(), m_fingers.end(), [&](const Finger& finger) {
        return finger.x >= c.x && finger.x < c.x + c.width && finger.y >= c.y && finger.y < c.y + c.height;
    });
}

bool TouchInputSource::action_pressed(std::string_view action) const {
    for (const auto& c : m_controls) if (c.action == action && active(c)) return true;
    return false;
}

float TouchInputSource::action_axis(std::string_view action) const {
    float result = 0.0f;
    for (const auto& c : m_controls) if (c.action == action && active(c)) result += c.value;
    return std::clamp(result, -1.0f, 1.0f);
}

} // namespace nf::runtime
