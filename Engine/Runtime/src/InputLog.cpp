// NF/Runtime/InputLog.cpp — deterministic input record + replay

#include <NF/Runtime/InputLog.hpp>

#include <algorithm>
#include <charconv>
#include <sstream>

namespace nf::runtime {

namespace {

/// Nine significant digits, the same as the scene format: an input log that
/// recorded 0.1 and replayed 0.0999999 would move the replayed entity a
/// different distance than the original, which defeats the log's only purpose.
std::string write_float(f32 v) {
    char buf[64] = {0};
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    return buf;
}

/// from_chars over an integer, the engine's own idiom: refuses a bad line
/// instead of leaving the destination uninitialised.
bool parse_u64(std::string_view text, u64& out) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    const auto res = std::from_chars(text.data(), text.data() + text.size(), out);
    return res.ec == std::errc();
}

bool parse_f32(std::string_view text, f32& out) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\r')) text.remove_suffix(1);
    const auto res = std::from_chars(text.data(), text.data() + text.size(), out);
    return res.ec == std::errc();
}

} // namespace

void InputLog::write(const std::string& action, f32 value) {
    m_pending[action] = value;
}

void InputLog::seal(u64 frame) {
    // An empty frame is recorded as an empty frame. Skipping it would make the
    // frame index a lie for everything after it.
    InputFrame f;
    f.frame  = frame;
    f.values.reserve(m_pending.size());
    for (const auto& kv : m_pending) f.values.push_back(kv);
    std::sort(f.values.begin(), f.values.end(),
              [](const std::pair<std::string, f32>& a,
                 const std::pair<std::string, f32>& b) { return a.first < b.first; });
    m_frames.push_back(std::move(f));
    m_pending.clear();
}

void InputLog::clear() {
    m_frames.clear();
    m_pending.clear();
}

f32 InputLog::value_at(u64 frame, std::string_view action) const {
    if (m_frames.empty()) return 0.0f;
    if (frame >= m_frames.size()) return 0.0f;
    const InputFrame& f = m_frames[static_cast<usize>(frame)];
    const auto it = std::lower_bound(
        f.values.begin(), f.values.end(), action,
        [](const std::pair<std::string, f32>& a, std::string_view b) { return a.first < b; });
    if (it == f.values.end() || it->first != action) return 0.0f;
    return it->second;
}

std::string InputLog::serialize() const {
    std::ostringstream out;
    out << "# NOVAForge InputLog v1\n";
    out << "frame_count: " << m_frames.size() << "\n";
    for (const InputFrame& f : m_frames) {
        out << "@" << f.frame;
        for (const auto& kv : f.values) {
            out << " " << kv.first << "=" << write_float(kv.second);
        }
        out << "\n";
    }
    return out.str();
}

InputLog InputLog::deserialize(std::string_view text) {
    InputLog log;
    std::istringstream stream((std::string(text)));
    std::string line;
    u64 expected_frame = 0;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.rfind("#", 0) == 0) continue;
        if (line.rfind("frame_count:", 0) == 0) continue;  // informational only

        if (line.rfind("@", 0) != 0) continue;
        std::string_view rest(line);
        rest.remove_prefix(1);

        u64 frame = 0;
        usize split = rest.find(' ');
        if (split == std::string_view::npos) split = rest.size();
        if (!parse_u64(rest.substr(0, split), frame)) return InputLog{};

        rest = rest.substr(split);
        // value_at() indexes storage directly and ignores the recorded index, so
        // "frame i is stored at i" is the invariant the whole log rests on. A log
        // whose lines were reordered would silently replay at the wrong time — a
        // desync that looks exactly like a correct replay — so the whole log is
        // refused rather than the stray line dropped, which would hand back a
        // shorter history as though it were the one that was recorded.
        if (frame != expected_frame) return InputLog{};
        ++expected_frame;

        InputFrame f;
        f.frame = frame;
        while (!rest.empty()) {
            while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
            if (rest.empty()) break;
            const usize eq = rest.find('=');
            if (eq == std::string_view::npos) break;
            const std::string name(rest.substr(0, eq));
            rest = rest.substr(eq + 1);
            const usize end = rest.find(' ');
            const std::string_view raw =
                (end == std::string_view::npos) ? rest : rest.substr(0, end);
            f32 v = 0.0f;
            if (parse_f32(raw, v)) f.values.emplace_back(name, v);
            if (end == std::string_view::npos) break;
            rest = rest.substr(end);
        }
        std::sort(f.values.begin(), f.values.end(),
                  [](const std::pair<std::string, f32>& a,
                     const std::pair<std::string, f32>& b) { return a.first < b.first; });
        log.m_frames.push_back(std::move(f));
    }
    return log;
}

bool InputRecorder::action_pressed(std::string_view action) const {
    const bool pressed = (m_real != nullptr) && m_real->action_pressed(action);
    m_log.write(std::string(action), pressed ? 1.0f : 0.0f);
    return pressed;
}

f32 InputRecorder::action_axis(std::string_view action) const {
    const f32 axis = (m_real != nullptr) ? m_real->action_axis(action) : 0.0f;
    m_log.write(std::string(action), axis);
    return axis;
}

void InputReplay::advance(u64 frame) {
    // Clamp at the end rather than wrapping: replaying past the recording would
    // either freeze on the last frame (holding a button forever) or restart the
    // log, both of which invent input the session never had.
    if (frame >= m_log->frame_count()) {
        m_frame = m_log->frame_count() == 0 ? 0 : m_log->frame_count() - 1;
        return;
    }
    m_frame = frame;
}

bool InputReplay::action_pressed(std::string_view action) const {
    return m_log->value_at(m_frame, action) != 0.0f;
}

f32 InputReplay::action_axis(std::string_view action) const {
    return m_log->value_at(m_frame, action);
}

} // namespace nf::runtime
