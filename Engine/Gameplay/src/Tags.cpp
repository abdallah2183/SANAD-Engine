// NF/Gameplay/Tags.cpp — hierarchical gameplay tags + query language.

#include <NF/Gameplay/Tags.hpp>

#include <cctype>
#include <memory>
#include <utility>

namespace nf::gameplay {

bool tag_matches_prefix(const std::string& tag, const std::string& prefix) {
    if (prefix.empty() || tag.size() < prefix.size()) return false;
    if (tag.compare(0, prefix.size(), prefix) != 0) return false;
    return tag.size() == prefix.size() || tag[prefix.size()] == '.';
}

// --- TagContainer ------------------------------------------------------------

void TagContainer::add(const std::string& tag) {
    if (!tag.empty()) m_tags.insert(tag);
}

void TagContainer::remove(const std::string& tag) {
    m_tags.erase(tag);
}

void TagContainer::clear() {
    m_tags.clear();
}

bool TagContainer::has_exact(const std::string& tag) const {
    return m_tags.find(tag) != m_tags.end();
}

bool TagContainer::has(const std::string& tag) const {
    if (tag.empty()) return false;
    // Exact hit, or a held descendant (holding "enemy.boss" answers the
    // query "enemy"). The reverse never matches: holding "enemy" does not
    // make the entity an "enemy.boss". Ordered set: lower_bound(tag) is the
    // only candidate that can carry `tag` as a prefix.
    const auto it = m_tags.lower_bound(tag);
    return it != m_tags.end() && tag_matches_prefix(*it, tag);
}

std::vector<std::string> TagContainer::all() const {
    return std::vector<std::string>(m_tags.begin(), m_tags.end());
}

// --- TagQuery ----------------------------------------------------------------

struct TagQuery::Node {
    enum class Kind { Tag, Not, And, Or };
    Kind kind = Kind::Tag;
    std::string tag; // Kind::Tag only
    std::shared_ptr<const Node> left;
    std::shared_ptr<const Node> right; // And/Or only

    bool evaluate(const TagContainer& tags) const {
        switch (kind) {
            case Kind::Tag: return tags.has(tag);
            case Kind::Not: return !left->evaluate(tags);
            case Kind::And: return left->evaluate(tags) && right->evaluate(tags);
            case Kind::Or: return left->evaluate(tags) || right->evaluate(tags);
        }
        return false;
    }
};

namespace {

bool is_tag_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' || c == '-';
}

// Recursive-descent parser: or := and ("|" and)*, and := unary ("&" unary)*,
// unary := "!" unary | "(" or ")" | tag.
class Parser {
public:
    explicit Parser(const std::string& text) : m_text(text) {}

    std::shared_ptr<TagQuery::Node> parse_or(std::string& err) {
        auto left = parse_and(err);
        if (!left) return nullptr;
        skip_ws();
        while (peek() == '|') {
            consume();
            auto right = parse_and(err);
            if (!right) {
                err = "expected operand after '|'";
                return nullptr;
            }
            auto node = std::make_shared<TagQuery::Node>();
            node->kind = TagQuery::Node::Kind::Or;
            node->left = std::move(left);
            node->right = std::move(right);
            left = std::move(node);
            skip_ws();
        }
        return left;
    }

    bool at_end() {
        skip_ws();
        return m_pos >= m_text.size();
    }

private:
    std::shared_ptr<TagQuery::Node> parse_and(std::string& err) {
        auto left = parse_unary(err);
        if (!left) return nullptr;
        skip_ws();
        while (peek() == '&') {
            consume();
            auto right = parse_unary(err);
            if (!right) {
                err = "expected operand after '&'";
                return nullptr;
            }
            auto node = std::make_shared<TagQuery::Node>();
            node->kind = TagQuery::Node::Kind::And;
            node->left = std::move(left);
            node->right = std::move(right);
            left = std::move(node);
            skip_ws();
        }
        return left;
    }

    std::shared_ptr<TagQuery::Node> parse_unary(std::string& err) {
        skip_ws();
        if (peek() == '!') {
            consume();
            auto child = parse_unary(err);
            if (!child) {
                err = "expected operand after '!'";
                return nullptr;
            }
            auto node = std::make_shared<TagQuery::Node>();
            node->kind = TagQuery::Node::Kind::Not;
            node->left = std::move(child);
            return node;
        }
        if (peek() == '(') {
            consume();
            auto inner = parse_or(err);
            if (!inner) return nullptr;
            skip_ws();
            if (peek() != ')') {
                err = "expected ')'";
                return nullptr;
            }
            consume();
            return inner;
        }
        std::string tag;
        while (m_pos < m_text.size() && is_tag_char(m_text[m_pos])) {
            tag.push_back(m_text[m_pos++]);
        }
        if (tag.empty()) {
            err = "expected a tag";
            return nullptr;
        }
        // Reject degenerate dots (".a", "a..b", "a.") — they never match.
        if (tag.front() == '.' || tag.back() == '.' || tag.find("..") != std::string::npos) {
            err = std::string("malformed tag '") + tag + "'";
            return nullptr;
        }
        auto node = std::make_shared<TagQuery::Node>();
        node->kind = TagQuery::Node::Kind::Tag;
        node->tag = std::move(tag);
        return node;
    }

    void skip_ws() {
        while (m_pos < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_pos])) != 0) {
            ++m_pos;
        }
    }
    char peek() const { return m_pos < m_text.size() ? m_text[m_pos] : '\0'; }
    void consume() { ++m_pos; }

    const std::string& m_text;
    usize m_pos = 0;
};

} // namespace

TagQuery TagQuery::parse(const std::string& expression) {
    TagQuery q;
    Parser parser(expression);
    std::string err;
    auto root = parser.parse_or(err);
    if (!root) {
        q.m_error = err.empty() ? "empty query" : err;
        return q;
    }
    if (!parser.at_end()) {
        q.m_error = "unexpected trailing input";
        return q;
    }
    q.m_root = std::move(root);
    q.m_ok = true;
    return q;
}

bool TagQuery::matches(const TagContainer& tags) const {
    if (!m_ok || !m_root) return false;
    return m_root->evaluate(tags);
}

} // namespace nf::gameplay
