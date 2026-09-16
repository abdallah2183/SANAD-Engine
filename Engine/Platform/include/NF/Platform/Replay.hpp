#pragma once

// NF/Platform/Replay.hpp — deterministic input recording & playback.
//
// The engine is deterministic given the same input stream (design doc 114,
// 115): InputRecorder diffs InputSystem states once per tick into an event
// list; InputPlayback re-injects those events into a fresh InputSystem, so a
// recorded session replays bit-identically (demos, bug repros, automated
// gameplay verification).
//
// Contract: call recorder.capture_tick(input.state()) once per frame AFTER
// window.poll_events() and BEFORE the next InputSystem::begin_frame() — the
// per-frame pressed/released sets are the diff source. Playback mirrors it:
// playback.apply_tick(input, tick) after begin_frame(), before game logic.

#include <NF/Core/Types.hpp>
#include <NF/Platform/InputSystem.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace nf {

enum class ReplayEventType : u8 {
    KeyDown = 0,
    KeyUp = 1,
    MouseDown = 2,
    MouseUp = 3,
    MouseMove = 4,
    MouseScroll = 5,
};

struct ReplayEvent {
    u32 tick = 0;
    ReplayEventType type = ReplayEventType::KeyDown;
    u32 code = 0; // KeyCode or MouseButton value
    float x = 0.0f; // mouse position (MouseMove) or scroll delta (MouseScroll)
    float y = 0.0f; // mouse position (MouseMove)

    bool operator==(const ReplayEvent& o) const {
        return tick == o.tick && type == o.type && code == o.code && x == o.x && y == o.y;
    }
};

class InputRecorder {
public:
    /// Diffs `state` against the previous tick and appends events for the
    /// current tick, then advances the tick. Call once per frame.
    void capture_tick(const InputState& state);

    const std::vector<ReplayEvent>& events() const { return m_events; }
    u32 tick() const { return m_tick; }
    bool empty() const { return m_events.empty(); }
    void clear();

    /// Deterministic bytes (little-endian).
    std::vector<u8> save() const; // 16-byte header + 20 bytes per event
    bool load(const u8* data, usize size, std::string& out_error);

private:
    std::vector<ReplayEvent> m_events;
    u32 m_tick = 0;
};

class InputPlayback {
public:
    InputPlayback() = default;
    explicit InputPlayback(std::vector<ReplayEvent> events);

    /// Injects every event recorded for `tick` into `input`. Ticks with no
    /// events are valid (silence replays as silence). Out-of-order ticks
    /// are ignored, never applied twice: playback is monotonic.
    void apply_tick(InputSystem& input, u32 tick);

    u32 tick_count() const; // one past the last recorded tick (0 when empty)
    usize event_count() const { return m_events.size(); }
    void reset(); // rewind to tick 0 (events kept)

private:
    std::vector<ReplayEvent> m_events;
    usize m_cursor = 0;
};

} // namespace nf
