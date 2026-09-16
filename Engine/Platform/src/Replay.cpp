// NF/Platform/Replay.cpp — input recording & playback.

#include <NF/Platform/Replay.hpp>

#include <cstring>

namespace nf {

void InputRecorder::capture_tick(const InputState& state) {
    const usize key_count = static_cast<usize>(KeyCode::Count);
    for (usize k = 0; k < key_count; ++k) {
        if (state.keys_pressed_this_frame[k]) {
            m_events.push_back(ReplayEvent{m_tick, ReplayEventType::KeyDown,
                                           static_cast<u32>(k), 0.0f, 0.0f});
        }
        if (state.keys_released_this_frame[k]) {
            m_events.push_back(ReplayEvent{m_tick, ReplayEventType::KeyUp,
                                           static_cast<u32>(k), 0.0f, 0.0f});
        }
    }
    const usize btn_count = static_cast<usize>(MouseButton::Count);
    for (usize b = 0; b < btn_count; ++b) {
        if (state.mouse_pressed_this_frame[b]) {
            m_events.push_back(ReplayEvent{m_tick, ReplayEventType::MouseDown,
                                           static_cast<u32>(b), 0.0f, 0.0f});
        }
        if (state.mouse_released_this_frame[b]) {
            m_events.push_back(ReplayEvent{m_tick, ReplayEventType::MouseUp,
                                           static_cast<u32>(b), 0.0f, 0.0f});
        }
    }
    if (state.mouse_delta_x != 0.0f || state.mouse_delta_y != 0.0f) {
        m_events.push_back(ReplayEvent{m_tick, ReplayEventType::MouseMove, 0,
                                       state.mouse_x, state.mouse_y});
    }
    if (state.scroll_delta != 0.0f) {
        m_events.push_back(ReplayEvent{m_tick, ReplayEventType::MouseScroll, 0,
                                       state.scroll_delta, 0.0f});
    }
    ++m_tick;
}

void InputRecorder::clear() {
    m_events.clear();
    m_tick = 0;
}

namespace {

void push_u32(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v & 0xFF));
    out.push_back(static_cast<u8>((v >> 8) & 0xFF));
    out.push_back(static_cast<u8>((v >> 16) & 0xFF));
    out.push_back(static_cast<u8>((v >> 24) & 0xFF));
}

void push_f32(std::vector<u8>& out, float v) {
    u32 u = 0;
    std::memcpy(&u, &v, 4);
    push_u32(out, u);
}

u32 read_u32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

float read_f32(const u8* p) {
    u32 u = read_u32(p);
    float v = 0.0f;
    std::memcpy(&v, &u, 4);
    return v;
}

} // namespace

std::vector<u8> InputRecorder::save() const {
    std::vector<u8> out;
    out.push_back('N');
    out.push_back('F');
    out.push_back('R');
    out.push_back('P');
    push_u32(out, 1); // format version
    push_u32(out, static_cast<u32>(m_events.size()));
    // Total ticks (the recorded span INCLUDING trailing silence), so a load
    // resumes capturing and sized playback at exactly the right tick.
    push_u32(out, m_tick);
    for (const auto& e : m_events) {
        push_u32(out, e.tick);
        out.push_back(static_cast<u8>(e.type));
        out.push_back(0);
        out.push_back(0);
        out.push_back(0); // padding: fixed 16-byte records
        push_u32(out, e.code);
        push_f32(out, e.x);
        push_f32(out, e.y);
    }
    return out;
}

bool InputRecorder::load(const u8* data, usize size, std::string& out_error) {
    m_events.clear();
    m_tick = 0;
    // Header: magic(4) version(4) event_count(4) tick_count(4).
    if (!data || size < 16) {
        out_error = "replay data too short";
        return false;
    }
    if (data[0] != 'N' || data[1] != 'F' || data[2] != 'R' || data[3] != 'P') {
        out_error = "not a replay stream (bad magic)";
        return false;
    }
    if (read_u32(data + 4) != 1) {
        out_error = "unsupported replay version";
        return false;
    }
    const u32 count = read_u32(data + 8);
    const u32 ticks = read_u32(data + 12);
    // Record: tick u32 + type u8 + 3 pad + code u32 + x f32 + y f32 = 20 bytes.
    constexpr usize kHeader = 16;
    constexpr usize kRecord = 20;
    if (size != kHeader + static_cast<usize>(count) * kRecord) {
        out_error = "replay size mismatch";
        return false;
    }
    u32 last_tick = 0;
    for (u32 i = 0; i < count; ++i) {
        const u8* p = data + kHeader + i * kRecord;
        ReplayEvent e;
        e.tick = read_u32(p);
        const u8 type = p[4];
        if (type > static_cast<u8>(ReplayEventType::MouseScroll)) {
            out_error = "replay holds an unknown event type";
            m_events.clear();
            return false;
        }
        e.type = static_cast<ReplayEventType>(type);
        e.code = read_u32(p + 8);
        // Codes beyond the enums are clamped at playback, not load: old
        // recordings stay readable when the enum grows.
        e.x = read_f32(p + 12);
        e.y = read_f32(p + 16);
        if (e.tick < last_tick) {
            out_error = "replay events out of order";
            m_events.clear();
            return false;
        }
        last_tick = e.tick;
        m_events.push_back(e);
    }
    if (!m_events.empty()) {
        // Ticks must at least cover the last event; a longer span (trailing
        // silence) restores verbatim.
        if (ticks < m_events.back().tick + 1) {
            out_error = "replay tick span shorter than its events";
            m_events.clear();
            return false;
        }
        m_tick = ticks;
    }
    return true;
}

InputPlayback::InputPlayback(std::vector<ReplayEvent> events)
    : m_events(std::move(events)) {}

void InputPlayback::apply_tick(InputSystem& input, u32 tick) {
    // Events are tick-ordered; the cursor only moves forward.
    while (m_cursor < m_events.size() && m_events[m_cursor].tick < tick) {
        ++m_cursor; // skipped ticks (seek forward) never replay
    }
    while (m_cursor < m_events.size() && m_events[m_cursor].tick == tick) {
        const ReplayEvent& e = m_events[m_cursor++];
        switch (e.type) {
            case ReplayEventType::KeyDown:
                if (e.code < static_cast<u32>(KeyCode::Count)) {
                    input.on_key_down(static_cast<KeyCode>(e.code));
                }
                break;
            case ReplayEventType::KeyUp:
                if (e.code < static_cast<u32>(KeyCode::Count)) {
                    input.on_key_up(static_cast<KeyCode>(e.code));
                }
                break;
            case ReplayEventType::MouseDown:
                if (e.code < static_cast<u32>(MouseButton::Count)) {
                    input.on_mouse_button(static_cast<MouseButton>(e.code), true);
                }
                break;
            case ReplayEventType::MouseUp:
                if (e.code < static_cast<u32>(MouseButton::Count)) {
                    input.on_mouse_button(static_cast<MouseButton>(e.code), false);
                }
                break;
            case ReplayEventType::MouseMove:
                input.on_mouse_move(e.x, e.y);
                break;
            case ReplayEventType::MouseScroll:
                input.on_mouse_scroll(e.x);
                break;
        }
    }
}

u32 InputPlayback::tick_count() const {
    if (m_events.empty()) return 0;
    return m_events.back().tick + 1;
}

void InputPlayback::reset() {
    m_cursor = 0;
}

} // namespace nf
