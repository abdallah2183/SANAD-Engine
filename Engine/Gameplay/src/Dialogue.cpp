// NF/Gameplay/Dialogue.cpp — dialogue trees.

#include <NF/Gameplay/Dialogue.hpp>

#include <sstream>

namespace nf::gameplay {

bool DialogueTree::add_node(DialogueNode node) {
    if (node.id.empty() || find(node.id)) return false;
    m_nodes.push_back(std::move(node));
    return true;
}

const DialogueNode* DialogueTree::find(const std::string& id) const {
    for (const auto& n : m_nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

bool DialogueTree::validate(const std::string& start_node, std::string& out_error) const {
    if (!find(start_node)) {
        out_error = std::string("dialogue start node missing: ") + start_node;
        return false;
    }
    for (const auto& n : m_nodes) {
        if (!n.next.empty() && !find(n.next)) {
            out_error = "node '" + n.id + "' links to missing node '" + n.next + "'";
            return false;
        }
        if (!n.next.empty() && !n.choices.empty()) {
            out_error = "node '" + n.id + "' has both next and choices (pick one)";
            return false;
        }
        for (const auto& c : n.choices) {
            if (c.id.empty()) {
                out_error = "node '" + n.id + "' has a choice with an empty id";
                return false;
            }
            if (!c.next_node.empty() && !find(c.next_node)) {
                out_error = "choice '" + c.id + "' links to missing node '" + c.next_node + "'";
                return false;
            }
        }
    }
    return true;
}

namespace {

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

std::string DialogueTree::serialize() const {
    // node|id|speaker|text|next|choice_count;choice|id|text|next|req|set;...
    std::ostringstream out;
    for (const auto& n : m_nodes) {
        out << "node|" << escape_field(n.id) << '|' << escape_field(n.speaker) << '|'
            << escape_field(n.text) << '|' << escape_field(n.next) << '|' << n.choices.size();
        for (const auto& c : n.choices) {
            out << ";choice|" << escape_field(c.id) << '|' << escape_field(c.text) << '|'
                << escape_field(c.next_node) << '|' << escape_field(c.requires_tag) << '|'
                << escape_field(c.sets_tag);
        }
        out << '\n';
    }
    return out.str();
}

bool DialogueTree::parse(const std::string& text) {
    std::vector<DialogueNode> parsed;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const auto parts = split_top(line, ';');
        if (parts.empty()) continue;
        const auto head = split_top(parts[0], '|');
        // node|id|speaker|text|next|choice_count
        if (head.size() != 6 || head[0] != "node") continue;
        DialogueNode node;
        node.id = unescape_field(head[1]);
        node.speaker = unescape_field(head[2]);
        node.text = unescape_field(head[3]);
        node.next = unescape_field(head[4]);
        if (node.id.empty()) continue;
        usize choice_count = 0;
        try {
            choice_count = static_cast<usize>(std::stoul(head[5]));
        } catch (...) {
            continue;
        }
        bool line_ok = true;
        for (usize i = 0; i < choice_count; ++i) {
            if (1 + i >= parts.size()) {
                line_ok = false;
                break;
            }
            const auto cp = split_top(parts[1 + i], '|');
            // choice|id|text|next|req|set
            if (cp.size() != 6 || cp[0] != "choice" || unescape_field(cp[1]).empty()) {
                line_ok = false;
                break;
            }
            DialogueChoice c;
            c.id = unescape_field(cp[1]);
            c.text = unescape_field(cp[2]);
            c.next_node = unescape_field(cp[3]);
            c.requires_tag = unescape_field(cp[4]);
            c.sets_tag = unescape_field(cp[5]);
            node.choices.push_back(std::move(c));
        }
        if (!line_ok) continue;
        bool dup = false;
        for (const auto& n : parsed) {
            if (n.id == node.id) {
                dup = true;
                break;
            }
        }
        if (!dup) parsed.push_back(std::move(node));
    }
    m_nodes = std::move(parsed);
    return true;
}

bool DialogueRunner::start(const DialogueTree& tree, const std::string& start_node) {
    const DialogueNode* n = tree.find(start_node);
    m_tree = &tree;
    m_current = n;
    return n != nullptr;
}

std::vector<const DialogueChoice*> DialogueRunner::available_choices(const TagContainer& tags) const {
    std::vector<const DialogueChoice*> out;
    if (!m_current) return out;
    for (const auto& c : m_current->choices) {
        if (c.requires_tag.empty() || tags.has(c.requires_tag)) {
            out.push_back(&c);
        }
    }
    return out;
}

bool DialogueRunner::choose(const std::string& choice_id, TagContainer& tags) {
    if (!m_current || !m_tree) return false;
    for (const auto& c : m_current->choices) {
        if (c.id != choice_id) continue;
        if (!c.requires_tag.empty() && !tags.has(c.requires_tag)) return false;
        if (!c.sets_tag.empty()) tags.add(c.sets_tag);
        m_current = c.next_node.empty() ? nullptr : m_tree->find(c.next_node);
        return true;
    }
    return false;
}

bool DialogueRunner::advance() {
    if (!m_current || !m_tree || !m_current->choices.empty()) return false;
    if (m_current->next.empty()) {
        m_current = nullptr; // terminal line: advancing ends the run
        return true;
    }
    m_current = m_tree->find(m_current->next);
    return m_current != nullptr;
}

} // namespace nf::gameplay
