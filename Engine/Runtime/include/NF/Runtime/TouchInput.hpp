#pragma once

#include <NF/Gameplay/GameplayModule.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace nf::runtime {

/// A rectangular on-screen control in normalized viewport coordinates.
/// More than one region may map to an action; signed values support axes.
struct TouchControl {
    std::string action;
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
    float value = 1.0f;
};

/// Platform-independent multi-touch action source. The platform backend must
/// send normalized positions and cancel_all() on focus loss / app suspension.
/// All calls belong on the game thread; this class does not receive OS events.
class TouchInputSource final : public gameplay::IInputSource {
public:
    bool add_control(TouchControl control);
    void clear_controls();
    bool touch_down(std::int64_t id, float x, float y);
    bool touch_move(std::int64_t id, float x, float y);
    void touch_up(std::int64_t id);
    void cancel_all();
    [[nodiscard]] bool action_pressed(std::string_view action) const override;
    [[nodiscard]] float action_axis(std::string_view action) const override;

private:
    struct Finger { std::int64_t id; float x; float y; };
    [[nodiscard]] bool active(const TouchControl& control) const;
    std::vector<TouchControl> m_controls;
    std::vector<Finger> m_fingers;
};

} // namespace nf::runtime
