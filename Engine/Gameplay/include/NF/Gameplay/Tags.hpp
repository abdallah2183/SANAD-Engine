#pragma once

// NF/Gameplay/Tags.hpp — hierarchical gameplay tags (design doc Section 211).
//
// Tags are dot-separated paths ("enemy.boss.fire"). A container holding
// "enemy.boss.fire" implicitly holds "enemy.boss" and "enemy", so broad
// queries ("enemy") match specific tags without enumerating them.
//
// TagQuery is a tiny boolean language over tags for gameplay rules:
//   "enemy.boss & !enemy.immune_fire | player"
// Precedence: ! binds tightest, then &, then |. Parentheses group.
// An empty/invalid query matches nothing (never everything — fail closed).

#include <NF/Core/Types.hpp>

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace nf::gameplay {

/// True when `tag` equals `prefix` or extends it ("a.b" extends "a").
bool tag_matches_prefix(const std::string& tag, const std::string& prefix);

class TagContainer {
public:
    void add(const std::string& tag);
    void remove(const std::string& tag);
    void clear();

    /// Exact membership (no hierarchy).
    bool has_exact(const std::string& tag) const;
    /// True when any held tag equals `tag` or extends it.
    bool has(const std::string& tag) const;
    bool empty() const { return m_tags.empty(); }
    usize size() const { return m_tags.size(); }

    /// All held tags, sorted (deterministic for saves/diffs).
    std::vector<std::string> all() const;

private:
    std::set<std::string> m_tags; // ordered => deterministic
};

/// Compiled tag query. Parse once, evaluate often.
class TagQuery {
public:
    /// Query tree node. Public only so the .cpp parser can build it;
    /// not part of the API.
    struct Node;
    /// Parses `expression`. On failure ok() is false and error() explains.
    static TagQuery parse(const std::string& expression);

    bool ok() const { return m_ok; }
    const std::string& error() const { return m_error; }

    /// Evaluates against a container. An invalid query matches nothing.
    bool matches(const TagContainer& tags) const;

private:
    TagQuery() = default;
    bool m_ok = false;
    std::string m_error;
    std::shared_ptr<const Node> m_root;
};

} // namespace nf::gameplay
