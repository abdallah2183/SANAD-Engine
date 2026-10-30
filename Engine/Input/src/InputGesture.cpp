#include <NF/Input/InputGesture.hpp>

#include <algorithm>

namespace nf::input {

// ----------------------------------------------------------------------------
// InputBuffer
// ----------------------------------------------------------------------------

InputBuffer::InputBuffer(f32 buffer_duration, size_t max_capacity)
    : m_buffer_duration(std::max(0.1f, buffer_duration))
    , m_max_capacity(std::max(size_t(8), max_capacity)) {}

void InputBuffer::record_event(const BufferedInputEvent& event) {
    BufferedInputEvent ev = event;
    ev.timestamp = m_current_time;
    m_events.push_back(std::move(ev));

    while (m_events.size() > m_max_capacity) {
        m_events.pop_front();
    }
}

void InputBuffer::update(f32 dt) {
    m_current_time += std::max(0.0f, dt);

    while (!m_events.empty() && (m_current_time - m_events.front().timestamp) > m_buffer_duration) {
        m_events.pop_front();
    }
}

void InputBuffer::clear() {
    m_events.clear();
}

std::vector<BufferedInputEvent> InputBuffer::recent_events(f32 duration_seconds) const {
    std::vector<BufferedInputEvent> result;
    const f32 cutoff = m_current_time - std::max(0.0f, duration_seconds);

    for (auto it = m_events.rbegin(); it != m_events.rend(); ++it) {
        if (it->timestamp < cutoff) {
            break;
        }
        result.push_back(*it);
    }

    std::reverse(result.begin(), result.end());
    return result;
}

bool InputBuffer::was_pressed(const std::string& action_name, f32 within_seconds) const {
    const f32 cutoff = m_current_time - std::max(0.0f, within_seconds);
    for (auto it = m_events.rbegin(); it != m_events.rend(); ++it) {
        if (it->timestamp < cutoff) {
            break;
        }
        if (it->action_name == action_name && it->type == InputEventType::Pressed) {
            return true;
        }
    }
    return false;
}

bool InputBuffer::was_released(const std::string& action_name, f32 within_seconds) const {
    const f32 cutoff = m_current_time - std::max(0.0f, within_seconds);
    for (auto it = m_events.rbegin(); it != m_events.rend(); ++it) {
        if (it->timestamp < cutoff) {
            break;
        }
        if (it->action_name == action_name && it->type == InputEventType::Released) {
            return true;
        }
    }
    return false;
}

// ----------------------------------------------------------------------------
// InputGestureManager
// ----------------------------------------------------------------------------

InputGestureManager::InputGestureManager() = default;

void InputGestureManager::register_chord(std::string name, std::vector<std::string> required_actions,
                                        f32 window_seconds, std::function<void()> cb)
{
    if (name.empty() || required_actions.size() < 2) {
        return;
    }
    InputChordDef def;
    def.name = std::move(name);
    def.required_actions = std::move(required_actions);
    def.window_seconds = std::max(0.01f, window_seconds);
    def.callback = std::move(cb);
    m_chords.push_back(std::move(def));
}

void InputGestureManager::register_combo(std::string name, std::vector<std::string> sequence,
                                        f32 max_step_gap, f32 max_total_duration,
                                        std::function<void(const std::string& name)> cb)
{
    if (name.empty() || sequence.size() < 2) {
        return;
    }
    InputComboDef def;
    def.name = std::move(name);
    def.sequence = std::move(sequence);
    def.max_step_gap = std::max(0.05f, max_step_gap);
    def.max_total_duration = std::max(def.max_step_gap, max_total_duration);
    def.callback = std::move(cb);
    m_combos.push_back(std::move(def));
}

void InputGestureManager::register_double_tap(const std::string& action_name, f32 tap_window,
                                             std::function<void()> cb)
{
    if (action_name.empty()) {
        return;
    }
    DoubleTapDef def;
    def.action_name = action_name;
    def.tap_window = std::max(0.05f, tap_window);
    def.callback = std::move(cb);
    m_double_taps[action_name] = std::move(def);
}

void InputGestureManager::register_charge(const std::string& action_name, f32 required_charge_time,
                                         std::function<void()> cb)
{
    if (action_name.empty()) {
        return;
    }
    ChargeDef def;
    def.action_name = action_name;
    def.required_charge_time = std::max(0.05f, required_charge_time);
    def.callback = std::move(cb);
    m_charges[action_name] = std::move(def);
}

void InputGestureManager::feed_action_event(const std::string& action_name, InputEventType type) {
    if (action_name.empty()) {
        return;
    }

    BufferedInputEvent ev;
    ev.action_name = action_name;
    ev.type = type;
    ev.timestamp = m_buffer.current_time();
    m_buffer.record_event(ev);

    // 1. Evaluate Double-Tap
    auto dt_it = m_double_taps.find(action_name);
    if (dt_it != m_double_taps.end()) {
        DoubleTapDef& dt_def = dt_it->second;
        if (type == InputEventType::Pressed) {
            const f32 gap = ev.timestamp - dt_def.last_release_time;
            if (dt_def.last_release_time >= 0.0f && gap <= dt_def.tap_window) {
                m_triggered_double_taps_this_frame.push_back(action_name);
                if (dt_def.callback) {
                    dt_def.callback();
                }
                dt_def.last_press_time = -100.0f;
                dt_def.last_release_time = -100.0f;
            } else {
                dt_def.last_press_time = ev.timestamp;
            }
        } else if (type == InputEventType::Released) {
            dt_def.last_release_time = ev.timestamp;
        }
    }

    // 2. Evaluate Charge state
    auto charge_it = m_charges.find(action_name);
    if (charge_it != m_charges.end()) {
        ChargeDef& c_def = charge_it->second;
        if (type == InputEventType::Pressed) {
            c_def.is_held = true;
            c_def.press_time = ev.timestamp;
            c_def.has_triggered = false;
        } else if (type == InputEventType::Released) {
            c_def.is_held = false;
            c_def.press_time = -100.0f;
            c_def.has_triggered = false;
        }
    }

    // 3. Evaluate Chords and Combos on Pressed events
    if (type == InputEventType::Pressed) {
        evaluate_chords();
        evaluate_combos();
    }
}

void InputGestureManager::feed_key_event(KeyCode key, InputEventType type, const std::string& action_alias) {
    BufferedInputEvent ev;
    ev.key = key;
    ev.type = type;
    ev.action_name = action_alias.empty() ? std::to_string(static_cast<u16>(key)) : action_alias;
    ev.timestamp = m_buffer.current_time();
    feed_action_event(ev.action_name, type);
}

void InputGestureManager::feed_gamepad_event(GamepadButton button, InputEventType type, const std::string& action_alias) {
    BufferedInputEvent ev;
    ev.pad_button = button;
    ev.type = type;
    ev.action_name = action_alias.empty() ? std::to_string(static_cast<u8>(button)) : action_alias;
    ev.timestamp = m_buffer.current_time();
    feed_action_event(ev.action_name, type);
}

void InputGestureManager::update(f32 dt) {
    m_triggered_combos_this_frame.clear();
    m_triggered_chords_this_frame.clear();
    m_triggered_double_taps_this_frame.clear();

    m_buffer.update(dt);

    // Evaluate hold charge triggers
    for (auto& pair : m_charges) {
        ChargeDef& c_def = pair.second;
        if (c_def.is_held && !c_def.has_triggered) {
            if ((m_buffer.current_time() - c_def.press_time) >= c_def.required_charge_time) {
                c_def.has_triggered = true;
                if (c_def.callback) {
                    c_def.callback();
                }
            }
        }
    }
}

bool InputGestureManager::is_combo_triggered(const std::string& combo_name) const {
    return std::find(m_triggered_combos_this_frame.begin(),
                     m_triggered_combos_this_frame.end(),
                     combo_name) != m_triggered_combos_this_frame.end();
}

bool InputGestureManager::is_chord_triggered(const std::string& chord_name) const {
    return std::find(m_triggered_chords_this_frame.begin(),
                     m_triggered_chords_this_frame.end(),
                     chord_name) != m_triggered_chords_this_frame.end();
}

bool InputGestureManager::is_double_tap_triggered(const std::string& action_name) const {
    return std::find(m_triggered_double_taps_this_frame.begin(),
                     m_triggered_double_taps_this_frame.end(),
                     action_name) != m_triggered_double_taps_this_frame.end();
}

bool InputGestureManager::is_charged(const std::string& action_name) const {
    auto it = m_charges.find(action_name);
    return it != m_charges.end() ? it->second.has_triggered : false;
}

f32 InputGestureManager::get_held_duration(const std::string& action_name) const {
    auto it = m_charges.find(action_name);
    if (it != m_charges.end() && it->second.is_held) {
        return m_buffer.current_time() - it->second.press_time;
    }
    return 0.0f;
}

void InputGestureManager::clear() {
    m_buffer.clear();
    m_chords.clear();
    m_combos.clear();
    m_double_taps.clear();
    m_charges.clear();
    m_triggered_combos_this_frame.clear();
    m_triggered_chords_this_frame.clear();
    m_triggered_double_taps_this_frame.clear();
}

void InputGestureManager::evaluate_chords() {
    if (m_buffer.events().empty()) {
        return;
    }

    const f32 latest_time = m_buffer.current_time();

    for (const auto& chord : m_chords) {
        bool all_present = true;
        for (const auto& req_action : chord.required_actions) {
            bool found = false;
            for (auto it = m_buffer.events().rbegin(); it != m_buffer.events().rend(); ++it) {
                if ((latest_time - it->timestamp) > chord.window_seconds) {
                    break;
                }
                if (it->action_name == req_action && it->type == InputEventType::Pressed) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                all_present = false;
                break;
            }
        }

        if (all_present) {
            m_triggered_chords_this_frame.push_back(chord.name);
            if (chord.callback) {
                chord.callback();
            }
        }
    }
}

void InputGestureManager::evaluate_combos() {
    const auto& events = m_buffer.events();
    if (events.empty()) {
        return;
    }

    for (const auto& combo : m_combos) {
        if (combo.sequence.empty()) {
            continue;
        }

        // Backward sequence matching
        i32 seq_idx = static_cast<i32>(combo.sequence.size()) - 1;
        f32 next_step_time = -1.0f;
        f32 final_step_time = -1.0f;
        f32 first_step_time = -1.0f;
        bool matched = false;

        for (auto it = events.rbegin(); it != events.rend(); ++it) {
            if (it->type != InputEventType::Pressed) {
                continue;
            }

            if (it->action_name == combo.sequence[static_cast<size_t>(seq_idx)]) {
                if (next_step_time >= 0.0f) {
                    const f32 gap = next_step_time - it->timestamp;
                    if (gap > combo.max_step_gap) {
                        break; // Gap too large
                    }
                } else {
                    final_step_time = it->timestamp;
                }

                next_step_time = it->timestamp;
                first_step_time = it->timestamp;
                seq_idx--;

                if (seq_idx < 0) {
                    // All steps found! Check total duration
                    if ((final_step_time - first_step_time) <= combo.max_total_duration) {
                        matched = true;
                    }
                    break;
                }
            }
        }

        if (matched) {
            m_triggered_combos_this_frame.push_back(combo.name);
            if (combo.callback) {
                combo.callback(combo.name);
            }
            if (combo.consume_inputs) {
                m_buffer.clear();
                break;
            }
        }
    }
}

} // namespace nf::input
