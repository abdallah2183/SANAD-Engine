// NF/Gameplay/Quest.cpp — staged quest log.

#include <NF/Gameplay/Quest.hpp>

#include <sstream>

namespace nf::gameplay {

bool QuestStage::complete() const {
    if (objectives.empty()) return true; // milestone stage: arriving completes it
    for (const auto& o : objectives) {
        if (!o.complete()) return false;
    }
    return true;
}

const QuestStage* Quest::current_stage() const {
    if (state != QuestState::Active) return nullptr;
    if (stage_index >= stages.size()) return nullptr;
    return &stages[stage_index];
}

bool QuestLog::define(Quest quest) {
    if (quest.id.empty() || find(quest.id)) return false;
    quest.state = QuestState::Inactive;
    quest.stage_index = 0;
    m_quests.push_back(std::move(quest));
    return true;
}

bool QuestLog::start(const std::string& quest_id) {
    Quest* q = find_mut(quest_id);
    if (!q || q->state != QuestState::Inactive) return false;
    q->state = QuestState::Active;
    q->stage_index = 0;
    // A quest with no stages completes immediately (degenerate but sane).
    if (q->stages.empty()) {
        q->state = QuestState::Completed;
    } else if (q->stages[0].complete()) {
        // First stage already complete (milestone with no objectives).
        q->stage_index = 1;
        if (q->stage_index >= q->stages.size()) q->state = QuestState::Completed;
    }
    return true;
}

bool QuestLog::fail(const std::string& quest_id) {
    Quest* q = find_mut(quest_id);
    if (!q || q->state != QuestState::Active) return false;
    q->state = QuestState::Failed;
    return true;
}

bool QuestLog::advance(const std::string& quest_id, const std::string& objective_id, u32 amount) {
    if (amount == 0) return false;
    Quest* q = find_mut(quest_id);
    if (!q || q->state != QuestState::Active) return false;
    if (q->stage_index >= q->stages.size()) return false;
    QuestStage& stage = q->stages[q->stage_index];
    bool matched = false;
    for (auto& o : stage.objectives) {
        if (o.id == objective_id && !o.complete()) {
            o.progress += amount;
            if (o.progress > o.target) o.progress = o.target; // saturate, never wrap
            matched = true;
            break;
        }
    }
    if (!matched) return false;
    // Advance past every complete stage (milestone stages chain forward).
    while (q->stage_index < q->stages.size() && q->stages[q->stage_index].complete()) {
        ++q->stage_index;
    }
    if (q->stage_index >= q->stages.size()) {
        q->state = QuestState::Completed;
    }
    return true;
}

const Quest* QuestLog::find(const std::string& quest_id) const {
    for (const auto& q : m_quests) {
        if (q.id == quest_id) return &q;
    }
    return nullptr;
}

Quest* QuestLog::find_mut(const std::string& quest_id) {
    for (auto& q : m_quests) {
        if (q.id == quest_id) return &q;
    }
    return nullptr;
}

usize QuestLog::active_count() const {
    usize n = 0;
    for (const auto& q : m_quests) {
        if (q.state == QuestState::Active) ++n;
    }
    return n;
}

usize QuestLog::completed_count() const {
    usize n = 0;
    for (const auto& q : m_quests) {
        if (q.state == QuestState::Completed) ++n;
    }
    return n;
}

namespace {

// Labels/titles may contain anything except newlines; ids may not contain
// the field separators. Validation keeps the format greppable.
std::string escape_field(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '|') out += "\\p";
        else if (c == ';') out += "\\s";
        else if (c == '\n') out += ' ';
        else out += c;
    }
    return out;
}

std::string unescape_field(const std::string& s) {
    std::string out;
    for (usize i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            out += (n == 'p') ? '|' : (n == 's') ? ';' : n;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<std::string> split_top(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::string cur;
    for (usize i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            cur += s[i++];
            cur += s[i];
        } else if (s[i] == sep) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += s[i];
        }
    }
    parts.push_back(cur);
    return parts;
}

} // namespace

std::string QuestLog::serialize() const {
    // quest|id|title|state|stage_index|stage_count;stage|id|label|obj_count;...
    // objective fields: id|label|target|progress (joined with ';').
    std::ostringstream out;
    for (const auto& q : m_quests) {
        out << "quest|" << escape_field(q.id) << '|' << escape_field(q.title) << '|'
            << static_cast<int>(q.state) << '|' << q.stage_index << '|' << q.stages.size();
        for (const auto& st : q.stages) {
            out << ";stage|" << escape_field(st.id) << '|' << escape_field(st.label) << '|'
                << st.objectives.size();
            for (const auto& o : st.objectives) {
                out << ";obj|" << escape_field(o.id) << '|' << escape_field(o.label) << '|'
                    << o.target << '|' << o.progress;
            }
        }
        out << '\n';
    }
    return out.str();
}

bool QuestLog::parse(const std::string& text) {
    std::vector<Quest> parsed;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const auto parts = split_top(line, ';');
        if (parts.empty()) continue;
        const auto head = split_top(parts[0], '|');
        // quest|id|title|state|stage_index|stage_count
        if (head.size() != 6 || head[0] != "quest") continue;
        Quest q;
        q.id = unescape_field(head[1]);
        q.title = unescape_field(head[2]);
        if (q.id.empty()) continue;
        int state = 0;
        usize stage_index = 0;
        usize stage_count = 0;
        try {
            state = std::stoi(head[3]);
            stage_index = static_cast<usize>(std::stoul(head[4]));
            stage_count = static_cast<usize>(std::stoul(head[5]));
        } catch (...) {
            continue;
        }
        if (state < 0 || state > 3) continue;
        q.state = static_cast<QuestState>(state);
        q.stage_index = stage_index;
        bool line_ok = true;
        usize cursor = 1;
        for (usize s = 0; s < stage_count && line_ok; ++s) {
            if (cursor >= parts.size()) {
                line_ok = false;
                break;
            }
            const auto sp = split_top(parts[cursor++], '|');
            // stage|id|label|obj_count
            if (sp.size() != 4 || sp[0] != "stage") {
                line_ok = false;
                break;
            }
            QuestStage stage;
            stage.id = unescape_field(sp[1]);
            stage.label = unescape_field(sp[2]);
            usize obj_count = 0;
            try {
                obj_count = static_cast<usize>(std::stoul(sp[3]));
            } catch (...) {
                line_ok = false;
                break;
            }
            for (usize o = 0; o < obj_count; ++o) {
                if (cursor >= parts.size()) {
                    line_ok = false;
                    break;
                }
                const auto op = split_top(parts[cursor++], '|');
                // obj|id|label|target|progress
                if (op.size() != 5 || op[0] != "obj") {
                    line_ok = false;
                    break;
                }
                QuestObjective obj;
                obj.id = unescape_field(op[1]);
                obj.label = unescape_field(op[2]);
                try {
                    obj.target = static_cast<u32>(std::stoul(op[3]));
                    obj.progress = static_cast<u32>(std::stoul(op[4]));
                } catch (...) {
                    line_ok = false;
                    break;
                }
                if (obj.progress > obj.target) obj.progress = obj.target;
                stage.objectives.push_back(std::move(obj));
            }
            q.stages.push_back(std::move(stage));
        }
        if (!line_ok) continue;
        // Clamp stage_index into range (a newer save with more stages loads
        // on an older build without reading out of bounds).
        if (q.stage_index > q.stages.size()) q.stage_index = q.stages.size();
        parsed.push_back(std::move(q));
    }
    m_quests = std::move(parsed);
    return true;
}

} // namespace nf::gameplay
