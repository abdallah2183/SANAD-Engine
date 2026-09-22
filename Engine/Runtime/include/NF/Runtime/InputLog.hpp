#pragma once

// NF/Runtime/InputLog.hpp — deterministic input record + replay
//
// A save restores *state*. An input log restores *behaviour*: the exact stream
// of inputs a session consumed, replayed into a freshly loaded scene so the same
// simulation runs again to the same place. One is useless for debugging a
// desync without the other, which is why they live in the same phase.
//
// What gets recorded is what gameplay actually polled, not every action a
// device could report. A replay only has to reproduce what the simulation
// consumed, an action nothing read cannot change the outcome, and recording on
// demand means the log is self-describing — no separate action list has to be
// carried alongside it and stay in sync.

#include <NF/Core/Types.hpp>
#include <NF/Gameplay/GameplayModule.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nf::runtime {

/// One frame's worth of reads. Values are sorted by action name so an entry
/// compares byte-for-byte and a log serializes deterministically.
struct InputFrame {
    u64                                       frame = 0;
    std::vector<std::pair<std::string, f32>>  values;
};

class InputLog {
public:
    InputLog() = default;

    /// Records `value` for `action` in the frame currently being written.
    /// Overwrites an earlier write to the same action in the same frame — the
    /// last read before the seal is the one the frame describes.
    void write(const std::string& action, f32 value);

    /// Closes the current frame. A frame in which nothing was read is sealed
    /// anyway and counts: "nothing read this frame" is part of the stream, and
    /// dropping empty frames would shift every later frame's index by one.
    void seal(u64 frame);

    /// Replaces the log's contents. Unsealing is deliberate rather than
    /// appending: a log is the record of one session, and two sessions' frames
    /// do not concatenate into one history.
    void clear();

    [[nodiscard]] const std::vector<InputFrame>& frames() const { return m_frames; }
    [[nodiscard]] u64 frame_count() const { return static_cast<u64>(m_frames.size()); }

    /// Value an action held at `frame`, or 0 when the action was not read then.
    /// A name absent from a frame and a name that read zero are both zero,
    /// which is what makes a log replayable against a scene that reads a
    /// different action set than it once did.
    [[nodiscard]] f32 value_at(u64 frame, std::string_view action) const;

    /// Text form for a save: `@<frame> name=value name=value` per line, values
    /// sorted so two runs of the same session produce identical bytes.
    [[nodiscard]] std::string serialize() const;
    static InputLog deserialize(std::string_view text);

private:
    std::vector<InputFrame>                m_frames;
    std::unordered_map<std::string, f32>   m_pending;
};

/// Wraps a real input source, recording everything gameplay reads through it.
/// Installed by Runtime::begin_input_capture(); the runtime seals one frame per
/// step. `real` may be null — a headless capture records "no source", which is
/// still a replayable stream (of zeros).
class InputRecorder final : public gameplay::IInputSource {
public:
    InputRecorder(gameplay::IInputSource* real, InputLog& log)
        : m_real(real), m_log(log) {}

    /// Seals the frame and hands back the log. After this the recorder is inert;
    /// Runtime::end_input_capture() is what uninstalls it.
    void seal(u64 frame) { m_log.seal(frame); }

    [[nodiscard]] gameplay::IInputSource* real_source() const { return m_real; }
    [[nodiscard]] InputLog& log() const { return m_log; }

    bool action_pressed(std::string_view action) const override;
    f32  action_axis(std::string_view action) const override;

private:
    gameplay::IInputSource* m_real;
    // Written to from the const query methods: IInputSource is a read-only view
    // of a device, and the recorder's whole purpose is to observe those reads.
    // The reference member's const-qualification constrains the reference, not
    // the log it refers to.
    InputLog& m_log;
};

/// Serves a recorded log as if it were a live device. Advanced once frame at a
/// time by the runtime; an action the current frame does not hold answers 0,
/// matching the "unknown action is false, not an error" contract the real
/// sources keep.
class InputReplay final : public gameplay::IInputSource {
public:
    explicit InputReplay(const InputLog& log) : m_log(&log) {}

    /// Moves to `frame`. Replays are allowed to run slower than the recording
    /// (a frame may be served several times) but not faster: advancing past the
    /// end holds the last frame *as it was recorded* rather than inventing input
    /// the session never had. Note that "the last frame" is the last sealed one,
    /// so a session that ended with a frame in which nothing was polled is
    /// replayed as reading nothing from then on — reaching back for the last
    /// frame that had input would hold a button the session had already
    /// released.
    void advance(u64 frame);

    [[nodiscard]] u64 current_frame() const { return m_frame; }

    bool action_pressed(std::string_view action) const override;
    f32  action_axis(std::string_view action) const override;

private:
    const InputLog* m_log = nullptr;
    u64             m_frame = 0;
};

} // namespace nf::runtime
